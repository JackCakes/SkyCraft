package dev.skycraft.progression;

import dev.skycraft.SkyCraft;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.util.Prediction;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;

public final class ResourceBridge {
	private static final int MAX_REQUEST = 4096;
	private static final Map<Integer, Item> ITEMS_BY_HASH = new HashMap<>();
	private static final Set<Integer> HASH_COLLISIONS = new HashSet<>();
	private static boolean indexed;

	private ResourceBridge() {}

	public static int itemHash(String id) {
		int hash = 0x811C9DC5;
		for (int i = 0; i < id.length(); i++) {
			hash ^= id.charAt(i) & 0xFF;
			hash *= 0x01000193;
		}
		return hash;
	}

	private static synchronized void ensureIndex() {
		if (indexed) return;
		for (Item item : BuiltInRegistries.ITEM) {
			var id = BuiltInRegistries.ITEM.getKey(item);
			if (id == null) continue;
			int hash = itemHash(id.toString());
			Item old = ITEMS_BY_HASH.putIfAbsent(hash, item);
			if (old != null && old != item) {
				HASH_COLLISIONS.add(hash);
				ITEMS_BY_HASH.remove(hash);
				SkyCraft.LOG.error("SkyCraft progression: registry hash collision {} between {} and {}",
					Integer.toUnsignedString(hash), BuiltInRegistries.ITEM.getKey(old), id);
			}
		}
		indexed = true;
		SkyCraft.LOG.info("SkyCraft progression: indexed {} Minecraft items ({} hash collisions)",
			ITEMS_BY_HASH.size(), HASH_COLLISIONS.size());
	}

	public static int grant(ServerPlayer player, int itemHash, int requested) {
		if (player == null || requested <= 0) return 0;
		ensureIndex();
		if (HASH_COLLISIONS.contains(itemHash)) {
			SkyCraft.LOG.warn("SkyCraft progression: rejected ambiguous item hash {}", Integer.toUnsignedString(itemHash));
			return 0;
		}
		Item item = ITEMS_BY_HASH.get(itemHash);
		if (item == null) {
			SkyCraft.LOG.warn("SkyCraft progression: unknown item hash {}", Integer.toUnsignedString(itemHash));
			return 0;
		}

		int count = Math.min(requested, MAX_REQUEST);
		int accepted = 0;
		int remaining = count;
		while (remaining > 0) {
			int chunk = Math.min(remaining, 64);
			ItemStack stack = new ItemStack(item, chunk);
			player.getInventory().add(stack);
			int overflow = stack.getCount();
			accepted += chunk - overflow;
			if (overflow > 0) {
				var dropped = player.drop(stack, false, Prediction.SERVER_ONLY);
				if (dropped != null) accepted += overflow;
			}
			remaining -= chunk;
		}
		SkyCraft.LOG.info("SkyCraft progression: granted {} x{} to {}",
			BuiltInRegistries.ITEM.getKey(item), accepted, player.getPlainTextName());
		return accepted;
	}
}
