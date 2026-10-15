#include "Game.h"
#include "CollisionNif.h"
#include "Compat.h"

#include <SimpleIni.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

// Minecraft's mobs as things that physically exist in Skyrim's world. Each mob gets a body: an invisible static reference whose model is
// nothing but one collision box the size of the mob's own bounding box (CollisionNif.h, the writer the block collision uses). Skyrim's
// player, NPCs, arrows and spells meet it as they meet a wall, and it follows the mob. A mob's box is axis-aligned in Minecraft, so the body
// never turns.
//
// It replaces the invisible creature (a rabbit scaled to fit) that used to stand in for each mob. That was an actor, which Skyrim's AI
// and engine treat as its own: it was switched off, unloaded and revived at will, and its shape was a rabbit's however it was scaled.
//
// The library of boxes, like the block shape library:
//   * Minecraft writes skycraft/mob_catalog.json the first time its world starts (MobCatalog.java): every distinct width and height among
//     all registered entity types, modded ones too, each also as a baby. At plugin load a model is made for every size in it into
//     Data/Meshes/SkyCraft/MobLibrary/mob_w<w>_h<h>.nif (sixteenths of a block), which Skyrim has from its start.
//   * A size that is not in the library (no catalogue yet on the very first start, a slime of an odd size, a mod's growing mob) is written
//     while the game runs over the next unused of the spare files made at plugin load (Skyrim loads only model files that existed when it
//     started), once per size, and used for every mob of that size after.
namespace skycraft
{
	namespace
	{
		struct BodyCfg
		{
			bool        on{ true };
			std::string catalog;  // path of mob_catalog.json (empty: look where the Minecraft that comes with SkyCraft writes it)
			int         maxBodies{ 64 };
			bool        skeleton{ true };  // each stand-in actor gets a skeleton of our own, a box, instead of the stock creature's
			bool        actors{ true };  // the stand-in actors are kept too, for Skyrim's NPCs to fight
		};

		const BodyCfg& Cfg()
		{
			static const BodyCfg settings = [] {
				CSimpleIniA ini;
				ini.SetUnicode();
				ini.LoadFile("Data/SKSE/Plugins/SkyCraft.ini");
				BodyCfg s;
				s.on = ini.GetBoolValue("Mobs", "bBoxBodies", true);
				s.catalog = ini.GetValue("Mobs", "sMobCatalog", "");
				s.skeleton = ini.GetBoolValue("Mobs", "bCustomSkeleton", true);
				s.actors = ini.GetBoolValue("Mobs", "bStandInActors", true);
				s.maxBodies = static_cast<int>(std::clamp<long>(ini.GetLongValue("Mobs", "iMaxBodies", 64), 4, 400));
				return s;
			}();
			return settings;
		}

		const std::filesystem::path kLibDir = "Data/Meshes/SkyCraft/MobLibrary";
		const std::filesystem::path kSlotDir = "Data/Meshes/SkyCraft/MobSlots";
		constexpr const char*       kLibModelDir = "SkyCraft\\MobLibrary\\";
		constexpr const char*       kSlotModelDir = "SkyCraft\\MobSlots\\";
		const std::filesystem::path kSkelDir = "Data/Meshes/SkyCraft/MobActors";
		constexpr const char*       kSkelModelDir = "SkyCraft\\MobActors\\";
		const std::filesystem::path kSkelMarker = "Data/SKSE/Plugins/SkyCraft/custom_skeleton.marker";
		std::unordered_set<std::string> skelKeys;  // sizes whose skeleton file was made at plugin load
		int                         skelSlotNext = 0;
		bool                        skelSlotsReady = false;
		bool                        skelOn = false;  // decided at plugin load
		constexpr int               kSlots = 200;
		constexpr int               kMaxWidth16 = 255;
		constexpr int               kMaxHeight16 = 400;

		int Sixteenths(float a_blocks, int a_max)
		{
			return std::clamp(static_cast<int>(std::lround(a_blocks * 16.0f)), 1, a_max);
		}

