# Weapon bridge rendering + editable-stat prototype

This remains experimental and separate from progression/stable builds.

## What the current SkyCraft renderer already gives us

SkyCraft's Minecraft client already turns normal Minecraft item render states into geometry/textures
that Skyrim draws. Dropped items go through WorldExporter, while held/equipped player items are part
of AvatarExporter. That means a custom Fabric item that has a valid Minecraft item model does not
need a second Skyrim renderer just to appear: it travels through the same existing item/render paths.

## Prototype direction

The first prototype registers four fixed Minecraft item ids:

- skycraft:skyrim_iron_dagger
- skycraft:skyrim_iron_sword
- skycraft:skyrim_iron_greatsword
- skycraft:skyrim_iron_battleaxe

Their combat numbers are loaded before registration from:

`config/skycraft-weapons.json`

The editable fields are:

- damage
- attackSpeed
- reach (Minecraft entity interaction range)
- durability

Restart Minecraft after editing because registry-time item properties are immutable after startup.

The registry ids are fixed. The JSON changes numbers, not identities. This is important for saved
Minecraft inventories and multiplayer compatibility.

## Stat semantics

Minecraft's player has base attack damage 1, base attack speed 4, and normal entity reach 3. The
prototype adds item modifiers so the configured values become the final held values.

This lets the families feel different without custom combat code:

- dagger: low damage, very fast, short reach
- sword: middle damage/speed/reach
- greatsword: high damage, slow, longer reach
- battleaxe: highest early damage, slowest

## Sprite/model path

Minecraft 26.3 requires BOTH a client item definition and a model:

`assets/skycraft/items/<id>.json`
→ points at
`assets/skycraft/models/item/<id>.json`
→ which points at
`assets/skycraft/textures/item/<id>.png`

For the first compile prototype, the custom ids deliberately use vanilla item textures as placeholders.
That proves registration, inventory display, held rendering, dropped rendering and Skyrim export before
we spend time polishing sprites. Replacing the texture reference with a SkyCraft PNG later needs no
combat-code change.

## Skyrim mapping plan

The included experimental WeaponMappings.example.json starts with four verified ordinary vanilla
FormIDs. The actual pickup transaction is NOT wired yet. Do not merge these into the resource bridge
until we add instance filtering so enchanted/unique/quest/scripted weapons cannot be flattened by
accident.

## Next sub-steps

1. Make the four items compile and appear in a dev inventory/command.
2. Test held-item render through the existing AvatarExporter and dropped-item render through WorldExporter.
3. Replace placeholder models with custom 16x16/32x32 sprites.
4. Add an explicit Skyrim weapon-transfer protocol carrying source FormID + weapon metadata.
5. Filter ordinary unenchanted instances first.
6. Expand to steel/orcish/dwarven/etc. once the generic path works.


## Core test family

The first test family is intentionally only four weapons:

- Iron Dagger
- Iron Sword
- Iron Greatsword
- Iron Battleaxe

All four use ordinary flat Minecraft-style item rendering. For this first behavior test the dagger,
sword and greatsword reuse Minecraft's iron-sword sprite and the battleaxe reuses the iron-axe sprite.
That keeps rendering boring and reliable while we validate stat feel. Custom 2D sprites can replace
those model references later without touching weapon logic.

The experimental build registers a no-cheats-required test command:

`/skyweapons testkit`

It adds one of each prototype weapon to the current Minecraft player's inventory. This is only an
experiment convenience and should be removed before the weapon bridge is merged into a stable build.

Suggested first-pass feel checks:

- dagger: quickest recharge and noticeably shortest melee reach;
- sword: baseline/balanced;
- greatsword: clearly slower, harder-hitting, longer reach;
- battleaxe: slowest and hardest-hitting of the four.

Edit `config/skycraft-weapons.json`, restart Minecraft, and rerun the test kit command to compare
different numbers. No recompilation is required for stat tuning.
