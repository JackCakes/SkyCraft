#include "ResourceBridge.h"
#include "Game.h"
#include "Compat.h"

#include <algorithm>
#include <cctype>

namespace skycraft
{
	namespace
	{
		constexpr float kActorRange = 80.0f * static_cast<float>(proto::kUnitsPerBlock);  // stand-ins exist this far out
		constexpr float kPi = 3.14159265f;
		constexpr float kHitMemorySeconds = 0.35f;   // how long a hit event can explain a health drop
		constexpr float kDotFlushSeconds = 0.5f;     // magic/other damage is batched (Minecraft i-frames)

		// Skyrim actors this plugin made as stand-ins for Minecraft's mobs (see below): everything else that
		// looks at Skyrim's actors (the actor table, hazards) must leave them out.
		std::unordered_set<RE::FormID> proxyIds;
		// Stand-ins held at zero alpha at the moment (see HideProxy): until their body has been found and hidden, so that nothing of a
		// stand-in's appearance (the fade-in Skyrim gives an actor whose 3D has just loaded) is ever drawn.
		std::unordered_set<RE::FormID> proxyFaded;
		// Seconds since the player last hit each stand-in (a hit event with the player as its cause): who gets the blame when the
		// stand-in's health drops. The player's own spells, cast at the mob they locked on to, are the usual way.
		std::unordered_map<RE::FormID, float> proxyPlayerHitAge;
		// Stand-ins the player has just hit with a weapon or an arrow (seconds left): Minecraft already has that blow (the swing or the
		// shot is sent to it as such), so what the hit takes off the stand-in is given back without being passed on. A spell is not
		// sent that way, so the damage it does to a stand-in is what hurts the real mob.
		std::unordered_map<RE::FormID, float> proxyWeaponHits;
		// Damage Skyrim dealt to a stand-in beyond what it could take (see StandInDamageGuard): Skyrim's hit would have killed the stand-in, so
		// the stand-in only takes what leaves it standing, and the rest is kept here, to be passed on to Minecraft's mob with the rest of the blow.
		std::unordered_map<RE::FormID, float> proxyOverkill;

		// ---- what hit the player (main thread) -------------------------------------------------
		struct RecentHit
		{
			RE::FormID   attacker{ 0 };
			proto::HurtKind kind{ proto::kHurtMelee };
			std::uint32_t flags{ 0 };
			float        age{ 99.0f };
		};
		RecentHit lastHit;               // from TESHitEvent (kind, power attack, ...)
		RE::FormID lastDamager{ 0 };     // from HandleHealthDamage
		float      lastDamagerAge{ 99.0f };

		struct PendingHurt
		{
			RE::FormID      attacker{ 0 };
			proto::HurtKind kind{ proto::kHurtOther };
			std::uint32_t   flags{ 0 };
			float           damage{ 0.0f };
			float           age{ 0.0f };
		};
		PendingHurt dot;  // accumulated magic / unattributed damage

		bool  essentialSet = false;
		bool  healthPrimed = false;
		float diagTimer = 5.0f;
		bool  engaged = false;
		float engagedTimer = 0.0f;

		// Diagnostics: the engine's own events (3D loaded or unloaded, cell attach or detach, combat, death) for a mob's stand-in, written to
		// the log in order with everything else, so what happens just before a stand-in is found switched off shows. Logs only; changes nothing.
		template <class E>
		class StandInEventLog final : public RE::BSTEventSink<E>
		{
		public:
			static StandInEventLog* Get(const char* a_name)
			{
				static StandInEventLog sink;
				sink.name = a_name;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const E* a_event, RE::BSTEventSource<E>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				RE::FormID id = 0;
				if constexpr (requires { a_event->formID; }) {
					id = static_cast<RE::FormID>(a_event->formID);
				} else if constexpr (requires { a_event->reference.get(); }) {
					id = a_event->reference.get() ? a_event->reference.get()->GetFormID() : 0;
				} else if constexpr (requires { a_event->actor.get(); }) {
					id = a_event->actor.get() ? a_event->actor.get()->GetFormID() : 0;
				} else if constexpr (requires { a_event->actorDying.get(); }) {
					id = a_event->actorDying.get() ? a_event->actorDying.get()->GetFormID() : 0;
				}
				int detail = -1;
				if constexpr (requires { static_cast<int>(a_event->loaded); }) {
					detail = static_cast<int>(a_event->loaded);
				} else if constexpr (requires { static_cast<int>(a_event->action); }) {
					detail = static_cast<int>(a_event->action);
				} else if constexpr (requires { static_cast<int>(a_event->newState); }) {
					detail = static_cast<int>(a_event->newState);
				} else if constexpr (requires { static_cast<int>(a_event->dead); }) {
					detail = static_cast<int>(a_event->dead);
				}
				static int logged = 0;
				if (id != 0 && proxyIds.contains(id) && logged++ < 300) {
					logger::info("Engine event on stand-in ({:08X}): {} ({})", id, name, detail);
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			const char* name{ "" };
		};

		class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			static HitSink* Get()
			{
				static HitSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (a_event && a_event->target) {  // diagnostic: does the engine report hits on a mob's collision box at all? (Only logs.)
					static int seen = 0;
					if (seen < 20) {
						if (const auto mobId = MobBodies::MobOfRef(a_event->target->GetFormID()); mobId >= 0) {
							++seen;
							logger::info("Hit event on the body of mob {}: source {:08X}, projectile {:08X}", mobId, a_event->source, a_event->projectile);
						}
					}
				}
				if (a_event && player && a_event->cause && a_event->cause.get() == player && a_event->target && proxyIds.contains(a_event->target->GetFormID())) {
					bool spell = false;
					if (auto* source = RE::TESForm::LookupByID(a_event->source)) {
						switch (source->GetFormType()) {
						case RE::FormType::Spell:
						case RE::FormType::Enchantment:
						case RE::FormType::Scroll:
						case RE::FormType::Ingredient:
						case RE::FormType::AlchemyItem:
							spell = true;
							break;
						default:
							break;
						}
					}
					proxyPlayerHitAge[a_event->target->GetFormID()] = 0.0f;  // the player hit a mob's stand-in
					if (!spell) {
						proxyWeaponHits[a_event->target->GetFormID()] = 0.6f;
					}
					return RE::BSEventNotifyControl::kContinue;
				}
				if (!a_event || !player || a_event->target.get() != player || !State().puppeting) {
					return RE::BSEventNotifyControl::kContinue;
				}
				RecentHit hit;
				hit.attacker = a_event->cause ? a_event->cause->GetFormID() : 0;
				hit.age = 0.0f;
				if (a_event->projectile != 0) {
					hit.kind = proto::kHurtProjectile;
				} else if (auto* source = RE::TESForm::LookupByID(a_event->source)) {
					switch (source->GetFormType()) {
					case RE::FormType::Spell:
					case RE::FormType::Enchantment:
					case RE::FormType::Scroll:
					case RE::FormType::Ingredient:
					case RE::FormType::AlchemyItem:
						hit.kind = proto::kHurtMagic;
						break;
					default:
						hit.kind = proto::kHurtMelee;
						break;
					}
				}
				if (a_event->flags.any(RE::TESHitEvent::Flag::kPowerAttack)) {
					hit.flags |= proto::kHurtPowerAttack;
				}
				if (a_event->flags.any(RE::TESHitEvent::Flag::kHitBlocked)) {
					hit.flags |= proto::kHurtBlockedInSkyrim;
				}
				lastHit = hit;
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// Skyrim's damage path calls this on the victim with the attacker; we only need "who".
		struct HandleHealthDamageHook
		{
			static void thunk(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
			{
				func(a_this, a_attacker, a_damage);
				if (a_attacker && a_attacker != a_this) {
					lastDamager = a_attacker->GetFormID();
					lastDamagerAge = 0.0f;
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void SendHurt(proto::HurtKind a_kind, float a_damage, RE::FormID a_attacker, std::uint32_t a_flags)
		{
			if (a_damage <= 0.01f) {
				return;
			}
			Link::Get().PushInput(proto::kInHurt, static_cast<std::uint16_t>(a_kind), static_cast<std::int32_t>(a_damage * 100.0f),
				static_cast<std::int32_t>(a_attacker), static_cast<std::int32_t>(a_flags));
			logger::info("player hit by {:08X}: {:.1f} Skyrim damage ({}) -> {:.1f} Minecraft", a_attacker, a_damage,
				a_kind == proto::kHurtMelee ? "melee" : a_kind == proto::kHurtProjectile ? "projectile" : a_kind == proto::kHurtMagic ? "magic" : "other",
				a_damage / 5.0f);
		}

		void SetEssential(RE::PlayerCharacter* a_player, bool a_on)
		{
			if (a_on == essentialSet) {
				return;
			}
			auto& flags = a_player->GetActorRuntimeData().boolFlags;
			if (a_on) {
				flags.set(RE::Actor::BOOL_FLAGS::kEssential);
			} else {
				flags.reset(RE::Actor::BOOL_FLAGS::kEssential);
			}
			essentialSet = a_on;
		}

		// Minecraft owns the player's health: Skyrim damage is refunded here and sent to Minecraft.
		void BridgePlayerDamage(RE::PlayerCharacter* a_player, float a_delta)
		{
			auto*       av = a_player->AsActorValueOwner();
			const float max = a_player->GetActorValueMax(RE::ActorValue::kHealth);
			const float cur = av->GetActorValue(RE::ActorValue::kHealth);
			const float deficit = max - cur;
			lastHit.age += a_delta;
			lastDamagerAge += a_delta;
			if (!healthPrimed) {
				// Whatever damage the save had before Minecraft took over isn't a new hit.
				healthPrimed = true;
				if (deficit > 0.0f) {
					av->RestoreActorValue(RE::ActorValue::kHealth, deficit);
				}
				return;
			}
			if (deficit > 0.01f) {
				av->RestoreActorValue(RE::ActorValue::kHealth, deficit);
				RE::FormID      attacker = 0;
				proto::HurtKind kind = proto::kHurtOther;
				std::uint32_t   flags = 0;
				if (lastHit.age < kHitMemorySeconds) {
					attacker = lastHit.attacker;
					kind = lastHit.kind;
					flags = lastHit.flags;
					lastHit.age = 99.0f;  // one hit event explains one health drop
				} else if (lastDamagerAge < kHitMemorySeconds) {
					attacker = lastDamager;
					kind = proto::kHurtMagic;  // damage over time from someone (spells, poison)
				}
				if (kind == proto::kHurtMelee || kind == proto::kHurtProjectile) {
					SendHurt(kind, deficit, attacker, flags);
				} else {
					dot.attacker = attacker ? attacker : dot.attacker;
					dot.kind = kind;
					dot.damage += deficit;
				}
			}
			dot.age += a_delta;
			if (dot.age >= kDotFlushSeconds) {
				SendHurt(dot.kind, dot.damage, dot.attacker, 0);
				dot = PendingHurt{};
			}
		}

		// The weapon calls below are the ones CommonLib has under different names or not at all in some versions:
		// whichever exists is used, and without any, the hit is a fist's.
		template <class T>
		float WeaponBaseDamage(T* a_weapon)
		{
			if constexpr (requires { a_weapon->GetAttackDamage(); }) {
				return static_cast<float>(a_weapon->GetAttackDamage());
			} else if constexpr (requires { a_weapon->attackDamage; }) {
				return static_cast<float>(a_weapon->attackDamage);
			} else {
				return 8.0f;
			}
		}

		template <class T>
		bool WeaponIsRanged(T* a_weapon)
		{
			bool ranged = false;
			if constexpr (requires { a_weapon->IsBow(); }) {
				ranged = ranged || a_weapon->IsBow();
			}
			if constexpr (requires { a_weapon->IsCrossbow(); }) {
				ranged = ranged || a_weapon->IsCrossbow();
			}
			return ranged;
		}

		template <class T>
		bool WeaponIsStaff(T* a_weapon)
		{
			if constexpr (requires { a_weapon->IsStaff(); }) {
				return a_weapon->IsStaff();
			} else {
				return false;
			}
		}

		template <class T>
		RE::TESForm* RightHandItem(T* a_actor)
		{
			if constexpr (requires { a_actor->GetEquippedObject(false); }) {
				return a_actor->GetEquippedObject(false);
			} else {
				return nullptr;
			}
		}

		void WriteActorTable(RE::PlayerCharacter* a_player)
		{
			static std::vector<proto::ActorRecord> records;
			records.clear();
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				Link::Get().WriteActors(nullptr, 0);
				return;
			}
			const auto playerPos = a_player->GetPosition();
			for (auto& handle : lists->highActorHandles) {
				auto actorPtr = handle.get();
				auto* actor = actorPtr.get();
				if (!actor || actor == a_player || actor->IsDisabled() || !actor->Is3DLoaded() || actor->IsGhost() || proxyIds.contains(actor->GetFormID())) {
					continue;
				}
				const auto pos = actor->GetPosition();
				if (pos.GetDistance(playerPos) > kActorRange) {
					continue;
				}
				proto::ActorRecord r{};
				r.formId = actor->GetFormID();
				r.flags = (actor->IsHostileToActor(a_player) ? proto::kActorHostile : 0u) | (actor->IsDead() ? proto::kActorDead : 0u) |
				          (actor->IsEssential() ? proto::kActorEssential : 0u) | (actor->IsInCombat() ? proto::kActorInCombat : 0u) |
			          (actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kAggression) >= 1.0f ? proto::kActorFighter : 0u);
				if (auto* state = actor->AsActorState(); state && state->GetWeaponState() == RE::WEAPON_STATE::kDrawn) {
					r.flags |= proto::kActorWeaponDrawn;
				}
				// A person (human, elf, orc...), not an animal: where there are people going about their day, nothing spawns.
				if (actor->HasKeywordString("ActorTypeNPC")) {
					r.flags |= proto::kActorPerson;
				}
				float weaponDamage = 4.0f;  // fists
				if (auto* held = RightHandItem(actor)) {
					if (auto* weapon = held->As<RE::TESObjectWEAP>()) {
						weaponDamage = WeaponBaseDamage(weapon);
						if (WeaponIsRanged(weapon)) {
							r.flags |= proto::kActorBow;
						}
					}
				}
				// What the NPC's blow does in Minecraft follows its weapon: a guard's sword hits harder than a farmer's fists.
				r.flags |= static_cast<std::uint32_t>(std::clamp(weaponDamage, 1.0f, 255.0f)) << proto::kActorDamageShift;
				const auto mc = SkyToMc(pos);
				r.x = static_cast<float>(mc.x);
				r.y = static_cast<float>(mc.y);
				r.z = static_cast<float>(mc.z);
				r.yaw = HeadingToMcYaw(actor->GetAngleZ());
				const float k = 1.0f / static_cast<float>(proto::kUnitsPerBlock);
				r.height = std::clamp(actor->GetHeight() * k, 0.3f, 12.0f);
				r.width = std::clamp(actor->GetBoundRadius() * 2.0f * k, 0.3f, 6.0f);
				const float maxHealth = actor->GetActorValueMax(RE::ActorValue::kHealth);
				r.healthFrac = maxHealth > 0.0f ? std::clamp(actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) / maxHealth, 0.0f, 1.0f) : 0.0f;
				r.level = actor->GetLevel();
				if (const char* name = actor->GetDisplayFullName()) {
					strncpy_s(r.name, name, _TRUNCATE);
				}
				records.push_back(r);
				if (records.size() >= proto::kMaxActors) {
					break;
				}
			}
			Link::Get().WriteActors(records.data(), static_cast<std::uint32_t>(records.size()));
		}

		// ---- Skyrim's own hit pipeline ---------------------------------------------------------
		// ID 38627 is Skyrim's melee "HitFrame" handler: it builds a HitData (Ctor 43995,
		// Populate 44001) and hands it to ID 38586, which applies the hit to the victim: damage,
		// blocking, hit reactions and stagger, pain voice, the hit event (crime, quests, the enemy
		// health bar), kill credit. We feed Minecraft's hits through the same function.
		using ProcessHitFn = void(RE::Actor*, RE::HitData&);
		using HitDataCtorFn = RE::HitData*(RE::HitData*);
		ProcessHitFn*  processHit = nullptr;
		HitDataCtorFn* hitDataCtor = nullptr;

		void ResolveHitPipeline()
		{
			if (!REL::Module::IsAE()) {
				logger::info("hit pipeline: not on AE; using plain damage");
				return;
			}
			// Only trust the address if the engine's own melee code really calls it.
			const auto  site = REL::ID(38627).address() + 0x4A8;
			const auto* code = reinterpret_cast<const std::uint8_t*>(site);
			const auto  target = REL::ID(38586).address();
			std::int32_t rel = 0;
			std::memcpy(&rel, code + 1, 4);
			if (code[0] != 0xE8 || site + 5 + rel != target) {
				logger::warn("hit pipeline: melee call site doesn't match this game build; using plain damage");
				return;
			}
			processHit = reinterpret_cast<ProcessHitFn*>(target);
			hitDataCtor = reinterpret_cast<HitDataCtorFn*>(REL::ID(43995).address());
			logger::info("hit pipeline: Minecraft hits go through Skyrim's hit processing");
		}

		// A vanilla weapon standing in for the Minecraft one: its impact set gives the blood
		// spray and impact sound, its type the skill and sounds.
		RE::TESObjectWEAP* StandInWeapon(std::uint32_t a_weapon)
		{
			RE::FormID id = 0;
			switch (a_weapon) {
			case proto::kWeaponBlade:
				id = 0x00012EB7;  // Iron Sword
				break;
			case proto::kWeaponAxe:
				id = 0x00013790;  // Iron War Axe
				break;
			case proto::kWeaponPierce:
				id = 0x0001397E;  // Iron Dagger
				break;
			case proto::kWeaponArrow:
				id = 0x00013985;  // Hunting Bow
				break;
			default:
				id = 0x00013982;  // Iron Mace (fists, tools, blunt things)
				break;
			}
			return RE::TESForm::LookupByID<RE::TESObjectWEAP>(id);
		}

		// The Skyrim skill a Minecraft hit trains: swords, maces and tools One-Handed; axes and
		// tridents/spears in hand Two-Handed; arrows and anything thrown Archery; fists nothing.
		RE::ActorValue HitSkill(const proto::McEvent& a_ev)
		{
			if (a_ev.flags & proto::kHitProjectile) {
				return RE::ActorValue::kArchery;
			}
			switch (a_ev.weapon & 0xFFu) {  // low byte: the weapon class (a mob's hit also carries the mob's id above it)
			case proto::kWeaponUnarmed:
				return RE::ActorValue::kNone;
			case proto::kWeaponAxe:
			case proto::kWeaponPierce:
				return RE::ActorValue::kTwoHanded;
			default:
				return RE::ActorValue::kOneHanded;
			}
		}

		// Skyrim's own skill experience (what its weapons, blocking, armour and crafting call), so
		// skills level up with Skyrim's notification and count towards the character's level.
		// Uses are counted like Skyrim counts them: a weapon hit is worth the weapon's damage.
		void TrainSkill(RE::PlayerCharacter* a_player, RE::ActorValue a_skill, float a_uses)
		{
			if (a_skill == RE::ActorValue::kNone || !(a_uses >= 0.05f)) {
				return;  // nothing, or a scratch (damage over time ticks)
			}
			const float before = a_player->AsActorValueOwner()->GetBaseActorValue(a_skill);
			a_player->AddSkillExperience(a_skill, std::min(a_uses, 500.0f));
			const float after = a_player->AsActorValueOwner()->GetBaseActorValue(a_skill);
			static int logged = 0;
			if (after > before || logged < 8) {
				++logged;
				logger::info("skill {} +{:.1f} uses ({:.0f}{})", static_cast<int>(a_skill), a_uses, after, after > before ? ", levelled up" : "");
			}
		}

		RE::NiAVObject* HitNode(RE::Actor* a_actor)
		{
			auto* root = a_actor->Get3D();
			if (!root) {
				return nullptr;
			}
			for (const char* name : { "NPC Spine2 [Spn2]", "NPC Spine1 [Spn1]", "NPC Spine [Spn0]", "NPC Pelvis [Pelv]" }) {
				if (auto* node = root->GetObjectByName(name)) {
					return node;
				}
			}
			return root;
		}

		// A Minecraft mob's hit on an actor's stand-in (kHitFromMob): the same damage and stagger as a hit
		// of the player's, but nobody in Skyrim is the attacker. The player didn't do it, so no bounty, no
		// combat with the player and no skill experience; the actor just takes the damage, staggers if it
		// was a big one, and bleeds where it was hit.
		// Takes health off an actor with no attacker, by whichever call this CommonLib has (they differ between versions).
		// Set while SkyCraft itself changes a stand-in's health (the sync to the mob's, giving back what Skyrim took): StandInDamageGuard
		// cut every call on a stand-in to nothing, ours included, so the stand-in's health never followed the mob's.
		inline int ownHealthChange = 0;

		template <class T>
		void DamageHealth(T* a_actor, float a_damage)
		{
			struct Own
			{
				Own() { ++ownHealthChange; }
				~Own() { --ownHealthChange; }
			} own;
			auto* values = a_actor->AsActorValueOwner();
			if constexpr (requires { values->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -a_damage); }) {
				values->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -a_damage);
			} else if constexpr (requires { a_actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -a_damage); }) {
				a_actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -a_damage);
			} else if constexpr (requires { values->RestoreActorValue(RE::ActorValue::kHealth, -a_damage); }) {
				values->RestoreActorValue(RE::ActorValue::kHealth, -a_damage);  // this CommonLib takes (value, amount); negative is damage
			} else if constexpr (requires { values->ModActorValue(RE::ActorValue::kHealth, -a_damage); }) {
				values->ModActorValue(RE::ActorValue::kHealth, -a_damage);
			} else {
				a_actor->DoDamage(a_damage, nullptr, true);
			}
		}

