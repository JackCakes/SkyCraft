package dev.skycraft.combat;

import dev.skycraft.link.Proto;
import net.minecraft.network.syncher.EntityDataAccessor;
import net.minecraft.network.syncher.EntityDataSerializers;
import net.minecraft.network.syncher.SynchedEntityData;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.sounds.SoundEvent;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityDimensions;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.HumanoidArm;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.Pose;
import net.minecraft.tags.ItemTags;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.Level;
import org.jspecify.annotations.Nullable;

/**
 * An invisible stand-in for one Skyrim actor, so Minecraft's own combat (swords, crits, sweeps,
 * enchantments, attack cooldown, bows, tridents) can target and hit Skyrim NPCs. What it receives is
 * collected into one hit per tick and forwarded to the real actor; its own health never drops.
 */
public class SkyrimActorEntity extends LivingEntity {
	private static final EntityDataAccessor<Integer> FORM_ID = SynchedEntityData.defineId(SkyrimActorEntity.class, EntityDataSerializers.INT);
	private static final EntityDataAccessor<Float> WIDTH = SynchedEntityData.defineId(SkyrimActorEntity.class, EntityDataSerializers.FLOAT);
	private static final EntityDataAccessor<Float> HEIGHT = SynchedEntityData.defineId(SkyrimActorEntity.class, EntityDataSerializers.FLOAT);

	// This tick's hit, flushed to Skyrim by SkyCombat after all attacks for the tick have landed
	// (Player.attack adds its sprint/enchantment knockback after hurtServer returns).
	private float pendingDamage;
	private int pendingFlags;
	private int pendingWeapon;
	/** Entity id + 1 of the mob that last hurt this actor (0: none); Skyrim attributes the hit to that mob's stand-in. */
	private int pendingMobId;
	private double pushX, pushZ;
	private float pushStrength;
	private boolean hitThisTick;
	// Who dealt this tick's damage: Minecraft mobs, and anything else (the player, fire, explosions
	// with no owner). Skyrim is told it was a mob's hit only if nothing else contributed.
	private boolean mobHit, otherHit;
	// The actor's Proto.ACTOR_* flags from Skyrim's last update (server side only).
	private int skyFlags;

	// Entity.invulnerableTime is private in this Minecraft; it isn't obfuscated, so reach it by name.
	private static final java.lang.reflect.Field IMMUNITY = findImmunity();

	private static java.lang.reflect.Field findImmunity() {
		for (Class<?> c = SkyrimActorEntity.class; c != null; c = c.getSuperclass()) {
			try {
				java.lang.reflect.Field f = c.getDeclaredField("invulnerableTime");
				f.setAccessible(true);
				return f;
			} catch (ReflectiveOperationException | RuntimeException e) {
				// try the parent class
			}
		}
		dev.skycraft.SkyCraft.LOG.warn("SkyCraft: can't reach Entity.invulnerableTime; Skyrim NPCs may shrug off repeated hits");
		return null;
	}

	private static void tickImmunity(Entity entity) {
		if (IMMUNITY == null) {
			return;
		}
		try {
			int left = IMMUNITY.getInt(entity);
			if (left > 0) {
				IMMUNITY.setInt(entity, left - 1);
			}
		} catch (ReflectiveOperationException | RuntimeException e) {
			// leave it
		}
	}

	public SkyrimActorEntity(EntityType<? extends SkyrimActorEntity> type, Level level) {
		super(type, level);
		this.setNoGravity(true);
		this.noPhysics = true;
		this.setInvisible(true);
		this.setSilent(true);
	}

	public void setSkyFlags(int flags) {
		this.skyFlags = flags;
	}

	/** One that fights (not a civilian who flees): see NpcFightsBack. */
	public boolean fighter() {
		return (this.skyFlags & Proto.ACTOR_FIGHTER) != 0;
	}

	/** A person (human, elf, orc...), not an animal or creature. */
	public boolean person() {
		return (this.skyFlags & Proto.ACTOR_PERSON) != 0;
	}

	/** Hostile to the player right now (a bandit, not a shopkeeper). */
	public boolean hostileToPlayer() {
		return (this.skyFlags & Proto.ACTOR_HOSTILE) != 0;
	}

	/** Holds a bow or crossbow. */
	public boolean bow() {
		return (this.skyFlags & Proto.ACTOR_BOW) != 0;
	}

