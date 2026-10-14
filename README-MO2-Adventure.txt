SkyCraft Adventure Integration (Sunp22 + JackCakes)
=================================================
Experimental combined build. Install with Mod Organizer 2; do not overwrite
your existing, working SkyCraft installation.

REQUIREMENTS
- Legitimate Skyrim Special Edition / Anniversary Edition, with matching SKSE64.
- Address Library for SKSE Plugins, matching your Skyrim runtime.
- Minecraft Java Edition account (sign in once via bundled Prism Launcher).
- For lock-on to Minecraft mobs: True Directional Movement AND the separate
  SkyCraft-TDM-VirtualTargets-patch (install as a higher-priority MO2 mod).

MO2 INSTALL
1. Back up Skyrim saves and your Minecraft SkyCraft world.
2. Disable, but do not delete, your current SkyCraft MO2 mod.
3. MO2 -> Install a new mod from an archive -> choose SkyCraft-*.zip.
4. Name the new mod "SkyCraft Adventure - Sunp22 + JackCakes" and enable it.
5. Ensure the installed folder begins with SKSE/Plugins/SkyCraft.dll; do NOT
   extract the inner SkyCraft-Minecraft.zip yourself.
6. Start Skyrim through the SKSE executable in MO2.
7. When Prism appears at first boot, sign in using your Minecraft Java Edition
   Microsoft account. SkyCraft starts Minecraft in the background.
8. For TDM lock-on, install True Directional Movement first, then install
   SkyCraft-TDM-VirtualTargets-patch.zip as a separate mod after it, with higher
   priority. Its DLL overrides TDM's original DLL.

CONTROLS AND TESTS
- F8: switch between Skyrim-native controls/magic and Minecraft controls/hotbar.
- F7 twice: reset placed and dug Minecraft world blocks (destructive).
- In Minecraft mode, /skymagic demo: grant sample hotbar spell items.
- /skyweapons testkit: grant Skyrim-style weapons.
- /skyaffixes testkit: grant test weapons with randomized modifiers.
- /landon gift: grant the cookie and Hidden Leaf kunai.
- Outside settlements, wait for natural mobs to spawn in Skyrim terrain.
- Test Skyrim-mode native spell attacks and third-person TDM mob lock-on.
- Test Minecraft hotbar magic, item transfers, NPC fights and blocks in each mode.

LIMITATIONS
- This is a separate experimental integration. Protect your saves.
- Orcish and Glass weapons without original artwork currently use temporary
  vanilla Minecraft icons rather than unique sprites.
- Affix rolls on converted weapons do not yet preserve Skyrim source FormIDs.
- The separate Skate 3 bridge is intentionally not bundled in this build.
- A successful compiler run does not mean all runtime combinations are stable.

DEBUG
Skyrim SKSE log: Documents/My Games/Skyrim Special Edition/SKSE/SkyCraft.log
Minecraft client: %LOCALAPPDATA%/SkyCraft/Prism/instances/SkyCraft/.minecraft/logs/latest.log
Source: https://github.com/JackCakes/SkyCraft/tree/integration/sunp22-adventure

Build workflow: https://github.com/JackCakes/SkyCraft/actions/workflows/build-adventure-mo2.yml
