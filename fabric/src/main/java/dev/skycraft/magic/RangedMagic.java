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
import net.minecraft.world.effect.MobEffectInstance;
import net.minecraft.world.effect.MobEffects;
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
    public static Item SPARKS;
    public static Item HEALING;
    public static Item GREATER_HEALING;
    public static Item LESSER_WARD;
    public static Item OAKFLESH;
    public static Item CANDLELIGHT;
    public static Item FIREBOLT;
    public static Item ICE_SPIKE;
    public static Item LIGHTNING_BOLT;

    private RangedMagic() {}

    public static void init() {
        FLAMES = register("flames", Spell.FLAMES);
        FROSTBITE = register("frostbite", Spell.FROSTBITE);
        SPARKS = register("sparks", Spell.SPARKS);
        HEALING = register("healing", Spell.HEALING);
        GREATER_HEALING = register("greater_healing", Spell.GREATER_HEALING);
        LESSER_WARD = register("lesser_ward", Spell.LESSER_WARD);
        OAKFLESH = register("oakflesh", Spell.OAKFLESH);
        CANDLELIGHT = register("candlelight", Spell.CANDLELIGHT);
        FIREBOLT = register("firebolt", Spell.FIREBOLT);
        ICE_SPIKE = register("ice_spike", Spell.ICE_SPIKE);
        LIGHTNING_BOLT = register("lightning_bolt", Spell.LIGHTNING_BOLT);

        ServerTickEvents.END_SERVER_TICK.register(RangedMagic::idleHandEffects);

        CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) -> {
            dispatcher.register(Commands.literal("skymagic")
                .then(Commands.literal("demo").executes(context -> {
                    ServerPlayer player = context.getSource().getPlayerOrException();
                    player.getInventory().add(new ItemStack(FLAMES));
                    player.getInventory().add(new ItemStack(FROSTBITE));
                    player.getInventory().add(new ItemStack(SPARKS));
                    player.getInventory().add(new ItemStack(HEALING));
                    player.getInventory().add(new ItemStack(GREATER_HEALING));
                    player.getInventory().add(new ItemStack(LESSER_WARD));
                    player.getInventory().add(new ItemStack(OAKFLESH));
                    player.getInventory().add(new ItemStack(CANDLELIGHT));
                    player.getInventory().add(new ItemStack(FIREBOLT));
                    player.getInventory().add(new ItemStack(ICE_SPIKE));
                    player.getInventory().add(new ItemStack(LIGHTNING_BOLT));
                    player.sendSystemMessage(Component.literal(
                        "SkyCraft magic demo added: novice set plus Firebolt, Ice Spike and Lightning Bolt."));
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
            if (spell == Spell.FIREBOLT || spell == Spell.ICE_SPIKE || spell == Spell.LIGHTNING_BOLT) {
                if (player instanceof ServerPlayer serverPlayer && level instanceof ServerLevel serverLevel) {
                    castBolt(serverLevel, serverPlayer, spell);
                }
                return InteractionResult.SUCCESS;
            }
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

        if (spell == Spell.HEALING || spell == Spell.GREATER_HEALING) {
            spawnHealing(level, player, from, spell == Spell.GREATER_HEALING);
            if (ticksRemaining % 5 == 0 && player.getHealth() < player.getMaxHealth()) {
                player.heal(spell == Spell.GREATER_HEALING ? 1.2F : 0.6F);
            }
            return;
        }

        if (spell == Spell.LESSER_WARD) {
            // Keep a short resistance effect refreshed only while the ward is actively held.
            player.addEffect(new MobEffectInstance(MobEffects.RESISTANCE, 6, 0, true, false));
            spawnWard(level, player, from);
            return;
        }

        if (spell == Spell.OAKFLESH) {
            // Early Alteration equivalent: a one-minute protective skin buff.
            player.addEffect(new MobEffectInstance(MobEffects.RESISTANCE, 20 * 60, 0, true, false));
            spawnOakflesh(level, player);
            player.stopUsingItem();
            return;
        }

        if (spell == Spell.CANDLELIGHT) {
            // Minecraft has no portable light orb without a block/entity; Night Vision is the
            // closest non-destructive equivalent and the cast still gets a visible light aura.
            player.addEffect(new MobEffectInstance(MobEffects.NIGHT_VISION, 20 * 60, 0, true, false));
            spawnCandlelight(level, player);
            player.stopUsingItem();
            return;
        }

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
            } else if (spell == Spell.FROSTBITE) {
                level.sendParticles(ParticleTypes.SNOWFLAKE, hand.x, hand.y, hand.z, 2, 0.08, 0.08, 0.08, 0.002);
                if ((player.tickCount & 7) == 0) {
                    level.sendParticles(ParticleTypes.END_ROD, hand.x, hand.y, hand.z, 1, 0.04, 0.04, 0.04, 0.0);
                }
            } else if (spell == Spell.SPARKS) {
                level.sendParticles(ParticleTypes.ELECTRIC_SPARK, hand.x, hand.y, hand.z, 2, 0.08, 0.08, 0.08, 0.01);
            } else if (spell == Spell.HEALING || spell == Spell.GREATER_HEALING) {
                level.sendParticles(ParticleTypes.HEART, hand.x, hand.y, hand.z, 1, 0.08, 0.08, 0.08, 0.0);
            } else if (spell == Spell.LESSER_WARD) {
                level.sendParticles(ParticleTypes.END_ROD, hand.x, hand.y, hand.z, 2, 0.10, 0.10, 0.10, 0.003);
            } else if (spell == Spell.OAKFLESH) {
                level.sendParticles(ParticleTypes.CRIT, hand.x, hand.y, hand.z, 1, 0.08, 0.08, 0.08, 0.0);
            } else if (spell == Spell.CANDLELIGHT) {
                level.sendParticles(ParticleTypes.END_ROD, hand.x, hand.y, hand.z, 2, 0.08, 0.08, 0.08, 0.004);
            } else if (spell == Spell.FIREBOLT) {
                level.sendParticles(ParticleTypes.FLAME, hand.x, hand.y, hand.z, 2, 0.06, 0.06, 0.06, 0.003);
            } else if (spell == Spell.ICE_SPIKE) {
                level.sendParticles(ParticleTypes.SNOWFLAKE, hand.x, hand.y, hand.z, 2, 0.06, 0.06, 0.06, 0.002);
            } else if (spell == Spell.LIGHTNING_BOLT) {
                level.sendParticles(ParticleTypes.ELECTRIC_SPARK, hand.x, hand.y, hand.z, 2, 0.06, 0.06, 0.06, 0.01);
            }
        }
    }

    private static Spell spellFor(ItemStack stack) {
        if (stack.is(FLAMES)) return Spell.FLAMES;
        if (stack.is(FROSTBITE)) return Spell.FROSTBITE;
        if (stack.is(SPARKS)) return Spell.SPARKS;
        if (stack.is(HEALING)) return Spell.HEALING;
        if (stack.is(GREATER_HEALING)) return Spell.GREATER_HEALING;
        if (stack.is(LESSER_WARD)) return Spell.LESSER_WARD;
        if (stack.is(OAKFLESH)) return Spell.OAKFLESH;
        if (stack.is(CANDLELIGHT)) return Spell.CANDLELIGHT;
        if (stack.is(FIREBOLT)) return Spell.FIREBOLT;
        if (stack.is(ICE_SPIKE)) return Spell.ICE_SPIKE;
        if (stack.is(LIGHTNING_BOLT)) return Spell.LIGHTNING_BOLT;
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
            } else if (spell == Spell.FROSTBITE) {
                level.sendParticles(ParticleTypes.SNOWFLAKE, p.x, p.y, p.z, 2, spread, spread, spread, 0.003);
                if ((i & 3) == 0) {
                    level.sendParticles(ParticleTypes.END_ROD, p.x, p.y, p.z, 1, spread * 0.45, spread * 0.45, spread * 0.45, 0.001);
                }
            } else if (spell == Spell.SPARKS) {
                level.sendParticles(ParticleTypes.ELECTRIC_SPARK, p.x, p.y, p.z, 3, spread, spread, spread, 0.015);
                if ((i & 3) == 0) {
                    level.sendParticles(ParticleTypes.END_ROD, p.x, p.y, p.z, 1, spread * 0.35, spread * 0.35, spread * 0.35, 0.002);
                }
            }
        }
    }

    private static void spawnImpact(ServerLevel level, Vec3 at, Spell spell) {
        if (spell == Spell.FLAMES) {
            level.sendParticles(ParticleTypes.FLAME, at.x, at.y, at.z, 6, 0.22, 0.30, 0.22, 0.02);
        } else if (spell == Spell.FROSTBITE) {
            level.sendParticles(ParticleTypes.SNOWFLAKE, at.x, at.y, at.z, 8, 0.25, 0.32, 0.25, 0.015);
        } else if (spell == Spell.SPARKS) {
            level.sendParticles(ParticleTypes.ELECTRIC_SPARK, at.x, at.y, at.z, 10, 0.24, 0.30, 0.24, 0.025);
        }
    }

    private static void spawnHealing(ServerLevel level, ServerPlayer player, Vec3 hand, boolean greater) {
        level.sendParticles(ParticleTypes.HEART, hand.x, hand.y, hand.z, greater ? 4 : 2, 0.10, 0.10, 0.10, 0.0);
        level.sendParticles(ParticleTypes.END_ROD, hand.x, hand.y, hand.z, greater ? 5 : 2, 0.12, 0.12, 0.12, 0.005);
        Vec3 chest = player.position().add(0.0, player.getBbHeight() * 0.62, 0.0);
        level.sendParticles(ParticleTypes.HEART, chest.x, chest.y, chest.z, 1, 0.18, 0.22, 0.18, 0.0);
    }

    private static void castBolt(ServerLevel level, ServerPlayer player, Spell spell) {
        Vec3 from = handPosition(player);
        SkyrimActorEntity target = SkyCombat.findTarget(player, 36.0);
        Vec3 to = target != null ? target.getBoundingBox().getCenter()
            : from.add(player.getLookAngle().scale(36.0));

        Vec3 delta = to.subtract(from);
        int steps = Math.max(4, (int) Math.ceil(delta.length() * 3.0));
        for (int i = 0; i <= steps; i++) {
            Vec3 p = from.add(delta.scale((double) i / steps));
            if (spell == Spell.FIREBOLT) {
                level.sendParticles(ParticleTypes.FLAME, p.x, p.y, p.z, 1, 0.035, 0.035, 0.035, 0.002);
            } else if (spell == Spell.ICE_SPIKE) {
                level.sendParticles(ParticleTypes.SNOWFLAKE, p.x, p.y, p.z, 1, 0.025, 0.025, 0.025, 0.001);
                if ((i & 3) == 0) {
                    level.sendParticles(ParticleTypes.END_ROD, p.x, p.y, p.z, 1, 0.01, 0.01, 0.01, 0.0);
                }
            } else {
                level.sendParticles(ParticleTypes.ELECTRIC_SPARK, p.x, p.y, p.z, 2, 0.025, 0.025, 0.025, 0.012);
            }
        }

        if (target == null) {
            return;
        }

        float damage = spell == Spell.FIREBOLT ? 6.0F : spell == Spell.ICE_SPIKE ? 6.5F : 7.0F;
        boolean hurt = target.hurtServer(level, level.damageSources().indirectMagic(player, player), damage);
        if (hurt && spell == Spell.FIREBOLT) {
            target.igniteForSeconds(2.0F);
        }
        if (hurt) {
            spawnImpact(level, target.getBoundingBox().getCenter(),
                spell == Spell.FIREBOLT ? Spell.FLAMES : spell == Spell.ICE_SPIKE ? Spell.FROSTBITE : Spell.SPARKS);
        }
    }

    private static void spawnWard(ServerLevel level, ServerPlayer player, Vec3 hand) {
        Vec3 look = player.getLookAngle();
        Vec3 center = hand.add(look.scale(0.75));
        level.sendParticles(ParticleTypes.END_ROD, center.x, center.y, center.z, 8, 0.38, 0.48, 0.10, 0.002);
        level.sendParticles(ParticleTypes.ENCHANTED_HIT, center.x, center.y, center.z, 4, 0.30, 0.36, 0.08, 0.0);
    }

    private static void spawnOakflesh(ServerLevel level, ServerPlayer player) {
        Vec3 center = player.position().add(0.0, player.getBbHeight() * 0.55, 0.0);
        level.sendParticles(ParticleTypes.CRIT, center.x, center.y, center.z, 22, 0.42, 0.70, 0.42, 0.02);
        level.sendParticles(ParticleTypes.SMOKE, center.x, center.y, center.z, 6, 0.28, 0.55, 0.28, 0.005);
    }

    private static void spawnCandlelight(ServerLevel level, ServerPlayer player) {
        Vec3 above = player.getEyePosition().add(0.0, 0.65, 0.0);
        level.sendParticles(ParticleTypes.END_ROD, above.x, above.y, above.z, 18, 0.20, 0.20, 0.20, 0.01);
        level.sendParticles(ParticleTypes.FIREWORK, above.x, above.y, above.z, 8, 0.16, 0.16, 0.16, 0.01);
    }

    public enum Spell {
        FLAMES(0.9F),
        FROSTBITE(0.7F),
        SPARKS(0.8F),
        HEALING(0.0F),
        GREATER_HEALING(0.0F),
        LESSER_WARD(0.0F),
        OAKFLESH(0.0F),
        CANDLELIGHT(0.0F),
        FIREBOLT(0.0F),
        ICE_SPIKE(0.0F),
        LIGHTNING_BOLT(0.0F);

        final float damagePerPulse;

        Spell(float damagePerPulse) {
            this.damagePerPulse = damagePerPulse;
        }
    }
}