		// The same key as MobCatalog.key in the Fabric mod: "w<width>_h<height>", both in sixteenths of a block.
		std::string SizeKey(int a_w16, int a_h16)
		{
			return fmt::format("w{}_h{}", a_w16, a_h16);
		}

		std::vector<std::uint8_t> BoxBytes(int a_w16, int a_h16)
		{
			const float u = float(proto::kUnitsPerBlock);
			const float half = a_w16 * u / 32.0f;
			collisionnif::Box box;  // the model's origin is at the mob's feet, in the middle of its box
			box.lo[0] = -half;
			box.lo[1] = -half;
			box.lo[2] = 0.0f;
			box.hi[0] = half;
			box.hi[1] = half;
			box.hi[2] = a_h16 * u / 16.0f;
			return collisionnif::Build({ box }, collisionnif::kMaterialStone, 0.01f);
		}

		// The actor skeleton for a mob size (revision 48): the box as before, but as a skeleton Skyrim's actor code can use, not a static. Its root
		// is "NPC Root [Root]" with "NPC COM [COM ]" (middle of the box) and "NPC Head [Head]" (near the top, where lock-on and head tracking
		// aim) under it, and its body is on the BIPED layer, keyframed, so Skyrim's swings, arrows and spells treat it as the actor's body.
		std::vector<std::uint8_t> SkeletonBytes(int a_w16, int a_h16)
		{
			const float u = float(proto::kUnitsPerBlock);
			const float half = a_w16 * u / 32.0f;
			collisionnif::Box box;
			box.lo[0] = -half;
			box.lo[1] = -half;
			box.lo[2] = 0.0f;
			box.hi[0] = half;
			box.hi[1] = half;
			box.hi[2] = a_h16 * u / 16.0f;
			collisionnif::Options opt;
			opt.actorNodes = true;
			opt.layer = 8;
			opt.motion = 6;
			return collisionnif::Build({ box }, collisionnif::kMaterialStone, 0.01f, opt);
		}

