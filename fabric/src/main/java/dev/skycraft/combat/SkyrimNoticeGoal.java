package dev.skycraft.combat;

import dev.skycraft.link.SkyLink;
import java.util.List;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.MobCategory;
import net.minecraft.world.entity.ai.goal.Goal;
import java.util.EnumSet;

/**
 * A Minecraft mob that has noticed a Skyrim NPC looks at it, the way it looks at a player who comes
 * near: a pig turns its head to follow whoever walks past, a zombie with nothing to fight eyes
 * the nearest passer-by. Minecraft's own look-at-player goal only knows players (and doesn't
 * consider an invisible stand-in), so this is the same thing for Skyrim's NPCs: within a few blocks,
 * in sight, now and then, for two to four seconds at a time.
 *
 * <p>Turn it off with the JVM argument {@code -Dskycraft.mobsNoticeNpcs=false}.
 */
public final class SkyrimNoticeGoal extends Goal {
	private static final boolean ENABLED = !"false".equalsIgnoreCase(System.getProperty("skycraft.mobsNoticeNpcs"));
	/** Minecraft's chance per tick that an idle mob takes notice of someone within range. */
	private static final float CHANCE = 0.02F;

	private final Mob mob;
	private final double range;
	private SkyrimActorEntity watched;
	private int lookTime;

	public SkyrimNoticeGoal(Mob mob) {
		this.mob = mob;
		// Minecraft's monsters look from 8 blocks, its animals from 6.
		this.range = mob.getType().getCategory() == MobCategory.MONSTER ? 8.0 : 6.0;
		this.setFlags(EnumSet.of(Goal.Flag.LOOK));
	}

	public static boolean wants(Mob mob) {
		MobCategory category = mob.getType().getCategory();
		return ENABLED && (category == MobCategory.MONSTER || category == MobCategory.CREATURE);
	}

	@Override
	public boolean canUse() {
		if (!SkyLink.active() || !(this.mob.level() instanceof ServerLevel) || this.mob.getTarget() != null || this.mob.getRandom().nextFloat() >= CHANCE) {
			return false;
		}
		List<SkyrimActorEntity> near = this.mob.level().getEntitiesOfClass(SkyrimActorEntity.class, this.mob.getBoundingBox().inflate(this.range, 3.0, this.range));
		SkyrimActorEntity best = null;
		double bestDistance = this.range * this.range;
		for (SkyrimActorEntity actor : near) {
			double distance = this.mob.distanceToSqr(actor);
			if (actor.isAlive() && distance < bestDistance && SkyrimMobs.canSee(this.mob, actor)) {
				best = actor;
				bestDistance = distance;
			}
		}
		this.watched = best;
		return best != null;
	}

	@Override
	public boolean canContinueToUse() {
		return this.watched != null && this.watched.isAlive() && !this.watched.isRemoved() && this.mob.getTarget() == null && this.lookTime > 0
			&& this.mob.distanceToSqr(this.watched) <= this.range * this.range;
	}

	@Override
	public void start() {
		this.lookTime = 40 + this.mob.getRandom().nextInt(40);
	}

	@Override
	public void stop() {
		this.watched = null;
	}

	// No @Override: not every version of Minecraft has this one (see SkyrimRoamGoal).
	public boolean requiresUpdateEveryTick() {
		return true;
	}

	@Override
	public void tick() {
		if (this.watched != null) {
			this.mob.getLookControl().setLookAt(this.watched.getX(), this.watched.getEyeY(), this.watched.getZ());
			this.lookTime--;
		}
	}
}
