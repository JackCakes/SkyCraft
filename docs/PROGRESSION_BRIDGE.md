# Skyrim -> Minecraft Universal Progression Bridge

The bridge is data-driven through:

`Data/SKSE/Plugins/SkyCraftMappings.json`

Each mapping has a Skyrim base FormID, Minecraft registry id, ratio, and enabled flag. The SKSE side
hashes the Minecraft registry id with FNV-1a 32-bit; Fabric indexes the live item registry with the
same hash. Adding another ordinary mapping does not require another protocol enum or Java/C++ switch.

## Progression.4

The default mapping pack now contains 51 enabled vanilla Skyrim mappings covering:

- core ingots/ores, Leather, Wheat and Leather Strips;
- common fruit, vegetables, raw/cooked meats and simple prepared foods;
- Bone Meal, berries, feathers, Honeycomb and Moon Sugar;
- twelve common vanilla arrow types, all feeding Minecraft arrows.

Several entries are semantic Minecraft equivalents rather than literal same-name items. Examples:
Pheasant Breast -> Chicken, Venison -> Mutton, Apple Pie/Sweet Roll -> Pumpkin Pie.

## Ratio carry-over

Ratio leftovers now accumulate across separate pickups during the current run. With the default
Leather Strips mapping, four separate one-strip acquisitions can therefore complete the 4:1 conversion.

The carry is bookkeeping only; the actual leftover items remain in Skyrim until a complete group is
acknowledged by Minecraft. Loading a save/new game clears in-flight requests and carry bookkeeping,
so it never stores hidden progression state in a save or SKSE co-save.

## Transaction safety

Skyrim keeps source items until Minecraft acknowledges the grant. Unknown registry ids, missing
Minecraft, ring failure, stale acknowledgements, or hash collisions leave the source item in Skyrim.

## Dedicated log

Progression activity is mirrored to:

`Documents/My Games/Skyrim Special Edition/SKSE/SkyCraftProgression.log`

This small log contains mapping load, ratio carry, requests, acknowledgements/conversions and errors,
so it can be shared without the much larger general SkyCraft log.

## Install / rollback

Install `SkyCraft-0.1.2-progression.4.zip` as a separate MO2 mod after normal SkyCraft and disable
the older progression test mod. Disable/remove progression.4 to restore the base SkyCraft files.
