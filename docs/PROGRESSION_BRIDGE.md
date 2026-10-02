# Skyrim -> Minecraft Progression Bridge (test build)

This branch adds a deliberately small first step toward progressing through Minecraft by adventuring
in Skyrim.

## Test mappings

| Skyrim loot | Minecraft result |
|---|---|
| Iron Ingot (`0005ACE4`) | Iron Ingot |
| Gold Ingot (`0005AD9E`) | Gold Ingot |
| Leather (`000DB5D2`) | Leather |
| Wheat (`0004B0BA`) | Wheat |
| Iron Ore (`00071CF3`) | Raw Iron |
| Gold Ore (`0005ACDE`) | Raw Gold |

Only newly acquired supported items are converted. Quest items, weapons, armor, keys, books and every
unlisted Skyrim item are ignored. Skyrim's common charcoal stick (`00033760`) is intentionally not
converted because it is used by the Thieves Guild quest **Hard Answers**; this test build avoids
risking quest progression.

## Safety model

The Skyrim inventory listener sends a transfer request but **does not remove the Skyrim item yet**.
Minecraft grants the resource on its authoritative server, then sends an acknowledgement back. Skyrim
only removes the acknowledged quantity. If Minecraft is unavailable, the request cannot be queued, or
Minecraft rejects it, the Skyrim item stays where it is.

A load/new-game event clears in-flight requests so an acknowledgement from an older game state cannot
remove an item after a quickload.

This feature adds no ESP/ESL, Papyrus quest, alias or persistent Skyrim record. Disabling/removing the
MO2 mod therefore removes the code path without leaving scripted save dependencies.

Minecraft inventory overflow is dropped at the Minecraft player's feet rather than silently deleting
the source resource.

## Settings

`Data/SKSE/Plugins/SkyCraft.ini`:

```ini
[ProgressionBridge]
bEnabled = 1
bIronIngots = 1
bGoldIngots = 1
bLeather = 1
bWheat = 1
bIronOre = 1
bGoldOre = 1
```

Restart Skyrim after changing these settings.

## Multiplayer

For the host, the shared-memory request is granted directly by the integrated Minecraft server.

For a guest connected to another SkyCraft world, the guest's local Skyrim sends the request to their
Minecraft client, which forwards it to the host server. The host adds the item to that guest's real
Minecraft inventory and returns an acknowledgement to the guest's Skyrim.

## Install / rollback

The GitHub Actions artifact contains an MO2-ready `SkyCraft-0.1.2-progression.2.zip`. Install it as a
separate mod after the normal SkyCraft entry so it wins file conflicts.

To roll back, disable/remove the progression test mod in MO2 and enable the original SkyCraft. On the
next launch SkyCraft's normal bundle-stamp logic restores the matching original Fabric jar while
keeping the existing Minecraft world/sign-in.

Converted items are gameplay state and are not automatically converted back when the mod is removed.