	/** Skyrim's damage for the weapon in its hand (fists when none), as reported in the actor table. */
	public int weaponDamage() {
		return (this.skyFlags >> Proto.ACTOR_DAMAGE_SHIFT) & 0xFF;
	}

	/** Its weapon is out of its sheath. */
	public boolean weaponDrawn() {
		return (this.skyFlags & Proto.ACTOR_WEAPON_DRAWN) != 0;
	}

	/** Already in a fight Skyrim runs itself. */
	public boolean inSkyrimCombat() {
		return (this.skyFlags & Proto.ACTOR_IN_COMBAT) != 0;
	}

	public int formId() {
		return this.entityData.get(FORM_ID);
	}

	public void setFormId(int formId) {
		this.entityData.set(FORM_ID, formId);
	}

	@Override
	protected void defineSynchedData(SynchedEntityData.Builder builder) {
		super.defineSynchedData(builder);
		builder.define(FORM_ID, 0);
		builder.define(WIDTH, 0.6F);
		builder.define(HEIGHT, 1.8F);
	}

	public void setSize(float width, float height) {
		if (Math.abs(this.entityData.get(WIDTH) - width) > 0.01F || Math.abs(this.entityData.get(HEIGHT) - height) > 0.01F) {
			this.entityData.set(WIDTH, width);
			this.entityData.set(HEIGHT, height);
			this.refreshDimensions();
		}
	}

	@Override
	public void onSyncedDataUpdated(EntityDataAccessor<?> accessor) {
		super.onSyncedDataUpdated(accessor);
		if (WIDTH.equals(accessor) || HEIGHT.equals(accessor)) {
			this.refreshDimensions();
		}
	}

	@Override
	protected EntityDimensions getDefaultDimensions(Pose pose) {
		return EntityDimensions.scalable(this.entityData.get(WIDTH), this.entityData.get(HEIGHT));
	}

	private static int mobHurtLogs;

	private static boolean isCreeper(@org.jspecify.annotations.Nullable Entity entity) {
		return entity != null && "creeper".equals(net.minecraft.core.registries.BuiltInRegistries.ENTITY_TYPE.getKey(entity.getType()).getPath());
	}

	@Override
	public boolean hurtServer(ServerLevel level, DamageSource source, float damage) {
		// A creeper's blast doesn't hurt Skyrim's people: it goes off for the player, and an NPC standing nearby is left alone.
		if (source.is(net.minecraft.tags.DamageTypeTags.IS_EXPLOSION) && (isCreeper(source.getEntity()) || isCreeper(source.getDirectEntity()))) {
			return false;
		}
		boolean hurt = super.hurtServer(level, source, damage);
		if (source.getEntity() instanceof Mob attacker && mobHurtLogs++ < 60) {
			dev.skycraft.SkyCraft.LOG.info("SkyCraft: {} ({}) attacked Skyrim actor {} for {}: {}", attacker.getName().getString(), source.type().msgId(),
				String.format("%08X", this.formId()), damage, hurt ? "landed" : "refused");
		}
		return hurt;
	}

	@Override
	protected void actuallyHurt(ServerLevel level, DamageSource source, float dmg) {
		// Minecraft has applied everything (crit, sharpness, strength, cooldown, invulnerability
		// frames). Hand the result to Skyrim instead of lowering our own health.
		if (this.isInvulnerableTo(level, source) || dmg <= 0.0F) {
			return;
		}
		this.pendingDamage += dmg;
		if (source.getDirectEntity() instanceof Projectile) {
			this.pendingFlags |= Proto.HIT_PROJECTILE;
		}
		this.pendingWeapon = weaponClass(source);
		// Only the player (or something the player threw or shot) is "the player's hit": that is what earns a bounty
		// and starts a fight with the player in Skyrim. Everything else is nobody's fault in Skyrim's eyes: a mob's
		// blow, a witch's potion, and also the poison, fire or fall damage that has no attacker at all.
		Entity attacker = source.getEntity();
		if (attacker instanceof net.minecraft.world.entity.player.Player
			|| (attacker == null && source.getDirectEntity() instanceof net.minecraft.world.entity.player.Player)) {
			this.otherHit = true;
		} else {
			this.mobHit = true;
			if (attacker instanceof Mob mobAttacker) {
				this.pendingMobId = mobAttacker.getId() + 1;
			}
		}
		if (source.is(net.minecraft.tags.DamageTypeTags.IS_FIRE)) {
			this.pendingFlags |= Proto.HIT_FIRE;
		}
		this.hitThisTick = true;
		this.getCombatTracker().recordDamage(source, dmg);
	}

