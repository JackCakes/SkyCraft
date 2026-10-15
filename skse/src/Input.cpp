#include "Game.h"

#include <SimpleIni.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>

namespace skycraft
{
	namespace
	{
		// DirectInput scan code (what Skyrim reports) -> SDL scancode / USB HID usage (what MC uses).
		constexpr auto kDikToSdl = [] {
			std::array<std::uint16_t, 256> t{};
			t[0x01] = 41;  // Esc
			for (int i = 0; i < 9; ++i) t[0x02 + i] = static_cast<std::uint16_t>(30 + i);  // 1-9
			t[0x0B] = 39;  // 0
			t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;  // - = Backspace Tab
			t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;  // Q W E R T
			t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;  // Y U I O P
			t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;               // [ ] Enter LCtrl
			t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;     // A S D F G
			t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;                // H J K L
			t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;  // ; ' ` LShift backslash
			t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;    // Z X C V B
			t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;  // N M , . /
			t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;  // RShift KP* LAlt Space Caps
			for (int i = 0; i < 10; ++i) t[0x3B + i] = static_cast<std::uint16_t>(58 + i);  // F1-F10
			t[0x45] = 83, t[0x46] = 71;                                             // NumLock ScrollLock
			t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;                 // KP7 KP8 KP9 KP-
			t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;                 // KP4 KP5 KP6 KP+
			t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;   // KP1 KP2 KP3 KP0 KP.
			t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;                              // OEM102 F11 F12
			t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;  // KPEnter RCtrl KP/ PrtSc RAlt
			t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;  // Pause Home Up PgUp Left
			t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;  // Right End Down PgDn Insert
			t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;             // Delete LWin RWin Menu
			return t;
		}();

		// Keys Skyrim keeps while Minecraft drives the player. Everything else is Minecraft's.
		constexpr std::uint32_t kDikEscape = 0x01;   // Skyrim journal / system menu (or closes an MC screen)
		constexpr std::uint32_t kDikConsole = 0x29;  // Skyrim console
		constexpr std::uint32_t kDikG = 0x22;        // Skyrim activate: doors, NPCs, containers, items
		constexpr std::uint32_t kDikH = 0x23;        // Skyrim wait (T is Minecraft chat)
		constexpr std::uint32_t kDikJ = 0x24;        // Skyrim quest journal
		constexpr std::uint32_t kDikM = 0x32;        // Skyrim map
		constexpr std::uint32_t kDikO = 0x18;        // Minecraft pause / options menu (Esc is Skyrim's)
		constexpr std::uint32_t kDikF9 = 0x43;       // Skyrim quickload

		bool IsSkyrimMenuKey(std::uint32_t a_code)
		{
			return a_code == kDikEscape || a_code == kDikConsole || a_code == kDikJ || a_code == kDikM || a_code == kDikF9;
		}

		// The key that switches between Minecraft's controls and Skyrim's (SkyCraft.ini [Controls]
		// sToggleKey). It's SkyCraft's in both modes: never sent to Minecraft, and in Minecraft mode
		// Skyrim never sees it either.
		constexpr std::uint32_t kDikF8 = 0x42;
		std::uint32_t           toggleKey = kDikF8;
		std::string             toggleKeyName = "F8";

		struct NamedKey
		{
			const char*   name;
			std::uint32_t dik;
		};
		constexpr NamedKey kNamedKeys[] = {
			{ "F1", 0x3B }, { "F2", 0x3C }, { "F3", 0x3D }, { "F4", 0x3E }, { "F5", 0x3F }, { "F6", 0x40 },
			{ "F7", 0x41 }, { "F8", 0x42 }, { "F9", 0x43 }, { "F10", 0x44 }, { "F11", 0x57 }, { "F12", 0x58 },
			{ "SCROLLLOCK", 0x46 }, { "PAUSE", 0xC5 }, { "HOME", 0xC7 }, { "PAGEUP", 0xC9 },
			{ "END", 0xCF }, { "PAGEDOWN", 0xD1 }, { "INSERT", 0xD2 }, { "DELETE", 0xD3 },
		};

