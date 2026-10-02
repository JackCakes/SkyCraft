package dev.skycraft.weapon;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.mojang.brigadier.arguments.StringArgumentType;
import dev.skycraft.SkyCraft;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.ChatFormatting;
import net.minecraft.commands.Commands;
import net.minecraft.core.component.DataComponents;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.util.RandomSource;
import net.minecraft.world.entity.EquipmentSlotGroup;
import net.minecraft.world.entity.ai.attributes.AttributeModifier;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.component.CustomData;
import net.minecraft.world.item.component.ItemAttributeModifiers;
import net.minecraft.world.item.component.ItemLore;

/**
 * Experimental Terraria-style per-weapon affixes.
 *
 * <p>One affix is rolled when a Skyrim weapon is first materialized as a Minecraft ItemStack. The
 * affix is stored on that exact stack in vanilla custom_data, so it survives saves, inventory moves
 * and multiplayer sync without adding a new persistent Skyrim record.
 */
public final class WeaponAffixes {
    private static final Gson GSON = new GsonBuilder().setPrettyPrinting().create();
    private static final Path CONFIG = FabricLoader.getInstance().getConfigDir().resolve("skycraft-weapon-affixes.json");
    private static final String ROOT = "SkyCraftAffix";
    private static final int FORMAT = 2;

    private static AffixConfig config = defaults();
    private static final Map<String, AffixDefinition> BY_ID = new LinkedHashMap<>();
    private static final List<String> CORE_WEAPONS = List.of(
        "skyrim_iron_dagger",
        "skyrim_iron_sword",
        "skyrim_iron_greatsword",
        "skyrim_iron_battleaxe"
    );

    private WeaponAffixes() {}