	@Override
	public void knockback(double power, double xd, double zd, DamageSource source, float damage, boolean comesFromEffect) {
		// Skyrim owns this actor's position. Remember the strongest push for Skyrim's stagger:
		// Minecraft pushes towards -(xd, zd).
		double len = Math.sqrt(xd * xd + zd * zd);
		if (len > 1e-6 && power > this.pushStrength) {
			this.pushStrength = (float) power;
			this.pushX = -xd / len;
			this.pushZ = -zd / len;
		}
		this.hitThisTick = true;
	}

	/** Player.crit() was called on us this tick. */
	public void markCritical() {
		this.pendingFlags |= Proto.HIT_CRITICAL;
	}

	/** Which kind of Skyrim weapon impact this hit should look and sound like. */
	private static int weaponClass(DamageSource source) {
		if (source.getDirectEntity() instanceof net.minecraft.world.entity.projectile.arrow.ThrownTrident) {
			return Proto.WEAPON_PIERCE;
		}
		if (source.getDirectEntity() instanceof Projectile) {
			return Proto.WEAPON_ARROW;
		}
		ItemStack weapon = source.getWeaponItem();
		if (weapon == null && source.getEntity() instanceof LivingEntity attacker) {
			weapon = attacker.getMainHandItem();
		}
		if (weapon == null || weapon.isEmpty()) {
			return Proto.WEAPON_UNARMED;
		}
		if (weapon.is(ItemTags.SWORDS)) {
			return Proto.WEAPON_BLADE;
		}
		if (weapon.is(ItemTags.AXES)) {
			return Proto.WEAPON_AXE;
		}
		if (weapon.is(Items.TRIDENT)) {
			return Proto.WEAPON_PIERCE;
		}
		return Proto.WEAPON_BLUNT;
	}

	/** Returns this tick's hit (damage, flags, push, weapon) and clears it; null if nothing hit us. */
	public float[] takeHit() {
		if (!this.hitThisTick) {
			return null;
		}
		int flags = this.pendingFlags | (this.mobHit && !this.otherHit ? Proto.HIT_FROM_MOB : 0);
		float[] hit = { this.pendingDamage, (float) this.pushX, (float) this.pushZ, this.pushStrength, Float.intBitsToFloat(flags),
			Float.intBitsToFloat(this.pendingWeapon | ((flags & Proto.HIT_FROM_MOB) != 0 ? (this.pendingMobId & 0xFFFFFF) << 8 : 0)) };
		this.pendingMobId = 0;
		this.mobHit = false;
		this.otherHit = false;
		this.pendingDamage = 0.0F;
		this.pendingFlags = 0;
		this.pushX = this.pushZ = 0.0;
		this.pushStrength = 0.0F;
		this.hitThisTick = false;
		return hit;
	}

	@Override
	public void tick() {
		// Position and rotation come from Skyrim (SkyCombat); keep hurt timers and fire ticking.
		this.baseTick();
		// aiStep never runs for a stand-in, and that is what counts the post-hit immunity and hurt
		// timers down: without this the first hit lands and every later one (a mob's, an arrow) bounces off.
		tickImmunity(this);
		if (this.hurtTime > 0) {
			this.hurtTime--;
		}
		this.setHealth(this.getMaxHealth());
	}

	@Override
	public boolean isPushable() {
		return false;
	}

	@Override
	protected void doPush(Entity entity) {
	}

	@Override
	public boolean canBeCollidedWith(@Nullable Entity other) {
		return false;
	}

	@Override
	public boolean shouldShowName() {
		return false;
	}

	@Override
	public boolean shouldBeSaved() {
		return false;
	}

	@Override
	protected @Nullable SoundEvent getHurtSound(DamageSource source) {
		return null; // Skyrim plays the NPC's own pain sounds
	}

	@Override
	protected @Nullable SoundEvent getDeathSound() {
		return null;
	}

	@Override
	public HumanoidArm getMainArm() {
		return HumanoidArm.RIGHT;
	}
}
