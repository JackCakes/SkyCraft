# Skyrim Inventory Access Experiment

This branch is intentionally separate from progression/stable work.

## Goal

Give SkyCraft players an explicit way to open Skyrim's real inventory while Minecraft is driving the
player, without restoring all intercepted Skyrim gameplay keys or changing Minecraft's normal
inventory controls.

## Prototype 1

- **F8** opens Skyrim's real `InventoryMenu` while SkyCraft is puppeting the player.
- SkyCraft releases all Minecraft input before asking Skyrim to show the menu.
- Once Skyrim's inventory is open, the existing blocking-menu path gives mouse/keyboard input back to Skyrim.
- Normal SkyCraft routing remains unchanged otherwise. Minecraft screens still own all keys while they are open.
- This branch is based on the user-tested progression.3 commit, not progression.4.

## Test

Install `SkyCraft-0.1.2-inventory-access.1.zip` as a separate MO2 mod below normal SkyCraft and
disable any other custom SkyCraft test build for this test.

1. Launch through SKSE and wait for Minecraft/SkyCraft to connect.
2. With no Minecraft GUI open, press **F8**.
3. Skyrim's item inventory should open.
4. Inspect items, then close the inventory normally.
5. Confirm Minecraft regains control after the menu closes.
6. Send `SkyCraft.log`; a successful request includes `F8: opening Skyrim inventory`.

Do not merge this experiment into progression/stable until it is built and tested in game.