    public static void init() {
        reload();
        CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) -> {
            dispatcher.register(Commands.literal("skyaffixes")
                .then(Commands.literal("testkit").executes(context -> {
                    ServerPlayer player = context.getSource().getPlayerOrException();
                    Item item = SkyrimWeapons.byId("skyrim_iron_sword");
                    if (item == null) {
                        player.sendSystemMessage(Component.literal("SkyCraft affix test unavailable: iron sword is not registered."));
                        return 0;
                    }
                    for (int i = 0; i < 12; i++) {
                        ItemStack stack = new ItemStack(item);
                        roll(stack, player.getRandom());
                        player.getInventory().add(stack);
                    }
                    player.sendSystemMessage(Component.literal("Added 12 independently rolled SkyCraft Iron Swords."));
                    return 1;
                }))
                .then(Commands.literal("give")
                    .then(Commands.argument("id", StringArgumentType.word()).executes(context -> {
                        ServerPlayer player = context.getSource().getPlayerOrException();
                        String id = StringArgumentType.getString(context, "id").toLowerCase(java.util.Locale.ROOT);
                        AffixDefinition def = BY_ID.get(id);
                        Item item = def == null ? null : testItemFor(def);
                        if (def == null || item == null) {
                            player.sendSystemMessage(Component.literal("Unknown or inapplicable affix '" + id + "'. Available: " + String.join(", ", BY_ID.keySet())));
                            return 0;
                        }
                        ItemStack stack = new ItemStack(item);
                        apply(stack, def, 0, 0, 0);
                        player.getInventory().add(stack);
                        player.sendSystemMessage(Component.literal("Added deterministic test weapon with affix: " + def.displayName));
                        return 1;
                    })))
                .then(Commands.literal("inspect").executes(context -> {
                    ServerPlayer player = context.getSource().getPlayerOrException();
                    ItemStack stack = player.getMainHandItem();
                    StoredAffix affix = readStored(stack).orElse(null);
                    if (affix == null) {
                        int sourceFormId = sourceFormId(stack);
                        int sourceBaseFormId = sourceBaseFormId(stack);
                        int sourceLevel = sourceLevel(stack);
                        player.sendSystemMessage(Component.literal(
                            "Held item has no SkyCraft affix"
                            + (sourceFormId != 0 ? String.format(" | Skyrim source %08X", sourceFormId) : "")
                            + (sourceBaseFormId != 0 ? String.format(" | base %08X", sourceBaseFormId) : "")
                            + (sourceLevel > 0 ? " | source level " + sourceLevel : "")
                            + "."
                        ));
                        return 0;
                    }
                    player.sendSystemMessage(Component.literal(
                        "Affix " + affix.id
                        + " | damage x" + trim(affix.damageMultiplier)
                        + " | speed x" + trim(affix.attackSpeedMultiplier)
                        + " | reach " + String.format("%+.2f", affix.reachAdd)
                        + " | durability x" + trim(affix.durabilityMultiplier)
                        + " | knockback " + String.format("%+.2f", affix.knockbackAdd)
                        + " | proc " + Math.round(affix.procChance * 100.0) + "% x" + trim(affix.procDamageMultiplier)
                        + (affix.sourceFormId != 0 ? String.format(" | Skyrim source %08X", affix.sourceFormId) : "")
                        + (affix.sourceBaseFormId != 0 ? String.format(" | base %08X", affix.sourceBaseFormId) : "")
                        + (affix.sourceLevel > 0 ? " | source level " + affix.sourceLevel : "")
                    ));
                    return 1;
                }))
                .then(Commands.literal("chances").executes(context -> {
                    ServerPlayer player = context.getSource().getPlayerOrException();
                    ItemStack stack = player.getMainHandItem();
                    if (!isEligible(stack)) {
                        player.sendSystemMessage(Component.literal("Hold a SkyCraft weapon to view its affix chances."));
                        return 0;
                    }
                    List<AffixDefinition> candidates = candidatesFor(stack, sourceBaseFormId, sourceLevel);
                    double total = candidates.stream().mapToDouble(def -> def.weight).sum();
                    StringBuilder out = new StringBuilder("Affix chances: ");
                    out.append(String.format("None %.1f%%", (1.0 - config.rollChance) * 100.0));
                    if (total > 0.0) {
                        for (AffixDefinition def : candidates) {
                            double chance = config.rollChance * def.weight / total * 100.0;
                            out.append(String.format(" | %s %.1f%%", def.displayName, chance));
                        }
                    }
                    player.sendSystemMessage(Component.literal(out.toString()));
                    return 1;
                }))
                .then(Commands.literal("reload").executes(context -> {
                    reload();
                    context.getSource().sendSuccess(() -> Component.literal("Reloaded SkyCraft weapon affixes for future rolls."), false);
                    return 1;
                })));
        });
    }

    public static boolean isEligible(ItemStack stack) {
        if (stack == null || stack.isEmpty()) return false;
        Identifier id = BuiltInRegistries.ITEM.getKey(stack.getItem());
        return id != null
            && SkyCraft.MOD_ID.equals(id.getNamespace())
            && SkyrimWeapons.byId(id.getPath()) == stack.getItem();
    }

    /** Roll at most one affix onto a fresh weapon. Existing affixed stacks are never rerolled. */
    public static Optional<String> roll(ItemStack stack, RandomSource random) {
        return roll(stack, random, 0);
    }

    public static Optional<String> roll(ItemStack stack, RandomSource random, int sourceFormId) {
        return roll(stack, random, sourceFormId, 0, 0);
    }

    public static Optional<String> roll(ItemStack stack, RandomSource random, int sourceFormId, int sourceBaseFormId, int sourceLevel) {
        if (!isEligible(stack) || hasRollMarker(stack) || BY_ID.isEmpty()) {
            return Optional.empty();
        }
        if (random.nextDouble() >= config.rollChance) {
            markNoAffix(stack, sourceFormId, sourceBaseFormId, sourceLevel);
            SkyCraft.LOG.info("SkyCraft affix: rolled no affix on {}", BuiltInRegistries.ITEM.getKey(stack.getItem()));
            return Optional.empty();
        }

        List<AffixDefinition> candidates = candidatesFor(stack);
        if (candidates.isEmpty()) {
            markNoAffix(stack, sourceFormId, sourceBaseFormId, sourceLevel);
            return Optional.empty();
        }

        double total = 0.0;
        for (AffixDefinition def : candidates) total += def.weight;
        if (!(total > 0.0)) {
            markNoAffix(stack, sourceFormId, sourceBaseFormId, sourceLevel);
            return Optional.empty();
        }

        double pick = random.nextDouble() * total;
        AffixDefinition chosen = null;
        for (AffixDefinition def : candidates) {
            pick -= def.weight;
            if (pick <= 0.0) {
                chosen = def;
                break;
            }
        }
        if (chosen == null) chosen = candidates.getFirst();

        apply(stack, chosen, sourceFormId, sourceBaseFormId, sourceLevel);
        SkyCraft.LOG.info("SkyCraft affix: rolled {} on {}", chosen.id, BuiltInRegistries.ITEM.getKey(stack.getItem()));
        return Optional.of(chosen.id);
    }

    /** Per-hit proc multiplier. Runtime proc values are stored on the stack, not re-read from config. */
    public static float rollHitDamageMultiplier(ItemStack stack, RandomSource random) {
        StoredAffix affix = readStored(stack).orElse(null);
        if (affix == null || affix.procChance <= 0.0 || affix.procDamageMultiplier <= 1.0) {
            return 1.0F;
        }
        return random.nextDouble() < affix.procChance ? (float) affix.procDamageMultiplier : 1.0F;
    }

    public static Optional<String> readId(ItemStack stack) {
        return readStored(stack).map(StoredAffix::id);
    }

    private static boolean hasRollMarker(ItemStack stack) {
        if (stack == null || stack.isEmpty()) return false;
        CustomData custom = stack.get(DataComponents.CUSTOM_DATA);
        if (custom == null) return false;
        CompoundTag stored = custom.copyTag().getCompoundOrEmpty(ROOT);
        return !stored.isEmpty() && stored.getIntOr("v", 0) == FORMAT;
    }

    private static void markNoAffix(ItemStack stack, int sourceFormId, int sourceBaseFormId, int sourceLevel) {
        CompoundTag root = stack.has(DataComponents.CUSTOM_DATA)
            ? stack.get(DataComponents.CUSTOM_DATA).copyTag()
            : new CompoundTag();
        CompoundTag stored = new CompoundTag();
        stored.putInt("v", FORMAT);
        stored.putString("id", "none");
        stored.putInt("sourceFormId", sourceFormId);
        stored.putInt("sourceBaseFormId", sourceBaseFormId);
        stored.putInt("sourceLevel", sourceLevel);
        root.put(ROOT, stored);
        stack.set(DataComponents.CUSTOM_DATA, CustomData.of(root));
    }

    private static List<AffixDefinition> candidatesFor(ItemStack stack) {
        return candidatesFor(stack, sourceBaseFormId(stack), sourceLevel(stack));
    }

    private static List<AffixDefinition> candidatesFor(ItemStack stack, int sourceBaseFormId, int sourceLevel) {
        return BY_ID.values().stream()
            .filter(def -> appliesTo(def, stack, sourceBaseFormId, sourceLevel))
            .toList();
    }

    private static boolean appliesTo(AffixDefinition def, ItemStack stack, int sourceBaseFormId, int sourceLevel) {
        if (def == null || stack == null || stack.isEmpty()) return false;
        Identifier itemId = BuiltInRegistries.ITEM.getKey(stack.getItem());
        if (itemId == null || !appliesToPath(def, itemId.getPath())) return false;
        if (sourceLevel < def.minSourceLevel) return false;
        if (def.maxSourceLevel > 0 && sourceLevel > def.maxSourceLevel) return false;
        if (!def.allowedSourceBaseFormIds.contains("*")) {
            if (sourceBaseFormId == 0) return false;
            String sourceHex = String.format("%08x", sourceBaseFormId);
            if (!def.allowedSourceBaseFormIds.contains(sourceHex)) return false;
        }
        return true;
    }

    private static boolean appliesToPath(AffixDefinition def, String path) {
        return def.allowedWeapons.contains("*") || def.allowedWeapons.contains(path);
    }

    private static Item testItemFor(AffixDefinition def) {
        for (String path : CORE_WEAPONS) {
            Item item = SkyrimWeapons.byId(path);
            if (item != null && appliesToPath(def, path)) return item;
        }
        return null;
    }

    private static int sourceFormId(ItemStack stack) {
        if (stack == null || stack.isEmpty()) return 0;
        CustomData custom = stack.get(DataComponents.CUSTOM_DATA);
        if (custom == null) return 0;
        CompoundTag stored = custom.copyTag().getCompoundOrEmpty(ROOT);
        if (stored.isEmpty() || stored.getIntOr("v", 0) != FORMAT) return 0;
        return stored.getIntOr("sourceFormId", 0);
    }

    private static int sourceBaseFormId(ItemStack stack) {
        if (stack == null || stack.isEmpty()) return 0;
        CustomData custom = stack.get(DataComponents.CUSTOM_DATA);
        if (custom == null) return 0;
        CompoundTag stored = custom.copyTag().getCompoundOrEmpty(ROOT);
        if (stored.isEmpty() || stored.getIntOr("v", 0) != FORMAT) return 0;
        return stored.getIntOr("sourceBaseFormId", 0);
    }

    private static int sourceLevel(ItemStack stack) {
        if (stack == null || stack.isEmpty()) return 0;
        CustomData custom = stack.get(DataComponents.CUSTOM_DATA);
        if (custom == null) return 0;
        CompoundTag stored = custom.copyTag().getCompoundOrEmpty(ROOT);
        if (stored.isEmpty() || stored.getIntOr("v", 0) != FORMAT) return 0;
        return stored.getIntOr("sourceLevel", 0);
    }

    private static Optional<StoredAffix> readStored(ItemStack stack) {
        if (stack == null || stack.isEmpty()) return Optional.empty();
        CustomData custom = stack.get(DataComponents.CUSTOM_DATA);
        if (custom == null) return Optional.empty();
        CompoundTag stored = custom.copyTag().getCompoundOrEmpty(ROOT);
        if (stored.isEmpty() || stored.getIntOr("v", 0) != FORMAT) return Optional.empty();
        Optional<String> id = stored.getString("id");
        if (id.isEmpty() || "none".equals(id.get())) return Optional.empty();
        return Optional.of(new StoredAffix(
            id.get(),
            stored.getDoubleOr("damageMultiplier", 1.0),
            stored.getDoubleOr("attackSpeedMultiplier", 1.0),
            stored.getDoubleOr("reachAdd", 0.0),
            stored.getDoubleOr("durabilityMultiplier", 1.0),
            stored.getDoubleOr("knockbackAdd", 0.0),
            stored.getDoubleOr("procChance", 0.0),
            stored.getDoubleOr("procDamageMultiplier", 1.0),
            stored.getIntOr("sourceFormId", 0),
            stored.getIntOr("sourceBaseFormId", 0),
            stored.getIntOr("sourceLevel", 0)
        ));
    }

    private static void apply(ItemStack stack, AffixDefinition def, int sourceFormId, int sourceBaseFormId, int sourceLevel) {
        ItemAttributeModifiers.Builder attrs = ItemAttributeModifiers.builder();
        for (ItemAttributeModifiers.Entry entry : stack.getOrDefault(DataComponents.ATTRIBUTE_MODIFIERS, ItemAttributeModifiers.EMPTY).modifiers()) {
            attrs.add(entry.attribute(), entry.modifier(), entry.slot());
        }

        String safeId = def.id.replaceAll("[^a-z0-9_./-]", "_");
        if (Math.abs(def.damageMultiplier - 1.0) > 1.0E-6) {
            attrs.add(Attributes.ATTACK_DAMAGE,
                new AttributeModifier(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "affix/" + safeId + "/damage"),
                    def.damageMultiplier - 1.0, AttributeModifier.Operation.ADD_MULTIPLIED_TOTAL),
                EquipmentSlotGroup.MAINHAND);
        }
        if (Math.abs(def.attackSpeedMultiplier - 1.0) > 1.0E-6) {
            attrs.add(Attributes.ATTACK_SPEED,
                new AttributeModifier(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "affix/" + safeId + "/speed"),
                    def.attackSpeedMultiplier - 1.0, AttributeModifier.Operation.ADD_MULTIPLIED_TOTAL),
                EquipmentSlotGroup.MAINHAND);
        }
        if (Math.abs(def.reachAdd) > 1.0E-6) {
            attrs.add(Attributes.ENTITY_INTERACTION_RANGE,
                new AttributeModifier(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "affix/" + safeId + "/reach"),
                    def.reachAdd, AttributeModifier.Operation.ADD_VALUE),
                EquipmentSlotGroup.MAINHAND);
        }
        if (Math.abs(def.knockbackAdd) > 1.0E-6) {
            attrs.add(Attributes.ATTACK_KNOCKBACK,
                new AttributeModifier(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "affix/" + safeId + "/knockback"),
                    def.knockbackAdd, AttributeModifier.Operation.ADD_VALUE),
                EquipmentSlotGroup.MAINHAND);
        }
        stack.set(DataComponents.ATTRIBUTE_MODIFIERS, attrs.build());

        int maxDamage = stack.getOrDefault(DataComponents.MAX_DAMAGE, 0);
        if (maxDamage > 0 && Math.abs(def.durabilityMultiplier - 1.0) > 1.0E-6) {
            int boosted = Math.max(1, Math.min(1_000_000, (int) Math.round(maxDamage * def.durabilityMultiplier)));
            stack.set(DataComponents.MAX_DAMAGE, boosted);
        }

        CompoundTag root = stack.has(DataComponents.CUSTOM_DATA)
            ? stack.get(DataComponents.CUSTOM_DATA).copyTag()
            : new CompoundTag();
        CompoundTag stored = new CompoundTag();
        stored.putInt("v", FORMAT);
        stored.putString("id", def.id);
        stored.putDouble("damageMultiplier", def.damageMultiplier);
        stored.putDouble("attackSpeedMultiplier", def.attackSpeedMultiplier);
        stored.putDouble("reachAdd", def.reachAdd);
        stored.putDouble("durabilityMultiplier", def.durabilityMultiplier);
        stored.putDouble("knockbackAdd", def.knockbackAdd);
        stored.putDouble("procChance", def.procChance);
        stored.putDouble("procDamageMultiplier", def.procDamageMultiplier);
        stored.putInt("sourceFormId", sourceFormId);
        stored.putInt("sourceBaseFormId", sourceBaseFormId);
        stored.putInt("sourceLevel", sourceLevel);
        root.put(ROOT, stored);
        stack.set(DataComponents.CUSTOM_DATA, CustomData.of(root));

        Component baseName = stack.getHoverName().copy();
        stack.set(DataComponents.CUSTOM_NAME,
            Component.literal(def.displayName + " ").withStyle(ChatFormatting.GOLD).append(baseName));

        List<Component> lore = new ArrayList<>();
        if (Math.abs(def.damageMultiplier - 1.0) > 1.0E-6)
            lore.add(Component.literal(percent(def.damageMultiplier - 1.0) + " damage").withStyle(ChatFormatting.GRAY));
        if (Math.abs(def.attackSpeedMultiplier - 1.0) > 1.0E-6)
            lore.add(Component.literal(percent(def.attackSpeedMultiplier - 1.0) + " attack speed").withStyle(ChatFormatting.GRAY));
        if (Math.abs(def.reachAdd) > 1.0E-6)
            lore.add(Component.literal(String.format("%+.2f reach", def.reachAdd)).withStyle(ChatFormatting.GRAY));
        if (Math.abs(def.durabilityMultiplier - 1.0) > 1.0E-6)
            lore.add(Component.literal(percent(def.durabilityMultiplier - 1.0) + " durability").withStyle(ChatFormatting.GRAY));
        if (Math.abs(def.knockbackAdd) > 1.0E-6)
            lore.add(Component.literal(String.format("%+.2f attack knockback", def.knockbackAdd)).withStyle(ChatFormatting.GRAY));
        if (def.procChance > 0.0 && def.procDamageMultiplier > 1.0)
            lore.add(Component.literal(Math.round(def.procChance * 100.0) + "% chance for x" + trim(def.procDamageMultiplier) + " damage")
                .withStyle(ChatFormatting.LIGHT_PURPLE));
        if (!lore.isEmpty()) stack.set(DataComponents.LORE, new ItemLore(lore));
    }

    private static String percent(double delta) {
        long pct = Math.round(delta * 100.0);
        return (pct >= 0 ? "+" : "") + pct + "%";
    }

    private static String trim(double value) {
        return value == Math.rint(value) ? Long.toString(Math.round(value)) : String.format("%.2f", value);
    }

    private static synchronized void reload() {
        AffixConfig loaded = readConfig();
        BY_ID.clear();
        for (AffixDefinition raw : loaded.affixes) {
            AffixDefinition def = sanitize(raw);
            if (def != null) BY_ID.put(def.id, def);
        }
        config = loaded;
        config.rollChance = clamp(config.rollChance, 0.0, 1.0);
        SkyCraft.LOG.info("SkyCraft affixes: loaded {} affixes; roll chance {}%", BY_ID.size(), Math.round(config.rollChance * 100.0));
    }

    private static AffixConfig readConfig() {
        if (!Files.exists(CONFIG)) {
            AffixConfig defaults = defaults();
            try {
                Files.createDirectories(CONFIG.getParent());
                Files.writeString(CONFIG, GSON.toJson(defaults));
            } catch (IOException e) {
                SkyCraft.LOG.warn("SkyCraft affixes: couldn't write default {}", CONFIG, e);
            }
            return defaults;
        }
        try {
            AffixConfig parsed = GSON.fromJson(Files.readString(CONFIG), AffixConfig.class);
            if (parsed == null || parsed.affixes == null) throw new IllegalArgumentException("missing affixes array");
            return parsed;
        } catch (RuntimeException | IOException e) {
            SkyCraft.LOG.warn("SkyCraft affixes: couldn't read {}; using defaults", CONFIG, e);
            return defaults();
        }
    }

    private static AffixDefinition sanitize(AffixDefinition d) {
        if (d == null || !d.enabled || d.id == null || d.displayName == null) return null;
        String id = d.id.trim().toLowerCase();
        if (!id.matches("[a-z0-9_./-]+")) return null;
        d.id = id;
        d.displayName = d.displayName.trim();
        d.weight = clamp(d.weight, 0.0, 1_000_000.0);
        d.damageMultiplier = clamp(d.damageMultiplier, 0.05, 10.0);
        d.attackSpeedMultiplier = clamp(d.attackSpeedMultiplier, 0.05, 10.0);
        d.reachAdd = clamp(d.reachAdd, -2.0, 5.0);
        d.durabilityMultiplier = clamp(d.durabilityMultiplier, 0.05, 10.0);
        d.knockbackAdd = clamp(d.knockbackAdd, -1.0, 5.0);
        d.procChance = clamp(d.procChance, 0.0, 1.0);
        d.procDamageMultiplier = clamp(d.procDamageMultiplier, 1.0, 20.0);
        d.minSourceLevel = Math.max(0, Math.min(65535, d.minSourceLevel));
        d.maxSourceLevel = Math.max(0, Math.min(65535, d.maxSourceLevel));
        if (d.maxSourceLevel > 0 && d.maxSourceLevel < d.minSourceLevel) {
            int swap = d.minSourceLevel;
            d.minSourceLevel = d.maxSourceLevel;
            d.maxSourceLevel = swap;
        }
        if (d.allowedSourceBaseFormIds == null || d.allowedSourceBaseFormIds.isEmpty()) {
            d.allowedSourceBaseFormIds = new ArrayList<>(List.of("*"));
        } else {
            ArrayList<String> sources = new ArrayList<>();
            for (String raw : d.allowedSourceBaseFormIds) {
                if (raw == null) continue;
                String source = raw.trim().toLowerCase(java.util.Locale.ROOT);
                if (source.startsWith("0x")) source = source.substring(2);
                if ("*".equals(source)) {
                    sources.clear();
                    sources.add("*");
                    break;
                }
                if (source.matches("[0-9a-f]{1,8}")) {
                    source = String.format("%08x", Long.parseUnsignedLong(source, 16));
                    if (!sources.contains(source)) sources.add(source);
                }
            }
            if (sources.isEmpty()) sources.add("*");
            d.allowedSourceBaseFormIds = sources;
        }
        if (d.allowedWeapons == null || d.allowedWeapons.isEmpty()) {
            d.allowedWeapons = new ArrayList<>(List.of("*"));
        } else {
            ArrayList<String> allowed = new ArrayList<>();
            for (String raw : d.allowedWeapons) {
                if (raw == null) continue;
                String weapon = raw.trim().toLowerCase(java.util.Locale.ROOT);
                if ("*".equals(weapon) || weapon.matches("[a-z0-9_./-]+")) {
                    if (!allowed.contains(weapon)) allowed.add(weapon);
                }
            }
            if (allowed.isEmpty()) allowed.add("*");
            d.allowedWeapons = allowed;
        }
        return d.weight > 0.0 && !d.displayName.isEmpty() ? d : null;
    }

    private static double clamp(double v, double min, double max) {
        return Double.isFinite(v) ? Math.max(min, Math.min(max, v)) : min;
    }

    private static AffixConfig defaults() {
        AffixConfig c = new AffixConfig();
        c.rollChance = 0.70;
        c.affixes = new ArrayList<>(List.of(
            new AffixDefinition("brutal", "Brutal", 25, 1.25, 1.0, 0.0, 1.0, 0.0, 1.0, List.of("*"), true),
            new AffixDefinition("swift", "Swift", 25, 1.0, 1.30, 0.0, 1.0, 0.0, 1.0, List.of("skyrim_iron_dagger", "skyrim_iron_sword"), true),
            new AffixDefinition("long", "Long", 15, 1.0, 1.0, 0.50, 1.0, 0.0, 1.0, List.of("skyrim_iron_sword", "skyrim_iron_greatsword"), true),
            new AffixDefinition("sturdy", "Sturdy", 15, 1.0, 1.0, 0.0, 1.50, 0.0, 1.0, List.of("*"), true),
            new AffixDefinition("volatile", "Volatile", 20, 1.0, 1.0, 0.0, 1.0, 0.10, 3.0, List.of("*"), true),
            new AffixDefinition("deadly", "Deadly", 12, 1.15, 1.15, 0.0, 1.0, 0.0, 1.0, List.of("*"), true),
            new AffixDefinition("flurried", "Flurried", 10, 0.90, 1.50, 0.0, 1.0, 0.0, 1.0, List.of("skyrim_iron_dagger", "skyrim_iron_sword"), true),
            new AffixDefinition("berserker", "Berserker", 10, 1.40, 0.80, 0.0, 1.10, 0.0, 1.0, List.of("skyrim_iron_greatsword", "skyrim_iron_battleaxe"), true),
            new AffixDefinition("giant", "Giant", 8, 1.20, 0.90, 0.75, 1.0, 0.0, 1.0, List.of("skyrim_iron_greatsword", "skyrim_iron_battleaxe"), true),
            new AffixDefinition("forceful", "Forceful", 10, 1.05, 1.0, 0.0, 1.0, 0.85, 0.0, 1.0, List.of("skyrim_iron_sword", "skyrim_iron_greatsword", "skyrim_iron_battleaxe"), true)
        ));
        AffixDefinition veteran = new AffixDefinition("veteran", "Veteran", 8, 1.20, 1.10, 0.0, 1.15, 0.0, 1.0, List.of("*"), true);
        veteran.minSourceLevel = 20;
        c.affixes.add(veteran);
        return c;
    }

    private record StoredAffix(
        String id,
        double damageMultiplier,
        double attackSpeedMultiplier,
        double reachAdd,
        double durabilityMultiplier,
        double knockbackAdd,
        double procChance,
        double procDamageMultiplier,
        int sourceFormId,
        int sourceBaseFormId,
        int sourceLevel
    ) {}

    public static final class AffixConfig {
        public double rollChance = 0.70;
        public List<AffixDefinition> affixes = new ArrayList<>();
    }

    public static final class AffixDefinition {
        public String id;
        public String displayName;
        public double weight = 1.0;
        public double damageMultiplier = 1.0;
        public double attackSpeedMultiplier = 1.0;
        public double reachAdd = 0.0;
        public double durabilityMultiplier = 1.0;
        public double knockbackAdd = 0.0;
        public double procChance = 0.0;
        public double procDamageMultiplier = 1.0;
        public List<String> allowedWeapons = new ArrayList<>(List.of("*"));
        public List<String> allowedSourceBaseFormIds = new ArrayList<>(List.of("*"));
        public int minSourceLevel = 0;
        public int maxSourceLevel = 0;
        public boolean enabled = true;

        public AffixDefinition() {}

        public AffixDefinition(String id, String displayName, double weight, double damageMultiplier,
                               double attackSpeedMultiplier, double reachAdd, double durabilityMultiplier,
                               double procChance, double procDamageMultiplier, List<String> allowedWeapons,
                               boolean enabled) {
            this(id, displayName, weight, damageMultiplier, attackSpeedMultiplier, reachAdd, durabilityMultiplier,
                0.0, procChance, procDamageMultiplier, allowedWeapons, enabled);
        }

        public AffixDefinition(String id, String displayName, double weight, double damageMultiplier,
                               double attackSpeedMultiplier, double reachAdd, double durabilityMultiplier,
                               double knockbackAdd, double procChance, double procDamageMultiplier,
                               List<String> allowedWeapons, boolean enabled) {
            this.id = id;
            this.displayName = displayName;
            this.weight = weight;
            this.damageMultiplier = damageMultiplier;
            this.attackSpeedMultiplier = attackSpeedMultiplier;
            this.reachAdd = reachAdd;
            this.durabilityMultiplier = durabilityMultiplier;
            this.knockbackAdd = knockbackAdd;
            this.procChance = procChance;
            this.procDamageMultiplier = procDamageMultiplier;
            this.allowedWeapons = new ArrayList<>(allowedWeapons);
            this.enabled = enabled;
        }
    }
}
