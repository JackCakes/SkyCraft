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
                        Item item = SkyrimWeapons.byId("skyrim_iron_sword");
                        if (def == null || item == null) {
                            player.sendSystemMessage(Component.literal("Unknown affix '" + id + "'. Available: " + String.join(", ", BY_ID.keySet())));
                            return 0;
                        }
                        ItemStack stack = new ItemStack(item);
                        apply(stack, def);
                        player.getInventory().add(stack);
                        player.sendSystemMessage(Component.literal("Added deterministic test weapon with affix: " + def.displayName));
                        return 1;
                    })))
                .then(Commands.literal("inspect").executes(context -> {
                    ServerPlayer player = context.getSource().getPlayerOrException();
                    ItemStack stack = player.getMainHandItem();
                    StoredAffix affix = readStored(stack).orElse(null);
                    if (affix == null) {
                        player.sendSystemMessage(Component.literal("Held item has no SkyCraft affix."));
                        return 0;
                    }
                    player.sendSystemMessage(Component.literal(
                        "Affix " + affix.id
                        + " | damage x" + trim(affix.damageMultiplier)
                        + " | speed x" + trim(affix.attackSpeedMultiplier)
                        + " | reach " + String.format("%+.2f", affix.reachAdd)
                        + " | durability x" + trim(affix.durabilityMultiplier)
                        + " | proc " + Math.round(affix.procChance * 100.0) + "% x" + trim(affix.procDamageMultiplier)
                    ));
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
        if (!isEligible(stack) || readId(stack).isPresent() || BY_ID.isEmpty()) {
            return Optional.empty();
        }
        if (random.nextDouble() >= config.rollChance) {
            return Optional.empty();
        }

        double total = 0.0;
        for (AffixDefinition def : BY_ID.values()) total += def.weight;
        if (!(total > 0.0)) return Optional.empty();

        double pick = random.nextDouble() * total;
        AffixDefinition chosen = null;
        for (AffixDefinition def : BY_ID.values()) {
            pick -= def.weight;
            if (pick <= 0.0) {
                chosen = def;
                break;
            }
        }
        if (chosen == null) chosen = BY_ID.values().iterator().next();

        apply(stack, chosen);
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

    private static Optional<StoredAffix> readStored(ItemStack stack) {
        if (stack == null || stack.isEmpty()) return Optional.empty();
        CustomData custom = stack.get(DataComponents.CUSTOM_DATA);
        if (custom == null) return Optional.empty();
        CompoundTag stored = custom.copyTag().getCompoundOrEmpty(ROOT);
        if (stored.isEmpty() || stored.getIntOr("v", 0) != FORMAT) return Optional.empty();
        Optional<String> id = stored.getString("id");
        if (id.isEmpty()) return Optional.empty();
        return Optional.of(new StoredAffix(
            id.get(),
            stored.getDoubleOr("damageMultiplier", 1.0),
            stored.getDoubleOr("attackSpeedMultiplier", 1.0),
            stored.getDoubleOr("reachAdd", 0.0),
            stored.getDoubleOr("durabilityMultiplier", 1.0),
            stored.getDoubleOr("procChance", 0.0),
            stored.getDoubleOr("procDamageMultiplier", 1.0)
        ));
    }

    private static void apply(ItemStack stack, AffixDefinition def) {
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
        stored.putDouble("procChance", def.procChance);
        stored.putDouble("procDamageMultiplier", def.procDamageMultiplier);
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
        d.procChance = clamp(d.procChance, 0.0, 1.0);
        d.procDamageMultiplier = clamp(d.procDamageMultiplier, 1.0, 20.0);
        return d.weight > 0.0 && !d.displayName.isEmpty() ? d : null;
    }

    private static double clamp(double v, double min, double max) {
        return Double.isFinite(v) ? Math.max(min, Math.min(max, v)) : min;
    }

    private static AffixConfig defaults() {
        AffixConfig c = new AffixConfig();
        c.rollChance = 0.70;
        c.affixes = new ArrayList<>(List.of(
            new AffixDefinition("brutal", "Brutal", 25, 1.25, 1.0, 0.0, 1.0, 0.0, 1.0, true),
            new AffixDefinition("swift", "Swift", 25, 1.0, 1.30, 0.0, 1.0, 0.0, 1.0, true),
            new AffixDefinition("long", "Long", 15, 1.0, 1.0, 0.50, 1.0, 0.0, 1.0, true),
            new AffixDefinition("sturdy", "Sturdy", 15, 1.0, 1.0, 0.0, 1.50, 0.0, 1.0, true),
            new AffixDefinition("volatile", "Volatile", 20, 1.0, 1.0, 0.0, 1.0, 0.10, 3.0, true)
        ));
        return c;
    }

    private record StoredAffix(
        String id,
        double damageMultiplier,
        double attackSpeedMultiplier,
        double reachAdd,
        double durabilityMultiplier,
        double procChance,
        double procDamageMultiplier
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
        public double procChance = 0.0;
        public double procDamageMultiplier = 1.0;
        public boolean enabled = true;

        public AffixDefinition() {}

        public AffixDefinition(String id, String displayName, double weight, double damageMultiplier,
                               double attackSpeedMultiplier, double reachAdd, double durabilityMultiplier,
                               double procChance, double procDamageMultiplier, boolean enabled) {
            this.id = id;
            this.displayName = displayName;
            this.weight = weight;
            this.damageMultiplier = damageMultiplier;
            this.attackSpeedMultiplier = attackSpeedMultiplier;
            this.reachAdd = reachAdd;
            this.durabilityMultiplier = durabilityMultiplier;
            this.procChance = procChance;
            this.procDamageMultiplier = procDamageMultiplier;
            this.enabled = enabled;
        }
    }
}
