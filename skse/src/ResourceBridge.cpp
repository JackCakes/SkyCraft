#include "ResourceBridge.h"

#include <SimpleIni.h>

namespace skycraft::ResourceBridge
{
	namespace
	{
		constexpr RE::FormID kIronIngot = 0x0005ACE4;
		constexpr RE::FormID kGoldIngot = 0x0005AD9E;
		constexpr RE::FormID kLeather = 0x000DB5D2;
		constexpr RE::FormID kWheat = 0x0004B0BA;
		constexpr RE::FormID kIronOre = 0x00071CF3;
		constexpr RE::FormID kGoldOre = 0x0005ACDE;
		constexpr std::int32_t kMaxPerRequest = 4096;

		struct Settings
		{
			bool enabled{ true };
			bool iron{ true };
			bool gold{ true };
			bool leather{ true };
			bool wheat{ true };
			bool ironOre{ true };
			bool goldOre{ true };
		};

		struct Pending
		{
			RE::FormID             formId{ 0 };
			proto::ResourceKind    kind{ proto::kResourceIronIngot };
			std::int32_t           count{ 0 };
		};

		Settings settings;
		std::unordered_map<std::uint32_t, Pending> pending;
		std::uint32_t nextRequestId = 1;

		Settings LoadSettings()
		{
			Settings out;
			CSimpleIniA ini;
			ini.SetUnicode();
			ini.LoadFile("Data/SKSE/Plugins/SkyCraft.ini");
			out.enabled = ini.GetBoolValue("ProgressionBridge", "bEnabled", true);
			out.iron = ini.GetBoolValue("ProgressionBridge", "bIronIngots", true);
			out.gold = ini.GetBoolValue("ProgressionBridge", "bGoldIngots", true);
			out.leather = ini.GetBoolValue("ProgressionBridge", "bLeather", true);
			out.wheat = ini.GetBoolValue("ProgressionBridge", "bWheat", true);
			out.ironOre = ini.GetBoolValue("ProgressionBridge", "bIronOre", true);
			out.goldOre = ini.GetBoolValue("ProgressionBridge", "bGoldOre", true);
			return out;
		}

		std::optional<proto::ResourceKind> KindFor(RE::FormID a_formId)
		{
			switch (a_formId) {
			case kIronIngot:
				return settings.iron ? std::optional{ proto::kResourceIronIngot } : std::nullopt;
			case kGoldIngot:
				return settings.gold ? std::optional{ proto::kResourceGoldIngot } : std::nullopt;
			case kLeather:
				return settings.leather ? std::optional{ proto::kResourceLeather } : std::nullopt;
			case kWheat:
				return settings.wheat ? std::optional{ proto::kResourceWheat } : std::nullopt;
			case kIronOre:
				return settings.ironOre ? std::optional{ proto::kResourceIronOre } : std::nullopt;
			case kGoldOre:
				return settings.goldOre ? std::optional{ proto::kResourceGoldOre } : std::nullopt;
			default:
				return std::nullopt;
			}
		}

		const char* KindName(proto::ResourceKind a_kind)
		{
			switch (a_kind) {
			case proto::kResourceIronIngot:
				return "iron ingot";
			case proto::kResourceGoldIngot:
				return "gold ingot";
			case proto::kResourceLeather:
				return "leather";
			case proto::kResourceWheat:
				return "wheat";
			case proto::kResourceIronOre:
				return "iron ore";
			case proto::kResourceGoldOre:
				return "gold ore";
			default:
				return "resource";
			}
		}

		std::uint32_t NewRequestId()
		{
			// Zero is kept as "not a request". Pending requests are tiny and short-lived, so skipping
			// an id already in the map is enough even when this eventually wraps.
			for (;;) {
				auto id = nextRequestId++;
				if (nextRequestId == 0) {
					nextRequestId = 1;
				}
				if (id != 0 && !pending.contains(id)) {
					return id;
				}
			}
		}

		bool Queue(RE::FormID a_formId, proto::ResourceKind a_kind, std::int32_t a_count)
		{
			if (a_count <= 0 || !Link::Get().McAlive()) {
				return false;
			}
			const auto requestId = NewRequestId();
			if (!Link::Get().PushInput(
					proto::kInResourceTransfer,
					static_cast<std::uint16_t>(a_kind),
					static_cast<std::int32_t>(requestId),
					a_count,
					static_cast<std::int32_t>(a_formId))) {
				logger::warn("progression bridge: input ring full; leaving Skyrim item untouched");
				return false;
			}
			pending.emplace(requestId, Pending{ a_formId, a_kind, a_count });
			logger::info("progression bridge: requested {} x{} -> Minecraft (request {})", KindName(a_kind), a_count, requestId);
			return true;
		}

		class ContainerSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			static ContainerSink* Get()
			{
				static ContainerSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESContainerChangedEvent* a_event,
				RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				if (!settings.enabled || !a_event || a_event->itemCount <= 0) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player || a_event->newContainer != player->GetFormID()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto kind = KindFor(a_event->baseObj);
				if (!kind) {
					return RE::BSEventNotifyControl::kContinue;
				}

				// Keep requests bounded. The item stays in Skyrim until Minecraft acknowledges each
				// chunk, so a failed/full ring never destroys the source item.
				std::int32_t remaining = a_event->itemCount;
				while (remaining > 0) {
					const auto chunk = std::min(remaining, kMaxPerRequest);
					if (!Queue(a_event->baseObj, *kind, chunk)) {
						break;
					}
					remaining -= chunk;
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		settings = LoadSettings();
		if (!settings.enabled) {
			logger::info("progression bridge disabled by SkyCraft.ini");
			return;
		}
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::Get());
			logger::info("progression bridge installed (iron {}, gold {}, leather {}, wheat {}, iron ore {}, gold ore {})",
				settings.iron ? "on" : "off", settings.gold ? "on" : "off", settings.leather ? "on" : "off",
				settings.wheat ? "on" : "off", settings.ironOre ? "on" : "off", settings.goldOre ? "on" : "off");
		} else {
			logger::warn("progression bridge: Skyrim event source unavailable");
		}
	}

	void OnGameLoaded()
	{
		if (!pending.empty()) {
			logger::info("progression bridge: clearing {} in-flight request(s) after load", pending.size());
			pending.clear();
		}
	}

	bool HandleEvent(const proto::McEvent& a_event)
	{
		if (a_event.type != proto::kEvResourceTransferAck) {
			return false;
		}

		const auto requestId = a_event.flags;
		const auto it = pending.find(requestId);
		if (it == pending.end()) {
			logger::warn("progression bridge: ignored unknown/stale acknowledgement {}", requestId);
			return true;
		}
		const Pending transfer = it->second;
		pending.erase(it);

		if (a_event.formId != transfer.formId || a_event.weapon != static_cast<std::uint32_t>(transfer.kind)) {
			logger::warn("progression bridge: acknowledgement {} did not match its request; Skyrim item kept", requestId);
			return true;
		}

		const auto accepted = std::clamp(static_cast<std::int32_t>(a_event.a), 0, transfer.count);
		if (accepted <= 0) {
			logger::warn("progression bridge: Minecraft declined request {}; Skyrim item kept", requestId);
			return true;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(transfer.formId);
		if (!player || !item) {
			logger::warn("progression bridge: couldn't remove acknowledged Skyrim item for request {}", requestId);
			return true;
		}

		player->RemoveItem(item, accepted, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
		logger::info("progression bridge: converted {} x{} into Minecraft (request {})", KindName(transfer.kind), accepted, requestId);
		return true;
	}
}