		// A mob's stand-in has the mob's own health (1:1, so a lock-on bar shows the mob's), which is a few dozen points: a witch's 26, a
		// zombie's 20. Skyrim's hits are bigger than that: one swing of a Skyrim sword takes 30 or more, and in Skyrim's control mode
		// those swings land on the stand-ins for real. A stand-in that Skyrim's damage took to zero (it is essential: it doesn't die, it goes
		// into bleedout) is one lock-on mods refuse (True Directional Movement drops a bleeding-out essential target), and with its AI off
		// nothing ever gets it up again: that is the stand-in that "vanishes after a while".
		// So Skyrim's damage to a stand-in is cut to what leaves it standing, whatever the hit (weapon, arrow, spell, an NPC's blow). What
		// was cut is kept (proxyOverkill) and still counts as damage to the mob when it is passed on to Minecraft. (The sign Skyrim gives the
		// damage here is not relied on: its size is cut and its sign kept.)
		struct StandInDamageGuard
		{
			static void thunk(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
			{
				if (a_this && ownHealthChange == 0 && proxyIds.contains(a_this->GetFormID()) && !a_this->IsDead()) {
					// A stand-in's health is its mob's, 1:1, and changes only when the mob's does. Skyrim's blow takes nothing off the stand-in
					// (it used to take what left it standing, and the bar then jumped back a frame later); all of it counts as damage to the
					// mob (proxyOverkill, see AbsorbProxyDamage), and the stand-in loses exactly what the mob then loses.
					const float size = std::fabs(a_damage);
					if (size > 0.0f) {
						// Diagnostic: every blow that reaches a stand-in, and who threw it (does an NPC's swing ever connect?).
						static int seen = 0;
						if (seen++ < 60) {
							logger::info("Blow on stand-in ({:08X}): {:.1f} damage from '{}' ({:08X}){}", a_this->GetFormID(), size,
								a_attacker ? a_attacker->GetDisplayFullName() : "nobody", a_attacker ? a_attacker->GetFormID() : 0u,
								a_attacker && a_attacker->IsPlayerRef() ? " (the player)" : "");
						}
						proxyOverkill[a_this->GetFormID()] += size;
						a_damage = 0.0f;
					}
				}
				func(a_this, a_attacker, a_damage);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Minecraft's blows are small and quick, so most of them don't stagger anyone; a real one (5% of the
		// NPC's health or more) makes them flinch, at most every couple of seconds so a fight isn't a stun-lock.
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> lastFlinch;

		// How much a Minecraft mob's blow staggers (and so shoves) whoever it hit. A mob's melee blow never does: knocking people back
		// on every swing of a zombie or a vindicator just gets in the way of the fight, so only a really heavy blow (a_stagger of 0.5 or
		// more, like a ravager's) does. Shots are the other way round: a big enough arrow makes its target flinch and lose a step, which
		// is what keeps Skyrim's enemies from closing in on an archer as fast as they otherwise would.
		float MobStagger(RE::Actor* a_actor, const proto::McEvent& a_ev, float a_damage, float a_stagger)
		{
			const bool projectile = (a_ev.flags & proto::kHitProjectile) != 0;
			if (!projectile) {
				if (a_stagger >= 0.5f) {
					return a_stagger;
				}
				// An ordinary blow makes an NPC flinch, a little and not too often: without it the mob seems to hit thin air. (Not the player,
				// who is steering, and not so hard that a fight turns into a stun-lock: every couple of seconds at most.)
				const float melee = a_actor->GetActorValueMax(RE::ActorValue::kHealth);
				const auto  stamp = std::chrono::steady_clock::now();
				auto&       seen = lastFlinch[a_actor->GetFormID()];
				if (!a_actor->IsPlayerRef() && melee > 0.0f && a_damage >= melee * 0.04f && stamp - seen > std::chrono::milliseconds(1800)) {
					seen = stamp;
					return 0.12f;
				}
				return 0.0f;
			}
			const float maxHealth = a_actor->GetActorValueMax(RE::ActorValue::kHealth);
			const auto  now = std::chrono::steady_clock::now();
			auto&       last = lastFlinch[a_ev.formId];
			if (a_stagger <= 0.0f && maxHealth > 0.0f && a_damage >= maxHealth * 0.05f && now - last > std::chrono::milliseconds(2500)) {
				last = now;
				a_stagger = 0.15f;
			}
			return a_stagger;
		}

		// Takes exactly this much health off an actor, and checks that it came off. Skyrim's calls for "damage with no attacker" are
		// not all honoured by every actor: on the player, each of them has been seen to take only part of what it is given (a share
		// that shrank as the player's health fell, down to nothing at around 85 of 100). So every way of taking health is tried in
		// turn on whatever is still owed, then again on what is still owed after that, until it is paid or nothing more comes off.
		// What each way took is logged, so a way that never works can be found and dropped. a_attacker (may be null) is the stand-in
		// of the mob that did it, for the ways that take someone to blame; it never goes through Skyrim's full hit processing (that is
		// where a skeever's disease is handed out).
		template <class T>
		float TakeHealthWith(T* a_actor, int a_way, float a_amount, RE::Actor* a_attacker)
		{
			auto*       values = a_actor->AsActorValueOwner();
			const float before = values->GetActorValue(RE::ActorValue::kHealth);
			switch (a_way) {
			case 0:
				DamageHealth(a_actor, a_amount);
				break;
			case 1:
				if constexpr (requires { a_actor->HandleHealthDamage(nullptr, 1.0f); }) {
					a_actor->HandleHealthDamage(a_attacker, a_amount);
				}
				break;
			case 2:
				a_actor->DoDamage(a_amount, nullptr, true);
				break;
			case 3:
				if constexpr (requires { values->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -a_amount); }) {
					values->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -a_amount);
				} else if constexpr (requires { a_actor->DamageActorValue(RE::ActorValue::kHealth, a_amount); }) {
					a_actor->DamageActorValue(RE::ActorValue::kHealth, a_amount);
				}
				break;
			default:
				break;
			}
			return before - values->GetActorValue(RE::ActorValue::kHealth);
		}

		template <class T>
		void DamageHealthExactly(T* a_actor, float a_damage, RE::Actor* a_attacker = nullptr)
		{
			auto*       values = a_actor->AsActorValueOwner();
			const float start = values->GetActorValue(RE::ActorValue::kHealth);
			const float want = start - a_damage;
			constexpr int kWays = 4;
			float         took[kWays] = {};
			for (int round = 0; round < 3; ++round) {
				float progress = 0.0f;
				for (int way = 0; way < kWays; ++way) {
					const float owed = values->GetActorValue(RE::ActorValue::kHealth) - want;
					if (owed <= 0.5f || a_actor->IsDead()) {
						break;
					}
					const float got = TakeHealthWith(a_actor, way, owed, a_attacker);
					took[way] += got;
					progress += std::max(0.0f, got);
				}
				if (values->GetActorValue(RE::ActorValue::kHealth) - want <= 0.5f || progress < 0.1f || a_actor->IsDead()) {
					break;
				}
			}
			const float now = values->GetActorValue(RE::ActorValue::kHealth);
			static int logged = 0;
			if (now > want + 0.5f && logged++ < 30) {
				logger::warn("damage to {}: asked for {:.1f}, health only went {:.1f} -> {:.1f} (ways: DamageHealth {:.1f}, HandleHealthDamage {:.1f}, DoDamage {:.1f}, other {:.1f}); "
							 "permanent {:.1f}, base {:.1f}, max {:.1f}, armor {:.1f}, magicka {:.1f}, stamina {:.1f}",
					a_actor->GetDisplayFullName(), a_damage, start, now, took[0], took[1], took[2], took[3], values->GetPermanentActorValue(RE::ActorValue::kHealth),
					values->GetBaseActorValue(RE::ActorValue::kHealth), a_actor->GetActorValueMax(RE::ActorValue::kHealth), values->GetActorValue(RE::ActorValue::kDamageResist),
					values->GetActorValue(RE::ActorValue::kMagicka), values->GetActorValue(RE::ActorValue::kStamina));
			}
		}

		RE::Actor* StandInForMob(std::int32_t a_mobId);  // defined with the stand-ins, below

		// The blood, hit sparks and flesh sound of a mob's blow on whoever it hit. Skyrim's hit processing (ProcessHit) takes the
		// health and plays the pain voice, but a weapon's blood comes from its impact data set, played on the victim's body: the
		// melee code does it from the swing's collision, which a Minecraft hit never has. So it is played here, for every blow a
		// mob lands. A mob hits with fists or claws, and a mace's blunt set (what \"fists\" would pick) leaves hardly a mark, so a
		// mob's blow always bleeds like a blade's, a shot like an arrow's.
		void PlayMobImpact(RE::Actor* a_actor, const proto::McEvent& a_ev, float a_damage)
		{
			auto*       impacts = RE::BGSImpactManager::GetSingleton();
			auto*       weapon = StandInWeapon((a_ev.flags & proto::kHitProjectile) != 0 ? proto::kWeaponArrow : proto::kWeaponBlade);
			auto*       node = HitNode(a_actor);
			if (!impacts || !weapon || !weapon->impactDataSet || !node || !(a_damage > 0.0f)) {
				return;
			}
			RE::NiPoint3 pick{ a_ev.b, -a_ev.c, 0.0f };  // the way the mob pushed, MC (x, z) -> Skyrim (x, y)
			pick = pick.Length() > 1e-3f ? pick / pick.Length() : RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
			impacts->PlayImpactEffect(a_actor, weapon->impactDataSet, node->name.c_str(), pick, 128.0f, false, false);
		}

		// The hit goes through Skyrim's own hit processing, with the attacking mob's stand-in as the attacker, so the
		// victim bleeds, blocks, flinches and cries out exactly as it does when Skyrim's own weapons hit it. Returns
		// false (nothing done) when that isn't possible: no hit pipeline on this game, or the mob has no stand-in.
		bool ApplyMobHitThroughSkyrim(RE::Actor* a_actor, const proto::McEvent& a_ev, float a_damage, float a_stagger)
		{
			if (!processHit || !hitDataCtor || a_damage <= 0.0f || a_actor->IsDead()) {
				return false;
			}
			const auto mobKey = static_cast<std::int32_t>((a_ev.weapon >> 8) & 0xFFFFFFu) - 1;
			auto*      attacker = mobKey >= 0 ? StandInForMob(mobKey) : nullptr;
			if (!attacker || attacker == a_actor) {
				return false;
			}
			a_stagger = MobStagger(a_actor, a_ev, a_damage, a_stagger);
			auto*        weapon = StandInWeapon(a_ev.weapon & 0xFFu);
			auto*        node = HitNode(a_actor);
			RE::NiPoint3 hitPos = node ? node->world.translate : a_actor->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, a_actor->GetHeight() * 0.6f };
			RE::NiPoint3 dir{ a_ev.b, -a_ev.c, 0.0f };  // the way the mob pushed, MC (x, z) -> Skyrim (x, y)
			dir = dir.Length() > 1e-3f ? dir / dir.Length() : RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
			const bool projectile = (a_ev.flags & proto::kHitProjectile) != 0;

			alignas(16) std::array<std::byte, sizeof(RE::HitData)> storage{};
			auto* hit = reinterpret_cast<RE::HitData*>(storage.data());
			hitDataCtor(hit);
			hit->Populate(attacker, a_actor, nullptr);
			hit->weapon = weapon;
			hit->hitPosition = hitPos;
			hit->hitDirection = dir;
			hit->totalDamage = a_damage;
			hit->physicalDamage = a_damage;
			hit->resistedPhysicalDamage = 0.0f;
			hit->resistedTypedDamage = 0.0f;
			hit->sneakAttackBonus = 1.0f;
			hit->bonusHealthDamageMult = 1.0f;
			hit->stagger = a_stagger;
			hit->pushBack = (a_stagger >= 0.25f || (projectile && a_stagger > 0.0f)) ? a_ev.d : 0.0f;  // (a light flinch doesn't shove)
			hit->skill = HitSkill(a_ev);
			hit->flags.reset(RE::HitData::Flag::kPowerAttack, RE::HitData::Flag::kCritical, RE::HitData::Flag::kSneakAttack, RE::HitData::Flag::kMeleeAttack);
			if (!projectile) {
				hit->flags.set(RE::HitData::Flag::kMeleeAttack);
			}
			const float healthBefore = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			processHit(a_actor, *hit);
			PlayMobImpact(a_actor, a_ev, a_damage);
			// Whoever was hit by a mob (a shot from across the field, most of all) turns on it: Skyrim's hit processing doesn't always
			// start the fight with an attacker that isn't a real combatant, and then the NPC just stands there and takes it.
			if (!a_actor->IsDead() && !a_actor->IsInCombat() && !a_actor->IsPlayerRef()) {
				a_actor->StartCombat(attacker);
			}
			static int logged = 0;
			if (logged++ < 40) {
				logger::info("a Minecraft mob hit {} ({:08X}) through Skyrim's hit processing for {:.0f} Skyrim damage: health {:.0f} -> {:.0f}", a_actor->GetDisplayFullName(),
					a_ev.formId, a_damage, healthBefore, a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth));
			}
			return true;
		}

