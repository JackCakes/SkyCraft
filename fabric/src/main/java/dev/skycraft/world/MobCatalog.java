package dev.skycraft.world;

import dev.skycraft.SkyCraft;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.entity.EntityType;

/**
 * The list of every distinct mob body size in the game, for the mob body library: the Skyrim-side stand-in of a Minecraft mob is a
 * collision-only box the size of the mob's own bounding box, made once per distinct size.
 *
 * A size is a width and a height in sixteenths of a block ({@link #sixteenths}), and its key is {@code w<width>_h<height>}. The SKSE side
 * (MobBodies.cpp) computes the same key from the width and height Minecraft reports for a live mob, so {@link #key} is the one definition.
 * Every registered entity type is walked, modded ones too, each at its own size and as a baby (half size), so a size no vanilla mob has
 * is in the library the next start. A size that is not in it (a slime of an odd size, a mod's growing mob) is still made while the game
 * runs, by the SKSE side, from spare model files it keeps for the purpose.
 *
 * Written to {@code skycraft/mob_catalog.json} in the game folder the first time the world starts, and again when the registered entity
 * types change ({@code -Dskycraft.dumpMobs=false} turns it off, {@code force} writes it every start).
 */
public final class MobCatalog {
	private MobCatalog() {}

	public static final int MAX_WIDTH = 255;
	public static final int MAX_HEIGHT = 400;

	/** A length in blocks as whole sixteenths, at least one, so no mob is a box of nothing. */
	public static int sixteenths(float blocks, int max) {
		return Math.max(1, Math.min(max, Math.round(blocks * 16.0F)));
	}

	public static String key(float width, float height) {
		return key(sixteenths(width, MAX_WIDTH), sixteenths(height, MAX_HEIGHT));
	}

	public static String key(int w16, int h16) {
		return "w" + w16 + "_h" + h16;
	}

	private static int typeCount() {
		int n = 0;
		for (EntityType<?> ignored : BuiltInRegistries.ENTITY_TYPE) {
			n++;
		}
		return n;
	}

	/** Whether the catalogue in a_file was made for the entity types that are registered now. */
	public static boolean upToDate(Path a_file) {
		try {
			return Files.readString(a_file, StandardCharsets.UTF_8).contains("\"entityTypes\": " + typeCount() + ",");
		} catch (IOException ex) {
			return false;
		}
	}

	private static final class Entry {
		final int w16;
		final int h16;
		final List<String> examples = new ArrayList<>();

		Entry(int w16, int h16) {
			this.w16 = w16;
			this.h16 = h16;
		}
	}

	private static void add(Map<String, Entry> sizes, String id, float width, float height) {
		if (!(width > 0.0F) || !(height > 0.0F) || width > 64.0F || height > 64.0F) {
			return;
		}
		int w16 = sixteenths(width, MAX_WIDTH);
		int h16 = sixteenths(height, MAX_HEIGHT);
		Entry e = sizes.computeIfAbsent(key(w16, h16), k -> new Entry(w16, h16));
		if (e.examples.size() < 4 && !e.examples.contains(id)) {
			e.examples.add(id);
		}
	}

	/** Walks every registered entity type (modded ones too) and writes the distinct body sizes as JSON. */
	public static void dump(Path file) {
		Map<String, Entry> sizes = new TreeMap<>();
		int types = 0;
		for (EntityType<?> type : BuiltInRegistries.ENTITY_TYPE) {
			types++;
			String id = BuiltInRegistries.ENTITY_TYPE.getKey(type).toString();
			try {
				float w = type.getWidth();
				float h = type.getHeight();
				add(sizes, id, w, h);
				add(sizes, id + " (baby)", w * 0.5F, h * 0.5F);
			} catch (RuntimeException ex) {
				SkyCraft.LOG.warn("SkyCraft: mob catalog: no size for {}", id);
			}
		}
		StringBuilder json = new StringBuilder("{\n  \"entityTypes\": " + types + ",\n  \"unit\": \"sixteenths\",\n  \"sizes\": [\n");
		boolean first = true;
		for (Entry e : sizes.values()) {
			json.append(first ? "" : ",\n").append("    {\"w\": ").append(e.w16).append(", \"h\": ").append(e.h16).append(", \"examples\": [");
			for (int i = 0; i < e.examples.size(); i++) {
				json.append(i == 0 ? "" : ", ").append('"').append(e.examples.get(i)).append('"');
			}
			json.append("]}");
			first = false;
		}
		json.append("\n  ]\n}\n");
		try {
			Files.createDirectories(file.toAbsolutePath().getParent());
			Files.writeString(file, json.toString(), StandardCharsets.UTF_8);
			SkyCraft.LOG.info("SkyCraft: mob catalog: {} distinct body sizes over {} entity types -> {}", sizes.size(), types, file);
		} catch (IOException ex) {
			SkyCraft.LOG.error("SkyCraft: couldn't write the mob catalog", ex);
		}
	}
}
