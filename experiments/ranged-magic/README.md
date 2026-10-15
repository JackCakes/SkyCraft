# Ranged Magic Demo

Branch: `experimental/ranged-magic`

## Demo items

- **Firebolt** — right-click to cast a fast orange flame trail toward the crosshair. Hits a mirrored Skyrim actor for 5 Minecraft damage and ignites the actor briefly.
- **Frostbolt** — right-click to cast a blue-white snow/ice trail toward the crosshair. Hits a mirrored Skyrim actor for 4 Minecraft damage.
- Both spells have a short server-side cooldown and a visible miss trail when no Skyrim actor is under the crosshair.

## Test

1. Install/package this branch as the normal SkyCraft MO2 mod.
2. Launch Skyrim through SKSE and enter the SkyCraft world.
3. Run `/skymagic demo` once.
4. Put Firebolt or Frostbolt in the hotbar.
5. Aim at a nearby Skyrim NPC and right-click.
6. Verify the colored trail/impact particles appear and the NPC receives damage.
7. Aim away from an NPC and right-click; the spell should still render a miss trail without damaging anything.

Debug commands remain available as `/skymagic fire` and `/skymagic frost`.
