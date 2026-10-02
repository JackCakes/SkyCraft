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

The four core vanilla FormIDs are now wired through the existing acknowledged resource-transfer path.
A weapon is eligible only when its current Skyrim inventory entry is conservative/plain: no quest
alias, equipped/favorited state, record or instance enchantment, temper health, poison, custom display
name, or ownership/stolen data. If any copy of that base weapon is special, automatic conversion for
that base form is skipped rather than risking removal of the wrong instance.

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


## Prototype 2 end-to-end pickup path

This branch now tests the actual gameplay loop rather than only /skyweapons testkit:

Skyrim pickup -> plain-instance safety check -> Minecraft registry grant -> ACK -> Skyrim removal.

The four enabled mappings live in the packaged SkyCraftMappings.json and target the registered
skycraft: weapon ids. The source weapon remains in Skyrim until Minecraft ACKs the grant.

The first test should use ordinary, unmodified items (console additem is fine):

- Iron Dagger 0001397E
- Iron Sword 00012EB7
- Iron Greatsword 0001359D
- Iron Battleaxe 00013980

Expected: each disappears from Skyrim only after the corresponding custom item reaches Minecraft.
Special copies are intentionally left alone in this prototype.