		// A key name from the list above (any case), or a DirectInput scan code (decimal or 0x hex).
		// 0 when it isn't usable: unknown, or one of the keys SkyCraft already routes (Esc, ~, G, H, J, M,
		// O, F9), since the toggle would swallow it.
		std::uint32_t ParseToggleKey(const std::string& a_text)
		{
			std::string s;
			for (const char c : a_text) {
				if (!std::isspace(static_cast<unsigned char>(c))) {
					s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
				}
			}
			std::uint32_t code = 0;
			for (const auto& key : kNamedKeys) {
				if (s == key.name) {
					code = key.dik;
				}
			}
			if (code == 0 && !s.empty()) {
				char*      end = nullptr;
				const auto number = std::strtoul(s.c_str(), &end, 0);
				if (end && *end == '\0' && number > 0 && number <= 0xFF) {
					code = static_cast<std::uint32_t>(number);
				}
			}
			if (IsSkyrimMenuKey(code) || code == kDikG || code == kDikH || code == kDikO) {
				return 0;
			}
			return code;
		}

		// Whether the keyboard is Minecraft's right now: one of its screens is up (chat, commands, an
		// anvil's name box, the inventory's search, ...), or one closed a moment ago. Everything typed
		// then is Minecraft's, and none of it may reach Skyrim, or the letters of a command open
		// Skyrim's map, journal and the like.
		//
		// The moment after a screen closes counts too: the key that closed it (Esc, Enter) is released
		// after the screen is gone, and that release must not open Skyrim's menu either, and Minecraft's
		// own report of its screens reaches Skyrim a frame late, so a screen's last keys arrive early.
		constexpr auto kScreenGrace = std::chrono::milliseconds(200);
		bool           screenWasOpen = false;
		std::chrono::steady_clock::time_point screenClosedAt{};
		// Keys whose press went to Minecraft while it had the keyboard. Their release is Minecraft's too,
		// even if the screen has closed by then (the hook below drops it on its way to Skyrim's menus).
		std::array<bool, 256> heldByMinecraft{};

		bool MinecraftHasKeyboard()
		{
			if (State().mcScreenOpen) {
				screenWasOpen = true;
				return true;
			}
			const auto now = std::chrono::steady_clock::now();
			if (screenWasOpen) {
				screenWasOpen = false;
				screenClosedAt = now;
			}
			return now - screenClosedAt < kScreenGrace;
		}

		// The toggle key was pressed: swap Minecraft's controls and Skyrim's. This only flips the
		// switch (and tells Minecraft to let go of what it holds); the per-frame update does the
		// rest, and says which mode it is now.
		void SwitchControls(bool a_skyrimMenuOpen)
		{
			if (a_skyrimMenuOpen) {
				return;  // a Skyrim menu has all the input
			}
			auto& st = State();
			if (st.worldReset != 0) {
				// Skyrim's controls are what holds the player while Minecraft's world is away.
				RE::SendHUDMessage::ShowHUDMessage("SkyCraft: wait for the world reset to finish.");
				return;
			}
			const bool toSkyrim = !st.skyrimMode;
			// A Minecraft screen (inventory, chat, ...) can't be seen or used in Skyrim's mode. Going back
			// is always allowed, so a screen Minecraft opened by itself can't lock the player out.
			if (toSkyrim && st.mcScreenOpen) {
				RE::SendHUDMessage::ShowHUDMessage("SkyCraft: close the Minecraft screen first.");
				return;
			}
			st.skyrimMode = toSkyrim;
			Input::ReleaseAll();
			logger::info("controls: {}", toSkyrim ? "Skyrim's" : "Minecraft's");
		}