		bool WriteBytes(const std::filesystem::path& a_path, const std::vector<std::uint8_t>& a_bytes)
		{
			std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
			out.write(reinterpret_cast<const char*>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
			return out.good();
		}

		std::unordered_set<std::string>                     libraryKeys;  // sizes whose model was made at plugin load
		std::unordered_map<std::string, RE::TESObjectSTAT*> forms;        // size key -> its base form (library or slot model)
		int                                                 slotNext = 0;
		bool                                                slotsReady = false;
		bool                                                broken = false;

		struct Body
		{
			RE::FormID   refId{ 0 };
			std::string  key;
			RE::NiPoint3 pos{};    // where Minecraft last said the mob is
			RE::NiPoint3 shown{};  // where the body is now
			float        velX{ 0.0f }, velZ{ 0.0f };
			float        width{ 0.0f }, height{ 0.0f };  // blocks, as Minecraft last said (0: not told)
			float        silence{ 0.0f };
			float        age{ 0.0f };
			bool         checked{ false };
		};
		std::unordered_map<std::int32_t, Body> bodies;
		std::unordered_map<std::int32_t, float> widthSeen;  // mob id -> its width as last told (vitals come before the first proxy event)

		template <class R>
		void DeleteRef(R* a_ref)
		{
			if (!a_ref) {
				return;
			}
			if constexpr (requires { a_ref->Disable(); }) {
				a_ref->Disable();
			}
			if constexpr (requires { a_ref->SetDelete(true); }) {
				a_ref->SetDelete(true);
			}
		}

		template <class R>
		void Place(R* a_ref, const RE::NiPoint3& a_pos)
		{
			if constexpr (requires { a_ref->SetPosition(a_pos, true); }) {
				a_ref->SetPosition(a_pos, true);
			} else if constexpr (requires { a_ref->SetPosition(a_pos); }) {
				a_ref->SetPosition(a_pos);
			}
			if constexpr (requires { a_ref->Update3DPosition(true); }) {
				a_ref->Update3DPosition(true);
			}
		}

		RE::TESObjectSTAT* MakeStatic(const std::string& a_modelPath, float a_sizeUnits)
		{
			auto* form = RE::IFormFactory::Create<RE::TESObjectSTAT>();
			if (!form) {
				return nullptr;
			}
			if (form->GetFormID() == 0) {
				form->SetFormID(0, true);
			}
			if (form->GetFormID() == 0) {
				return nullptr;
			}
			static_cast<RE::TESModel*>(form)->SetModel(a_modelPath.c_str());
			const auto s = static_cast<std::int16_t>(std::clamp(a_sizeUnits + 40.0f, 1.0f, 32000.0f));
			form->boundData.boundMin.x = static_cast<std::int16_t>(-s);
			form->boundData.boundMin.y = static_cast<std::int16_t>(-s);
			form->boundData.boundMin.z = -20;
			form->boundData.boundMax.x = s;
			form->boundData.boundMax.y = s;
			form->boundData.boundMax.z = s;
			return form;
		}

		// The base form for a size: its library model if it has one, else the next spare file written with the box (once per size).
		RE::TESObjectSTAT* FormFor(int a_w16, int a_h16)
		{
			const auto key = SizeKey(a_w16, a_h16);
			if (const auto it = forms.find(key); it != forms.end()) {
				return it->second;
			}
			const float        size = std::max(a_w16 * 0.5f, float(a_h16)) * float(proto::kUnitsPerBlock) / 16.0f;
			RE::TESObjectSTAT* form = nullptr;
			if (libraryKeys.contains(key)) {
				form = MakeStatic(std::string(kLibModelDir) + "mob_" + key + ".nif", size);
			} else if (slotsReady && slotNext < kSlots) {
				const auto file = fmt::format("slot{:04d}.nif", slotNext++);
				if (WriteBytes(kSlotDir / file, BoxBytes(a_w16, a_h16))) {
					form = MakeStatic(std::string(kSlotModelDir) + file, size);
					logger::info("Mob bodies: size {} is not in the library; made from spare model {} ({} of {} used)", key, file, slotNext, kSlots);
				}
			} else {
				static bool said = false;
				if (!said) {
					said = true;
					logger::warn("Mob bodies: size {} is not in the library and the spare models are used up; mobs of such sizes have no body until the game is restarted", key);
				}
			}
			forms[key] = form;
			return form;
		}

		void RemoveBody(std::int32_t a_mobId)
		{
			const auto it = bodies.find(a_mobId);
			if (it == bodies.end()) {
				return;
			}
			DeleteRef(RE::TESForm::LookupByID<RE::TESObjectREFR>(it->second.refId));
			bodies.erase(it);
		}

		bool MakeBody(RE::PlayerCharacter* a_player, Body& a_body)
		{
			auto* handler = RE::TESDataHandler::GetSingleton();
			auto* cell = a_player->GetParentCell();
			if (!handler || !cell) {
				return false;
			}
			const int w16 = Sixteenths(a_body.width, kMaxWidth16);
			const int h16 = Sixteenths(a_body.height, kMaxHeight16);
			auto*     form = FormFor(w16, h16);
			if (!form) {
				return false;
			}
			RE::NiPointer<RE::TESObjectREFR> ref;
			ref = handler->CreateReferenceAtLocation(form, a_body.pos, RE::NiPoint3(), cell, a_player->GetWorldspace(), nullptr, nullptr, RE::ObjectRefHandle(), false, true).get();
			if (!ref) {
				return false;
			}
			ref->SetActivationBlocked(true);
			a_body.refId = ref->GetFormID();
			a_body.key = SizeKey(w16, h16);
			a_body.shown = a_body.pos;
			return true;
		}

		// ---- Round-one probe for the custom stand-in actors (see below): what Skyrim and this CommonLib accept, written to SkyCraft.log. ----
		// A custom stand-in is an actor made at runtime for each mob size: its own NPC and race records, a skeleton of our own with the mob's
		// box as its collision. Nothing in that is known to work on this install yet, so before it is built the probe asks, one step at a time:
		//   1. how Skyrim's own creature skeletons are laid out (the block types in the file: where an actor's collision is kept, and in what shape)
		//   2. whether an NPC record and a race record can be made at runtime and copied from a stock creature's
		//   3. whether an actor made from the copies gets a 3D, and what kind of character controller it has
		// A marker file is written before step 2 and removed after step 3: if Skyrim crashes in between, the marker is still there at the next
		// start, and the probe doesn't run again (the log says so).
		const std::filesystem::path kProbeMarker = "Data/SKSE/Plugins/SkyCraft/mob_actor_probe.marker";

		template <class S>
		bool ProbeRead(S& a_stream, std::vector<std::uint8_t>& a_out, std::size_t a_count)
		{
			if (a_count > (1u << 20)) {
				return false;
			}
			const auto at = a_out.size();
			a_out.resize(at + a_count);
			if (a_count == 0) {
				return true;
			}
			if constexpr (requires { a_stream.read(static_cast<char*>(nullptr), std::uint32_t(0)); }) {
				a_stream.read(reinterpret_cast<char*>(a_out.data() + at), static_cast<std::uint32_t>(a_count));
				return a_stream.good();
			} else {
				return false;
			}
		}

		// Logs the block types (with counts) of a .nif in Skyrim's files, from its header alone.
		void DumpBlockTypes(const std::string& a_path)
		{
			RE::BSResourceNiBinaryStream stream(a_path);
			if (!stream.good()) {
				logger::info("Mob actor probe: {} can't be opened", a_path);
				return;
			}
			std::vector<std::uint8_t> b;
			auto                      u32at = [&](std::size_t a_at) {
                std::uint32_t v = 0;
                std::memcpy(&v, b.data() + a_at, 4);
                return v;
			};
			for (int i = 0; i < 80; ++i) {
				if (!ProbeRead(stream, b, 1)) {
					return;
				}
				if (b.back() == '\n') {
					break;
				}
			}
			std::size_t p = b.size();
			if (!ProbeRead(stream, b, 4 + 1 + 4 + 4 + 4)) {
				return;
			}
			const std::uint32_t blocks = u32at(p + 9);
			for (int i = 0; i < 3; ++i) {
				if (!ProbeRead(stream, b, 1) || !ProbeRead(stream, b, b.back())) {
					return;
				}
			}
			p = b.size();
			if (blocks > 100000 || !ProbeRead(stream, b, 2)) {
				return;
			}
			std::uint16_t types = 0;
			std::memcpy(&types, b.data() + p, 2);
			std::vector<std::string> names;
			for (std::uint16_t i = 0; i < types; ++i) {
				p = b.size();
				if (!ProbeRead(stream, b, 4)) {
					return;
				}
				const auto len = u32at(p);
				p = b.size();
				if (!ProbeRead(stream, b, len)) {
					return;
				}
				names.emplace_back(reinterpret_cast<const char*>(b.data() + p), len);
			}
			p = b.size();
			if (!ProbeRead(stream, b, std::size_t(blocks) * 2)) {
				return;
			}
			std::vector<int> count(names.size(), 0);
			for (std::uint32_t i = 0; i < blocks; ++i) {
				std::uint16_t t = 0;
				std::memcpy(&t, b.data() + p + std::size_t(i) * 2, 2);
				if (t < count.size()) {
					++count[t];
				}
			}
			std::string line;
			for (std::size_t i = 0; i < names.size(); ++i) {
				line += fmt::format("{}{} x{}", i ? ", " : "", names[i], count[i]);
			}
			logger::info("Mob actor probe: {} has {} blocks: {}", a_path, blocks, line);
		}

		enum class ProbeState
		{
			kIdle,
			kPlaced,
			kDone
		};
		ProbeState probeState = ProbeState::kIdle;
		RE::FormID probeRef = 0;
		float      probeWait = 0.0f;

		template <class D>
		RE::TESNPC* ProbeRabbit(D* a_data)
		{
			if constexpr (requires { a_data->template GetFormArray<RE::TESNPC>(); }) {
				for (auto* npc : a_data->template GetFormArray<RE::TESNPC>()) {
					if (npc && npc->GetName() && std::string_view(npc->GetName()) == "Rabbit") {
						return npc;
					}
				}
			}
			return nullptr;
		}

		void FinishProbe(const char* a_why)
		{
			logger::info("Mob actor probe: finished ({})", a_why);
			std::error_code ec;
			std::filesystem::remove(kProbeMarker, ec);
			probeState = ProbeState::kDone;
		}

		void RunProbe(RE::PlayerCharacter* a_player, float a_delta)
		{
			// Crash guard: experimental NPC/race cloning must be explicitly enabled.
			if (!Cfg().actors || !Cfg().skeleton) return;
			if (probeState == ProbeState::kDone) {
				return;
			}
			if (probeState == ProbeState::kPlaced) {
				probeWait += a_delta;
				if (probeWait < 2.0f) {
					return;
				}
				if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(probeRef)) {
					logger::info("Mob actor probe: the actor made from the copies has {} 3D", ref->Get3D() ? "a" : "NO");
					if (auto* actor = ref->template As<RE::Actor>()) {
						if constexpr (requires { actor->GetCharController(); }) {
							auto* controller = actor->GetCharController();
							logger::info("Mob actor probe: character controller: {}{}", controller ? "present" : "none",
								skyrim_cast<RE::bhkCharProxyController*>(controller) ? ", a proxy controller (capsule)" : ", not a proxy controller");
						}
					}
					DeleteRef(ref);
				}
				FinishProbe("the copies work as far as placing an actor");
				return;
			}
			if (!a_player || !a_player->Is3DLoaded()) {
				return;
			}
			std::error_code ec;
			if (std::filesystem::exists(kProbeMarker, ec)) {
				logger::warn("Mob actor probe: not run: the previous one didn't finish (Skyrim probably crashed in it); delete {} to try again", kProbeMarker.string());
				probeState = ProbeState::kDone;
				return;
			}
			DumpBlockTypes("Meshes\\Actors\\Rabbit\\Character Assets\\skeleton.nif");
			DumpBlockTypes("Meshes\\Actors\\Wolf\\Character Assets\\skeleton.nif");
			DumpBlockTypes("Meshes\\Actors\\Character\\Character Assets\\skeleton.nif");
			std::filesystem::create_directories(kProbeMarker.parent_path(), ec);
			std::ofstream(kProbeMarker) << "mob actor probe running\n";
			auto* data = RE::TESDataHandler::GetSingleton();
			auto* rabbit = data ? ProbeRabbit(data) : nullptr;
			if (!rabbit) {
				FinishProbe("no stock rabbit to copy");
				return;
			}
			bool  copied = false;
			auto* npc = compat::CloneForm(rabbit, copied);
			if (!npc) {
				FinishProbe("an NPC record can't be made at runtime");
				return;
			}
			logger::info("Mob actor probe: an NPC record was made at runtime ({:08X})", npc->GetFormID());
			logger::info("Mob actor probe: copying the rabbit's NPC record into it: {}", copied ? "done" : "the engine made no duplicate");
			if constexpr (requires { rabbit->race; }) {
				auto* stockRace = rabbit->race;
				logger::info("Mob actor probe: the rabbit's race is {:08X}", stockRace ? stockRace->GetFormID() : 0u);
				if (stockRace) {
					bool  raceCopied = false;
					auto* race = compat::CloneForm(stockRace, raceCopied);
					logger::info("Mob actor probe: a race record {} at runtime", race ? "was made" : "can't be made");
					if (race) {
						if (raceCopied) {
							logger::info("Mob actor probe: copied the rabbit's race into it");
						}
						if constexpr (requires { npc->race = race; }) {
							npc->race = race;
						}
					}
				}
			}
			if (!copied) {
				FinishProbe("no copy, so no actor is made");
				return;
			}
			RE::NiPointer<RE::TESObjectREFR> ref;
			if constexpr (requires { a_player->PlaceObjectAtMe(npc, false); }) {
				ref = a_player->PlaceObjectAtMe(npc, false);
			}
			if (!ref) {
				FinishProbe("an actor can't be placed from the copies");
				return;
			}
			if (auto* actor = ref->template As<RE::Actor>()) {
				if constexpr (requires { actor->SetAlpha(0.0f); }) {
					actor->SetAlpha(0.0f);
				}
			}
			probeRef = ref->GetFormID();
			probeWait = 0.0f;
			probeState = ProbeState::kPlaced;
		}
	}

