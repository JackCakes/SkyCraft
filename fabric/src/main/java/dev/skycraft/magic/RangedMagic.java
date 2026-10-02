package dev.skycraft.magic;

import dev.skycraft.SkyCraft;
import dev.skycraft.combat.SkyCombat;
import dev.skycraft.combat.SkyrimActorEntity;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.fabricmc.fabric.api.event.player.UseItemCallback;
import net.minecraft.commands.Commands;
import net.minecraft.core.Registry;
import net.minecraft.core.particles.ParticleTypes;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.phys.Vec3;

/**
 * Playable Minecraft-side ranged magic demo.
 *
 * <p>Firebolt and Frostbolt are real held Minecraft items. Right-clicking one resolves against
 * SkyCombat's mirrored Skyrim actors, so spell hits travel through the same bridge as normal
 * Minecraft combat. The command form remains as a useful fallback/debug harness.
 */
public final class RangedMagic {
    private static final double RANGE = 28.0;
    private static final long COOLDOWN_TICKS = 8;
    private static final Map<UUID, Long> LAST_CAST = new HashMap<>();

    public static Item FIREBOLT;
    public static Item FROSTBOLT;

    private RangedMagic() {}

    public static void init() {
        FIREBOLT = register("firebolt");
        FROSTBOLT = register("frostbolt");

        UseItemCallback.EVENT.register((player, level, hand) -> {
            ItemStack held = player.getItemInHand(hand);
            Spell spell = held.is(FIREBOLT) ? Spell.FIRE : held.is(FROSTBOLT) ? Spell.FROST : null;
            if (spell == null) {
                return InteractionResult.PASS;
            }

            // Client reports success so vanilla sends the use packet; actual casting is server-only.
            if (player instanceof ServerPlayer serverPlayer) {
                cast(serverPlayer, spell);
            }
            return InteractionResult.SUCCESS;
        });

        CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) -> {
            dispatcher.register(Commands.literal("skymagic")
                .then(Commands.literal("fire").executes(context ->
                    cast(context.getSource().getPlayerOrException(), Spell.FIRE) ? 1 : 0))
                .then(Commands.literal("frost").executes(context ->
                    cast(context.getSource().getPlayerOrException(), Spell.FROST) ? 1 : 0))
                .then(Commands.literal("demo").executes(context -> {
                    ServerPlayer player = context.getSource().getPlayerOrException();
                    player.getInventory().add(new ItemStack(FIREBOLT));
                    player.getInventory().add(new ItemStack(FROSTBOLT));
                    player.sendSystemMessage(Component.literal("SkyCraft magic demo added: Firebolt + Frostbolt. Right-click to cast."));
                    return 1;
                })));
        });
    }

    private static Item register(String path) {
        Identifier id = Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, path);
        ResourceKey<Item> key = ResourceKey.create(Registries.ITEM, id);
        return Registry.register(BuiltInRegistries.ITEM, id, new Item(new Item.Properties().setId(key).stacksTo(1)));
    }

    /** Cast one ranged spell from a Minecraft player toward their crosshair. */
    public static boolean cast(ServerPlayer player, Spell spell) {
        ServerLevel level = player.level();
        long now = level.getGameTime();
        long previous = LAST_CAST.getOrDefault(player.getUUID(), Long.MIN_VALUE / 2);
        if (now - previous < COOLDOWN_TICKS) {
            return false;
        }

        LAST_CAST.put(player.getUUID(), now);
        Vec3 from = player.getEyePosition();
        SkyrimActorEntity target = SkyCombat.findTarget(player, RANGE);

        if (target == null) {
            Vec3 miss = from.add(player.getLookAngle().scale(RANGE));
            spawnTrail(level, from, miss, spell, false);
            return true;
        }

        Vec3 to = target.getBoundingBox().getCenter();
        spawnTrail(level, from, to, spell, true);
        spawnImpact(level, to, spell);

        boolean hurt = target.hurtServer(level, level.damageSources().indirectMagic(player, player), spell.damage);
        if (hurt && spell == Spell.FIRE) {
            target.igniteForSeconds(3.0F);
        }

        SkyCraft.LOG.info("SkyCraft magic: {} cast {} at {} for {}", player.getName().getString(),
            spell.id, target.getName().getString(), spell.damage);
        return true;
    }

    private static void spawnTrail(ServerLevel level, Vec3 from, Vec3 to, Spell spell, boolean hit) {
        Vec3 delta = to.subtract(from);
        double length = delta.length();
        int steps = Math.max(2, (int) Math.ceil(length * 4.0));
        for (int i = 1; i <= steps; i++) {
            Vec3 p = from.add(delta.scale((double) i / steps));
            if (spell == Spell.FIRE) {
                level.sendParticles(ParticleTypes.FLAME, p.x, p.y, p.z, 1, 0.025, 0.025, 0.025, 0.0);
                if ((i & 3) == 0) {
                    level.sendParticles(ParticleTypes.SMOKE, p.x, p.y, p.z, 1, 0.015, 0.015, 0.015, 0.0);
                }
            } else {
                level.sendParticles(ParticleTypes.SNOWFLAKE, p.x, p.y, p.z, 1, 0.03, 0.03, 0.03, 0.0);
                if ((i & 3) == 0) {
                    level.sendParticles(ParticleTypes.END_ROD, p.x, p.y, p.z, 1, 0.01, 0.01, 0.01, 0.0);
                }
            }
        }
        if (!hit) {
            level.sendParticles(spell == Spell.FIRE ? ParticleTypes.SMOKE : ParticleTypes.SNOWFLAKE,
                to.x, to.y, to.z, 6, 0.12, 0.12, 0.12, 0.01);
        }
    }

    private static void spawnImpact(ServerLevel level, Vec3 at, Spell spell) {
        if (spell == Spell.FIRE) {
            level.sendParticles(ParticleTypes.FLAME, at.x, at.y, at.z, 18, 0.35, 0.45, 0.35, 0.03);
            level.sendParticles(ParticleTypes.LARGE_SMOKE, at.x, at.y, at.z, 5, 0.22, 0.28, 0.22, 0.01);
        } else {
            level.sendParticles(ParticleTypes.SNOWFLAKE, at.x, at.y, at.z, 24, 0.4, 0.5, 0.4, 0.02);
            level.sendParticles(ParticleTypes.END_ROD, at.x, at.y, at.z, 8, 0.25, 0.35, 0.25, 0.01);
        }
    }

    public enum Spell {
        FIRE("firebolt", 5.0F),
        FROST("frostbolt", 4.0F);

        final String id;
        final float damage;

        Spell(String id, float damage) {
            this.id = id;
            this.damage = damage;
        }
    }
}