		// The key that throws Minecraft's world away (SkyCraft.ini [Controls] sResetKey): every block
		// built or dug, everywhere, so nothing is left floating where Skyrim has moved on. It takes two
		// presses, so a slip of the finger doesn't cost the player their builds. 0: switched off.
		constexpr std::uint32_t kDikF7 = 0x41;
		std::uint32_t           resetKey = kDikF7;
		std::string             resetKeyName = "F7";
		constexpr auto          kResetConfirmWindow = std::chrono::seconds(6);
		std::chrono::steady_clock::time_point resetAskedAt{};

		// The reset key was pressed: the first press asks, a second one within a few seconds does it.
		// This only tells Minecraft (and remembers it did); the per-frame update takes the player to
		// Skyrim's controls when Minecraft says it's on it, and back once the new world is there.
		void AskWorldReset(bool a_skyrimMenuOpen)
		{
			if (a_skyrimMenuOpen) {
				return;  // a Skyrim menu has all the input
			}
			auto& st = State();
			if (st.worldReset != 0) {
				RE::SendHUDMessage::ShowHUDMessage("SkyCraft: the world is already being reset.");
				return;
			}
			if (!st.mcInWorld) {
				RE::SendHUDMessage::ShowHUDMessage("SkyCraft: Minecraft isn't in its world yet.");
				return;
			}
			const auto now = std::chrono::steady_clock::now();
			if (resetAskedAt == std::chrono::steady_clock::time_point{} || now - resetAskedAt > kResetConfirmWindow) {
				resetAskedAt = now;
				const std::string note = "SkyCraft: press " + resetKeyName + " again to reset the Minecraft world. Everything built or dug is removed.";
				RE::SendHUDMessage::ShowHUDMessage(note.c_str());
				return;
			}
			resetAskedAt = {};
			st.worldReset = 1;
			Input::ReleaseAll();
			Link::Get().PushInput(proto::kInResetWorld, 0);
			logger::info("world reset: asked Minecraft to start over");
		}

		float lookDx = 0.0f;
		float lookDy = 0.0f;

		// G on something Skyrim can activate (door, NPC, container, item, furniture) activates it in
		// Skyrim. Furniture (chairs, beds, crafting stations, pull-bar levers) hands the player to
		// Skyrim while it's used (Game.cpp, SkyrimTakeover).
		void ActivateSkyrimTarget()
		{
			auto* pick = RE::CrosshairPickData::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!pick || !player) {
				return;
			}
			auto target = pick->GetActiveTarget().get();
			if (!target || target.get() == player) {
				logger::info("G: nothing to activate under the crosshair");
				return;
			}
			target->ActivateRef(player, 0, nullptr, 0, false);
			logger::info("activated {:08X} ({})", target->GetFormID(), target->GetDisplayFullName());
		}

