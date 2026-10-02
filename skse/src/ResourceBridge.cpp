#include "ResourceBridge.h"

#include <SimpleIni.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

namespace skycraft::ResourceBridge
{
	namespace
	{
		constexpr std::int32_t kMaxMinecraftPerRequest = 4096;
		constexpr char kMappingsPath[] = "Data/SKSE/Plugins/SkyCraftMappings.json";

		struct Settings { bool enabled{ true }; };

		struct Mapping
		{
			RE::FormID formId{ 0 };
			std::string name;
			std::string minecraftItem;
			std::uint32_t itemHash{ 0 };
			std::int32_t skyrimCount{ 1 };
			std::int32_t minecraftCount{ 1 };
		};

		struct Pending
		{
			RE::FormID formId{ 0 };
			std::string name;
			std::uint32_t itemHash{ 0 };
			std::int32_t sourceCount{ 0 };
			std::int32_t targetCount{ 0 };
			std::int32_t skyrimPerGroup{ 1 };
			std::int32_t minecraftPerGroup{ 1 };
		};

		Settings settings;
		std::unordered_map<RE::FormID, Mapping> mappings;
		std::unordered_map<std::uint32_t, Pending> pending;
		std::uint32_t nextRequestId = 1;

		std::uint32_t ItemHash(std::string_view a_id)
		{
			std::uint32_t hash = 0x811C9DC5u;
			for (const unsigned char ch : a_id) {
				hash ^= ch;
				hash *= 0x01000193u;
			}
			return hash;
		}

		bool ParseFormId(const nlohmann::json& a_value, RE::FormID& a_out)
		{
			try {
				if (a_value.is_number_unsigned()) {
					const auto value = a_value.get<std::uint64_t>();
					if (value <= 0xFFFFFFFFull) {
						a_out = static_cast<RE::FormID>(value);
						return true;
					}
					return false;
				}
				if (!a_value.is_string()) return false;
				auto text = a_value.get<std::string>();
				if (text.starts_with("0x") || text.starts_with("0X")) text.erase(0, 2);
				std::size_t used = 0;
				const auto value = std::stoull(text, &used, 16);
				if (used != text.size() || value > 0xFFFFFFFFull) return false;
				a_out = static_cast<RE::FormID>(value);
				return true;
			} catch (...) {
				return false;
			}
		}

		Settings LoadSettings()
		{
			Settings out;
			CSimpleIniA ini;
			ini.SetUnicode();
			ini.LoadFile("Data/SKSE/Plugins/SkyCraft.ini");
			out.enabled = ini.GetBoolValue("ProgressionBridge", "bEnabled", true);
			return out;
		}

		bool LoadMappings()
		{
			mappings.clear();
			std::ifstream in(kMappingsPath);
			if (!in) {
				logger::error("progression bridge: missing {}", kMappingsPath);
				return false;
			}
			nlohmann::json root;
			try { in >> root; }
			catch (const std::exception& e) {
				logger::error("progression bridge: couldn't parse {}: {}", kMappingsPath, e.what());
				return false;
			}
			if (!root.is_object() || !root.contains("mappings") || !root["mappings"].is_array()) {
				logger::error("progression bridge: {} needs a mappings array", kMappingsPath);
				return false;
			}

			std::size_t skipped = 0;
			for (const auto& entry : root["mappings"]) {
				try {
					if (!entry.is_object() || !entry.value("enabled", true)) continue;
					RE::FormID formId = 0;
					if (!entry.contains("skyrimFormId") || !ParseFormId(entry["skyrimFormId"], formId) || formId == 0)
						throw std::runtime_error("bad skyrimFormId");
					const auto minecraftItem = entry.value("minecraftItem", std::string{});
					if (minecraftItem.empty() || minecraftItem.find(':') == std::string::npos)
						throw std::runtime_error("bad minecraftItem");
					const auto skyrimCount = entry.value("skyrimCount", 1);
					const auto minecraftCount = entry.value("minecraftCount", 1);
					if (skyrimCount <= 0 || skyrimCount > 4096 || minecraftCount <= 0 || minecraftCount > kMaxMinecraftPerRequest)
						throw std::runtime_error("counts must be 1..4096");
					if (!RE::TESForm::LookupByID<RE::TESBoundObject>(formId))
						throw std::runtime_error("Skyrim form doesn't exist");
					if (mappings.contains(formId))
						throw std::runtime_error("duplicate skyrimFormId");

					Mapping mapping;
					mapping.formId = formId;
					mapping.name = entry.value("name", minecraftItem);
					mapping.minecraftItem = minecraftItem;
					mapping.itemHash = ItemHash(minecraftItem);
					mapping.skyrimCount = skyrimCount;
					mapping.minecraftCount = minecraftCount;
					mappings.emplace(formId, std::move(mapping));
				} catch (const std::exception& e) {
					++skipped;
					logger::warn("progression bridge: skipped mapping: {}", e.what());
				}
			}
			logger::info("progression bridge: loaded {} universal mapping(s) from {} ({} skipped)", mappings.size(), kMappingsPath, skipped);
			return !mappings.empty();
		}

