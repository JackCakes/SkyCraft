# Weapon bridge experiment

This branch is the isolated proving ground for Skyrim -> Minecraft weapon translation.

## Source-verified constraints

- SkyCraft already owns the player/control bridge and already supports multiplayer; this experiment does not replace either system.
- Conversion uses the existing SKSE <-> Minecraft shared-memory progression transport.
- Skyrim source items are removed only after Minecraft acknowledges the grant.
- Ordinary weapon mappings are intentionally conservative: quest, equipped, favorited, enchanted, tempered, poisoned, renamed, stolen/owned, charged, or otherwise special instances remain Skyrim-side.
- No ESP, Papyrus quest, save-persistent form, root-game patch, or base-install overwrite is required. Packaging stays MO2-friendly.

## Current experimental catalog

The branch currently registers Iron and Dwarven/Dwemer Minecraft-side weapon counterparts. The item registry uses external config for provisional damage, speed, reach, and durability so balance can change later without redesigning the bridge.

The current user-supplied sprite archive is not committed wholesale to this branch. Do not invent replacement art for missing material tiers. New tiers should either use actual supplied/approved assets or remain data/research-only until those bytes are available.

## Next implementation slices

1. Keep acquisition/ACK safety stable while expanding verified vanilla FormID mappings.
2. Add material families in small batches (Elven, Orcish, Glass, Ebony, Daedric, Ancient Nord/Falmer, then DLC families) only when matching Minecraft item assets are present.
3. Generalize the catalog so weapon class (dagger/sword/axe/mace/greatsword/battleaxe/warhammer/bow) supplies behavior and material tier supplies a small progression offset. Numbers are provisional.
4. Preserve a test command and dedicated logs for each batch.
5. Only promote a batch to feature/skyrim-minecraft-progression after the experimental build is green and the relevant behavior has been user-tested.

## In-game regression test

Pick up a plain mapped weapon and verify: it appears Minecraft-side; the Skyrim copy disappears only after the ACK; the custom item is unstackable; attack speed/reach differ by class. Repeat with an enchanted, tempered, favorited/equipped, renamed, poisoned, stolen/owned, and quest-marked instance and verify those copies stay in Skyrim. Also fill Minecraft inventory before a pickup and verify no source weapon is lost.
