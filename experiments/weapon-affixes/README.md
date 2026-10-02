# Weapon Affix / Bonus Experiment

Branch: `experimental/weapon-affixes`

This branch is intentionally based on the current weapon-bridge experiment and is not merged into
progression/stable work.

## Goal

Make two otherwise identical Skyrim weapons feel like separate loot drops after they become Minecraft
items. The first prototype uses one Terraria-style prefix per converted weapon.

A fresh SkyCraft weapon has a configurable 70% chance to receive one weighted affix. The selected
affix is stored on that exact Minecraft ItemStack in `minecraft:custom_data`, so it persists through
normal Minecraft saves/inventory moves and syncs to multiplayer clients.

## First prototype affixes

| Affix | Effect |
| --- | --- |
| Brutal | +25% attack damage |
| Swift | +30% attack speed |
| Long | +0.50 entity reach |
| Sturdy | +50% max durability |
| Volatile | 10% chance on each hit to deal 3x final damage |

Static effects are per-stack Minecraft attribute/data-component overrides. Volatile is evaluated when
damage reaches SkyCraft's Skyrim actor stand-in, after Minecraft has already applied normal weapon
damage/cooldown/crit logic, then the resulting damage is forwarded to Skyrim.

## Editable config

On first run the Fabric side writes:

`config/skycraft-weapon-affixes.json`

The whole table is data-driven: roll chance, weights and numeric effects can be edited. Use
`/skyaffixes reload` to reload the table for future drops; existing weapons keep the affix and
numbers already baked into their ItemStack.

## Testing without waiting on Skyrim pickup conversion

`/skyaffixes testkit`

adds 12 independently rolled SkyCraft Iron Swords. This is only a development command. Inspect names,
lore, attack speed/reach/damage, and durability.

For deterministic testing, use:

`/skyaffixes give brutal`
`/skyaffixes give swift`
`/skyaffixes give long`
`/skyaffixes give sturdy`
`/skyaffixes give volatile`

Hold an affixed weapon and run `/skyaffixes inspect` to print the exact stored values. With a Volatile
weapon, enough hits should eventually make the log show:

`SkyCraft affix proc: 3.0x damage on <actor>`

## Safety / scope

- Every weapon is rolled exactly once. A failed affix roll is also marked, so a normal weapon cannot later reroll itself into a bonus.
- Runtime proc numbers are copied into the weapon's own custom data when it rolls. Editing/reloading the config changes future drops only; existing Volatile/etc. weapons keep the behavior they originally rolled.
- No ESP/ESL, Papyrus state, Skyrim save records or SKSE co-save data are added.
- One affix per weapon for prototype 1.
- No life-steal, status effects, elemental procs, rarity tiers or multi-affix combinations yet.
- Affixes are server authoritative because the Minecraft server creates the ItemStack and rolls procs.
- If the weapon bridge is not yet user-validated, this branch can still validate the Minecraft-side
  affix mechanics independently with the test command.
