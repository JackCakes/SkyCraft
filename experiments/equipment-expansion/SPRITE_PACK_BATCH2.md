# Equipment Expansion Asset Inventory

Branch: `experimental/equipment-expansion`

This branch starts from the user-tested core weapon bridge commit
`f0ee1e2781bbd779c21665557e0acf18bb8ed266`.

The user supplied `skycraft_weapon_sprites_batch2.zip` as source art. The ZIP contains Minecraft-style
item sprites under `assets/skycraft/textures/item/` plus 512px preview copies. Treat the asset names
below as the authoritative inventory of that batch; do not invent missing art names.

## Sprite families present

- Daedric: battleaxe, bow, dagger, greatsword, mace, sword, war axe, warhammer
- Dragonbone: battleaxe, bow, dagger, greatsword, mace, sword, war axe, warhammer
- Draugr / Ancient Nord: battleaxe, bow, greatsword, sword, war axe
- Dwarven / Dwemer: battleaxe, bow, crossbow, dagger, greatsword, mace, sword, war axe, warhammer
- Ebony: battleaxe, bow, dagger, greatsword, mace, sword, war axe, warhammer
- Elven: battleaxe, bow, dagger, greatsword, mace, sword, war axe, warhammer
- Falmer: bow, sword, war axe
- Glass: battleaxe, bow, dagger, greatsword, mace, sword, war axe, warhammer
- Orcish: battleaxe, bow, dagger, greatsword, mace, sword, war axe, warhammer
- Stalhrim: battleaxe, bow, dagger, greatsword, mace, sword, war axe, warhammer

The ZIP does not contain Steel, Silver, Forsworn, Nordic, Nord Hero, Imperial or additional Falmer
weapon sprites. Do not pretend it does; use isolated placeholders only when explicitly needed.

## Expansion rules

1. Keep the validated ACK-before-remove transaction.
2. Only ordinary weapon/armor instances auto-convert. Quest, enchanted, tempered, equipped,
   favorited, renamed, poisoned, stolen/owned or otherwise special instances remain Skyrim-side.
3. Verify Skyrim FormIDs from reliable source material before adding mappings.
4. Prefer data-driven catalog/mapping entries; avoid one-off hardcoded transfer behavior.
5. Custom Minecraft weapon items remain stack size 1.
6. Damage/speed/reach balance is provisional. Feature coverage comes first.
7. Do not merge this branch into validated progression until isolated build + in-game testing passes.
8. Armor work can begin here after weapon-family coverage is established, using the same conservative
   instance-safety policy.


## Verified vanilla Orcish catalog

Source verification pass completed for the ordinary unenchanted base-game Orcish family:

| Item | Skyrim FormID |
|---|---|
| Dagger | 0001398E |
| Sword | 00013991 |
| War Axe | 0001398B |
| Mace | 00013990 |
| Greatsword | 0001398F |
| Battleaxe | 0001398C |
| Warhammer | 00013992 |
| Bow | 0001398D |

All eight corresponding sprites are present in the user-supplied batch2 archive. These IDs are for ordinary base-game records; enchanted/unique instances remain excluded by the conservative transfer policy.
