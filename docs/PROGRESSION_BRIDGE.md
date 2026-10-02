# Skyrim -> Minecraft Universal Progression Bridge

This test build moves the conversion table out of C++/Java and into an editable JSON file:

`Data/SKSE/Plugins/SkyCraftMappings.json`

Each mapping has a Skyrim base FormID, Minecraft registry id, ratio, and enabled flag. The SKSE side
hashes the Minecraft registry id with FNV-1a 32-bit; Fabric indexes the live item registry with the
same hash. Adding another ordinary mapping no longer needs another protocol enum or Java/C++ switch.

The first test set contains Iron/Gold ingots, Leather, Wheat, Iron/Gold ore, Red/Green Apple, Bread,
Carrot, Potato, Chicken's Egg, Bone Meal, Iron Arrow, and a 4 Leather Strips -> 1 Leather ratio.

Ratios are acquisition-batch based in this first test. Four strips received in one event convert;
separate one-strip acquisitions do not yet accumulate.

Skyrim keeps the source item until Minecraft acknowledges the grant. Unknown registry ids, missing
Minecraft, ring failure, stale acknowledgements, or hash collisions leave the Skyrim item untouched.

No ESP/ESL or Papyrus state is added. Install `SkyCraft-0.1.2-progression.3.zip` after normal
SkyCraft in MO2; disable it to roll back.
