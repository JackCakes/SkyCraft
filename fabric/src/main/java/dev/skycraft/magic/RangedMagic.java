package dev.skycraft.magic;

import dev.skycraft.SkyCraft;
import dev.skycraft.combat.SkyCombat;
import dev.skycraft.combat.SkyrimActorEntity;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.commands.Commands;
import net.minecraft.core.Registry;
import net.minecraft.core.particles.ParticleTypes;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.ItemUseAnimation;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.Vec3;

/**
 * Skyrim-style channeled destruction magic for the Minecraft player.
 *
 * <p>Equipping Flames or Frostbite leaves the rendered hand visually empty while a small aura
 * gathers around it. Holding right-click starts Minecraft's normal continuous item-use state:
 * the arm stays raised and this item receives an onUseTick every tick until the button is released.
 * Each tick renders a connected stream from the hand; damage is applied in small pulses to the
 * mirrored Skyrim actor under the crosshair.
 */
public final class RangedMagic {
    private static final double RANGE = 18.0;
    private static final int DAMAGE_INTERVAL = 4; // five damage pulses per second at 20 TPS

    public static Item FLAMES;
    public static Item FROSTBITE;

    private RangedMagic() {}

    public static void init() {
        FLAMES = register("flames", Spell.FLAMES);
        FROSTBITE = register("frostbite", Spell.FROSTBITE);

        ServerTickEvents.END_SERVER_TICK.register(RangedMagic::idleHandEffects);

        CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) -> {
            dispatcher.register(Commands.literal("skymagic")
                .then(Commands.literal("demo").executes(context -> {
                    ServerPlayer player = context.getSource().getPlayerOrException();
                    player.getInventory().add(new ItemStack(FLAMES));
                    player.getInventory().add(new ItemStack(FROSTBITE));
                    player.sendSystemMessage(Component.literal(
                        "SkyCraft magic demo added: Flames + Frostbite. Equip one and hold right-click to channel."));
                    return 1;
                })));
        });
    }

    private static Item register(String path, Spell spell) {
        Identifier id = Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, path);
        ResourceKey<Item> key = ResourceKey.create(Registries.ITEM, id);
        return Registry.register(BuiltInRegistries.ITEM, id,
            new ChannelSpellItem(new Item.Properties().setId(key).stacksTo(1), spell));
    }

    private static final class ChannelSpellItem extends Item {
        private final Spell spell;

        ChannelSpellItem(Properties properties, Spell spell) {
            super(properties);
            this.spell = spell;
        }

        @Override
        public InteractionResult use(Level level, net.minecraft.world.entity.player.Player player, InteractionHand hand) {
            player.startUsingItem(hand);
            return InteractionResult.CONSUME;
        }

        @Override
        public int getUseDuration(ItemStack stack, LivingEntity user) {
            return 72000;
        }

        @Override
        public ItemUseAnimation getUseAnimation(ItemStack stack) {
            // BOW gives us a stable held/extended casting pose while the spell item itself is hidden.
            return ItemUseAnimation.BOW;
        }

        @Override
        public void onUseTick(Level level, LivingEntity living, ItemStack stack, int ticksRemaining) {
            if (!(level instanceof ServerLevel serverLevel) || !(living instanceof ServerPlayer player)) {
                return;
            }
            channelTick(serverLevel, player, spell, ticksRemaining);
        }
    }

    private static void channelTick(ServerLevel level, ServerPlayer player, Spell spell, int ticksRemaining) {
        Vec3 from = handPosition(player);
        Vec3 aim = player.getLookAngle();
        SkyrimActorEntity target = SkyCombat.findTarget(player, RANGE);
        Vec3 to = target != null
            ? target.getBoundingBox().getCenter()
            : from.add(aim.scale(RANGE));

        spawnStream(level, from, to, spell);

        if (target != null && ticksRemaining % DAMAGE_INTERVAL == 0) {
            boolean hurt = target.hurtServer(level, level.damageSources().indirectMagic(player, player), spell.damagePerPulse);
            if (hurt && spell == Spell.FLAMES) {
                target.igniteForSeconds(1.0F);
            }
            if (hurt) {
                spawnImpact(level, target.getBoundingBox().getCenter(), spell);
            }
        }
    }

    /** Low-key particles while a spell is equipped but not being channeled, like Skyrim's ready hand. */
    private static void idleHandEffects(MinecraftServer server) {
        for (ServerPlayer player : server.getPlayerList().getPlayers()) {
            if (!player.isAlive() || player.isUsingItem()) {
                continue;
            }
            Spell spell = spellFor(player.getMainHandItem());
            if (spell == null) {
                spell = spellFor(player.getOffhandItem());
            }
            if (spell == null) {
                continue;
            }

            // Only every other tick: visible aura without turning the idle hand into a particle cloud.
            if ((player.tickCount & 1) != 0) {
                continue;
            }
            ServerLevel level = player.level();
            Vec3 hand = handPosition(player);
            if (spell == Spell.FLAMES) {
                level.sendParticles(ParticleTypes.FLAME, hand.x, hand.y, hand.z, 1, 0.07, 0.07, 0.07, 0.002);
                if ((player.tickCount & 7) == 0) {
                    level.sendParticles(ParticleTypes.SMOKE, hand.x, hand.y, hand.z, 1, 0.05, 0.05, 0.05, 0.0);
                }
            } else {
                level.sendParticles(ParticleTypes.SNOWFLAKE, hand.x, hand.y, hand.z, 2, 0.08, 0.08, 0.08, 0.002);
                if ((player.tickCount & 7) == 0) {
                    level.sendParticles(ParticleTypes.END_ROD, hand.x, hand.y, hand.z, 1, 0.04, 0.04, 0.04, 0.0);
                }
            }
        }
    }

    private static Spell spellFor(ItemStack stack) {
        if (stack.is(FLAMES)) return Spell.FLAMES;
        if (stack.is(FROSTBITE)) return Spell.FROSTBITE;
        return null;
    }

    /**
     * Approximate the Minecraft player's casting hand in world space.
     * It deliberately starts beside/below the eye instead of at the crosshair so the stream
     * visibly originates from the arm.
     */
    private static Vec3 handPosition(ServerPlayer player) {
        Vec3 eye = player.getEyePosition();
        Vec3 look = player.getLookAngle();
        Vec3 right = new Vec3(-look.z, 0.0, look.x);
        if (right.lengthSqr() > 1.0E-5) {
            right = right.normalize();
        }
        return eye.add(look.scale(0.38)).add(right.scale(0.30)).add(0.0, -0.34, 0.0);
    }

    private static void spawnStream(ServerLevel level, Vec3 from, Vec3 to, Spell spell) {
        Vec3 delta = to.subtract(from);
        double length = delta.length();
        // Dense enough to read as one continuous spray instead of separated projectiles.
        int steps = Math.max(3, (int) Math.ceil(length * 2.4));
        for (int i = 0; i <= steps; i++) {
            double t = (double) i / steps;
            Vec3 p = from.add(delta.scale(t));
            double spread = 0.015 + t * 0.10;
            if (spell == Spell.FLAMES) {
                level.sendParticles(ParticleTypes.FLAME, p.x, p.y, p.z, 2, spread, spread, spread, 0.004);
                if ((i & 2) == 0) {
                    level.sendParticles(ParticleTypes.SMOKE, p.x, p.y, p.z, 1, spread * 0.6, spread * 0.6, spread * 0.6, 0.002);
                }
            } else {
                level.sendParticles(ParticleTypes.SNOWFLAKE, p.x, p.y, p.z, 2, spread, spread, spread, 0.003);
                if ((i & 3) == 0) {
                    level.sendParticles(ParticleTypes.END_ROD, p.x, p.y, p.z, 1, spread * 0.45, spread * 0.45, spread * 0.45, 0.001);
                }
            }
        }
    }

    private static void spawnImpact(ServerLevel level, Vec3 at, Spell spell) {
        if (spell == Spell.FLAMES) {
            level.sendParticles(ParticleTypes.FLAME, at.x, at.y, at.z, 6, 0.22, 0.30, 0.22, 0.02);
        } else {
            level.sendParticles(ParticleTypes.SNOWFLAKE, at.x, at.y, at.z, 8, 0.25, 0.32, 0.25, 0.015);
        }
    }

    public enum Spell {
        FLAMES(0.9F),
        FROSTBITE(0.7F);

        final float damagePerPulse;

        Spell(float damagePerPulse) {
            this.damagePerPulse = damagePerPulse;
        }
    }
}