		void OpenWaitMenu()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->CanSleepWait(nullptr)) {
				return;  // CanSleepWait already showed Skyrim's reason (enemies nearby, ...)
			}
			if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
				queue->AddMessage(RE::SleepWaitMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
			}
		}

		// The sink may be registered before the rest of SkyCraft is up (see Input::InstallSink): until then it lets everything by.
		std::atomic<bool> sinkReady{ false };
		bool              sinkRegistered = false;

		// A keyboard event made into nothing: still in the chain, but neither a press, a release nor a key held down, and no
		// character. Every event sink that runs after SkyCraft's (other mods' hotkeys: OBody's menu on O, and the like) is handed
		// this same chain, so a key that belongs to a Minecraft screen has to be emptied here or they would act on it too.
		void Swallow(RE::InputEvent* a_event)
		{
			if (a_event->GetEventType() == RE::INPUT_EVENT_TYPE::kButton) {
				auto* button = a_event->AsButtonEvent();
				button->value = 0.0f;
				button->heldDownSecs = 0.0f;
			} else if (a_event->GetEventType() == RE::INPUT_EVENT_TYPE::kChar) {
				a_event->AsCharEvent()->keyCode = 0;
			}
		}

		class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static InputSink* Get()
			{
				static InputSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_event || !sinkReady) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto& st = State();
				auto& link = Link::Get();
				// Menus that pause the game stop the per-frame update, so check them here.
				const bool menuOpen = Game::SkyrimMenuOpen();
				if (menuOpen && !st.skyrimMenuOpen) {
					Input::ReleaseAll();
				}
				if (menuOpen) {
					st.skyrimMenuOpen = true;
				}
				const bool route = st.puppeting && !menuOpen;
				const bool mcKeys = MinecraftHasKeyboard();

				for (auto* e = *a_event; e; e = e->next) {
					switch (e->GetEventType()) {
					case RE::INPUT_EVENT_TYPE::kMouseMove:
						{
							if (!route) {
								break;
							}
							auto* mm = e->AsMouseMoveEvent();
							if (st.mcScreenOpen) {
								const int x = std::clamp(st.cursorX.load() + mm->mouseInputX, 0, st.viewportW.load() - 1);
								const int y = std::clamp(st.cursorY.load() + mm->mouseInputY, 0, st.viewportH.load() - 1);
								st.cursorX = x;
								st.cursorY = y;
								link.PushInput(proto::kInCursor, 0, x, y);
							} else {
								lookDx += static_cast<float>(mm->mouseInputX);
								lookDy += static_cast<float>(mm->mouseInputY);
							}
							break;
						}
					case RE::INPUT_EVENT_TYPE::kButton:
						{
							auto* button = e->AsButtonEvent();
							const bool down = button->IsDown();
							const bool up = button->IsUp();
							// The controls switch works in either mode, whoever has the input.
							if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() == toggleKey) {
								if (down) {
									SwitchControls(menuOpen);
								}
								break;
							}
							if (resetKey != 0 && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() == resetKey) {
								if (down && !mcKeys) {  // a text box has the keyboard: it's typing, not the reset key
									AskWorldReset(menuOpen);
								}
								break;
							}
							// In Skyrim's own control mode the attack button is Skyrim's (the click still goes to it);
							// Combat is told, so the mobs in front of the player take the blow too.
							if (st.skyrimMode && !menuOpen && (down || up) && button->GetDevice() == RE::INPUT_DEVICE::kMouse && button->GetIDCode() == 0) {
								Combat::NoteSkyrimAttackButton(down);
							}
							if (!route || (!down && !up)) {
								break;
							}
							const auto code = button->GetIDCode();
							if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
								if (down) {
									heldByMinecraft[code & 0xFF] = mcKeys;
								}
								// With a Minecraft screen up (chat, inventory, options) every key is
								// Minecraft's, so typing works and Esc closes the screen.
								if (!mcKeys) {
									if (IsSkyrimMenuKey(code)) {
										break;  // MenuControls (see the hook below) opens Skyrim's menu
									}
									if (code == kDikG || code == kDikH || code == kDikO) {
										if (down) {
											if (code == kDikG) {
												ActivateSkyrimTarget();
											} else if (code == kDikH) {
												OpenWaitMenu();
											} else {
												Input::ReleaseAll();
												link.PushInput(proto::kInOpenMenu, 0);
											}
										}
										break;
									}
								}
								if (const auto sdl = kDikToSdl[code & 0xFF]) {
									link.PushInput(proto::kInKey, sdl, down ? 1 : 0);
								}
							} else if (button->GetDevice() == RE::INPUT_DEVICE::kMouse) {
								if (code == 8 || code == 9) {
									if (down) {
										link.PushInput(proto::kInScroll, 0, code == 8 ? 120 : -120);
									}
									break;
								}
								static constexpr std::uint16_t kButtons[8] = { 1, 3, 2, 4, 5, 0, 0, 0 };
								const std::uint16_t sdlButton = code < 8 ? kButtons[code] : std::uint16_t(0);
								if (sdlButton) {
									link.PushInput(proto::kInMouseButton, sdlButton, down ? 1 : 0);
								}
							}
							break;
						}
					case RE::INPUT_EVENT_TYPE::kChar:
						if (route && mcKeys) {
							link.PushInput(proto::kInText, 0, static_cast<std::int32_t>(e->AsCharEvent()->keyCode));
						}
						break;
					default:
						break;
					}
				}
				// Whatever a Minecraft screen (chat, inventory, options, ...) was typed or clicked into stays out of everything
				// that comes after: the keys were Minecraft's, not Skyrim's or any other mod's.
				if (route) {
					for (auto* e = *a_event; e; e = e->next) {
						if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kChar) {
							if (mcKeys) {
								Swallow(e);
							}
						} else if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kButton && e->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
							const auto code = e->AsButtonEvent()->GetIDCode() & 0xFF;
							if (mcKeys || heldByMinecraft[code]) {
								if (e->AsButtonEvent()->IsUp()) {
									heldByMinecraft[code] = false;  // its release ends here too
								}
								Swallow(e);
							} else if (code == kDikO) {
								// O opens Minecraft's pause menu in Minecraft's controls (the loop above has sent it). Other mods' sinks run
								// after this one and would see the same press: OBody opens its menu on O. In Skyrim's controls (no routing) it
								// is theirs again.
								Swallow(e);
							}
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// MenuControls opens Skyrim's menus (Tab, I, J, M, Wait, quicksave, ...). While Minecraft
		// drives the player only Esc, the console, M (map), J (journal) and F9 (quickload) reach
		// it (F5 is Minecraft's camera); while an MC screen is open nothing does.
		struct MenuControlsHook
		{
			static RE::BSEventNotifyControl thunk(RE::MenuControls* a_this, RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_source)
			{
				auto& st = State();
				if (!a_event || !*a_event || !st.puppeting || st.skyrimMenuOpen || Game::SkyrimMenuOpen()) {
					return func(a_this, a_event, a_source);  // a Skyrim menu is up: all its input
				}
				std::vector<RE::InputEvent*> keep;
				std::vector<RE::InputEvent*> all;
				const bool                   mcKeys = MinecraftHasKeyboard();
				for (auto* e = *a_event; e; e = e->next) {
					all.push_back(e);
					if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kButton && e->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
						auto*      button = e->AsButtonEvent();
						const auto code = button->GetIDCode() & 0xFF;
						if (heldByMinecraft[code]) {
							// Minecraft's key (pressed while a text box or screen had the keyboard): its
							// release is Minecraft's too, Skyrim never sees either half.
							if (button->IsUp()) {
								heldByMinecraft[code] = false;
							}
							continue;
						}
						if (!mcKeys && IsSkyrimMenuKey(button->GetIDCode())) {
							keep.push_back(e);
						}
					}
				}
				if (keep.empty()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::vector<RE::InputEvent*> savedNext;
				for (auto* e : all) {
					savedNext.push_back(e->next);
				}
				for (std::size_t i = 0; i < keep.size(); ++i) {
					keep[i]->next = i + 1 < keep.size() ? keep[i + 1] : nullptr;
				}
				RE::InputEvent* head = keep.front();
				const auto      result = func(a_this, &head, a_source);
				for (std::size_t i = 0; i < all.size(); ++i) {
					all[i]->next = savedNext[i];
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// PlayerControls turns input into the Skyrim player's own actions: moving, looking,
		// attacking, jumping, sneaking, the camera. While Minecraft drives the player it gets none.
		// (Disabling Skyrim's player controls instead would also stop its NPCs fighting the player.)
		struct PlayerControlsHook
		{
			static RE::BSEventNotifyControl thunk(RE::PlayerControls* a_this, RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_source)
			{
				if (!State().minecraftOwnsPlayer || Game::SkyrimMenuOpen()) {
					return func(a_this, a_event, a_source);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace Input
	{
		// Registers the input sink. Called as soon as Skyrim's input exists (SKSE's kInputLoaded), well before other mods
		// register theirs, because sinks run in the order they were added and SkyCraft's has to see the keys first to keep
		// Minecraft's away from the rest; Install() calls it again, which does nothing if it has already run.
		void InstallSink()
		{
			if (sinkRegistered) {
				return;
			}
			if (auto* devices = RE::BSInputDeviceManager::GetSingleton()) {
				devices->AddEventSink(InputSink::Get());
				sinkRegistered = true;
			}
		}

		void Install()
		{
			CSimpleIniA ini;
			ini.SetUnicode();
			ini.LoadFile("Data/SKSE/Plugins/SkyCraft.ini");
			toggleKeyName = ini.GetValue("Controls", "sToggleKey", "F8");
			toggleKey = ParseToggleKey(toggleKeyName);
			if (toggleKey == 0) {
				logger::warn("SkyCraft.ini [Controls] sToggleKey = {}: not a key SkyCraft can use (F1-F12, Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock, or a DirectInput scan code that isn't Esc, ~, G, H, J, M, O or F9); using F8", toggleKeyName);
				toggleKey = kDikF8;
				toggleKeyName = "F8";
			}
			// The reset key: "None" (or nothing) switches it off.
			resetKeyName = ini.GetValue("Controls", "sResetKey", "F7");
			std::string resetText;
			for (const char c : resetKeyName) {
				if (!std::isspace(static_cast<unsigned char>(c))) {
					resetText.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
				}
			}
			if (resetText.empty() || resetText == "none" || resetText == "off" || resetText == "0") {
				resetKey = 0;
			} else {
				resetKey = ParseToggleKey(resetKeyName);
				if (resetKey == 0 || resetKey == toggleKey) {
					logger::warn("SkyCraft.ini [Controls] sResetKey = {}: not a key SkyCraft can use (see sToggleKey, and it can't be the same key); using F7", resetKeyName);
					resetKey = kDikF7;
					resetKeyName = "F7";
				}
				if (resetKey == toggleKey) {
					logger::warn("sToggleKey is F7, so the world reset key is switched off; set sResetKey to another key to use it");
					resetKey = 0;
				}
			}
			State().skyrimMode = ini.GetBoolValue("Controls", "bStartInSkyrimMode", true);
			logger::info("controls switch: {} (scan code {:#x}); starting with {}'s controls", toggleKeyName, toggleKey, State().skyrimMode ? "Skyrim" : "Minecraft");
			if (resetKey != 0) {
				logger::info("world reset key: {} (scan code {:#x})", resetKeyName, resetKey);
			} else {
				logger::info("world reset key: off");
			}

			InstallSink();
			sinkReady = true;
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_MenuControls[0] };
			MenuControlsHook::func = vtbl.write_vfunc(0x1, MenuControlsHook::thunk);
			REL::Relocation<std::uintptr_t> playerVtbl{ RE::VTABLE_PlayerControls[0] };
			PlayerControlsHook::func = playerVtbl.write_vfunc(0x1, PlayerControlsHook::thunk);
			logger::info("input hooks installed");
		}

		void ConsumeLook(float& a_dx, float& a_dy)
		{
			a_dx = lookDx;
			a_dy = lookDy;
			lookDx = lookDy = 0.0f;
		}

		void ReleaseAll()
		{
			Link::Get().PushInput(proto::kInReleaseAll, 0);
		}

		const std::string& ToggleKeyName()
		{
			return toggleKeyName;
		}

		void SetActivatePromptKey(bool a_minecraftControls)
		{
			static std::uint16_t savedKey = 0xFFFF;
			auto*                controls = RE::ControlMap::GetSingleton();
			auto*                context = controls ? controls->controlMap[RE::UserEvents::INPUT_CONTEXT_ID::kGameplay] : nullptr;
			if (!context) {
				return;
			}
			for (auto& mapping : context->deviceMappings[RE::INPUT_DEVICE::kKeyboard]) {
				if (mapping.eventID != "Activate") {
					continue;
				}
				if (a_minecraftControls && savedKey == 0xFFFF) {
					savedKey = mapping.inputKey;
					mapping.inputKey = static_cast<std::uint16_t>(kDikG);
				} else if (!a_minecraftControls && savedKey != 0xFFFF) {
					mapping.inputKey = savedKey;
					savedKey = 0xFFFF;
				}
			}
		}
	}
}
