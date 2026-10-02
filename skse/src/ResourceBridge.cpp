#include "ResourceBridge.h"

#include <SimpleIni.h>
#include <fmt/format.h>
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
			bool protectQuestItems{ true };
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
			bool protectQuestItems{ true };
		};

		Settings settings;
		std::unordered_map<RE::FormID, Mapping> mappings;
		std::unordered_map<std::uint32_t, Pending> pending;

		// Source items that were acquired while the bridge was running but did not yet make a
		// complete ratio group. They remain physically in Skyrim; this is only eligibility
		// bookkeeping, so there is no save-data mutation and no hidden inventory.
		std::unordered_map<RE::FormID, std::int64_t> carry;

		std::shared_ptr<spdlog::logger> progressionLog;
		std::uint32_t nextRequestId = 1;

		void SetupProgressionLog()
		{
			auto dir = SKSE::log::log_directory();
			if (!dir) {
				return;
			}
			try {
				auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "SkyCraftProgression.log").string(), true);
				progressionLog = std::make_shared<spdlog::logger>("skycraft_progression", std::move(sink));
				progressionLog->set_level(spdlog::level::info);
				progressionLog->flush_on(spdlog::level::info);
				progressionLog->set_pattern("[%H:%M:%S.%e] [%l] %v");
			} catch (const std::exception& e) {
				logger::warn("progression bridge: couldn't create dedicated log: {}", e.what());
			}
		}

		template <class... Args>
		void BridgeInfo(fmt::format_string<Args...> a_format, Args&&... a_args)
		{
			const auto message = fmt::format(a_format, std::forward<Args>(a_args)...);
			logger::info("{}", message);
			if (progressionLog) {
				progressionLog->info("{}", message);
			}
		}

		template <class... Args>
		void BridgeWarn(fmt::format_string<Args...> a_format, Args&&... a_args)
		{
			const auto message = fmt::format(a_format, std::forward<Args>(a_args)...);
			logger::warn("{}", message);
			if (progressionLog) {
				progressionLog->warn("{}", message);
			}
		}

		template <class... Args>
		void BridgeError(fmt::format_string<Args...> a_format, Args&&... a_args)
		{
			const auto message = fmt::format(a_format, std::forward<Args>(a_args)...);
			logger::error("{}", message);
			if (progressionLog) {
				progressionLog->error("{}", message);
			}
		}

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
				if (!a_value.is_string()) {
					return false;
				}
				auto text = a_value.get<std::string>();
				if (text.starts_with("0x") || text.starts_with("0X")) {
					text.erase(0, 2);
				}
				std::size_t used = 0;
				const auto value = std::stoull(text, &used, 16);
				if (used != text.size() || value > 0xFFFFFFFFull) {
					return false;
				}
				a_out = static_cast<RE::FormID>(value);
				return true;
			} catch (...) {
				return false;
			}
		}


		RE::TESBoundObject* ResolveMappedObject(const nlohmann::json& a_entry, RE::FormID& a_formId, std::string& a_source)
		{
			const bool hasPlugin = a_entry.contains("skyrimPlugin");
			const bool hasLocal = a_entry.contains("skyrimLocalFormId");
			if (hasPlugin || hasLocal) {
				if (!hasPlugin || !hasLocal || !a_entry["skyrimPlugin"].is_string()) {
					throw std::runtime_error("plugin-aware mapping needs both skyrimPlugin and skyrimLocalFormId");
				}
				const auto plugin = a_entry["skyrimPlugin"].get<std::string>();
				RE::FormID localId = 0;
				if (plugin.empty() || !ParseFormId(a_entry["skyrimLocalFormId"], localId)) {
					throw std::runtime_error("bad plugin-aware form reference");
				}
				auto* data = RE::TESDataHandler::GetSingleton();
				auto* object = data ? data->LookupForm<RE::TESBoundObject>(localId, plugin) : nullptr;
				if (!object) {
					throw std::runtime_error(fmt::format("couldn't resolve {:08X} from {}", localId, plugin));
				}
				a_formId = object->GetFormID();
				a_source = fmt::format("{}:{:08X}", plugin, localId);
				return object;
			}

			if (!a_entry.contains("skyrimFormId") || !ParseFormId(a_entry["skyrimFormId"], a_formId) || a_formId == 0) {
				throw std::runtime_error("bad skyrimFormId");
			}
			auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(a_formId);
			if (!object) {
				throw std::runtime_error("Skyrim form doesn't exist");
			}
			a_source = fmt::format("{:08X}", a_formId);
			return object;
		}

		bool IsQuestProtected(RE::PlayerCharacter* a_player, RE::TESBoundObject* a_item)
		{
			if (!a_player || !a_item) {
				return false;
			}
			auto inventory = a_player->GetInventory([a_item](RE::TESBoundObject& a_object) {
				return &a_object == a_item;
			});
			const auto found = inventory.find(a_item);
			if (found == inventory.end() || !found->second.second) {
				return false;
			}
			return found->second.second->IsQuestObject();
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
				BridgeError("progression bridge: missing {}", kMappingsPath);
				return false;
			}

			nlohmann::json root;
			try {
				in >> root;
			} catch (const std::exception& e) {
				BridgeError("progression bridge: couldn't parse {}: {}", kMappingsPath, e.what());
				return false;
			}
			if (!root.is_object() || !root.contains("mappings") || !root["mappings"].is_array()) {
				BridgeError("progression bridge: {} needs a mappings array", kMappingsPath);
				return false;
			}

			std::size_t skipped = 0;
			for (const auto& entry : root["mappings"]) {
				try {
					if (!entry.is_object() || !entry.value("enabled", true)) {
						continue;
					}
					RE::FormID formId = 0;
					std::string formSource;
					auto* boundObject = ResolveMappedObject(entry, formId, formSource);
					const auto minecraftItem = entry.value("minecraftItem", std::string{});
					if (minecraftItem.empty() || minecraftItem.find(':') == std::string::npos) {
						throw std::runtime_error("bad minecraftItem");
					}
					const auto skyrimCount = entry.value("skyrimCount", 1);
					const auto minecraftCount = entry.value("minecraftCount", 1);
					if (skyrimCount <= 0 || skyrimCount > 4096 || minecraftCount <= 0 || minecraftCount > kMaxMinecraftPerRequest) {
						throw std::runtime_error("counts must be 1..4096");
					}
					if (mappings.contains(formId)) {
						throw std::runtime_error("duplicate skyrimFormId");
					}

					Mapping mapping;
					mapping.formId = formId;
					mapping.name = entry.value("name", minecraftItem);
					mapping.minecraftItem = minecraftItem;
					mapping.itemHash = ItemHash(minecraftItem);
					mapping.skyrimCount = skyrimCount;
					mapping.minecraftCount = minecraftCount;
					mapping.protectQuestItems = entry.value("protectQuestItems", true);
					mappings.emplace(formId, std::move(mapping));
				} catch (const std::exception& e) {
					++skipped;
					BridgeWarn("progression bridge: skipped mapping: {}", e.what());
				}
			}

			BridgeInfo("progression bridge: loaded {} universal mapping(s) from {} ({} skipped)",
				mappings.size(), kMappingsPath, skipped);
			return !mappings.empty();
		}

		std::uint32_t NewRequestId()
		{
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

		std::int64_t ReservedSourceCount(RE::FormID a_formId)
		{
			std::int64_t total = 0;
			for (const auto& [id, transfer] : pending) {
				(void)id;
				if (transfer.formId == a_formId) {
					total += transfer.sourceCount;
				}
			}
			return total;
		}

		void CapCarryToInventory(RE::FormID a_formId)
		{
			auto it = carry.find(a_formId);
			if (it == carry.end()) {
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(a_formId);
			if (!player || !item) {
				it->second = 0;
				return;
			}
			const auto actual = static_cast<std::int64_t>(std::max(0, player->GetItemCount(item)));
			const auto unreserved = std::max<std::int64_t>(0, actual - ReservedSourceCount(a_formId));
			it->second = std::clamp<std::int64_t>(it->second, 0, unreserved);
			if (it->second == 0) {
				carry.erase(it);
			}
		}

		void RestoreCarry(const Pending& a_transfer, std::int32_t a_sourceCount)
		{
			if (a_sourceCount <= 0) {
				return;
			}
			carry[a_transfer.formId] += a_sourceCount;
			CapCarryToInventory(a_transfer.formId);
		}

		bool Queue(const Mapping& a_mapping, std::int32_t a_sourceCount, std::int32_t a_targetCount)
		{
			if (a_sourceCount <= 0 || a_targetCount <= 0 || !Link::Get().McAlive()) {
				return false;
			}

			const auto requestId = NewRequestId();
			if (!Link::Get().PushInput(
					proto::kInResourceTransfer,
					0,
					static_cast<std::int32_t>(requestId),
					a_targetCount,
					static_cast<std::int32_t>(a_mapping.itemHash))) {
				BridgeWarn("progression bridge: input ring full; leaving Skyrim item untouched");
				return false;
			}

			pending.emplace(requestId, Pending{
				a_mapping.formId,
				a_mapping.name,
				a_mapping.itemHash,
				a_sourceCount,
				a_targetCount,
				a_mapping.skyrimCount,
				a_mapping.minecraftCount,
				a_mapping.protectQuestItems
			});
			BridgeInfo("progression bridge: requested {} x{} -> {} x{} (request {})",
				a_mapping.name, a_sourceCount, a_mapping.minecraftItem, a_targetCount, requestId);
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

				const auto found = mappings.find(a_event->baseObj);
				if (found == mappings.end()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto& mapping = found->second;
				auto* mappedItem = RE::TESForm::LookupByID<RE::TESBoundObject>(mapping.formId);
				if (mapping.protectQuestItems && IsQuestProtected(player, mappedItem)) {
					BridgeInfo("progression bridge: kept quest-protected {} in Skyrim", mapping.name);
					return RE::BSEventNotifyControl::kContinue;
				}

				// Every newly acquired mapped item becomes eligible. Incomplete ratio groups stay
				// physically in Skyrim and carry across later pickups in this run.
				auto& eligible = carry[mapping.formId];
				eligible += a_event->itemCount;
				CapCarryToInventory(mapping.formId);

				auto carryIt = carry.find(mapping.formId);
				if (carryIt == carry.end()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				std::int64_t groups = carryIt->second / mapping.skyrimCount;
				if (groups <= 0) {
					BridgeInfo("progression bridge: carrying {} x{} toward {}:{} ratio",
						mapping.name, carryIt->second, mapping.skyrimCount, mapping.minecraftCount);
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto maxGroups = std::max<std::int32_t>(1, kMaxMinecraftPerRequest / mapping.minecraftCount);
				while (groups > 0) {
					const auto chunkGroups = static_cast<std::int32_t>(std::min<std::int64_t>(groups, maxGroups));
					const auto sourceCount = chunkGroups * mapping.skyrimCount;
					const auto targetCount = chunkGroups * mapping.minecraftCount;

					if (!Queue(mapping, sourceCount, targetCount)) {
						break;
					}

					auto current = carry.find(mapping.formId);
					if (current != carry.end()) {
						current->second -= sourceCount;
						if (current->second <= 0) {
							carry.erase(current);
						}
					}
					groups -= chunkGroups;
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		SetupProgressionLog();
		BridgeInfo("SkyCraft progression 0.1.2-progression.5 starting");

		settings = LoadSettings();
		if (!settings.enabled) {
			BridgeInfo("progression bridge disabled by SkyCraft.ini");
			return;
		}
		if (!LoadMappings()) {
			BridgeWarn("progression bridge: no valid mappings loaded; inventory conversion disabled");
			return;
		}

		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::Get());
			BridgeInfo("universal progression bridge installed");
		} else {
			BridgeWarn("progression bridge: Skyrim event source unavailable");
		}
	}

	void OnGameLoaded()
	{
		if (!pending.empty()) {
			BridgeInfo("progression bridge: clearing {} in-flight request(s) after load", pending.size());
			pending.clear();
		}
		if (!carry.empty()) {
			BridgeInfo("progression bridge: clearing {} ratio carry entrie(s) after load; source items remain in Skyrim", carry.size());
			carry.clear();
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
			BridgeWarn("progression bridge: ignored unknown/stale acknowledgement {}", requestId);
			return true;
		}

		const Pending transfer = it->second;
		pending.erase(it);

		if (a_event.weapon != transfer.itemHash) {
			RestoreCarry(transfer, transfer.sourceCount);
			BridgeWarn("progression bridge: acknowledgement {} had the wrong item hash; Skyrim item kept", requestId);
			return true;
		}

		const auto accepted = std::clamp(static_cast<std::int32_t>(a_event.a), 0, transfer.targetCount);
		const auto completeGroups = accepted / transfer.minecraftPerGroup;
		const auto removeCount = std::min(transfer.sourceCount, completeGroups * transfer.skyrimPerGroup);
		if (removeCount <= 0) {
			RestoreCarry(transfer, transfer.sourceCount);
			BridgeWarn("progression bridge: Minecraft declined request {}; Skyrim item kept", requestId);
			return true;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(transfer.formId);
		if (!player || !item) {
			RestoreCarry(transfer, transfer.sourceCount);
			BridgeWarn("progression bridge: couldn't remove acknowledged Skyrim item for request {}", requestId);
			return true;
		}

		if (transfer.protectQuestItems && IsQuestProtected(player, item)) {
			RestoreCarry(transfer, transfer.sourceCount);
			BridgeWarn("progression bridge: request {} became quest-protected before acknowledgement; Skyrim source kept", requestId);
			return true;
		}

		const auto actual = std::max(0, player->GetItemCount(item));
		const auto safeRemove = std::min(removeCount, actual);
		if (safeRemove <= 0) {
			BridgeWarn("progression bridge: request {} was granted by Minecraft but the Skyrim source item is no longer present", requestId);
			return true;
		}
		if (safeRemove != removeCount) {
			BridgeWarn("progression bridge: request {} source count changed before acknowledgement; removing {} of expected {}",
				requestId, safeRemove, removeCount);
		}

		player->RemoveItem(item, safeRemove, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);

		const auto unconsumedSource = transfer.sourceCount - safeRemove;
		if (unconsumedSource > 0) {
			RestoreCarry(transfer, unconsumedSource);
		}
		if (accepted % transfer.minecraftPerGroup != 0) {
			BridgeWarn("progression bridge: request {} accepted a partial ratio; only complete groups were removed", requestId);
		}

		BridgeInfo("progression bridge: converted {} x{} into Minecraft x{} (request {})",
			transfer.name, safeRemove, accepted, requestId);
		return true;
	}
}
