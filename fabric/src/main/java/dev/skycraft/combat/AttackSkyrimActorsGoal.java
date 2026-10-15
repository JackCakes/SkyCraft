package dev.skycraft.combat;

import dev.skycraft.link.SkyLink;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Set;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.MobCategory;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.entity.ai.goal.Goal;
import net.minecraft.world.item.BowItem;
import net.minecraft.world.item.CrossbowItem;
import net.minecraft.world.item.Item;

/**
 * Hostile Minecraft mobs go for Skyrim's NPCs as eagerly as they go for a player. A mob with no
 * one else to fight picks the nearest NPC stand-in ({@link SkyrimActorEntity}) as its target, from
 * as far as Skyrim's land is known around it, whether or not the NPC has done anything to it, and
 * then its own attack goals do the rest: shooting, swelling. A player in reach still takes priority
 * (their usual targeting goals replace this target).
 *
 * <p>Getting the mob to its target over Skyrim's ground is {@link SkyrimChaseGoal}'s job, since
 * Minecraft's pathfinding can't see it. Landing the blow is partly this goal's: Minecraft's melee goal
 * lets go of a target it has no route to, which on Skyrim's ground it never has, so a zombie
 * standing at an NPC would swing only now and then. While this goal has a melee mob in reach of
 * its NPC it strikes with the mob's own attack ({@code doHurtTarget}: its damage, knockback and
 * enchantments, through Minecraft's damage rules) once a second, as a mob does a player. The NPC's
 * invulnerability frames keep that and the mob's own goal from doubling up. This goal holds no goal
 * flags, so it never keeps the mob's own attack goals from running.
 *
 * <p>Turn it off with the JVM argument {@code -Dskycraft.mobsAttackNpcs=false}.
 */
public final class AttackSkyrimActorsGoal extends Goal {
	/** Skyrim sends its geometry about this far, so a mob can't be shown to see farther than this. */
	private static final double RANGE = 24.0;
	/** How far above or below a mob an NPC may be and still be gone after. */
	private static final double HEIGHT = 12.0;
	/** The gap between a mob's body and its NPC's within which it can strike, in blocks. */
	private static final double STRIKE_REACH = 1.4;
	private static final long STRIKE_EVERY = 20;
	private static final boolean ENABLED = !"false".equalsIgnoreCase(System.getProperty("skycraft.mobsAttackNpcs"));
	// Mobs run by a brain or by their own scripted attacks: they choose their own targets.
	private static final Set<String> OWN_AI = Set.of(
		"piglin", "piglin_brute", "zoglin", "hoglin", "warden", "breeze", "creaking", "enderman", "shulker", "ender_dragon", "wither", "giant"
	);

	private final Mob mob;
	private SkyrimActorEntity chosen;
	private long nextStrike;

	public AttackSkyrimActorsGoal(Mob mob) {
		this.mob = mob;
	}

	/** Hostile, goal-driven mobs. */
	public static boolean wants(Mob mob) {
		// (A creeper only ever goes for the player: it doesn't swell at an NPC, and its blast spares them, see SkyrimActorEntity.)
		return ENABLED
			&& mob.getType().getCategory() == MobCategory.MONSTER
			&& !OWN_AI.contains(BuiltInRegistries.ENTITY_TYPE.getKey(mob.getType()).getPath())
			&& !"creeper".equals(BuiltInRegistries.ENTITY_TYPE.getKey(mob.getType()).getPath());
	}

	@Override
	public boolean canUse() {
		if (!SkyLink.active() || !(this.mob.level() instanceof ServerLevel) || this.mob.getTarget() != null || this.mob.isPassenger()
			|| this.mob.tickCount % 5 != 0) {
			return false;
		}
		List<SkyrimActorEntity> candidates = new ArrayList<>();
		for (SkyrimActorEntity actor : this.mob.level().getEntitiesOfClass(SkyrimActorEntity.class, this.mob.getBoundingBox().inflate(RANGE, HEIGHT, RANGE))) {
			if (actor.isAlive() && this.mob.distanceToSqr(actor) < RANGE * RANGE && Math.abs(actor.getY() - this.mob.getY()) < HEIGHT) {
				candidates.add(actor);
			}
		}
		if (candidates.isEmpty()) {
			return false;
		}
		candidates.sort(Comparator.comparingDouble((SkyrimActorEntity a) -> this.mob.distanceToSqr(a)));
		// The nearest one it can see; failing that, simply the nearest: it heard it, and goes round whatever is in the way.
		SkyrimActorEntity best = candidates.getFirst();
		for (int i = 0; i < Math.min(4, candidates.size()); i++) {
			if (SkyrimMobs.canSee(this.mob, candidates.get(i))) {
				best = candidates.get(i);
				break;
			}
		}
		this.chosen = best;
		return true;
	}

	@Override
	public boolean canContinueToUse() {
		LivingEntity target = this.mob.getTarget();
		return SkyLink.active() && target != null && target == this.chosen && target.isAlive() && !target.isRemoved()
			&& this.mob.distanceToSqr(target) < RANGE * RANGE * 2.25;
	}

	@Override
	public void start() {
		this.mob.setTarget(this.chosen);
		this.nextStrike = 0L;
	}

	@Override
	public void stop() {
		if (this.mob.getTarget() == this.chosen) {
			this.mob.setTarget(null);
		}
		this.chosen = null;
	}

	// No @Override: not every version of Minecraft has this one (see SkyrimRoamGoal).
	public boolean requiresUpdateEveryTick() {
		return true;
	}

	@Override
	public void tick() {
		if (!(this.mob.getTarget() instanceof SkyrimActorEntity npc) || !(this.mob.level() instanceof ServerLevel level) || !isMelee(this.mob)) {
			return;
		}
		long now = level.getGameTime();
		if (now < this.nextStrike || !npc.isAlive()) {
			return;
		}
		double gap = Math.hypot(this.mob.getX() - npc.getX(), this.mob.getZ() - npc.getZ()) - this.mob.getBbWidth() * 0.5 - npc.getBbWidth() * 0.5;
		if (gap > STRIKE_REACH || Math.abs(this.mob.getY() - npc.getY()) > Math.max(this.mob.getBbHeight(), npc.getBbHeight())) {
			return;
		}
		if (!SkyrimMobs.canSee(this.mob, npc)) {
			return;
		}
		this.mob.getLookControl().setLookAt(npc, 30.0F, 30.0F);
		this.mob.swing(InteractionHand.MAIN_HAND, this.mob.getMainHandItem().getAttackAnimation());
		this.mob.doHurtTarget(level, npc);
		this.nextStrike = now + STRIKE_EVERY;
	}

	/** A mob that fights with its body or a held weapon (not one that shoots, or swells like a creeper). */
	private static boolean isMelee(Mob mob) {
		if (mob.getAttribute(Attributes.ATTACK_DAMAGE) == null || !SkyrimMobs.walker(mob)) {
			return false;
		}
		Item held = mob.getMainHandItem().getItem();
		return !(held instanceof BowItem || held instanceof CrossbowItem) && !"creeper".equals(BuiltInRegistries.ENTITY_TYPE.getKey(mob.getType()).getPath());
	}
}