		// a_standInAttacker: let the mob's stand-in be the attacker in Skyrim's hit processing (NPCs). Not for the player: a Skeever's
		// bite carries a disease (Ataxia) in Skyrim, and a hit "from" its stand-in must never give the player one.
		void ApplyMobHit(RE::Actor* a_actor, const proto::McEvent& a_ev, float a_damage, float a_stagger, bool a_standInAttacker = true)
		{
			if (a_standInAttacker && ApplyMobHitThroughSkyrim(a_actor, a_ev, a_damage, a_stagger)) {
				return;
			}
			if (a_damage > 0.0f) {
				// Taken straight off the health value: an attacker-less DoDamage can be dropped by Skyrim, and then
				// the NPC never feels the mob at all.
				const float healthBefore = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
				if (a_actor->IsEssential() && a_damage >= healthBefore) {
					a_damage = std::max(0.0f, healthBefore - 1.0f);  // an essential NPC is left at 1 health, not killed
				}
				{
					const auto mobKey = static_cast<std::int32_t>((a_ev.weapon >> 8) & 0xFFFFFFu) - 1;
					DamageHealthExactly(a_actor, a_damage, mobKey >= 0 ? StandInForMob(mobKey) : nullptr);
				}
				logger::info("damage to {}: health {:.0f} -> {:.0f}", a_actor->GetDisplayFullName(), healthBefore, a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth));
				if (!a_actor->IsDead() && a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) <= 0.0f && !a_actor->IsEssential()) {
					a_actor->KillImmediate();
				}
			}
			a_stagger = MobStagger(a_actor, a_ev, a_damage, a_stagger);
			if (a_stagger > 0.0f && !a_actor->IsDead()) {
				const float pushHeading = std::atan2(a_ev.b, -a_ev.c);  // MC (x, z) -> Skyrim heading
				float       sdir = (pushHeading - a_actor->GetAngleZ()) / (2.0f * kPi) + 0.5f;
				sdir -= std::floor(sdir);
				a_actor->SetGraphVariableFloat("staggerDirection", sdir);
				a_actor->SetGraphVariableFloat("staggerMagnitude", a_stagger);
				a_actor->NotifyAnimationGraph("staggerStart");
			}
			PlayMobImpact(a_actor, a_ev, a_damage);
			logger::info("a Minecraft mob hit {} ({:08X}, level {}) for {:.1f} Minecraft -> {:.0f} Skyrim damage{}", a_actor->GetDisplayFullName(), a_ev.formId,
				a_actor->GetLevel(), a_ev.a, a_damage, a_stagger > 0.0f ? ", stagger" : "");
		}

		// A Minecraft hit on an actor's stand-in: real damage, scaled so Minecraft gear stays
		// meaningful against higher-level enemies, delivered the way a Skyrim weapon would.
		void ApplyHit(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_ev.formId);
			if (!actor || actor->IsDead()) {
				return;
			}
			const float scale = 5.0f + 0.25f * static_cast<float>(actor->GetLevel());
			const float damage = a_ev.a * scale;
			const bool  crit = (a_ev.flags & proto::kHitCritical) != 0;
			const bool  projectile = (a_ev.flags & proto::kHitProjectile) != 0;
			// Sprint hits, knockback enchantments and crits stagger; ordinary swings don't
			// (Minecraft's attack rate would otherwise stun-lock everything).
			// Melee doesn't knock anyone back (no sprint-hit shove, no knockback enchantment push); a critical blow still makes the
			// victim flinch. A shot's push is kept: it's what stops Skyrim's enemies closing in on an archer quickly.
			const float push = projectile ? a_ev.d : 0.0f;
			const float stagger = projectile ? ((push > 0.45f || crit) ? std::clamp(crit ? 0.35f : (push - 0.4f) * 1.2f, 0.25f, 1.0f) : 0.0f) : (crit ? 0.25f : 0.0f);
			if ((a_ev.flags & proto::kHitFromMob) != 0) {
				ApplyMobHit(actor, a_ev, damage, stagger);
				return;
			}
			auto*       weapon = StandInWeapon(a_ev.weapon & 0xFFu);
			auto*       node = HitNode(actor);
			RE::NiPoint3 hitPos = node ? node->world.translate : actor->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, actor->GetHeight() * 0.6f };
			RE::NiPoint3 dir = hitPos - a_player->GetPosition();
			dir.z = 0.0f;
			dir = dir.Length() > 1e-3f ? dir / dir.Length() : RE::NiPoint3{ 0.0f, 1.0f, 0.0f };

			if (processHit && damage > 0.0f) {
				alignas(16) std::array<std::byte, sizeof(RE::HitData)> storage{};
				auto* hit = reinterpret_cast<RE::HitData*>(storage.data());
				hitDataCtor(hit);
				hit->Populate(a_player, actor, nullptr);
				hit->weapon = weapon;
				hit->hitPosition = hitPos;
				hit->hitDirection = dir;
				hit->totalDamage = damage;
				hit->physicalDamage = damage;
				hit->resistedPhysicalDamage = 0.0f;
				hit->resistedTypedDamage = 0.0f;
				hit->sneakAttackBonus = 1.0f;
				hit->bonusHealthDamageMult = 1.0f;
				hit->stagger = stagger;
				hit->pushBack = stagger > 0.0f ? push : 0.0f;
				hit->skill = HitSkill(a_ev);
				hit->flags.reset(RE::HitData::Flag::kPowerAttack, RE::HitData::Flag::kCritical, RE::HitData::Flag::kSneakAttack, RE::HitData::Flag::kMeleeAttack);
				if (!projectile) {
					hit->flags.set(RE::HitData::Flag::kMeleeAttack);
				}
				if (crit) {
					hit->flags.set(RE::HitData::Flag::kCritical);
				}
				if (projectile && push > 0.45f) {  // (a power attack always staggers in Skyrim)
					hit->flags.set(RE::HitData::Flag::kPowerAttack);
				}
				processHit(actor, *hit);
			} else if (damage > 0.0f) {
				actor->DoDamage(damage, a_player, true);
				if (stagger > 0.0f && !actor->IsDead()) {
					const float pushHeading = std::atan2(a_ev.b, -a_ev.c);  // MC (x, z) -> Skyrim heading
					float       sdir = (pushHeading - actor->GetAngleZ()) / (2.0f * kPi) + 0.5f;
					sdir -= std::floor(sdir);
					actor->SetGraphVariableFloat("staggerDirection", sdir);
					actor->SetGraphVariableFloat("staggerMagnitude", stagger);
					actor->NotifyAnimationGraph("staggerStart");
				}
			}
			// Blood spray and the weapon-on-flesh sound, like a Skyrim blade connecting.
			if (auto* impacts = RE::BGSImpactManager::GetSingleton(); impacts && weapon && weapon->impactDataSet && node && damage > 0.0f) {
				RE::NiPoint3 pick = dir;
				impacts->PlayImpactEffect(actor, weapon->impactDataSet, node->name.c_str(), pick, 128.0f, false, false);
			}
			if (damage > 0.0f) {
				TrainSkill(a_player, HitSkill(a_ev), a_ev.a);
			}
			if (!actor->IsDead() && !actor->IsPlayerTeammate() && !actor->IsInCombat()) {
				actor->StartCombat(a_player);
			}
			logger::info("hit {} ({:08X}, level {}) for {:.1f} Minecraft -> {:.0f} Skyrim damage{}{}{}", actor->GetDisplayFullName(), a_ev.formId,
				actor->GetLevel(), a_ev.a, damage, crit ? ", critical" : "", projectile ? ", projectile" : "", stagger > 0.0f ? ", stagger" : "");
		}

		// ---- the player's health, shared between the two games ---------------------------------------------
		// In Minecraft's controls Minecraft owns the player's health (BridgePlayerDamage). In Skyrim's controls Skyrim's player is the
		// real one: it takes what Skyrim's NPCs deal, and what Minecraft's mobs deal to the Minecraft player riding along (kEvPlayerHurt).
		// Minecraft's player only shows Skyrim's health (SkyState::healthFrac). On the way in, Skyrim's player takes Minecraft's health.
		void SetPlayerHealthFraction(RE::PlayerCharacter* a_player, float a_frac)
		{
			if (a_player->IsDead() || !(a_frac > 0.0f)) {
				return;
			}
			auto*       av = a_player->AsActorValueOwner();
			const float max = a_player->GetActorValueMax(RE::ActorValue::kHealth);
			const float target = std::max(1.0f, std::min(1.0f, a_frac) * max);
			const float cur = av->GetActorValue(RE::ActorValue::kHealth);
			if (cur > target + 0.01f) {
				DamageHealthExactly(a_player, cur - target);
			} else if (cur < target - 0.01f) {
				av->RestoreActorValue(RE::ActorValue::kHealth, target - cur);
			}
			logger::info("Skyrim's controls: player health set to {:.0f} of {:.0f} (Minecraft had {:.0f}%)", av->GetActorValue(RE::ActorValue::kHealth), max, a_frac * 100.0f);
		}

		// Is the player holding a block up? (Skyrim's own: the right mouse button with a shield or weapon in hand.)
		template <class P>
		bool PlayerIsBlocking(P* a_player)
		{
			bool blocking = false;
			if constexpr (requires { a_player->IsBlocking(); }) {
				blocking = a_player->IsBlocking();
			} else {
				a_player->GetGraphVariableBool("IsBlocking", blocking);
			}
			return blocking;
		}

		// How much of a Minecraft mob's blow or shot the player's block takes, 0 when it is not held up against it. A blow from the
		// front (about 140 degrees across) is blocked, one from behind isn't, as in Skyrim. A shield takes more than a weapon does, and
		// the more skilled the player is at Block the more either takes, up to Skyrim's cap of four fifths.
		float PlayerBlockedShare(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev)
		{
			if (!PlayerIsBlocking(a_player)) {
				return 0.0f;
			}
			// a_ev.b, a_ev.c: the way the mob is from the player, Minecraft (x, z) -> Skyrim (x, y).
			RE::NiPoint3 toMob{ a_ev.b, -a_ev.c, 0.0f };
			if (toMob.Length() > 1e-3f) {
				toMob = toMob / toMob.Length();
				const float heading = a_player->GetAngleZ();  // Skyrim heading: 0 is north (+Y), clockwise
				if (std::sin(heading) * toMob.x + std::cos(heading) * toMob.y < 0.34f) {
					return 0.0f;  // it came from the side or the back
				}
			}
			bool shield = false;
			if constexpr (requires { a_player->GetEquippedObject(true); }) {
				if (auto* left = a_player->GetEquippedObject(true)) {
					shield = left->IsArmor();
				}
			}
			const float skill = a_player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kBlock);
			const float share = (shield ? 0.30f : 0.15f) + skill * (shield ? 0.005f : 0.0025f);
			return std::clamp(share, 0.0f, 0.8f);
		}

		void ApplyPlayerHurt(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev)
		{
			float damage = a_ev.a * 5.0f;  // the same exchange rate as the other way (SendHurt)
			if (a_player->IsDead() || !(damage > 0.01f)) {
				return;
			}
			// Held up against it, Skyrim's block takes its share of the blow (Skyrim's controls only: in Minecraft's, Minecraft's shield does).
			if (const float blocked = PlayerBlockedShare(a_player, a_ev); blocked > 0.0f) {
				const float before = damage;
				damage *= 1.0f - blocked;
				a_player->NotifyAnimationGraph("blockHitStart");
				static int logged = 0;
				if (logged++ < 40) {
					logger::info("Skyrim's controls: the block took {:.0f}% of a Minecraft mob's hit ({:.0f} -> {:.0f} Skyrim damage)", blocked * 100.0f, before, damage);
				}
			}
			proto::McEvent hit{};
			hit.type = proto::kEvHitActor;
			hit.formId = a_player->GetFormID();
			hit.a = a_ev.a;
			hit.b = a_ev.b;
			hit.c = a_ev.c;
			hit.flags = proto::kHitFromMob | (a_ev.flags & proto::kHitProjectile);  // (the flag tells a shot from a blow: see MobStagger)
			hit.weapon = (a_ev.formId & 0xFFFFFFu) << 8;
			ApplyMobHit(a_player, hit, damage, 0.0f, false);
			logger::info("Skyrim's controls: a Minecraft mob hurt the player for {:.1f} Minecraft -> {:.0f} Skyrim damage, health now {:.0f}", a_ev.a, damage,
				a_player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth));
		}

		// CommonLib versions differ in how a reference's turn is set; use whichever this one has.
		template <class T>
		void TurnTo(T* a_ref, float a_heading)
		{
			if constexpr (requires { a_ref->SetRotationZ(a_heading); }) {
				a_ref->SetRotationZ(a_heading);
			} else if constexpr (requires { a_ref->data.angle.z = a_heading; }) {
				a_ref->data.angle.z = a_heading;
			}
		}

		// Turns an NPC to face a Minecraft mob (the event's a/b/c are the mob's position).
		void FaceMob(RE::Actor* a_actor, const proto::McEvent& a_ev)
		{
			const auto  target = McToSky(a_ev.a, a_ev.b, a_ev.c);
			const auto  pos = a_actor->GetPosition();
			const float dx = target.x - pos.x;
			const float dy = target.y - pos.y;
			if (dx * dx + dy * dy > 1.0f) {
				TurnTo(a_actor, std::atan2(dx, dy));  // Skyrim heading: 0 is north (+Y), clockwise
			}
		}

		// A Skyrim NPC readies for a fight with a Minecraft mob, or stands down. Skyrim's AI doesn't know the mob
		// exists, so what its combat would do is done here: weapon out and face it; when it's over, weapon away.
		void NpcStance(const proto::McEvent& a_ev)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_ev.formId);
			if (!actor || actor->IsDead() || actor->IsInCombat() || !actor->Is3DLoaded()) {
				return;  // a fight Skyrim runs itself is left to it
			}
			const bool drawn = actor->AsActorState()->IsWeaponDrawn();
			if (a_ev.d > 0.5f) {
				FaceMob(actor, a_ev);
				const auto ws = actor->AsActorState()->GetWeaponState();
				if (!drawn && ws != RE::WEAPON_STATE::kDrawing && ws != RE::WEAPON_STATE::kWantToDraw) {
					actor->DrawWeaponMagicHands(true);
				}
			} else if (drawn) {
				actor->DrawWeaponMagicHands(false);
			}
		}

		// An archer's string is drawn, and the arrow let go a moment later.
		struct BowRelease
		{
			std::uint32_t formId;
			float         delay;
		};
		std::vector<BowRelease> bowReleases;

		// A Skyrim NPC fights back at a Minecraft mob in reach: it turns to face it and plays its attack (with the
		// weapon out, as in any Skyrim fight; a bow is drawn and loosed); the damage the mob takes is Minecraft's.
		// a_ev.d: 1 for a bow or crossbow.
		void NpcSwing(const proto::McEvent& a_ev)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_ev.formId);
			if (!actor || actor->IsDead() || actor->IsInCombat() || !actor->Is3DLoaded()) {
				return;  // a fight Skyrim runs itself is left to it
			}
			const auto weaponState = actor->AsActorState()->GetWeaponState();
			const bool drawn = weaponState == RE::WEAPON_STATE::kDrawn;
			FaceMob(actor, a_ev);
			if (!drawn) {
				// Weapon out first, and only then swing: a swing asked for during the draw makes the draw itself look like the attack.
				if (weaponState != RE::WEAPON_STATE::kDrawing && weaponState != RE::WEAPON_STATE::kWantToDraw) {
					actor->DrawWeaponMagicHands(true);
				}
				return;
			}
			actor->NotifyAnimationGraph("attackStart");
			if (a_ev.d > 0.5f) {
				bowReleases.push_back({ a_ev.formId, 0.85f });
			}
		}

		void UpdateNpcBows(float a_delta)
		{
			for (auto it = bowReleases.begin(); it != bowReleases.end();) {
				it->delay -= a_delta;
				if (it->delay <= 0.0f) {
					if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->formId); actor && !actor->IsDead()) {
						actor->NotifyAnimationGraph("attackRelease");
					}
					it = bowReleases.erase(it);
				} else {
					++it;
				}
			}
		}


		// ---- Minecraft's hostile mobs as invisible Skyrim actors ------------------------------------------
		// Skyrim's combat AI can only fight an actor. So for every hostile Minecraft mob near its NPCs (reported by
		// Minecraft as kEvMobProxy) there is a stand-in Skyrim actor kept where the mob stands, hidden, with its
		// own aggression and damage taken away, and Skyrim's NPCs are put into combat with it, which is all it takes
		// for them to draw, close in, block, shoot, or run, as they do against any enemy. Health the stand-in loses
		// to them goes back to Minecraft (kInProxyHurt) as damage to the real mob, and is given back to the stand-in.
		// If anything here can't be done on this CommonLib or this game (no suitable base creature, no way to place
		// an actor), nothing is made and Minecraft's own stand-in fighting (NpcFightsBack) carries on alone.
		struct MobProxy
		{
			RE::FormID    formId{ 0 };
			RE::NiPoint3  pos{};
			float         lastHealth{ 0.0f };
			float         silence{ 0.0f };  // seconds since Minecraft last reported the mob
			std::string   named;            // the name the stand-in has been given (the mob's), so it isn't set again and again
			float         height{ 0.0f };   // the mob's height in blocks, as Minecraft last said (0: not told)
			float         fitted{ 0.0f };   // the height the stand-in was last sized to
			bool          targetOnly{ false };  // there for the player to lock on to and hit with spells: NPCs aren't sent against it
			float         pendingDamage{ 0.0f };  // damage taken since it was last handed to Minecraft (batched: Minecraft's i-frames)
			float         flushAge{ 0.0f };       // seconds since it was
			bool          byPlayer{ false };      // the player did some of the pending damage
			float         heading{ 0.0f };        // the mob's heading (Skyrim's: radians, 0 = north), as last reported
			float         shownHeading{ 0.0f };   // the heading the stand-in has now (it turns smoothly towards `heading`)
			RE::NiPoint3  shown{};                // where the mob is taken to be now (smoothed, carried on by its velocity)
			bool          placed{ false };        // `shown` and `shownHeading` hold something (else the next placement snaps)
			float         velX{ 0.0f }, velZ{ 0.0f };  // the mob's velocity, blocks per second in Minecraft's x and z (kEvMobVitals)
			float         eye{ 0.0f };            // the mob's eye height in blocks above its feet (0: not told)
			float         scale{ 1.0f };          // the scale the stand-in was fitted to
			float         mcHealth{ 0.0f };       // the mob's health and max health (kEvMobVitals); mcMax 0: not told yet
			float         mcMax{ 0.0f };
			int           hideFrames{ 0 };        // frames the stand-in has been hidden every frame (its body can load in after the actor exists)
			float         warm{ 0.0f };           // seconds left of its warm-up: a new (or switched-on) actor is kept parked high above the player, hidden, while whatever it plays when it first loads plays out unseen
			float         velY{ 0.0f };           // the mob's vertical speed in Skyrim units per second, worked out from its reported positions
		};
		// A stand-in is a skeever, and a skeever's head is a hand's breadth off the ground. Anything that puts something over an
		// actor's head (lock-on and target-HUD mods draw the name there) would put it there, a zombie's chest deep. So the stand-in is
		// sized to its mob's height: the head is then where the mob's is. Its height as it was made (scale 1) is kept per actor, since
		// stand-ins are reused for later mobs.
		std::unordered_map<RE::FormID, float> proxyBaseHeight;
		constexpr bool                        kFitProxyHeight = true;
		std::unordered_map<std::int32_t, MobProxy>    mobProxies;
		std::unordered_map<std::int32_t, std::string> mobNames;  // mob id -> its name, as Minecraft sent it (kEvMobName)
		// Stand-ins whose mob is gone. They are never disabled or deleted (destroying an actor Skyrim's AI is still working on
		// crashes the game): they are taken out of the fight, parked out of the way, and used again for the next mob.
		std::vector<RE::FormID>                    proxyPool;
		float                                      parkTimer = 0.0f;
		float                                      sweepTimer = 0.0f;
		RE::TESNPC*                                proxyBase = nullptr;
		bool                                       proxyBaseSearched = false;
		bool                                       proxiesUnavailable = false;
		float                                      proxyFightTimer = 0.0f;
		float                                      mirrorOffFor = 0.0f;  // seconds the Skyrim-mode mirror of Minecraft's world has been off
		constexpr std::size_t                      kMaxProxies = 12;
		constexpr bool                             kProxyAiOff = true;  // if the NPCs stop fighting the stand-ins with this on, set it to false
		// Stand-ins that died anyway: their bodies are disabled after a moment (the NPCs need to let go of them first).
		struct DeadProxy
		{
			RE::FormID formId;
			float      wait;
		};
		std::vector<DeadProxy> deadProxies;

		RE::Actor* StandInForMob(std::int32_t a_mobId)
		{
			const auto it = mobProxies.find(a_mobId);
			if (it == mobProxies.end()) {
				return nullptr;
			}
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->second.formId);
			return actor && !actor->IsDead() ? actor : nullptr;
		}

		// The stand-in shows up wherever Skyrim shows an actor's name (the enemy health bar, the crosshair): it
		// carries its mob's name rather than a skeever's.
		template <class A>
		void NameProxy(A* a_actor, const std::string& a_name)
		{
			if constexpr (requires { a_actor->SetDisplayName(RE::BSFixedString(a_name.c_str()), true); }) {
				a_actor->SetDisplayName(RE::BSFixedString(a_name.c_str()), true);
			}
		}

		// True when the creature has no spells, abilities or shaders of its own to bring along (the Wabbajack's rabbit has).
		template <class N>
		bool BaseIsPlain(N* a_npc)
		{
			bool plain = true;
			if constexpr (requires { a_npc->actorEffects->numSpells; }) {
				if (a_npc->actorEffects && (a_npc->actorEffects->numSpells > 0)) {
					plain = false;
				}
			}
			return plain;
		}

		template <class D>
		RE::TESNPC* PickProxyBase(D* a_data)
		{
			// A small, plain creature that is no use to anyone: its attacks, size and looks don't matter, it is hidden. What matters is that it
			// is quiet: a rabbit has no growls or hisses to make, a skeever has plenty. SkyCraft.ini [Mobs] sStandInBase chooses another: a
			// creature's name, or "Plugin.esp:0xFORMID" for one from a mod (a silent actor made for the purpose, say).
			std::vector<std::string> names{ "Rabbit", "Mudcrab", "Wolf" };  // (no skeever: its noises and its disease are the old trouble)
			const std::string        wanted = StandInBaseSetting();
			if (!wanted.empty()) {
				if (const auto colon = wanted.find(':'); colon != std::string::npos) {
					if constexpr (requires { a_data->template LookupForm<RE::TESNPC>(RE::FormID{ 0 }, std::string_view{}); }) {
						const std::string plugin = wanted.substr(0, colon);
						const auto        id = static_cast<RE::FormID>(std::strtoul(wanted.c_str() + colon + 1, nullptr, 16));
						if (auto* npc = a_data->template LookupForm<RE::TESNPC>(id, plugin)) {
							return npc;
						}
						logger::warn("sStandInBase '{}' wasn't found; using a stock creature instead", wanted);
					}
				} else {
					names.insert(names.begin(), wanted);
				}
			}
			// A creature of the right name is not always a plain one: the game has several rabbits, and the one the Wabbajack's
			// transformation makes carries spells of its own, which show as a red mist where it appears. Only a base with no
			// spells or abilities at all is used; the ones turned down are logged by name and FormID.
			for (const auto& name : names) {
				if constexpr (requires { a_data->template GetFormArray<RE::TESNPC>(); }) {
					for (auto* npc : a_data->template GetFormArray<RE::TESNPC>()) {
						if (npc && npc->GetName() && std::string_view(npc->GetName()) == name) {
							if (!BaseIsPlain(npc)) {
								logger::info("Stand-in base {:08X} ('{}') turned down: it has spells or abilities of its own", npc->GetFormID(), name);
								continue;
							}
							return npc;
						}
					}
				}
			}
			return nullptr;
		}

		// The custom stand-in records: one NPC record per mob size in the library, made at runtime (as the block forms are) from the stock
		// creature's, so each size has a stand-in of its own that is not a stock creature. If a record can't be made the stock creature is used.
		std::unordered_map<std::string, RE::TESNPC*> customBases;
		std::unordered_map<RE::FormID, std::string>  actorSizeKey;  // a stand-in actor -> the size record it was made from
		bool       skeletonTrialFailed = false;  // the first actor with a skeleton of our own got no 3D: no more are made
		bool       skeletonTrialPending = false;  // a record with a custom skeleton was made; the first actor made from one is the trial
		RE::FormID skeletonTrialActor = 0;
		float      skeletonTrialAge = 0.0f;

		RE::TESNPC* FindProxyBase();

		RE::TESNPC* CustomProxyBase(const std::string& a_key)
		{
			if (const auto it = customBases.find(a_key); it != customBases.end()) {
				return it->second;
			}
			RE::TESNPC* made = nullptr;
			if (auto* stock = FindProxyBase()) {
				// Only a size that is to have a skeleton of its own is cloned (the trial marker is set first: a clone Skyrim can't take must not
				// be tried again at the next start); every other size uses the stock creature, as before.
				bool        copied = false;
				RE::TESNPC* npc = nullptr;
				if (MobBodies::CustomSkeletonOn() && !skeletonTrialFailed) {
					if (skeletonTrialActor == 0) {
						MobBodies::ArmSkeletonTrial();
					}
					npc = compat::CloneForm(stock, copied);
					if (!copied && skeletonTrialActor == 0) {
						MobBodies::SkeletonTrialPassed();  // nothing was cloned, so there is nothing to crash on: no marker left behind
						skeletonTrialFailed = true;
						logger::warn("Custom skeletons need a copy of a creature's NPC and race records, and the engine made no duplicate of the stock creature; the stock creature is used");
					}
				}
				if (npc) {
					const auto ownId = npc->GetFormID();
					if (copied && ownId != 0 && npc->GetFormID() == ownId) {
						made = npc;
						int w16 = 0, h16 = 0;
						if (MobBodies::CustomSkeletonOn() && !skeletonTrialFailed && MobBodies::ParseKey(a_key, w16, h16)) {
							// Our own skeleton: a race record copied from the stock creature's, pointing at a skeleton file that is this size's box, and the
							// NPC's bounds set to the box, so the actor is the mob's shape.
							const auto model = MobBodies::SkeletonModel(a_key, w16, h16);
							if constexpr (requires { stock->race; npc->race = stock->race; }) {
								if (!model.empty() && stock->race) {
									bool  raceCopied = false;
									auto* race = compat::CloneForm(stock->race, raceCopied);
									bool  raceOk = false;
									if (race && raceCopied && compat::RaceLooksCopied(race, stock->race)) {
										raceOk = compat::SetSkeleton(race, model.c_str());
									}
									if (raceOk) {
										npc->race = race;
										const float u = float(proto::kUnitsPerBlock);
										if constexpr (requires { npc->boundData.boundMin.x = std::int16_t(0); }) {
											npc->boundData.boundMin.x = static_cast<std::int16_t>(-w16 * u / 32.0f);
											npc->boundData.boundMin.y = static_cast<std::int16_t>(-w16 * u / 32.0f);
											npc->boundData.boundMin.z = 0;
											npc->boundData.boundMax.x = static_cast<std::int16_t>(w16 * u / 32.0f);
											npc->boundData.boundMax.y = static_cast<std::int16_t>(w16 * u / 32.0f);
											npc->boundData.boundMax.z = static_cast<std::int16_t>(h16 * u / 16.0f);
										}
										if (skeletonTrialActor == 0) {
											skeletonTrialPending = true;
										}
										logger::info("Custom stand-in {:08X} for mob size {}: its race {:08X} has the skeleton {}", npc->GetFormID(), a_key, race->GetFormID(), model);
									} else {
										logger::warn("Couldn't make a race with a skeleton of its own for mob size {} (the engine's duplicate of the stock race was missing or didn't hold its data); the stock creature's skeleton is used", a_key);
									}
								}
							}
						}
					}
				}
				static int logged = 0;
				if (logged++ < 12) {
					if (made) {
						logger::info("Custom stand-in record {:08X} made for mob size {} (from '{}')", made->GetFormID(), a_key, stock->GetName());
					} else {
						logger::warn("Couldn't make a custom stand-in record for mob size {}; the stock creature is used", a_key);
					}
				}
				if (!made) {
					made = stock;
				}
			}
			customBases[a_key] = made;
			return made;
		}

		RE::TESNPC* FindProxyBase()
		{
			if (!proxyBaseSearched) {
				proxyBaseSearched = true;
				if (auto* data = RE::TESDataHandler::GetSingleton()) {
					proxyBase = PickProxyBase(data);
				}
				if (proxyBase) {
					logger::info("Mob stand-ins will be made from '{}' ({:08X})", proxyBase->GetName(), proxyBase->GetFormID());
				} else {
					logger::warn("No creature found to stand in for Minecraft's mobs; Skyrim's NPCs will fight them the simple way");
					proxiesUnavailable = true;
				}
			}
			return proxyBase;
		}

		template <class P, class B>
		RE::Actor* PlaceProxyActor(P* a_player, B* a_base)
		{
			if constexpr (requires { a_player->PlaceObjectAtMe(a_base, false); }) {
				auto ref = a_player->PlaceObjectAtMe(a_base, false);
				return ref ? ref.get()->template As<RE::Actor>() : nullptr;
			} else {
				return nullptr;
			}
		}

		template <class A>
		void SetAV(A* a_values, RE::ActorValue a_av, float a_value)
		{
			if constexpr (requires { a_values->SetActorValue(a_av, a_value); }) {
				a_values->SetActorValue(a_av, a_value);
			}
		}

		// Holds a stand-in at no alpha (or lets it go): a freshly made or switched-on actor's 3D loads a moment later and Skyrim fades it in,
		// which would show as a rabbit shimmering into being where the mob is. At zero alpha that fade-in is never seen.
		template <class A>
		void FadeProxy(A* a_actor, bool a_hidden)
		{
			const auto id = a_actor->GetFormID();
			if (a_hidden) {
				// (Applied again on every call while held: an actor with no 3D yet may not take the alpha, and the 3D it gets takes it fresh.)
				if constexpr (requires { a_actor->SetAlpha(0.0f); }) {
					a_actor->SetAlpha(0.0f);
				}
				proxyFaded.insert(id);
			} else if (proxyFaded.erase(id) > 0) {
				if constexpr (requires { a_actor->SetAlpha(1.0f); }) {
					a_actor->SetAlpha(1.0f);
				}
			}
		}

		template <class O>
		void CullObject(O* a_object, bool a_culled)
		{
			if constexpr (requires { a_object->SetAppCulled(a_culled); }) {
				a_object->SetAppCulled(a_culled);
			}
		}

		// A stand-in made or switched on can come with a visual effect of its own (a red mist, like the one the Wabbajack's transformations
		// leave): its spawn is not something to see. Shaders and magic effects on it are stopped for its first moments (the same window
		// its body is held hidden in), so a later spell's flames on it still show.
		template <class A>
		void StripSpawnEffects(A* a_actor)
		{
			if constexpr (requires { a_actor->StopAllShaders(); }) {
				a_actor->StopAllShaders();
			}
			if constexpr (requires { RE::ProcessLists::GetSingleton()->StopAllMagicEffects(*a_actor); }) {
				if (auto* lists = RE::ProcessLists::GetSingleton()) {
					lists->StopAllMagicEffects(*a_actor);
				}
			}
		}

		// Hides the stand-in's body and nothing else. Culling the actor's whole 3D root hides it too, but lock-on and target mods
		// (and Skyrim's own targeting) treat an actor whose root is hidden as one that isn't there, and the stand-in can then be
		// neither locked on to nor aimed at. Its body is a set of geometry nodes: with those hidden it is not drawn (nor does it
		// cast a shadow), while the actor itself, its skeleton, its head node and its collision are as they were. Effects that
		// attach to an actor (flames on a burning target) are not geometry of its own, so a spell's fire still shows.
		// SkyCraft.ini [Mobs] sStandInHide picks the way: geometry (this), root (the whole 3D), alpha, or none (shown, to see where
		// stand-ins stand).
		template <class A>
		void HideProxy(A* a_actor)
		{
			const int mode = StandInHideMode();
			auto*     root = a_actor->Get3D();
			if (mode == 0) {
				return;
			}
			if (!root) {
				// Nothing to hide yet: its body loads in a moment. Held at no alpha until it has, so its spawn is never seen.
				FadeProxy(a_actor, true);
				return;
			}
			if (mode == 3) {
				if constexpr (requires { a_actor->SetAlpha(0.0f); }) {
					a_actor->SetAlpha(0.0f);
				}
				return;
			}
			if (mode == 2) {
				CullObject(root, true);
				return;
			}
			CullObject(root, false);  // (in case an earlier way, or Skyrim, left the root hidden)
			int hidden = 0;
			if constexpr (requires { root->AsNode(); root->AsGeometry(); root->AsNode()->GetChildren(); }) {
				std::vector<RE::NiAVObject*> stack{ root };
				for (std::size_t visited = 0; !stack.empty() && visited < 2048; ++visited) {
					auto* object = stack.back();
					stack.pop_back();
					if (!object) {
						continue;
					}
					if (object->AsGeometry()) {
						CullObject(object, true);
						++hidden;
					} else if (auto* node = object->AsNode()) {
						for (auto& child : node->GetChildren()) {
							if (child) {
								stack.push_back(child.get());
							}
						}
					}
				}
			}
			// A new stand-in's body loads in a moment after the actor exists. Until it has been found and hidden the whole actor stays culled, so
			// the rabbit never shows (and never fades in); once its geometry is hidden the actor itself is shown again, for lock-on.
			CullObject(root, hidden == 0);
			FadeProxy(a_actor, hidden == 0);  // (body found and hidden: the actor is back to full alpha, for lock-on; nothing of it is drawn anyway)
			static int logged = 0;
			if (logged++ < 6) {
				logger::info("Mob stand-in ({:08X}): hid {} geometry nodes (the actor itself is left showing, so it can be locked on to)", a_actor->GetFormID(), hidden);
			}
			if (hidden == 0) {
				static bool warned = false;
				if (!warned) {
					warned = true;
					logger::warn("Mob stand-in: found no geometry to hide on its skeleton (or this CommonLib can't); set [Mobs] sStandInHide = root if it shows");
				}
			}
		}

		// Takes the stand-in out of the fight on its own side: it never starts one, never hurts anyone, and is
		// invisible. (Called again on every update: the 3D model only exists a moment after the actor is made.)
		template <class A>
		void QuietProxy(A* a_actor, bool a_bigPool = true)
		{
			auto* values = a_actor->AsActorValueOwner();
			SetAV(values, RE::ActorValue::kAggression, 0.0f);
			SetAV(values, RE::ActorValue::kConfidence, 4.0f);
			SetAV(values, RE::ActorValue::kAttackDamageMult, 0.0f);
			SetAV(values, RE::ActorValue::kAssistance, 0.0f);  // calls no one to its side
			// So much health that Skyrim's NPCs can't kill it (Minecraft's mob is the one that dies, and the stand-in then goes back
			// to the pool): a stand-in that died would leave an invisible Skeever body lying there.
			// (Only until Minecraft has told it the mob's health: from then on its health is the mob's, 1:1, see SyncProxyHealth.)
			if (a_bigPool && a_actor->GetActorValueMax(RE::ActorValue::kHealth) < 50000.0f) {
				SetAV(values, RE::ActorValue::kHealth, 100000.0f);
			}
			// With the mob's real health it could now be killed by one big hit, and a dead stand-in is an invisible body that groans and
			// ragdolls where the mob stands. Essential, it is never killed: at no health it just goes down, and the mob dies in Minecraft anyway.
			if constexpr (requires { a_actor->GetActorRuntimeData().boolFlags.set(RE::Actor::BOOL_FLAGS::kEssential); }) {
				a_actor->GetActorRuntimeData().boolFlags.set(RE::Actor::BOOL_FLAGS::kEssential);
			}
			// Its own AI off: no attacks (no Ataxia), no growls, no wandering off its spot.
			if (kProxyAiOff) {
				static bool logged = false;
				if constexpr (requires { a_actor->EnableAI(false); }) {
					a_actor->EnableAI(false);
					if (!logged) {
						logged = true;
						logger::info("Mob stand-ins have their own AI switched off");
					}
				} else if (!logged) {
					logged = true;
					logger::warn("This CommonLib has no way to switch a stand-in's AI off; they keep their own (and may growl)");
				}
			}
			// (It used to be taken out of any fight it had with the player here, on every report from Minecraft. The engine log showed a
			// stand-in the player hit going into combat and being switched off 6 ms later, which is when this ran; with its AI off it can't
			// attack anyway, so it is left alone. Pooled stand-ins are still taken out of combat when they are put away.)
			HideProxy(a_actor);
		}

		template <class A>
		void SetProxyScale(A* a_actor, float a_scale)
		{
			if constexpr (requires { a_actor->GetReferenceRuntimeData().refScale = std::uint16_t(100); }) {
				a_actor->GetReferenceRuntimeData().refScale = static_cast<std::uint16_t>(std::clamp(a_scale * 100.0f, 10.0f, 1000.0f));
			}
			if (auto* node = a_actor->Get3D()) {
				node->local.scale = a_scale;  // (the stand-in's 3D may not exist yet; it's fitted again on the next update)
			}
		}

		// The node a lock-on or target-HUD mod aims at: the stand-in's head. A skeever has no "NPC Head [Head]"; its skeleton names the
		// bone for the creature, so the nearest thing to the top of the rig whose name says "head" is taken.
		template <class N>
		RE::NiAVObject* FindHeadNode(N* a_root)
		{
			if (!a_root) {
				return nullptr;
			}
			for (const char* name : { "NPC Head [Head]", "Skeever Head", "Head", "Bip01 Head" }) {
				if (auto* node = a_root->GetObjectByName(name)) {
					return node;
				}
			}
			RE::NiAVObject* found = nullptr;
			if constexpr (requires { a_root->AsNode()->GetChildren(); }) {
				std::vector<RE::NiAVObject*> queue{ a_root };
				for (std::size_t at = 0; at < queue.size() && at < 256 && !found; ++at) {
					auto* node = queue[at]->AsNode();
					if (!node) {
						continue;
					}
					for (auto& child : node->GetChildren()) {
						if (!child) {
							continue;
						}
						std::string lower = child->name.c_str() ? child->name.c_str() : "";
						for (auto& ch : lower) {
							ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
						}
						if (lower.find("head") != std::string::npos && lower.find("headtrack") == std::string::npos) {
							found = child.get();
							break;
						}
						queue.push_back(child.get());
					}
				}
			}
			return found;
		}

		// A stand-in's head height (units above its feet) per unit of scale, as measured on its own skeleton once it has one.
		// Where a stand-in's head is, in the actor's own frame (x right, y forward, z up), per unit of scale. It is measured on the skeleton
		// once, and the stand-in is then put wherever makes its head land on the mob's eyes (see FollowProxy).
		struct HeadFit
		{
			float x, y, z;
		};
		std::unordered_map<RE::FormID, HeadFit> proxyHeadFit;

		// Sizes the stand-in to its mob and measures where its head is (once). Where the head ends up is FollowProxy's business.
		template <class A>
		void FitProxyHeight(A* a_actor, MobProxy& a_proxy)
		{
			if (!kFitProxyHeight || !(a_proxy.height > 0.2f) || !(a_proxy.height < 16.0f)) {
				return;
			}
			auto* root = a_actor->Get3D();
			if (!root) {
				return;  // no skeleton yet (it loads a moment after the actor exists): fitted on a later update
			}
			const float units = static_cast<float>(proto::kUnitsPerBlock);
			auto        fit = proxyHeadFit.find(a_actor->GetFormID());
			if (fit == proxyHeadFit.end()) {
				auto* head = FindHeadNode(root);
				if (!head) {
					// No head to be found on this skeleton: size it by its bounds.
					auto base = proxyBaseHeight.find(a_actor->GetFormID());
					if (base == proxyBaseHeight.end()) {
						const float height = a_actor->GetHeight();
						if (!(height > 1.0f)) {
							return;
						}
						base = proxyBaseHeight.emplace(a_actor->GetFormID(), height).first;
					}
					if (std::fabs(a_proxy.fitted - a_proxy.height) < 0.05f) {
						return;
					}
					a_proxy.scale = std::clamp(a_proxy.height * units / base->second, 0.25f, 10.0f);
					SetProxyScale(a_actor, a_proxy.scale);
					a_proxy.fitted = a_proxy.height;
					return;
				}
				// Where the head is measured on the skeleton as it stands, in the skeleton's own frame, so it doesn't matter which way
				// the stand-in happens to face (it is turned to the mob's heading every frame).
				const float        scaleNow = root->world.scale > 0.05f ? root->world.scale : 1.0f;
				const RE::NiPoint3 d = head->world.translate - root->world.translate;
				HeadFit            f{ 0.0f, 0.0f, d.z / scaleNow };
				if constexpr (requires { root->world.rotate.entry[0][0]; }) {
					const auto& r = root->world.rotate.entry;
					f.x = (r[0][0] * d.x + r[1][0] * d.y + r[2][0] * d.z) / scaleNow;
					f.y = (r[0][1] * d.x + r[1][1] * d.y + r[2][1] * d.z) / scaleNow;
					f.z = (r[0][2] * d.x + r[1][2] * d.y + r[2][2] * d.z) / scaleNow;
				}
				if (!(f.z * scaleNow > 1.0f)) {
					return;  // not posed yet
				}
				fit = proxyHeadFit.emplace(a_actor->GetFormID(), f).first;
				logger::info("Mob stand-in ({:08X}): head node '{}' is ({:.1f}, {:.1f}, {:.1f}) units from its origin at scale 1", a_actor->GetFormID(), head->name.c_str(),
					f.x, f.y, f.z);
			}
			// A mob's eyes: Minecraft says where (a spider's are low, a giraffe-tall enderman's high); else about this far up its body.
			const float eyeBlocks = a_proxy.eye > 0.1f ? a_proxy.eye : a_proxy.height * 0.92f;
			if (std::fabs(a_proxy.fitted - eyeBlocks) < 0.05f) {
				return;
			}
			a_proxy.scale = std::clamp(eyeBlocks * units / std::max(fit->second.z, 1.0f), 0.25f, 8.0f);
			SetProxyScale(a_actor, a_proxy.scale);
			a_proxy.fitted = eyeBlocks;
			static int logged = 0;
			if (logged++ < 10) {
				logger::info("Sized a mob stand-in ({:08X}) so its head is {:.2f} blocks up (scale {:.2f})", a_actor->GetFormID(), eyeBlocks, a_proxy.scale);
			}
		}

		// Keeps a falling stand-in from counting its fall (it is put back where the mob is every frame, so it never really falls, but
		// Skyrim's character controller would otherwise add the fall time up and hurt it when it "lands").
		template <class A>
		void ResetProxyFall(A* a_actor, float a_z)
		{
			if constexpr (requires { a_actor->GetCharController(); }) {
				if (auto* controller = a_actor->GetCharController()) {
					if constexpr (requires { controller->fallTime = 0.0f; }) {
						controller->fallTime = 0.0f;
					}
					if constexpr (requires { controller->fallStartHeight = a_z; }) {
						controller->fallStartHeight = a_z;
					}
				}
			}
		}

		float WrapHeading(float a_heading)
		{
			constexpr float twoPi = 6.28318530718f;
			a_heading = std::fmod(a_heading, twoPi);
			if (a_heading > twoPi / 2.0f) {
				a_heading -= twoPi;
			} else if (a_heading < -twoPi / 2.0f) {
				a_heading += twoPi;
			}
			return a_heading;
		}

		// Where a stand-in waits, unseen, while it warms up (or in the pool): far above the player.
		RE::NiPoint3 WarmSpot(RE::PlayerCharacter* a_player)
		{
			auto spot = a_player->GetPosition();
			spot.z += 60.0f * static_cast<float>(proto::kUnitsPerBlock);
			return spot;
		}

		// True Directional Movement doesn't lock on to an actor's head, it locks on to a node of the actor's skeleton: by default the
		// *torso* (its "Reticle Anchor" setting is Body), or the head if that is set, each named by the actor's race (body part data). The
		// stand-in is a rabbit or a wolf, set down so that its *head* is where the mob's eyes are; its torso is then behind and below
		// that (a four-legged body hangs off its head), a block or more from where the mob's body is, and that is where the lock-on
		// reticle, the camera and the spells aimed with it went. (The name over its head is placed another way, which is why it was right.)
		// So the nodes lock-on goes by are put where they belong on the mob: the torso at the middle of its body, the head at its eyes.
		// Only their world position is set, after the stand-in's 3D has been moved for the frame, so nothing that rebuilds the skeleton
		// is left holding a wrong local position; what draws it (nothing: it is hidden) is not affected.
		constexpr bool kPinTargetNodes = true;  // if lock-on goes somewhere odd with this on, set it to false

		template <class A>
		RE::NiAVObject* LockOnNodeOf(A* a_actor, RE::NiAVObject* a_root, bool a_torso)
		{
			if constexpr (requires { a_actor->GetRace()->bodyPartData->parts[RE::BGSBodyPartDefs::LIMB_ENUM::kTorso]->targetName.c_str(); }) {
				auto* race = a_actor->GetRace();
				auto* data = race ? race->bodyPartData : nullptr;
				auto* part = data ? data->parts[a_torso ? RE::BGSBodyPartDefs::LIMB_ENUM::kTorso : RE::BGSBodyPartDefs::LIMB_ENUM::kHead] : nullptr;
				if (part && part->targetName.c_str() && part->targetName.c_str()[0] != '\0') {
					return a_root->GetObjectByName(part->targetName.c_str());
				}
			}
			return nullptr;
		}

		template <class A>
		void PinTargetNodes(A* a_actor, const MobProxy& a_proxy)
		{
			// (Not before the stand-in's head has been measured, which has to be done on a skeleton nobody has moved.)
			if (!kPinTargetNodes || !(a_proxy.height > 0.2f) || !proxyHeadFit.contains(a_actor->GetFormID())) {
				return;
			}
			auto* root = a_actor->Get3D();
			if (!root) {
				return;
			}
			const float units = static_cast<float>(proto::kUnitsPerBlock);
			const float eyeBlocks = a_proxy.eye > 0.1f ? a_proxy.eye : a_proxy.height * 0.92f;
			RE::NiPoint3 body = a_proxy.shown;
			body.z += a_proxy.height * 0.5f * units;
			RE::NiPoint3 head = a_proxy.shown;
			head.z += eyeBlocks * units;
			auto* torsoNode = LockOnNodeOf(a_actor, root, true);
			auto* headNode = LockOnNodeOf(a_actor, root, false);
			if (torsoNode) {
				torsoNode->world.translate = body;
			}
			if (headNode && headNode != torsoNode) {
				headNode->world.translate = head;
			}
			static std::unordered_set<RE::FormID> logged;
			if (logged.size() < 12 && logged.insert(a_actor->GetFormID()).second) {
				logger::info("Mob stand-in ({:08X}): lock-on torso node '{}', head node '{}' (put on the mob's body and eyes)", a_actor->GetFormID(),
					torsoNode ? torsoNode->name.c_str() : "(none found)", headNode ? headNode->name.c_str() : "(none found)");
			}
		}

		// Puts the stand-in where its mob is, every frame (Minecraft reports ten times a second; Skyrim draws many more, so between
		// reports the mob is carried on by its velocity and the stand-in glides to it). It faces the way the mob faces, and is placed
		// so that its *head* is at the mob's eyes, whatever the stand-in's own shape: a lock-on or target-bar mod aims at the head.
		template <class A>
		void FollowProxy(A* a_actor, MobProxy& a_proxy, float a_delta)
		{
			const float units = static_cast<float>(proto::kUnitsPerBlock);
			const float ahead = std::min(a_proxy.silence, 0.25f);
			RE::NiPoint3 mob = a_proxy.pos;
			mob.x += a_proxy.velX * ahead * units;
			mob.y -= a_proxy.velZ * ahead * units;  // (Minecraft's z is Skyrim's -y)
			mob.z += a_proxy.velY * ahead;
			if (!a_proxy.placed || mob.GetDistance(a_proxy.shown) > 3.0f * units) {
				a_proxy.shown = mob;
				a_proxy.shownHeading = a_proxy.heading;
				a_proxy.placed = true;
			} else {
				const float follow = 1.0f - std::exp(-40.0f * a_delta);
				a_proxy.shown.x += (mob.x - a_proxy.shown.x) * follow;
				a_proxy.shown.y += (mob.y - a_proxy.shown.y) * follow;
				a_proxy.shown.z += (mob.z - a_proxy.shown.z) * follow;
				const float turn = 1.0f - std::exp(-18.0f * a_delta);
				a_proxy.shownHeading = WrapHeading(a_proxy.shownHeading + WrapHeading(a_proxy.heading - a_proxy.shownHeading) * turn);
			}
			RE::NiPoint3 place = a_proxy.shown;
			const float  eyeBlocks = a_proxy.eye > 0.1f ? a_proxy.eye : (a_proxy.height > 0.2f ? a_proxy.height * 0.92f : 0.0f);
			if (const auto fit = proxyHeadFit.find(a_actor->GetFormID()); fit != proxyHeadFit.end() && eyeBlocks > 0.0f) {
				const float s = a_proxy.scale > 0.05f ? a_proxy.scale : 1.0f;
				const float c = std::cos(a_proxy.shownHeading);
				const float n = std::sin(a_proxy.shownHeading);
				// The head's offset from the stand-in's origin, turned to the way it faces now; the origin goes where that puts the head on the eyes.
				place.x -= (fit->second.x * c + fit->second.y * n) * s;
				place.y -= (-fit->second.x * n + fit->second.y * c) * s;
				place.z += eyeBlocks * units - fit->second.z * s;
			}
			TurnTo(a_actor, a_proxy.shownHeading);
			a_actor->SetPosition(place, true);
			// With its AI off nothing else moves the stand-in's 3D after its data: its body, head node and hit shapes would stay where it was made.
			if constexpr (requires { a_actor->Update3DPosition(true); }) {
				a_actor->Update3DPosition(true);
			} else if (auto* root = a_actor->Get3D()) {
				root->local.translate = place;
			}
			ResetProxyFall(a_actor, place.z);
			if (a_proxy.hideFrames < 120) {  // a body that loads in late is hidden the frame it appears
				++a_proxy.hideFrames;
				HideProxy(a_actor);
				StripSpawnEffects(a_actor);
			}
			PinTargetNodes(a_actor, a_proxy);  // (last: after everything that moves or rebuilds the stand-in's 3D this frame)
			if (DiagnosticsEnabled()) {
				static float since = 0.0f;
				since += a_delta;
				if (since > 2.0f) {
					since = 0.0f;
					const auto  now = a_actor->GetPosition();
					auto*       root = a_actor->Get3D();
					auto*       head = root ? FindHeadNode(root) : nullptr;
					const auto  eye = a_proxy.shown + RE::NiPoint3{ 0.0f, 0.0f, (a_proxy.eye > 0.1f ? a_proxy.eye : a_proxy.height * 0.92f) * units };
					logger::info("stand-in {:08X}: mob at ({:.0f}, {:.0f}, {:.0f}), actor at ({:.0f}, {:.0f}, {:.0f}), 3D at ({:.0f}, {:.0f}, {:.0f}), head off the mob's eyes by {:.0f} units",
						a_actor->GetFormID(), a_proxy.shown.x, a_proxy.shown.y, a_proxy.shown.z, now.x, now.y, now.z, root ? root->world.translate.x : 0.0f,
						root ? root->world.translate.y : 0.0f, root ? root->world.translate.z : 0.0f, head ? head->world.translate.GetDistance(eye) : -1.0f);
				}
			}
		}

		template <class A>
		void StopProxyCombat(A* a_actor)
		{
			if constexpr (requires { a_actor->StopCombat(); }) {
				a_actor->StopCombat();
			}
		}

		// Every NPC that has the stand-in as its target stops: the mob blew up or died or was lost, so there is nothing left to hunt
		// (Skyrim's NPCs would otherwise go and look for it where it was last seen).
		void ReleaseHunters(RE::Actor* a_standIn)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists || !a_standIn) {
				return;
			}
			for (auto& handle : lists->highActorHandles) {
				auto  ptr = handle.get();
				auto* npc = ptr.get();
				if (npc && npc->IsInCombat() && npc->GetActorRuntimeData().currentCombatTarget.get().get() == a_standIn) {
					npc->StopCombat();
				}
			}
		}

		// What a stand-in has lost since last time goes to Minecraft as damage to the real mob, from the player if the player did it (a
		// spell is the usual way), else from the NPC nearest it. It is sent in batches: Minecraft's mobs shrug off a second blow
		// within half a second or so, which is most of what a concentration spell (flames) would otherwise be.
		constexpr float kProxyFlushSeconds = 0.55f;
		constexpr float kProxyBigHit = 6.0f;           // a hit this hard (a firebolt) is sent at once
		constexpr float kPlayerBlameSeconds = 6.0f;    // damage this soon after the player hit it is the player's (burning goes on after the spell)

		void FlushProxyDamage(std::int32_t a_mobId, MobProxy& a_proxy)
		{
			if (a_proxy.pendingDamage <= 0.01f) {
				a_proxy.pendingDamage = 0.0f;
				return;
			}
			Link::Get().PushInput(proto::kInProxyHurt, a_proxy.byPlayer ? 1 : 0, static_cast<std::int32_t>(a_proxy.pendingDamage * 100.0f), a_mobId, 0);
			static int logged = 0;
			if (logged++ < 40) {
				logger::info("Stand-in ({:08X}) of mob {} lost {:.1f}{}: sent to Minecraft", a_proxy.formId, a_mobId, a_proxy.pendingDamage, a_proxy.byPlayer ? ", the player's doing" : "");
			}
			a_proxy.pendingDamage = 0.0f;
			a_proxy.byPlayer = false;
			a_proxy.flushAge = 0.0f;
		}

		// What Skyrim has taken off a stand-in since last looked goes to Minecraft as damage to the real mob (batched, see above), and is given
		// back to the stand-in: Minecraft's mob is the one that has health, and its health comes back through SyncProxyHealth.
		template <class A>
		void AbsorbProxyDamage(A* a_actor, MobProxy& a_proxy)
		{
			const float health = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			float       damage = health < a_proxy.lastHealth - 0.01f ? a_proxy.lastHealth - health : 0.0f;
			const float taken = damage;
			// What Skyrim's blow would have taken beyond what the stand-in could (see StandInDamageGuard) is the mob's to lose as well.
			if (const auto over = proxyOverkill.find(a_proxy.formId); over != proxyOverkill.end()) {
				damage += over->second;
				proxyOverkill.erase(over);
			}
			if (damage > 0.01f) {
				// A blow from the player's weapon or an arrow is already in Minecraft (the swing or the shot is sent to it as such), so
				// it is only given back here; a spell (or fire, or an NPC's blow) is passed on as damage to the mob.
				if (!proxyWeaponHits.contains(a_proxy.formId)) {
					a_proxy.pendingDamage += damage;
					// The player's if they hit it lately; a stand-in that only exists to be aimed at has no one else to blame.
					const auto hit = proxyPlayerHitAge.find(a_proxy.formId);
					if (a_proxy.targetOnly || (hit != proxyPlayerHitAge.end() && hit->second < kPlayerBlameSeconds)) {
						a_proxy.byPlayer = true;
					}
				}
				if (taken > 0.0f) {
					DamageHealth(a_actor, -taken);  // given back
				}
			}
			a_proxy.lastHealth = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
		}

		// The stand-in's health is the mob's: the same max, the same current value, as Minecraft last said. A lock-on mod's bar, or the
		// number on it, is then the mob's own.
		template <class A>
		void SyncProxyHealth(A* a_actor, MobProxy& a_proxy)
		{
			if (!(a_proxy.mcMax > 0.0f)) {
				return;
			}
			auto*       values = a_actor->AsActorValueOwner();
			const float target = std::clamp(a_proxy.mcHealth, 0.1f, a_proxy.mcMax);
			// The max a health bar goes by is the base value plus whatever sits on top of it (a modifier left by a perk or an effect), so
			// the base is set to what makes that total Minecraft's max exactly, not just to Minecraft's max.
			const float max = a_actor->GetActorValueMax(RE::ActorValue::kHealth);
			if (std::fabs(max - a_proxy.mcMax) > 0.05f) {
				const float base = values->GetBaseActorValue(RE::ActorValue::kHealth);
				SetAV(values, RE::ActorValue::kHealth, std::max(1.0f, base + (a_proxy.mcMax - max)));
			}
			float now = values->GetActorValue(RE::ActorValue::kHealth);
			if (std::fabs(now - target) > 0.05f) {
				DamageHealth(a_actor, now - target);  // (negative: it heals)
				now = values->GetActorValue(RE::ActorValue::kHealth);
				// The first way did not take (the stand-in kept the health it was made with while its mob had lost some, in the log): the
				// other ways of changing health are tried on what is still owed, as DamageHealthExactly does. They run as SkyCraft's own change
				// (ownHealthChange), or StandInDamageGuard would cut them to nothing like Skyrim's hits.
				if (std::fabs(now - target) > 0.5f) {
					++ownHealthChange;
					for (int way : { 3, 2, 1 }) {
						const float owed = now - target;
						if (std::fabs(owed) <= 0.5f) {
							break;
						}
						if (way != 3 && owed < 0.0f) {
							continue;  // the other ways only take health off
						}
						TakeHealthWith(a_actor, way, owed, nullptr);
						now = values->GetActorValue(RE::ActorValue::kHealth);
						static int fixedBy[4] = {};
						if (std::fabs(now - target) <= 0.5f && fixedBy[way]++ < 3) {
							logger::info("Stand-in ({:08X}) health follows the mob's by way {} (the plain way did not take)", a_proxy.formId, way);
						}
					}
					--ownHealthChange;
				}
			}
			a_proxy.lastHealth = values->GetActorValue(RE::ActorValue::kHealth);  // our own change isn't damage done to it
			// If it still isn't Minecraft's, say so (a few times): the bar would then be wrong, and this says by how much and why.
			const float maxNow = a_actor->GetActorValueMax(RE::ActorValue::kHealth);
			if (std::fabs(a_proxy.lastHealth - target) > 0.5f || std::fabs(maxNow - a_proxy.mcMax) > 0.5f) {
				// Not while it is warming up out of sight (its health is set again every frame then, and used to fill the log's whole allowance
				// of these at once, so the log never said whether the health followed the mob's afterwards).
				static int logged = 0;
				if (a_proxy.warm <= 0.0f && logged++ < 30) {
					logger::warn("Stand-in ({:08X}) health isn't the mob's: has {:.1f}/{:.1f}, mob has {:.1f}/{:.1f} (base {:.1f}, permanent {:.1f})", a_proxy.formId,
						a_proxy.lastHealth, maxNow, target, a_proxy.mcMax, values->GetBaseActorValue(RE::ActorValue::kHealth),
						values->GetPermanentActorValue(RE::ActorValue::kHealth));
				}
			}
		}

		// A stand-in knocked down (it is essential: at no health it goes into bleedout instead of dying) is one lock-on mods refuse, and with
		// its AI off it never gets up by itself. StandInDamageGuard keeps Skyrim's damage from doing it; this gets one up if it happens anyway.
		template <class A>
		void ReviveProxy(A* a_actor, MobProxy& a_proxy)
		{
			// Skyrim has more than one way of putting an essential actor down: bleedout, but also "essential down" and unconscious. Which one
			// a hit from the player's own weapon ends in isn't something to guess, so any life state that isn't alive (a stand-in that is
			// really dead is dropped by the caller before this) counts as knocked down. Lock-on and the target bar refuse all of them.
			bool down = false;
			int  state = -1;
			if constexpr (requires { a_actor->AsActorState()->actorState1.lifeState; }) {
				state = static_cast<int>(a_actor->AsActorState()->actorState1.lifeState);
				down = state != static_cast<int>(RE::ACTOR_LIFE_STATE::kAlive) && state != static_cast<int>(RE::ACTOR_LIFE_STATE::kDead) &&
				       state != static_cast<int>(RE::ACTOR_LIFE_STATE::kDying);
			} else if constexpr (requires { a_actor->AsActorState()->actorState2.lifeState; }) {
				state = static_cast<int>(a_actor->AsActorState()->actorState2.lifeState);
				down = state != static_cast<int>(RE::ACTOR_LIFE_STATE::kAlive) && state != static_cast<int>(RE::ACTOR_LIFE_STATE::kDead) &&
				       state != static_cast<int>(RE::ACTOR_LIFE_STATE::kDying);
			}
			if constexpr (requires { a_actor->AsActorState()->IsBleedingOut(); }) {
				down = down || a_actor->AsActorState()->IsBleedingOut();
			}
			if (!down) {
				return;
			}
			SyncProxyHealth(a_actor, a_proxy);
			bool stood = false;
			if constexpr (requires { a_actor->AsActorState()->actorState1.lifeState = RE::ACTOR_LIFE_STATE::kAlive; }) {
				a_actor->AsActorState()->actorState1.lifeState = RE::ACTOR_LIFE_STATE::kAlive;
				stood = true;
			} else if constexpr (requires { a_actor->AsActorState()->actorState2.lifeState = RE::ACTOR_LIFE_STATE::kAlive; }) {
				a_actor->AsActorState()->actorState2.lifeState = RE::ACTOR_LIFE_STATE::kAlive;
				stood = true;
			}
			// Its animation graph may still be lying in bleedout; with its AI off nothing else will tell it to get up.
			if constexpr (requires { a_actor->NotifyAnimationGraph("BleedoutStop"); }) {
				a_actor->NotifyAnimationGraph("BleedoutStop");
			}
			static int logged = 0;
			if (logged++ < 10) {
				logger::warn("Mob stand-in ({:08X}) was knocked down (life state {}); {}", a_actor->GetFormID(), state, stood ? "got it up again" : "this CommonLib has no way to get it up (it is dropped and made again instead)");
			}
		}

		// A mob is gone: its stand-in leaves the fight and waits in the pool (see proxyPool).
		void RemoveProxy(std::int32_t a_mobId)
		{
			auto it = mobProxies.find(a_mobId);
			if (it == mobProxies.end()) {
				return;
			}
			FlushProxyDamage(a_mobId, it->second);
			{
				static int loggedRemove = 0;
				if (loggedRemove++ < 60) {
					logger::info("Stand-in ({:08X}) of mob {} removed from the mob list (silence {:.1f} s); it goes to the pool", it->second.formId, a_mobId, it->second.silence);
				}
			}
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->second.formId); actor && !actor->IsDead()) {
				ReleaseHunters(actor);
				StopProxyCombat(actor);
				if (std::ranges::find(proxyPool, it->second.formId) == proxyPool.end()) {
					proxyPool.push_back(it->second.formId);
				}
			} else {
				if (actor) {
					ReleaseHunters(actor);
					deadProxies.push_back({ it->second.formId, 2.0f });  // died: the body goes in a moment
				}
				proxyIds.erase(it->second.formId);  // it died or is gone: forgotten
			}
			mobProxies.erase(it);
		}

		void ClearProxies()
		{
			MobBodies::Clear();
			while (!mobProxies.empty()) {
				RemoveProxy(mobProxies.begin()->first);
			}
			mobNames.clear();
		}

		// Pooled stand-ins wait high above the player where nothing can reach them.
		void ParkPool(RE::PlayerCharacter* a_player, float a_delta)
		{
			for (auto it = deadProxies.begin(); it != deadProxies.end();) {
				it->wait -= a_delta;
				if (it->wait > 0.0f) {
					++it;
					continue;
				}
				if (auto* body = RE::TESForm::LookupByID<RE::Actor>(it->formId)) {
					if constexpr (requires { body->Disable(); }) {
						body->Disable();  // not deleted: destroying an actor Skyrim is still working with can crash the game
					}
				}
				it = deadProxies.erase(it);
			}
			// A stand-in nobody is tracking (its mob's record was lost somewhere along the way) would stand where it was left, a skeever
			// in the open with no mob. Every few seconds anything that is a stand-in and isn't in use or waiting in the pool goes to the pool.
			sweepTimer -= a_delta;
			if (sweepTimer <= 0.0f) {
				sweepTimer = 3.0f;
				std::unordered_set<RE::FormID> inUse;
				for (const auto& entry : mobProxies) {
					inUse.insert(entry.second.formId);
				}
				inUse.insert(proxyPool.begin(), proxyPool.end());
				for (const auto& dead : deadProxies) {
					inUse.insert(dead.formId);
				}
				for (const auto id : proxyIds) {
					if (!inUse.contains(id)) {
						proxyPool.push_back(id);
						logger::info("Mob stand-in {:08X} had no mob; put away", id);
					}
				}
			}
			parkTimer -= a_delta;
			if (parkTimer > 0.0f || proxyPool.empty()) {
				return;
			}
			parkTimer = 0.25f;
			auto spot = a_player->GetPosition();
			spot.z += 60.0f * static_cast<float>(proto::kUnitsPerBlock);
			std::unordered_set<RE::FormID> live;
			for (const auto& entry : mobProxies) {
				live.insert(entry.second.formId);
			}
			for (auto it = proxyPool.begin(); it != proxyPool.end();) {
				// A stand-in whose mob lives can be in the pool by mistake (put there twice, one entry left over once it was reused):
				// parking it switched it off under its mob, which is the stand-in that vanished and came back a moment later.
				if (live.contains(*it)) {
					it = proxyPool.erase(it);
					continue;
				}
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(*it);
				if (!actor || actor->IsDead()) {
					if (actor) {
						deadProxies.push_back({ *it, 2.0f });
					}
					proxyIds.erase(*it);
					it = proxyPool.erase(it);
					continue;
				}
				if (actor->IsDisabled()) {
					++it;  // already put away
					continue;
				}
				{
					static int loggedPark = 0;
					if (loggedPark++ < 60) {
						logger::info("Stand-in ({:08X}) parked: switched off in the pool", actor->GetFormID());
					}
				}
				StopProxyCombat(actor);
				QuietProxy(actor);  // hidden again, in case its 3D was rebuilt since
				actor->SetPosition(spot, true);
				// Taken out of the world altogether (switched off, not deleted: see the pool's comment), so a stand-in with no mob can
				// never be seen, heard or bumped into wherever it ends up. Switched on again when the next mob needs one.
				FadeProxy(actor, true);
				if constexpr (requires { actor->Disable(); actor->Enable(false); }) {
					actor->Disable();
				}
				++it;
			}
		}

		// A mob reported by Minecraft: make or move its stand-in. a_ev.formId = the mob's id, a/b/c = its position.
		void MobProxyEvent(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev)
		{
			if (MobBodies::Enabled()) {  // a mob's body is a collision box (MobBodies.cpp); the stand-in actor, for Skyrim's NPCs to fight, goes on below
				MobBodies::OnProxy(a_player, a_ev);
				if (!MobBodies::KeepActors()) {
					return;
				}
			}
			const auto mobId = static_cast<std::int32_t>(a_ev.formId);
			if ((a_ev.flags & proto::kMobProxyGone) != 0) {
				mobNames.erase(mobId);
				RemoveProxy(mobId);
				return;
			}
			const auto pos = McToSky(a_ev.a, a_ev.b, a_ev.c);
			auto       it = mobProxies.find(mobId);
			if (it == mobProxies.end()) {
				if (proxiesUnavailable || a_player->IsDead()) {
					return;
				}
				RE::Actor* actor = nullptr;
				float      warm = 0.0f;
				const std::string sizeKey = MobBodies::Enabled() ? MobBodies::KeyFor(mobId, std::bit_cast<float>(a_ev.weapon)) : std::string();
				if (!sizeKey.empty()) {  // a parked stand-in of this mob's size first
					for (auto pooled = proxyPool.rbegin(); pooled != proxyPool.rend(); ++pooled) {
						if (const auto have = actorSizeKey.find(*pooled); have != actorSizeKey.end() && have->second == sizeKey) {
							std::swap(*pooled, proxyPool.back());
							break;
						}
					}
				}
				while (!proxyPool.empty() && !actor) {  // a parked one first
					actor = RE::TESForm::LookupByID<RE::Actor>(proxyPool.back());
					if (!actor || actor->IsDead()) {
						proxyIds.erase(proxyPool.back());
						actor = nullptr;
					} else if (actor->IsDisabled()) {
						FadeProxy(actor, true);  // held at no alpha before its 3D reloads, so its coming back is never seen
						warm = 1.5f;
						if constexpr (requires { actor->Enable(false); }) {
							actor->Enable(false);
						}
					}
					proxyPool.pop_back();
				}
				auto* base = actor ? nullptr : (sizeKey.empty() ? FindProxyBase() : CustomProxyBase(sizeKey));
				if (!actor && base && mobProxies.size() + proxyPool.size() < kMaxProxies) {
					actor = PlaceProxyActor(a_player, base);
					warm = 2.0f;
					if (actor && !sizeKey.empty()) {
						actorSizeKey[actor->GetFormID()] = sizeKey;
					}
					if (actor && skeletonTrialPending && skeletonTrialActor == 0) {
						skeletonTrialActor = actor->GetFormID();
						skeletonTrialAge = 0.0f;
						skeletonTrialPending = false;
					}
				} else if (!actor && base) {
					return;  // as many stand-ins as there will ever be, and none free
				}
				if (!actor) {
					if (base && !proxiesUnavailable) {
						proxiesUnavailable = true;
						logger::warn("Couldn't place a stand-in actor for a Minecraft mob; falling back to the simple NPC fighting");
					}
					return;
				}
				FadeProxy(actor, true);  // just made where the player stands: held at no alpha, so it is never seen there (or fading in) before it is moved
				MobProxy made;
				made.formId = actor->GetFormID();
				made.warm = warm;
				if (warm > 0.0f) {
					const auto spot = WarmSpot(a_player);
					actor->SetPosition(spot, true);  // out of sight at once: what it plays as it first loads is played up there
					ResetProxyFall(actor, spot.z);
				}
				made.lastHealth = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
				proxyIds.insert(made.formId);
				QuietProxy(actor, true);
				it = mobProxies.emplace(mobId, made).first;
				static int logged = 0;
				if (logged++ < 10) {
					logger::info("Made a stand-in ({:08X}) for Minecraft mob {}{}", made.formId, mobId, warm > 0.0f ? "; warming up out of sight" : "");
				}
			}
			if (it->second.placed && it->second.silence > 0.03f) {  // its vertical speed, from where it was reported before
				const float vy = (pos.z - it->second.pos.z) / it->second.silence;
				it->second.velY += (std::clamp(vy, -3000.0f, 3000.0f) - it->second.velY) * 0.5f;
			}
			it->second.pos = pos;
			it->second.heading = WrapHeading(McYawToHeading(a_ev.d));
			it->second.silence = 0.0f;
			it->second.targetOnly = (a_ev.flags & proto::kMobProxyTargetOnly) != 0;
			if (const float height = std::bit_cast<float>(a_ev.weapon); height > 0.0f && height < 64.0f) {  // the mob's height, in the weapon field
				it->second.height = height;
			}
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->second.formId)) {
				QuietProxy(actor, !(it->second.mcMax > 0.0f));
				FitProxyHeight(actor, it->second);
				if (!(it->second.warm > 0.0f)) {
					FollowProxy(actor, it->second, 0.0f);
				}
				if (const auto name = mobNames.find(mobId); name != mobNames.end() && it->second.named != name->second) {
					NameProxy(actor, name->second);
					it->second.named = name->second;
				}
			}
		}

		// A mob's health, eye height and velocity, from Minecraft (kEvMobVitals).
		void MobVitalsEvent(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev)
		{
			if (MobBodies::Enabled()) {
				MobBodies::OnVitals(a_player, a_ev);
				if (!MobBodies::KeepActors()) {
					return;
				}
			}
			const auto it = mobProxies.find(static_cast<std::int32_t>(a_ev.formId));
			if (it == mobProxies.end()) {
				return;
			}
			auto& proxy = it->second;
			proxy.mcHealth = a_ev.a;
			proxy.mcMax = a_ev.b;
			if (a_ev.c > 0.05f && a_ev.c < 32.0f) {
				proxy.eye = a_ev.c;
			}
			proxy.velX = std::bit_cast<float>(a_ev.flags);
			proxy.velZ = std::bit_cast<float>(a_ev.weapon);
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(proxy.formId); actor && !actor->IsDead()) {
				AbsorbProxyDamage(actor, proxy);  // anything Skyrim did since last frame is counted before the mob's health replaces it
				SyncProxyHealth(actor, proxy);
			}
		}

		// A mob's name, from Minecraft: 24 ASCII bytes spread over a, b, c, d, flags, weapon.
		void MobNameEvent(const proto::McEvent& a_ev)
		{
			std::array<char, 25> text{};
			std::memcpy(text.data() + 0, &a_ev.a, 4);
			std::memcpy(text.data() + 4, &a_ev.b, 4);
			std::memcpy(text.data() + 8, &a_ev.c, 4);
			std::memcpy(text.data() + 12, &a_ev.d, 4);
			std::memcpy(text.data() + 16, &a_ev.flags, 4);
			std::memcpy(text.data() + 20, &a_ev.weapon, 4);
			const auto mobId = static_cast<std::int32_t>(a_ev.formId);
			const std::string name(text.data());
			if (name.empty()) {
				return;
			}
			mobNames[mobId] = name;
			if (const auto it = mobProxies.find(mobId); it != mobProxies.end() && it->second.named != name) {
				if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->second.formId)) {
					NameProxy(actor, name);
					it->second.named = name;
				}
			}
		}

		// Each frame: stand-ins whose mob has gone quiet are dropped, and what Skyrim's NPCs took off a stand-in is
		// handed to Minecraft and given back.
		// Diagnostics for stand-ins that seem to vanish: what Skyrim says about one is logged whenever it changes (not every frame), so the
		// log shows what a hit left behind and whether it is gone after the switch back from Minecraft's controls.
		template <class A>
		void WatchProxy(A* a_actor, const MobProxy& a_proxy, bool a_skyrimMode)
		{
			static std::unordered_map<RE::FormID, std::uint32_t> seen;
			static int                                           logged = 0;
			int state = -1;
			if constexpr (requires { a_actor->AsActorState()->actorState1.lifeState; }) {
				state = static_cast<int>(a_actor->AsActorState()->actorState1.lifeState);
			}
			bool ragdoll = false;
			if constexpr (requires { a_actor->IsInRagdollState(); }) {
				ragdoll = a_actor->IsInRagdollState();
			}
			const bool  disabled = a_actor->IsDisabled();
			const bool  has3d = a_actor->Get3D() != nullptr;
			const bool  loaded = a_actor->Is3DLoaded();
			const bool  combat = a_actor->IsInCombat();
			const bool  nearMob = a_actor->GetPosition().GetDistance(a_proxy.shown) < 140.0f;
			const bool  silent = a_proxy.silence > 1.0f;
			const std::uint32_t key = (disabled ? 1u : 0u) | (has3d ? 2u : 0u) | (loaded ? 4u : 0u) | (combat ? 8u : 0u) | (ragdoll ? 16u : 0u) |
			                          (nearMob ? 32u : 0u) | (silent ? 64u : 0u) | (a_skyrimMode ? 128u : 0u) | (static_cast<std::uint32_t>(state + 1) << 8);
			auto& last = seen[a_proxy.formId];
			if (last != key && logged < 150) {
				last = key;
				++logged;
				logger::info("Watch stand-in ({:08X}): {} disabled={} 3d={} loaded={} combat={} life={} ragdoll={} near-mob={} silent={}", a_proxy.formId,
					a_skyrimMode ? "skyrim-mode" : "minecraft-mode", disabled, has3d, loaded, combat, state, ragdoll, nearMob, silent);
			}
		}

		// Lock-on is LockOn.cpp's: Minecraft's mobs are handed to True Directional Movement as virtual targets there. The stand-ins are
		// only reported to it (as actors it must never pick), whenever the set of them changes, so a mob is never offered twice.
		std::vector<std::uint32_t> ignoredStandIns;

		void PublishLockTargets(float)
		{
			std::vector<std::uint32_t> now(proxyIds.begin(), proxyIds.end());
			std::sort(now.begin(), now.end());
			if (now == ignoredStandIns) {
				return;
			}
			ignoredStandIns = now;
			LockOn::SetIgnoredActors(ignoredStandIns);
		}

		void UpdateProxies(RE::PlayerCharacter* a_player, float a_delta)
		{
			PublishLockTargets(a_delta);
			if (skeletonTrialActor != 0 && skeletonTrialActor != 0xFFFFFFFFu && !skeletonTrialFailed) {  // the first actor with a custom skeleton: does it get a 3D?
				skeletonTrialAge += a_delta;
				auto* trial = RE::TESForm::LookupByID<RE::Actor>(skeletonTrialActor);
				if (trial && trial->Is3DLoaded() && skeletonTrialAge > 1.0f) {
					logger::info("Custom skeleton trial: the actor has a 3D with its own skeleton; custom skeletons stay on");
					MobBodies::SkeletonTrialPassed();
					skeletonTrialActor = 0xFFFFFFFFu;
				} else if (skeletonTrialAge > 6.0f) {
					logger::warn("Custom skeleton trial: the actor got no 3D in 6 seconds; custom skeletons are off from now on, and from the next start");
					skeletonTrialFailed = true;
					MobBodies::SkeletonTrialFailed();
				}
			}
			if (MobBodies::Enabled()) {
				MobBodies::Update(a_player, a_delta);
				if (!MobBodies::KeepActors()) {
					return;  // no stand-in actors to keep
				}
			}
			for (auto hit = proxyPlayerHitAge.begin(); hit != proxyPlayerHitAge.end();) {
				hit->second += a_delta;
				hit = hit->second > 30.0f ? proxyPlayerHitAge.erase(hit) : std::next(hit);
			}
			for (auto hit = proxyWeaponHits.begin(); hit != proxyWeaponHits.end();) {
				hit->second -= a_delta;
				hit = hit->second <= 0.0f ? proxyWeaponHits.erase(hit) : std::next(hit);
			}
			for (auto it = mobProxies.begin(); it != mobProxies.end();) {
				auto& proxy = it->second;
				// Silence is only counted while Minecraft is really there and in steps of at most 0.1 s: a long Skyrim frame (the
				// log shows 38 s ones), a mode switch or a stalled Minecraft server is not the mob going away, and used to
				// make every stand-in vanish at once (so no lock-on) and come back elsewhere.
				if (Link::Get().McAlive()) {
					proxy.silence += std::min(a_delta, 0.1f);
				}
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(proxy.formId);
				// A knocked-down stand-in is got up again before it is judged (see ReviveProxy).
				if (actor && !actor->IsDead()) {
					ReviveProxy(actor, proxy);
				}
				if (!actor || actor->IsDead() || proxy.silence > 10.0f) {
					static int dropped = 0;
					if (dropped++ < 40) {
						logger::info("Stand-in ({:08X}) of mob {} dropped: {}", proxy.formId, it->first,
							!actor ? "Skyrim no longer has the actor" : actor->IsDead() ? "it died in Skyrim" : "Minecraft stopped reporting the mob");
					}
					const auto id = it->first;
					++it;
					RemoveProxy(id);
					continue;
				}
				// A stand-in whose mob is alive is never left switched off, whatever switched it off (a stand-in is only put away when its
				// mob is gone): it is switched on again and kept out of sight for a moment while it loads.
				// Skyrim hands a stand-in that was switched off back with its own default health (a rabbit's), not the mob's. That drop is
				// not damage, so it is not passed on to Minecraft (it used to be: 21 "lost" at a time, the player's doing, killing mobs nobody hit).
				bool reloaded = false;
				if (actor->IsDisabled()) {
					{
						static int loggedOff = 0;
						if (loggedOff++ < 40) {
							const auto at = actor->GetPosition();
							logger::info("Stand-in ({:08X}) found switched off: at ({:.0f} {:.0f} {:.0f}), mob shown at ({:.0f} {:.0f} {:.0f}), silence {:.1f} s, pending damage {:.1f}, in pool {}, in dead list {}",
								proxy.formId, at.x, at.y, at.z, proxy.shown.x, proxy.shown.y, proxy.shown.z, proxy.silence, proxy.pendingDamage,
								std::ranges::find(proxyPool, proxy.formId) != proxyPool.end(),
								std::ranges::any_of(deadProxies, [&](const DeadProxy& d) { return d.formId == proxy.formId; }));
						}
					}
					{
						// What the stand-in looked like when it was found off: the clue to what switched it off.
						static int loggedWhy = 0;
						if (loggedWhy++ < 40) {
							// (A generic lambda, so the checks below are in a template and the way this CommonLib spells it is picked at compile time.)
							const int life = [](auto* a) {
								int state = -1;
								if constexpr (requires { a->AsActorState()->actorState1.lifeState; }) {
									state = static_cast<int>(a->AsActorState()->actorState1.lifeState);
								} else if constexpr (requires { a->AsActorState()->actorState2.lifeState; }) {
									state = static_cast<int>(a->AsActorState()->actorState2.lifeState);
								}
								return state;
							}(actor);
							const auto flags = [](auto* a) {
								std::uint32_t f = 0;
								if constexpr (requires { static_cast<std::uint32_t>(a->formFlags); }) {
									f = static_cast<std::uint32_t>(a->formFlags);
								}
								return f;
							}(actor);
							logger::info("Stand-in ({:08X}) found off: form flags {:08X}, same cell as the player {}", proxy.formId, flags,
								actor->GetParentCell() == a_player->GetParentCell());
							logger::info("Stand-in ({:08X}) found off: health {:.1f} (last {:.1f}, mob's {:.1f}/{:.1f}), life state {}, in combat {}, warm {:.1f}, hit by the player {} s ago",
								proxy.formId, actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth), proxy.lastHealth, proxy.mcHealth, proxy.mcMax, life,
								actor->IsInCombat(), proxy.warm, proxyPlayerHitAge.contains(proxy.formId) ? proxyPlayerHitAge[proxy.formId] : -1.0f);
						}
					}
					FadeProxy(actor, true);
					if constexpr (requires { actor->Enable(false); }) {
						actor->Enable(false);
					}
					reloaded = true;
					proxy.warm = std::max(proxy.warm, 1.5f);  // out of sight up in the air while it loads: what it plays as it comes back is seen there, not at the mob
					proxy.placed = false;
					static int switchedBack = 0;
					if (switchedBack++ < 20) {
						logger::info("Stand-in ({:08X}) of mob {} had been switched off while its mob lives; switched back on", proxy.formId, it->first);
					}
				}
				if (reloaded || proxy.warm > 0.0f) {
					// Just switched back on, or still warming up out of sight: whatever its health reads is not damage, and nothing can be
					// hitting it yet. SyncProxyHealth below gives it the mob's health again.
					proxy.lastHealth = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
					proxyOverkill.erase(proxy.formId);
				} else {
					AbsorbProxyDamage(actor, proxy);
				}
				SyncProxyHealth(actor, proxy);  // every frame, so the health is the mob's at all times, not only when Minecraft reports it
				WatchProxy(actor, proxy, State().skyrimMode);
				if (proxy.warm > 0.0f) {
					proxy.warm -= a_delta;
					const auto spot = WarmSpot(a_player);
					actor->SetPosition(spot, true);
					ResetProxyFall(actor, spot.z);
					HideProxy(actor);
					StripSpawnEffects(actor);
					if (proxy.warm <= 0.0f) {
						proxy.placed = false;  // it is brought to its mob in one step
						proxy.hideFrames = 0;
					}
				} else {
					FollowProxy(actor, proxy, a_delta);
				}
				proxy.flushAge += a_delta;
				if (proxy.pendingDamage > 0.0f && (proxy.pendingDamage >= kProxyBigHit || proxy.flushAge >= kProxyFlushSeconds)) {
					FlushProxyDamage(it->first, proxy);
				}
				++it;
			}
			ParkPool(a_player, a_delta);
			if (mobProxies.empty()) {
				return;
			}
			// The NPCs: any not already fighting something are put into combat with the nearest stand-in. Skyrim does the rest.
			proxyFightTimer -= a_delta;
			if (proxyFightTimer > 0.0f) {
				return;
			}
			proxyFightTimer = 0.5f;
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			const auto playerPos = a_player->GetPosition();
			for (auto& handle : lists->highActorHandles) {
				auto  actorPtr = handle.get();
				auto* npc = actorPtr.get();
				if (!npc || npc == a_player || npc->IsDead() || npc->IsDisabled() || !npc->Is3DLoaded() || npc->IsInCombat() || proxyIds.contains(npc->GetFormID()) ||
					npc->GetPosition().GetDistance(playerPos) > kActorRange) {
					continue;
				}
				const bool  fighter = npc->AsActorValueOwner()->GetActorValue(RE::ActorValue::kAggression) >= 1.0f;
				const float reach = (fighter ? 30.0f : 20.0f) * static_cast<float>(proto::kUnitsPerBlock);  // (a skeleton or pillager shoots from further off than 12 blocks)
				RE::Actor*  best = nullptr;
				float       bestDistance = reach;
				for (const auto& entry : mobProxies) {
					const auto& proxy = entry.second;
					if (proxy.targetOnly) {
						continue;  // there for the player to lock on to: no business of the NPCs
					}
					auto*       standIn = RE::TESForm::LookupByID<RE::Actor>(proxy.formId);
					const float d = standIn ? npc->GetPosition().GetDistance(proxy.pos) : reach;
					if (standIn && d < bestDistance) {
						bestDistance = d;
						best = standIn;
					}
				}
				if (best) {
					npc->StartCombat(best);
					static int logged = 0;
					if (logged++ < 20) {
						logger::info("{} goes into combat with a Minecraft mob", npc->GetDisplayFullName());
					}
				}
			}
		}

		void KillPlayer(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev)
		{
			if (a_player->IsDead()) {
				return;
			}
			logger::info("Minecraft player died (killer {:08X}); killing the Skyrim player", a_ev.formId);
			SetEssential(a_player, false);
			auto* killer = a_ev.formId ? RE::TESForm::LookupByID<RE::Actor>(a_ev.formId) : nullptr;
			const float health = a_player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			a_player->KillImpl(killer, health + 1.0f, true, false);
			if (!a_player->IsDead()) {
				a_player->KillImmediate();
			}
		}

		bool AnyoneFighting(RE::PlayerCharacter* a_player)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return false;
			}
			const auto playerPos = a_player->GetPosition();
			for (auto& handle : lists->highActorHandles) {
				auto actor = handle.get();
				if (actor && !actor->IsDead() && actor->IsInCombat() && actor->GetActorRuntimeData().currentCombatTarget.get().get() == a_player &&
					actor->GetPosition().GetDistance(playerPos) < 6000.0f) {
					return true;
				}
			}
			return false;
		}

		// How an enemy's combat AI sees the player (read under SEH: these arrays belong to Skyrim's
		// AI, which may be changing them).
		struct CombatView
		{
			int   unreachableSpots = -1;
			float nearestSpot = -1.0f;  // units from the player
			int   detectLevel = -99;
			int   targetFlags = -1;
			float lastKnownGap = -1.0f;  // how far the player is from where the group last placed them
			int   attackers = -1;
		};

		bool ReadCombatView(const RE::CombatController* a_cc, std::uint32_t a_playerHandle, const RE::NiPoint3& a_playerPos, CombatView& a_out)
		{
			__try {
				if (a_cc->state) {
					const auto& spots = a_cc->state->unreachableLocations;
					a_out.unreachableSpots = static_cast<int>(spots.size());
					for (std::uint32_t i = 0; i < spots.size() && i < 64; ++i) {
						const float d = spots[i].loc.pos.GetDistance(a_playerPos);
						if (a_out.nearestSpot < 0.0f || d < a_out.nearestSpot) {
							a_out.nearestSpot = d;
						}
					}
				}
				if (a_cc->combatGroup) {
					const auto& targets = a_cc->combatGroup->targets;
					for (std::uint32_t i = 0; i < targets.size() && i < 32; ++i) {
						const auto& t = targets[i];
						if (t.targetHandle.native_handle() == a_playerHandle) {
							a_out.detectLevel = t.detectLevel;
							a_out.targetFlags = static_cast<int>(t.flags.underlying());
							a_out.lastKnownGap = t.lastKnownLoc.pos.GetDistance(a_playerPos);
							a_out.attackers = t.attackerCount;
						}
					}
				}
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// ---- Minecraft lava and fire on Skyrim's NPCs -------------------------------------------
		// Standing in lava, fire or a campfire (or on magma) hurts like it does in Minecraft,
		// scaled like Minecraft's hits on them, sets them burning (Skyrim's own campfire burn,
		// HazardFireSpell, for the flames on their body), and they scramble out to the nearest
		// spot that isn't burning.
		constexpr RE::FormID kHazardFireSpell = 0x000153BD;
		constexpr float      kHazardTick = 0.2f;

		struct Burning
		{
			float        afterburn = 0.0f;  // seconds left burning after leaving the fire
			float        castCooldown = 0.0f;
			bool         escaping = false;
			RE::NiPoint3 escapeTo;
		};
		std::unordered_map<RE::FormID, Burning> burning;
		float                                   hazardTimer = 0.0f;

		std::uint8_t HazardAtActor(RE::Actor* a_actor)
		{
			const auto   mc = SkyToMc(a_actor->GetPosition());
			const int    x = int(std::floor(mc.x)), z = int(std::floor(mc.z)), feet = int(std::floor(mc.y + 0.1));
			std::uint8_t worst = proto::kHazardNone;
			for (int y = feet; y <= feet + 1; ++y) {
				const auto h = BlockLights::HazardAt(x, y, z);
				if (h == proto::kHazardLava || (h == proto::kHazardFire && worst != proto::kHazardLava)) {
					worst = h;
				}
			}
			if (worst == proto::kHazardNone && BlockLights::HazardAt(x, int(std::floor(mc.y - 0.1)), z) == proto::kHazardMagma) {
				worst = proto::kHazardMagma;
			}
			return worst;
		}

		// The nearest block column around the actor whose feet and head blocks don't burn.
		bool FindEscape(RE::Actor* a_actor, RE::NiPoint3& a_out)
		{
			const auto mc = SkyToMc(a_actor->GetPosition());
			const int  x0 = int(std::floor(mc.x)), z0 = int(std::floor(mc.z)), feet = int(std::floor(mc.y + 0.1));
			for (int r = 1; r <= 5; ++r) {
				float best = 1e9f;
				int   bx = 0, bz = 0;
				for (int dx = -r; dx <= r; ++dx) {
					for (int dz = -r; dz <= r; ++dz) {
						if (std::max(std::abs(dx), std::abs(dz)) != r) {
							continue;
						}
						const int x = x0 + dx, z = z0 + dz;
						bool      safe = true;
						for (int y = feet - 1; y <= feet + 1 && safe; ++y) {
							const auto h = BlockLights::HazardAt(x, y, z);
							safe = h == proto::kHazardNone || (y == feet - 1 && h == proto::kHazardFire);
						}
						const float d = float((x + 0.5 - mc.x) * (x + 0.5 - mc.x) + (z + 0.5 - mc.z) * (z + 0.5 - mc.z));
						if (safe && d < best) {
							best = d, bx = x, bz = z;
						}
					}
				}
				if (best < 1e8f) {
					a_out = McToSky(bx + 0.5, mc.y, bz + 0.5);
					return true;
				}
			}
			return false;
		}

		void UpdateHazards(RE::PlayerCharacter* a_player, float a_delta)
		{
			// Scramble out: a quick run toward safety, every frame, facing the way they go.
			for (auto& [id, b] : burning) {
				if (!b.escaping) {
					continue;
				}
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!actor || actor->IsDead() || !actor->Is3DLoaded()) {
					b.escaping = false;
					continue;
				}
				auto         pos = actor->GetPosition();
				RE::NiPoint3 to = b.escapeTo - pos;
				to.z = 0.0f;
				const float dist = to.Length();
				if (dist < 10.0f) {
					b.escaping = false;
					continue;
				}
				const float step = std::min(dist, 380.0f * a_delta);
				pos += to * (step / dist);
				actor->SetPosition(pos, true);
				actor->SetHeading(std::atan2(to.x, to.y));
			}

			hazardTimer -= a_delta;
			if (hazardTimer > 0.0f) {
				return;
			}
			hazardTimer = kHazardTick;
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			static auto* fireSpell = RE::TESForm::LookupByID<RE::SpellItem>(kHazardFireSpell);
			const auto   playerPos = a_player->GetPosition();
			for (auto& handle : lists->highActorHandles) {
				auto actorPtr = handle.get();
				auto* actor = actorPtr.get();
				if (!actor || actor == a_player || actor->IsDead() || !actor->Is3DLoaded() || actor->GetPosition().GetDistance(playerPos) > kActorRange || proxyIds.contains(actor->GetFormID())) {
					continue;
				}
				const auto hazard = HazardAtActor(actor);
				auto       it = burning.find(actor->GetFormID());
				if (hazard == proto::kHazardNone && (it == burning.end() || it->second.afterburn <= 0.0f)) {
					continue;
				}
				auto& b = burning[actor->GetFormID()];
				// Minecraft damage per second: lava 8 (4 every half second), fire 2, magma 1,
				// then burning 1 a second once out; scaled like a Minecraft hit on them.
				float mcPerSecond = 1.0f;
				if (hazard == proto::kHazardLava) {
					mcPerSecond = 8.0f;
					b.afterburn = 8.0f;
				} else if (hazard == proto::kHazardFire) {
					mcPerSecond = 2.0f;
					b.afterburn = std::max(b.afterburn, 4.0f);
				} else if (hazard == proto::kHazardMagma) {
					mcPerSecond = 1.0f;
				} else {
					b.afterburn -= kHazardTick;
				}
				const float scale = 5.0f + 0.25f * static_cast<float>(actor->GetLevel());
				actor->DoDamage(mcPerSecond * kHazardTick * scale, nullptr, true);
				// Flames on the body while they burn (not on magma: that only scorches feet).
				b.castCooldown -= kHazardTick;
				if (fireSpell && hazard != proto::kHazardMagma && b.castCooldown <= 0.0f && !actor->IsDead()) {
					if (auto* caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
						caster->CastSpellImmediate(fireSpell, false, actor, 1.0f, false, 0.0f, nullptr);
					}
					b.castCooldown = 1.5f;
				}
				if (hazard != proto::kHazardNone && !b.escaping && !actor->IsDead()) {
					b.escaping = FindEscape(actor, b.escapeTo);
				}
			}
			std::erase_if(burning, [](const auto& a_entry) { return a_entry.second.afterburn <= 0.0f && !a_entry.second.escaping; });
		}

		void LogNearbyCombat(RE::PlayerCharacter* a_player)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			const auto playerPos = a_player->GetPosition();
			int        hostiles = 0, fighting = 0, targeting = 0, attacking = 0;
			float      nearest = 1e9f;
			RE::Actor* closest = nullptr;
			for (auto& handle : lists->highActorHandles) {
				auto actor = handle.get();
				if (!actor || actor->IsDead() || !actor->IsHostileToActor(a_player)) {
					continue;
				}
				const float d = actor->GetPosition().GetDistance(playerPos);
				if (d > 4000.0f) {
					continue;
				}
				++hostiles;
				if (d < nearest) {
					nearest = d;
					closest = actor.get();
				}
				fighting += actor->IsInCombat() ? 1 : 0;
				targeting += actor->GetActorRuntimeData().currentCombatTarget.get().get() == a_player ? 1 : 0;
				attacking += actor->IsAttacking() ? 1 : 0;
			}
			if (!hostiles) {
				return;
			}
			auto* av = closest->AsActorValueOwner();
			logger::info("combat: {} hostile within 4000 units, {} in combat, {} targeting the player, {} attacking | nearest {} at {:.0f} (aggression {:.0f}, weapon {}) | "
						 "player weapon state {}",
				hostiles, fighting, targeting, attacking, closest->GetDisplayFullName(), nearest, av ? av->GetActorValue(RE::ActorValue::kAggression) : -1.0f,
				closest->AsActorState()->IsWeaponDrawn() ? "out" : "away", static_cast<int>(a_player->AsActorState()->GetWeaponState()));
			// Does its AI think it can reach the player, and where does it think the player is?
			if (auto* cc = closest->GetActorRuntimeData().combatController) {
				CombatView  view;
				const bool  ok = ReadCombatView(cc, a_player->GetHandle().native_handle(), playerPos, view);
				const auto* package = closest->GetCurrentPackage();
				logger::info("  {}: {} unreachable spots (nearest {:.0f} from the player) | player to its group: detect level {}, flags {}, {:.0f} from last known spot, "
							 "{} attackers | package {:08X} type {} | it moving {}, player moving {}, player midair {}{}",
					closest->GetDisplayFullName(), view.unreachableSpots, view.nearestSpot, view.detectLevel, view.targetFlags, view.lastKnownGap, view.attackers,
					package ? package->GetFormID() : 0u, package ? static_cast<int>(package->packData.packType.underlying()) : -1, closest->IsMoving(), a_player->IsMoving(),
					a_player->IsInMidair(), ok ? "" : " (read faulted)");
				// Skyrim's own gate on updating an NPC's combat controller (AE ID 33216) and each
				// of its checks on the NPC: whichever is "true" (or "off") is what stops it.
				if (REL::Module::IsAE()) {
					using GateFn = bool (*)(RE::CombatController*);
					using ActorFn = bool (*)(RE::Actor*);
					using ProcessFn = bool (*)(RE::AIProcess*);
					static const auto  gate = reinterpret_cast<GateFn>(REL::ID(33216).address());
					static const auto  knocked = reinterpret_cast<ActorFn>(REL::ID(37484).address());
					static const auto  fading = reinterpret_cast<ProcessFn>(REL::ID(39452).address());
					static const auto  talking = reinterpret_cast<ActorFn>(REL::ID(37199).address());
					static const auto* combatAi = reinterpret_cast<const std::uint8_t*>(REL::ID(380233).address());
					auto*              process = closest->GetActorRuntimeData().currentProcess;
					const auto         bits = closest->GetActorRuntimeData().boolBits;
					logger::info("  {} combat update allowed {} | combat AI on {}, controller flag41 {}, inactive {}, processMe {}, paralyzed {}, 3D {}, "
								 "knocked {}, fading {}, talking to player {}, started {} | player controls {:#x}",
						closest->GetDisplayFullName(), gate(cc), *combatAi, cc->unk41, cc->inactive, bits.all(RE::Actor::BOOL_BITS::kProcessMe),
						bits.all(RE::Actor::BOOL_BITS::kParalyzed), closest->Is3DLoaded(), knocked(closest), process ? fading(process) : false,
						talking(closest), cc->startedCombat,
						RE::ControlMap::GetSingleton() ? RE::ControlMap::GetSingleton()->GetRuntimeData().enabledControls.underlying() : 0u);
				}
			}
		}
	}

	namespace
	{
		// ---- Minecraft explosions in Skyrim's physics ------------------------------------------
		struct PendingExplosion
		{
			RE::NiPoint3 center;
			float        radius;  // Minecraft blocks
			float        delay;   // seconds: after the blast's damage has landed
			bool         sparesPeople{ false };  // a creeper's: Skyrim's people aren't thrown about by it (kEvExplosion flags bit 0)
		};
		std::vector<PendingExplosion> pendingExplosions;

		// People a blast caught. When one of them dies (Minecraft's damage lands a moment after the
		// blast) and Skyrim turns them into a ragdoll, the whole body is thrown away from it.
		struct PendingFling
		{
			RE::ActorHandle actor;
			RE::NiPoint3    center;
			float           speed;     // Havok metres per second
			float           timeLeft;  // seconds to wait for the ragdoll
		};
		std::vector<PendingFling> pendingFlings;

		// Every dynamic body of an actor's ragdoll gets the same velocity: out and up from the blast.
		bool FlingRagdoll(RE::Actor* a_actor, const RE::NiPoint3& a_center, float a_speed)
		{
			auto* root = a_actor->Get3D();
			auto* cell = a_actor->GetParentCell();
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!root || !world) {
				return false;
			}
			RE::NiPoint3 dir = a_actor->GetPosition() - a_center;
			dir.z = 0.0f;
			const float flat = dir.Length();
			dir = flat > 1.0f ? dir / flat : RE::NiPoint3{};
			dir.z = 0.9f;
			dir /= dir.Length();
			int bodies = 0;
			RE::BSWriteLockGuard lock(world->worldLock);
			RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_collision) {
				auto* rigid = a_collision->body ? netimmerse_cast<RE::bhkRigidBody*>(a_collision->body.get()) : nullptr;
				auto* body = rigid ? rigid->GetRigidBody() : nullptr;
				if (body) {
					using Motion = RE::hkpMotion::MotionType;
					const auto type = body->motion.type.get();
					if (type != Motion::kFixed && type != Motion::kKeyframed && type != Motion::kCharacter && type != Motion::kInvalid) {
						const float mass = body->motion.GetMass();
						body->ApplyLinearImpulse(RE::hkVector4(dir.x * a_speed * mass, dir.y * a_speed * mass, dir.z * a_speed * mass, 0.0f));
						++bodies;
					}
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return bodies > 0;
		}

		void UpdateFlings(float a_delta)
		{
			for (auto it = pendingFlings.begin(); it != pendingFlings.end();) {
				auto actor = it->actor.get();
				it->timeLeft -= a_delta;
				bool done = !actor || it->timeLeft <= 0.0f;
				if (actor && actor->IsDead() && FlingRagdoll(actor.get(), it->center, it->speed)) {
					logger::info("flung the body of {} ({:.1f} m/s)", actor->GetName(), it->speed);
					done = true;
				}
				it = done ? pendingFlings.erase(it) : it + 1;
			}
		}

		// People within reach are knocked away the way Skyrim's own explosions do it (the dead
		// ragdoll and fly); loose physics objects get a kick away from the blast.
		void ApplyExplosion(RE::PlayerCharacter* a_player, const PendingExplosion& a_blast)
		{
			const float reach = a_blast.radius * 2.0f * static_cast<float>(proto::kUnitsPerBlock);
			const float power = a_blast.radius / 4.0f;  // TNT = 1
			std::vector<RE::TESObjectREFR*> things;
			if (auto* tes = RE::TES::GetSingleton()) {
				tes->ForEachCell([&](RE::TESObjectCELL* a_cell) {
					a_cell->ForEachReferenceInRange(a_blast.center, reach, [&](RE::TESObjectREFR* a_ref) {
						if (a_ref && a_ref != a_player && !a_ref->IsDisabled() && !a_ref->IsDeleted()) {
							things.push_back(a_ref);
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				});
			}
			int people = 0, objects = 0;
			for (auto* ref : things) {
				const auto  pos = ref->GetPosition();
				const float dist = pos.GetDistance(a_blast.center);
				const float falloff = std::clamp(1.0f - dist / reach, 0.0f, 1.0f);
				if (falloff <= 0.0f) {
					continue;
				}
				if (auto* actor = ref->As<RE::Actor>()) {
					if (a_blast.sparesPeople) {
						continue;  // a creeper's blast doesn't touch Skyrim's NPCs: no knock, no fling (the damage is Minecraft's, and refused there too)
					}
					if (!actor->IsDead()) {
						if (auto* process = actor->GetActorRuntimeData().currentProcess) {
							process->KnockExplosion(actor, a_blast.center, 25.0f * falloff * power);
						}
					}
					// If the blast kills them (or already has), the body flies.
					pendingFlings.push_back({ actor->GetHandle(), a_blast.center, (6.0f + 12.0f * falloff) * power, 2.0f });
					++people;
					continue;
				}
				auto* root = ref->Get3D();
				auto* cell = ref->GetParentCell();
				auto* world = cell ? cell->GetbhkWorld() : nullptr;
				if (!root || !world) {
					continue;
				}
				RE::NiPoint3 dir = pos - a_blast.center;
				dir.z += 0.5f * dist + 30.0f;  // out and up
				const float len = dir.Length();
				if (len < 1e-3f) {
					continue;
				}
				dir /= len;
				const float speed = 14.0f * falloff * power;  // Havok metres per second
				bool        moved = false;
				RE::BSWriteLockGuard lock(world->worldLock);
				RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_collision) {
					auto* rigid = a_collision->body ? netimmerse_cast<RE::bhkRigidBody*>(a_collision->body.get()) : nullptr;
					auto* body = rigid ? rigid->GetRigidBody() : nullptr;
					if (body) {
						using Motion = RE::hkpMotion::MotionType;
						const auto type = body->motion.type.get();
						if (type != Motion::kFixed && type != Motion::kKeyframed && type != Motion::kCharacter && type != Motion::kInvalid) {
							const float mass = body->motion.GetMass();
							body->ApplyLinearImpulse(RE::hkVector4(dir.x * speed * mass, dir.y * speed * mass, dir.z * speed * mass, 0.0f));  // wakes it too
							moved = true;
						}
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				objects += moved ? 1 : 0;
			}
			logger::info("Minecraft explosion (radius {:.1f}): knocked {} people away, threw {} objects", a_blast.radius, people, objects);
		}

		// What Minecraft's world tells Skyrim, event by event. In Skyrim's own control mode only what happens
		// between the mobs and Skyrim's NPCs matters: the Minecraft player isn't fighting, only riding along.
		void HandleEvent(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev, bool a_skyrimMode)
		{
			switch (a_ev.type) {
			case proto::kEvHitActor:
				if (!a_skyrimMode || (a_ev.flags & proto::kHitFromMob) != 0) {
					ApplyHit(a_player, a_ev);
				}
				break;
			case proto::kEvPlayerDied:
				KillPlayer(a_player, a_ev);
				break;
			case proto::kEvExplosion:
				pendingExplosions.push_back({ McToSky(a_ev.a, a_ev.b, a_ev.c), a_ev.d, 0.25f, (a_ev.flags & 1u) != 0 });
				break;
			case proto::kEvArrowStuck:
				WorldRender::StickArrow(a_ev.formId, a_ev.a, a_ev.b, a_ev.c, a_ev.d, std::bit_cast<float>(a_ev.flags));
				break;
			case proto::kEvNpcSwing:
				NpcSwing(a_ev);
				break;
			case proto::kEvNpcStance:
				NpcStance(a_ev);
				break;
			case proto::kEvMobProxy:
				MobProxyEvent(a_player, a_ev);
				break;
			case proto::kEvMobName:
				MobNameEvent(a_ev);
				break;
			case proto::kEvMobVitals:
				MobVitalsEvent(a_player, a_ev);
				break;
			case proto::kEvSkillUse:
				if (!a_skyrimMode && a_ev.formId >= static_cast<std::uint32_t>(RE::ActorValue::kOneHanded) &&
					a_ev.formId <= static_cast<std::uint32_t>(RE::ActorValue::kEnchanting)) {
					TrainSkill(a_player, static_cast<RE::ActorValue>(a_ev.formId), a_ev.a);
				}
				break;
			default:
				break;
			}
		}

		// ---- the player's own attacks in Skyrim's control mode ---------------------------------------
		// Skyrim swings the sword and draws the bow; Minecraft never sees it. The attack button is noted by the
		// input sink (Input.cpp), and the per-frame update works out what kind of attack it was and tells
		// Minecraft (kInSkyrimSwing), which hits the mobs in front of the player.
		using Clock = std::chrono::steady_clock;
		struct AttackButton
		{
			bool             down;
			Clock::time_point at;
		};
		std::mutex                attackLock;
		std::vector<AttackButton> attackQueue;  // written by the input sink, read by the per-frame update

		struct HeldWeapon
		{
			bool  usable = true;   // a weapon or bare fists; not a spell, a staff or a torch
			bool  ranged = false;  // a bow or crossbow
			float damage = 2.0f;   // fists
		};

		HeldWeapon ReadHeldWeapon(RE::PlayerCharacter* a_player)
		{
			HeldWeapon held;
			if (RE::TESForm* form = RightHandItem(a_player)) {
				if (auto* weapon = form->As<RE::TESObjectWEAP>()) {
					held.damage = WeaponBaseDamage(weapon);
					held.ranged = WeaponIsRanged(weapon);
					held.usable = !WeaponIsStaff(weapon);
				} else {
					held.usable = false;  // a spell in the hand: not a blow on a mob
				}
			}
			// Skyrim's own skill makes the same weapon hit harder, as it does for a hit on a Skyrim enemy.
			auto*       values = a_player->AsActorValueOwner();
			const float skill = held.ranged ? values->GetActorValue(RE::ActorValue::kArchery) :
			                                  std::max(values->GetActorValue(RE::ActorValue::kOneHanded), values->GetActorValue(RE::ActorValue::kTwoHanded));
			held.damage *= 1.0f + std::clamp(skill, 0.0f, 100.0f) / 100.0f;
			return held;
		}

		constexpr float kPowerAttackHold = 0.45f;  // seconds the button is held before Skyrim's swing becomes a power attack
		constexpr float kSwingHitDelay = 0.30f;    // seconds from the press to the blade meeting what's in front
		constexpr float kPowerHitDelay = 0.25f;    // the same after a power attack begins
		constexpr float kBowDrawTime = 1.0f;       // seconds of holding the string for a full draw
		constexpr float kMinBowDraw = 0.25f;       // a quicker release than this isn't a shot

		struct AttackTracker
		{
			bool       holding = false;
			bool       powerSent = false;
			HeldWeapon weapon;
			Clock::time_point since{};
			bool       normalPending = false;
			Clock::time_point normalAt{};
			bool       powerPending = false;
			Clock::time_point powerAt{};
			float      pendingDamage = 0.0f;
		} attack;

		void ResetSkyrimAttacks()
		{
			{
				std::scoped_lock lock(attackLock);
				attackQueue.clear();
			}
			attack = AttackTracker{};
		}

		void SendSwing(RE::PlayerCharacter* a_player, proto::SwingKind a_kind, float a_skyrimDamage, float a_charge)
		{
			const int   charge = static_cast<int>(std::clamp(a_charge, 0.0f, 1.0f) * 100.0f);
			const float yaw = HeadingToMcYaw(a_player->data.angle.z);
			const float pitch = a_player->data.angle.x * (180.0f / kPi);
			Link::Get().PushInput(proto::kInSkyrimSwing, static_cast<std::uint16_t>(static_cast<int>(a_kind) | (charge << 8)), static_cast<std::int32_t>(a_skyrimDamage * 100.0f),
				static_cast<std::int32_t>(yaw * 100.0f), static_cast<std::int32_t>(pitch * 100.0f));
			static int logged = 0;
			if (logged++ < 20) {
				logger::info("Skyrim swing (kind {}, {:.1f} damage, charge {}%) sent to Minecraft", static_cast<int>(a_kind), a_skyrimDamage, charge);
			}
		}

		void UpdateSkyrimAttacks(RE::PlayerCharacter* a_player)
		{
			std::vector<AttackButton> presses;
			{
				std::scoped_lock lock(attackLock);
				presses.swap(attackQueue);
			}
			for (const auto& press : presses) {
				if (press.down) {
					// With the weapon away, the attack button only takes it out: there's no blow to pass on.
					attack.holding = false;
					if (a_player->IsDead() || !a_player->AsActorState()->IsWeaponDrawn()) {
						continue;
					}
					attack.weapon = ReadHeldWeapon(a_player);
					if (attack.weapon.usable) {
						attack.holding = true;
						attack.powerSent = false;
						attack.since = press.at;
					}
				} else if (attack.holding) {
					attack.holding = false;
					const float held = std::chrono::duration<float>(press.at - attack.since).count();
					if (attack.weapon.ranged) {
						if (held >= kMinBowDraw) {
							SendSwing(a_player, proto::kSwingArrow, attack.weapon.damage, held / kBowDrawTime);
						}
					} else if (!attack.powerSent) {
						attack.normalPending = true;
						attack.pendingDamage = attack.weapon.damage;
						attack.normalAt = std::max(press.at, attack.since + std::chrono::milliseconds(static_cast<int>(kSwingHitDelay * 1000.0f)));
					}
				}
			}
			const auto now = Clock::now();
			if (attack.holding && !attack.weapon.ranged && !attack.powerSent && std::chrono::duration<float>(now - attack.since).count() >= kPowerAttackHold) {
				attack.powerSent = true;  // held long enough: Skyrim has turned the swing into a power attack
				attack.powerPending = true;
				attack.pendingDamage = attack.weapon.damage;
				attack.powerAt = now + std::chrono::milliseconds(static_cast<int>(kPowerHitDelay * 1000.0f));
			}
			if (attack.normalPending && now >= attack.normalAt) {
				attack.normalPending = false;
				SendSwing(a_player, proto::kSwingMelee, attack.pendingDamage, 0.0f);
			}
			if (attack.powerPending && now >= attack.powerAt) {
				attack.powerPending = false;
				SendSwing(a_player, proto::kSwingPower, attack.pendingDamage * 1.8f, 0.0f);
			}
		}
	}

	namespace Combat
	{
		void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
			HandleHealthDamageHook::func = vtbl.write_vfunc(0x104, HandleHealthDamageHook::thunk);
			REL::Relocation<std::uintptr_t> characterVtbl{ RE::VTABLE_Character[0] };  // (the stand-ins are Characters; same slot as the player's)
			StandInDamageGuard::func = characterVtbl.write_vfunc(0x104, StandInDamageGuard::thunk);
			logger::info("Stand-in damage guard installed: Skyrim's hits can no longer knock a mob's stand-in down");
			if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
				events->AddEventSink<RE::TESHitEvent>(HitSink::Get());
				events->AddEventSink<RE::TESObjectLoadedEvent>(StandInEventLog<RE::TESObjectLoadedEvent>::Get("3D loaded (1) / unloaded (0)"));
				events->AddEventSink<RE::TESCellAttachDetachEvent>(StandInEventLog<RE::TESCellAttachDetachEvent>::Get("cell attach / detach"));
				events->AddEventSink<RE::TESCombatEvent>(StandInEventLog<RE::TESCombatEvent>::Get("combat state"));
				events->AddEventSink<RE::TESDeathEvent>(StandInEventLog<RE::TESDeathEvent>::Get("death"));
			}
			ResolveHitPipeline();
			logger::info("combat hooks installed");
		}

		void PerFrame(RE::PlayerCharacter* a_player, bool a_puppeting, float a_delta)
		{
			auto& link = Link::Get();
			if (!a_puppeting) {
				if (essentialSet) {
					SetEssential(a_player, false);
				}
				healthPrimed = false;
				engaged = false;
				// In Skyrim's own control mode Minecraft's world is still there around the player: its mobs and
				// Skyrim's NPCs fight each other in it, and the player's swings hit the mobs. (Minecraft's player
				// only rides along, so whatever happens to it isn't the Skyrim player's death.)
				const bool mirror = State().skyrimMode && State().worldVisible && link.McAlive();
				// Events are always drained, so stale hits don't land when control resumes.
				proto::McEvent ev;
				while (link.PopEvent(ev)) {
					if (ResourceBridge::HandleEvent(ev)) {
						continue;
					}
					if (ev.type == proto::kEvPlayerDied) {
						if (link.McAlive() && !State().skyrimMode) {
							KillPlayer(a_player, ev);
						}
					} else if (ev.type == proto::kEvPlayerHealth) {
						if (State().skyrimMode) {
							SetPlayerHealthFraction(a_player, ev.a);
						}
					} else if (ev.type == proto::kEvPlayerHurt) {
						if (State().skyrimMode) {
							ApplyPlayerHurt(a_player, ev);
						}
					} else if (mirror) {
						HandleEvent(a_player, ev, true);
					}
				}
				if (mirror) {
					WriteActorTable(a_player);
					UpdateFlings(a_delta);
					for (auto it = pendingExplosions.begin(); it != pendingExplosions.end();) {
						it->delay -= a_delta;
						if (it->delay <= 0.0f) {
							ApplyExplosion(a_player, *it);
							it = pendingExplosions.erase(it);
						} else {
							++it;
						}
					}
					UpdateSkyrimAttacks(a_player);
					UpdateNpcBows(a_delta);
					UpdateProxies(a_player, a_delta);
					mirrorOffFor = 0.0f;
				} else {
					pendingExplosions.clear();
					pendingFlings.clear();
					ResetSkyrimAttacks();
					// A Skyrim menu, a load, a stall in Minecraft (its heartbeat is only trusted for 3 s): none is a reason to drop a stand-in
					// whose mob is alive, in either mode. They go when Minecraft's world is really gone (Minecraft closed, or out of its
					// world) for a good while, or when the player has died (the game is about to reload).
					static float playerDeadFor = 0.0f;
					mirrorOffFor = (link.McAlive() && State().mcInWorld) ? 0.0f : mirrorOffFor + a_delta;
					playerDeadFor = a_player->IsDead() ? playerDeadFor + a_delta : 0.0f;
					if (mirrorOffFor > 30.0f || playerDeadFor > 6.0f) {
						ClearProxies();
					}
					ParkPool(a_player, a_delta);
					link.WriteActors(nullptr, 0);
				}
				return;
			}
			// Skyrim can't kill the player while Minecraft decides: one huge hit knocks them down
			// (essential) for a moment instead, and Minecraft's health does the rest.
			mirrorOffFor = 0.0f;
			SetEssential(a_player, true);
			WriteActorTable(a_player);
			engagedTimer -= a_delta;
			if (engagedTimer <= 0.0f) {
				engagedTimer = 0.25f;
				engaged = AnyoneFighting(a_player);
			}

			proto::McEvent ev;
			while (link.PopEvent(ev)) {
				if (ResourceBridge::HandleEvent(ev)) {
					continue;
				}
				HandleEvent(a_player, ev, false);
			}
			UpdateFlings(a_delta);
			UpdateNpcBows(a_delta);
			UpdateProxies(a_player, a_delta);
			for (auto it = pendingExplosions.begin(); it != pendingExplosions.end();) {
				it->delay -= a_delta;
				if (it->delay <= 0.0f) {
					ApplyExplosion(a_player, *it);
					it = pendingExplosions.erase(it);
				} else {
					++it;
				}
			}
			if (a_player->IsDead()) {
				return;
			}
			BridgePlayerDamage(a_player, a_delta);
			UpdateHazards(a_player, a_delta);

			diagTimer -= a_delta;
			if (diagTimer <= 0.0f && DiagnosticsEnabled()) {
				diagTimer = 5.0f;
				LogNearbyCombat(a_player);
			}
		}

		bool PlayerEngaged() { return engaged; }

		bool IsStandIn(RE::FormID a_id) { return proxyIds.contains(a_id); }
		void GetStandInIds(std::vector<std::uint32_t>& a_out) { a_out.assign(proxyIds.begin(), proxyIds.end()); }

		void NoteSkyrimAttackButton(bool a_down)
		{
			std::scoped_lock lock(attackLock);
			if (attackQueue.size() < 16) {
				attackQueue.push_back({ a_down, Clock::now() });
			}
		}
	}
}