		std::uint32_t NewRequestId()
		{
			for (;;) {
				auto id = nextRequestId++;
				if (nextRequestId == 0) nextRequestId = 1;
				if (id != 0 && !pending.contains(id)) return id;
			}
		}

		bool Queue(const Mapping& m, std::int32_t sourceCount, std::int32_t targetCount)
		{
			if (sourceCount <= 0 || targetCount <= 0 || !Link::Get().McAlive()) return false;
			const auto requestId = NewRequestId();
			if (!Link::Get().PushInput(proto::kInResourceTransfer, 0,
					static_cast<std::int32_t>(requestId), targetCount, static_cast<std::int32_t>(m.itemHash))) {
				logger::warn("progression bridge: input ring full; leaving Skyrim item untouched");
				return false;
			}
			pending.emplace(requestId, Pending{ m.formId, m.name, m.itemHash, sourceCount, targetCount, m.skyrimCount, m.minecraftCount });
			logger::info("progression bridge: requested {} x{} -> {} x{} (request {})",
				m.name, sourceCount, m.minecraftItem, targetCount, requestId);
			return true;
		}

		class ContainerSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			static ContainerSink* Get() { static ContainerSink sink; return &sink; }

			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESContainerChangedEvent* e,
				RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				if (!settings.enabled || !e || e->itemCount <= 0) return RE::BSEventNotifyControl::kContinue;
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player || e->newContainer != player->GetFormID()) return RE::BSEventNotifyControl::kContinue;
				const auto found = mappings.find(e->baseObj);
				if (found == mappings.end()) return RE::BSEventNotifyControl::kContinue;
				const auto& m = found->second;

				std::int32_t groups = e->itemCount / m.skyrimCount;
				if (groups <= 0) return RE::BSEventNotifyControl::kContinue;
				const auto maxGroups = std::max<std::int32_t>(1, kMaxMinecraftPerRequest / m.minecraftCount);
				while (groups > 0) {
					const auto chunkGroups = std::min(groups, maxGroups);
					if (!Queue(m, chunkGroups * m.skyrimCount, chunkGroups * m.minecraftCount)) break;
					groups -= chunkGroups;
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
		if (!LoadMappings()) {
			logger::warn("progression bridge: no valid mappings loaded; inventory conversion disabled");
			return;
		}
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::Get());
			logger::info("universal progression bridge installed");
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

	bool HandleEvent(const proto::McEvent& e)
	{
		if (e.type != proto::kEvResourceTransferAck) return false;
		const auto requestId = e.flags;
		const auto it = pending.find(requestId);
		if (it == pending.end()) {
			logger::warn("progression bridge: ignored unknown/stale acknowledgement {}", requestId);
			return true;
		}
		const Pending transfer = it->second;
		pending.erase(it);
		if (e.weapon != transfer.itemHash) {
			logger::warn("progression bridge: acknowledgement {} had the wrong item hash; Skyrim item kept", requestId);
			return true;
		}
		const auto accepted = std::clamp(static_cast<std::int32_t>(e.a), 0, transfer.targetCount);
		const auto completeGroups = accepted / transfer.minecraftPerGroup;
		const auto removeCount = std::min(transfer.sourceCount, completeGroups * transfer.skyrimPerGroup);
		if (removeCount <= 0) {
			logger::warn("progression bridge: Minecraft declined request {}; Skyrim item kept", requestId);
			return true;
		}
		if (accepted % transfer.minecraftPerGroup != 0)
			logger::warn("progression bridge: request {} accepted a partial ratio; removing only complete groups", requestId);

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(transfer.formId);
		if (!player || !item) {
			logger::warn("progression bridge: couldn't remove acknowledged Skyrim item for request {}", requestId);
			return true;
		}
		player->RemoveItem(item, removeCount, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
		logger::info("progression bridge: converted {} x{} into Minecraft x{} (request {})",
			transfer.name, removeCount, accepted, requestId);
		return true;
	}
}