	namespace MobBodies
	{
		bool Enabled()
		{
			return Cfg().on && !broken;
		}

		// Skyrim's combat AI can only fight an actor, so the stand-in actors are kept alongside the bodies unless this is off.
		bool KeepActors()
		{
			return Cfg().actors;
		}

		// Reads mob_catalog.json (written by the Fabric side: MobCatalog.java) and writes a model for every size in it, then makes the spare
		// files for sizes it didn't list.
		void PrepareFiles()
		{
			if (!Cfg().on) {
				return;
			}
			const auto started = std::chrono::steady_clock::now();
			std::error_code ec;
			std::filesystem::remove_all(kSlotDir, ec);
			std::filesystem::create_directories(kSlotDir, ec);
			std::filesystem::create_directories(kLibDir, ec);
			if (!std::filesystem::is_directory(kSlotDir) || !std::filesystem::is_directory(kLibDir)) {
				logger::warn("Mob bodies: can't make the folders under Data/Meshes/SkyCraft; no mob bodies");
				broken = true;
				return;
			}
			int spare = 0;
			for (int i = 0; i < kSlots; ++i) {
				spare += WriteBytes(kSlotDir / fmt::format("slot{:04d}.nif", i), BoxBytes(8, 8)) ? 1 : 0;
			}
			slotsReady = spare == kSlots;

			// Skeletons of our own for the stand-in actors: the same box as the body, in a file that is the actor's skeleton (see Combat.cpp).
			std::filesystem::create_directories(kSkelDir, ec);
			skelOn = Cfg().skeleton && std::filesystem::is_directory(kSkelDir);
			if (skelOn && std::filesystem::exists(kSkelMarker, ec)) {
				skelOn = false;
				logger::warn("Custom skeletons are off: the last time one was tried it never got a 3D (Skyrim probably crashed or the actor failed); delete {} to try again", kSkelMarker.string());
			}
			if (skelOn) {
				int skelSpare = 0;
				for (int i = 0; i < kSlots; ++i) {
					skelSpare += WriteBytes(kSkelDir / fmt::format("slot{:04d}.nif", i), SkeletonBytes(8, 8)) ? 1 : 0;
				}
				skelSlotsReady = skelSpare == kSlots;
			}

			std::vector<std::filesystem::path> candidates;
			if (!Cfg().catalog.empty()) {
				candidates.emplace_back(Cfg().catalog);
			} else {
				if (const char* local = std::getenv("LOCALAPPDATA")) {
					candidates.push_back(std::filesystem::path(local) / "SkyCraft" / "Prism" / "instances" / "SkyCraft" / ".minecraft" / "skycraft" / "mob_catalog.json");
				}
				candidates.emplace_back("Data/SKSE/Plugins/SkyCraft/mob_catalog.json");
			}
			std::string text;
			std::filesystem::path used;
			for (const auto& c : candidates) {
				std::ifstream in(c, std::ios::binary);
				if (in) {
					text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
					used = c;
					break;
				}
			}
			int         made = 0;
			std::size_t pos = 0;
			while (!text.empty() && (pos = text.find("{\"w\": ", pos)) != std::string::npos) {
				pos += 6;
				char*     end = nullptr;
				const int w = static_cast<int>(std::strtol(text.c_str() + pos, &end, 10));
				const auto hAt = text.find("\"h\": ", pos);
				if (hAt == std::string::npos) {
					break;
				}
				const int h = static_cast<int>(std::strtol(text.c_str() + hAt + 5, &end, 10));
				if (w < 1 || w > kMaxWidth16 || h < 1 || h > kMaxHeight16) {
					continue;
				}
				const auto key = SizeKey(w, h);
				if (!libraryKeys.contains(key) && WriteBytes(kLibDir / ("mob_" + key + ".nif"), BoxBytes(w, h))) {
					libraryKeys.insert(key);
					++made;
				}
				if (skelOn && !skelKeys.contains(key) && WriteBytes(kSkelDir / ("sk_" + key + ".nif"), SkeletonBytes(w, h))) {
					skelKeys.insert(key);
				}
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
			if (text.empty()) {
				logger::info("Mob bodies: no mob catalogue yet ({} places tried); Minecraft writes it the first time its world starts. Until the next start every size is made from the {} spare models ({} ms)", candidates.size(), spare, ms);
			} else {
				logger::info("Mob bodies: library of {} body sizes in {} from {}; {} spare models for any other size ({} ms)", made, std::filesystem::absolute(kLibDir, ec).string(), used.string(), spare, ms);
			}
		}

		// A mob's body is made (or moved) from kEvMobProxy: its position and yaw, and its height in the weapon field.
		void OnProxy(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev)
		{
			const auto mobId = static_cast<std::int32_t>(a_ev.formId);
			if ((a_ev.flags & proto::kMobProxyGone) != 0) {
				RemoveBody(mobId);
				widthSeen.erase(mobId);
				return;
			}
			const auto pos = McToSky(a_ev.a, a_ev.b, a_ev.c);
			auto       it = bodies.find(mobId);
			if (it == bodies.end()) {
				if (static_cast<int>(bodies.size()) >= Cfg().maxBodies || a_player->IsDead()) {
					return;
				}
				it = bodies.emplace(mobId, Body{}).first;
				if (const auto w = widthSeen.find(mobId); w != widthSeen.end()) {
					it->second.width = w->second;
				}
			}
			auto& body = it->second;
			body.pos = pos;
			body.silence = 0.0f;
			if (const float height = std::bit_cast<float>(a_ev.weapon); height > 0.0f && height < 64.0f) {
				body.height = height;
			}
			if (body.refId == 0 && body.width > 0.0f && body.height > 0.0f) {
				if (!MakeBody(a_player, body)) {
					bodies.erase(it);
				}
			}
		}

		// The body's width (d) comes with the mob's vitals, so a body is made once both it and the height are known; a mob that changes size
		// (a slime that splits, a mob that grows) gets a body of its new size.
		void OnVitals(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev)
		{
			if (a_ev.d > 0.0f && a_ev.d < 64.0f) {
				widthSeen[static_cast<std::int32_t>(a_ev.formId)] = a_ev.d;
			}
			const auto it = bodies.find(static_cast<std::int32_t>(a_ev.formId));
			if (it == bodies.end()) {
				return;
			}
			auto& body = it->second;
			body.velX = std::bit_cast<float>(a_ev.flags);
			body.velZ = std::bit_cast<float>(a_ev.weapon);
			if (a_ev.d > 0.0f && a_ev.d < 64.0f) {
				const bool changed = body.refId != 0 && SizeKey(Sixteenths(a_ev.d, kMaxWidth16), Sixteenths(body.height, kMaxHeight16)) != body.key;
				body.width = a_ev.d;
				if (changed) {
					DeleteRef(RE::TESForm::LookupByID<RE::TESObjectREFR>(body.refId));
					body.refId = 0;
				}
				if (body.refId == 0 && body.height > 0.0f && !MakeBody(a_player, body)) {
					bodies.erase(it);
				}
			}
		}

		// Main thread, every frame: bodies follow their mobs (carried on by the mob's velocity between reports), and a body whose mob has been
		// silent for long goes.
		void Update(RE::PlayerCharacter* a_player, float a_delta)
		{
			RunProbe(a_player, a_delta);
			for (auto it = bodies.begin(); it != bodies.end();) {
				auto& body = it->second;
				if (Link::Get().McAlive()) {
					body.silence += std::min(a_delta, 0.1f);
				}
				body.age += a_delta;
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(body.refId);
				if (body.silence > 10.0f || (body.refId != 0 && !ref)) {
					DeleteRef(ref);
					it = bodies.erase(it);
					continue;
				}
				if (ref) {
					const float        carry = std::min(body.silence, 0.2f);
					const RE::NiPoint3 want(body.pos.x + body.velX * carry * float(proto::kUnitsPerBlock), body.pos.y - body.velZ * carry * float(proto::kUnitsPerBlock), body.pos.z);
					const float        dx = want.x - body.shown.x, dy = want.y - body.shown.y, dz = want.z - body.shown.z;
					if (dx * dx + dy * dy + dz * dz > 0.25f) {
						body.shown = want;
						Place(ref, want);
					}
					if (!body.checked && body.age > 1.5f) {
						body.checked = true;
						static int logged = 0;
						if (logged++ < 8) {
							logger::info("Mob body ({:08X}, {}): 3D {}", body.refId, body.key, ref->Get3D() ? "loaded" : "NOT loaded");
						}
					}
				}
				++it;
			}
		}

		bool CustomSkeletonOn()
		{
			return Enabled() && skelOn;
		}

		// The skeleton model (relative to Data\\Meshes) for a size key, or empty: its file from the library, else the next spare file written with the box.
		std::string SkeletonModel(const std::string& a_key, int a_w16, int a_h16)
		{
			if (!skelOn) {
				return {};
			}
			if (skelKeys.contains(a_key)) {
				return std::string(kSkelModelDir) + "sk_" + a_key + ".nif";
			}
			if (skelSlotsReady && skelSlotNext < kSlots) {
				const auto file = fmt::format("slot{:04d}.nif", skelSlotNext++);
				if (WriteBytes(kSkelDir / file, SkeletonBytes(a_w16, a_h16))) {
					return std::string(kSkelModelDir) + file;
				}
			}
			return {};
		}

		bool ParseKey(const std::string& a_key, int& a_w16, int& a_h16)
		{
			return std::sscanf(a_key.c_str(), "w%d_h%d", &a_w16, &a_h16) == 2 && a_w16 > 0 && a_h16 > 0;
		}

		// The first actor with a custom skeleton is a trial: the marker stays until it has a 3D, and a start that finds it doesn't try again.
		void ArmSkeletonTrial()
		{
			std::error_code ec;
			std::filesystem::create_directories(kSkelMarker.parent_path(), ec);
			std::ofstream(kSkelMarker) << "custom skeleton trial running\n";
		}

		void SkeletonTrialPassed()
		{
			std::error_code ec;
			std::filesystem::remove(kSkelMarker, ec);
		}

		void SkeletonTrialFailed()
		{
			skelOn = false;
		}

		bool AnyBodies()
		{
			return !bodies.empty();
		}

		// The mob a body belongs to (for diagnostics), or -1.
		std::int32_t MobOfRef(RE::FormID a_refId)
		{
			for (const auto& [id, body] : bodies) {
				if (body.refId == a_refId) {
					return id;
				}
			}
			return -1;
		}

		// The size key ("w<width>_h<height>", sixteenths of a block) of a mob whose width has been reported; empty if it hasn't.
		std::string KeyFor(std::int32_t a_mobId, float a_height)
		{
			const auto w = widthSeen.find(a_mobId);
			if (w == widthSeen.end() || !(a_height > 0.0f) || a_height >= 64.0f) {
				return {};
			}
			return SizeKey(Sixteenths(w->second, kMaxWidth16), Sixteenths(a_height, kMaxHeight16));
		}

		void Clear()
		{
			widthSeen.clear();
			while (!bodies.empty()) {
				RemoveBody(bodies.begin()->first);
			}
		}
	}
}
