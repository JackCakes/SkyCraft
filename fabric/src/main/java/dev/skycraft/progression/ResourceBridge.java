package dev.skycraft.progression;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.util.Prediction;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;

/**
 * Grants resources requested by the player's own Skyrim. The Skyrim side keeps its original item
 * until it receives an acknowledgement, so a missing link or rejected request is non-destructive.
 */
public final class ResourceBridge {
	private static final int MAX_REQUEST = 4096;

	private ResourceBridge() {
	}

	public static int grant(ServerPlayer player, int kind, int requested) {
		if (player == null || requested <= 0) {
			return 0;
		}
		int count = Math.min(requested, MAX_REQUEST);
		Item item = switch (kind) {
			case Proto.RESOURCE_IRON_INGOT -> Items.IRON_INGOT;
			case Proto.RESOURCE_GOLD_INGOT -> Items.GOLD_INGOT;
			default -> null;
		};
		if (item == null) {
			SkyCraft.LOG.warn("SkyCraft progression: unknown resource kind {}", kind);
			return 0;
		}

		int accepted = 0;
		int remaining = count;
		while (remaining > 0) {
			int chunk = Math.min(remaining, 64);
			ItemStack stack = new ItemStack(item, chunk);
			player.getInventory().add(stack);
			int overflow = stack.getCount();
			accepted += chunk - overflow;

			// Full Minecraft inventory must not destroy a Skyrim item. Overflow becomes a normal
			// Minecraft item entity at the player's feet and is still considered accepted.
			if (overflow > 0) {
				var dropped = player.drop(stack, false, Prediction.SERVER_ONLY);
				if (dropped != null) {
					accepted += overflow;
				}
			}
			remaining -= chunk;
		}

		SkyCraft.LOG.info("SkyCraft progression: granted resource {} x{} to {}", kind, accepted, player.getPlainTextName());
		return accepted;
	}
}
