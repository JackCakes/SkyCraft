package dev.skycraft.weapon;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonObject;
import dev.skycraft.SkyCraft;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.Map;
import net.fabricmc.loader.api.FabricLoader;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.commands.Commands;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.EquipmentSlotGroup;
import net.minecraft.world.entity.ai.attributes.AttributeModifier;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.component.ItemAttributeModifiers;

/**
 * Experimental fixed catalog of Skyrim-flavoured Minecraft melee items.
 *
 * Stats are loaded from config/skycraft-weapons.json before item registration. The registry ids stay
 * fixed so worlds remain loadable; players can tune damage, attack speed, reach and durability
 * without recompiling the mod, then restart Minecraft to apply the new values.
 */
public final class SkyrimWeapons {
    private static final Gson GSON = new GsonBuilder().setPrettyPrinting().create();
    private static final Path CONFIG = FabricLoader.getInstance().getConfigDir().resolve("skycraft-weapons.json");

    private static final LinkedHashMap<String, Stats> DEFAULTS = new LinkedHashMap<>();
    private static final Map<String, Item> ITEMS = new LinkedHashMap<>();

    private enum WeaponClass {
        DAGGER(4.0, 2.0, 1.8, 150),
        SWORD(6.0, 1.6, 2.5, 200),
        WAR_AXE(7.0, 1.4, 2.5, 250),
        MACE(8.0, 1.2, 2.5, 300),
        GREATSWORD(11.0, 1.0, 3.0, 350),
        BATTLEAXE(12.0, 0.9, 3.0, 400),
        WARHAMMER(13.0, 0.8, 3.0, 450);

        final double damage;
        final double speed;
        final double reach;
        final int durability;

        WeaponClass(double damage, double speed, double reach, int durability) {
            this.damage = damage;
            this.speed = speed;
            this.reach = reach;
            this.durability = durability;
        }
    }

    private enum MaterialTier {
        IRON(0.0, 0),
        STEEL(1.0, 75),
        DWARVEN(2.0, 150),
        ORCISH(3.0, 225),
        ELVEN(4.0, 300),
        GLASS(5.0, 400),
        EBONY(6.0, 525),
        DAEDRIC(7.0, 675);

        final double damageBonus;
        final int durabilityBonus;

        MaterialTier(double damageBonus, int durabilityBonus) {
            this.damageBonus = damageBonus;
            this.durabilityBonus = durabilityBonus;
        }
    }

    static {
        add("skyrim_iron_dagger", MaterialTier.IRON, WeaponClass.DAGGER);
        add("skyrim_iron_sword", MaterialTier.IRON, WeaponClass.SWORD);
        add("skyrim_iron_greatsword", MaterialTier.IRON, WeaponClass.GREATSWORD);
        add("skyrim_iron_battleaxe", MaterialTier.IRON, WeaponClass.BATTLEAXE);

        addTier("steel", MaterialTier.STEEL);
        add("skyrim_dwarven_dagger", MaterialTier.DWARVEN, WeaponClass.DAGGER);
        add("skyrim_dwarven_sword", MaterialTier.DWARVEN, WeaponClass.SWORD);
        add("skyrim_dwarven_war_axe", MaterialTier.DWARVEN, WeaponClass.WAR_AXE);
        add("skyrim_dwarven_mace", MaterialTier.DWARVEN, WeaponClass.MACE);
        add("skyrim_dwarven_greatsword", MaterialTier.DWARVEN, WeaponClass.GREATSWORD);
        add("skyrim_dwarven_battleaxe", MaterialTier.DWARVEN, WeaponClass.BATTLEAXE);
        add("skyrim_dwarven_warhammer", MaterialTier.DWARVEN, WeaponClass.WARHAMMER);

        addTier("orcish", MaterialTier.ORCISH);
    }

    private static void addTier(String materialName, MaterialTier material) {
        add("skyrim_" + materialName + "_dagger", material, WeaponClass.DAGGER);
        add("skyrim_" + materialName + "_sword", material, WeaponClass.SWORD);
        add("skyrim_" + materialName + "_war_axe", material, WeaponClass.WAR_AXE);
        add("skyrim_" + materialName + "_mace", material, WeaponClass.MACE);
        add("skyrim_" + materialName + "_greatsword", material, WeaponClass.GREATSWORD);
        add("skyrim_" + materialName + "_battleaxe", material, WeaponClass.BATTLEAXE);
        add("skyrim_" + materialName + "_warhammer", material, WeaponClass.WARHAMMER);
    }

    private static void add(String id, MaterialTier material, WeaponClass weaponClass) {
        DEFAULTS.put(id, new Stats(
            weaponClass.damage + material.damageBonus,
            weaponClass.speed,
            weaponClass.reach,
            weaponClass.durability + material.durabilityBonus));
    }

    private SkyrimWeapons() {}

    public static void init() {
        Map<String, Stats> configured = load();
        for (var entry : DEFAULTS.entrySet()) {
            String id = entry.getKey();
            Stats stats = sanitize(configured.getOrDefault(id, entry.getValue()), entry.getValue());
            ITEMS.put(id, register(id, stats));
            SkyCraft.LOG.info("SkyCraft weapon prototype: {} damage={} speed={} reach={} durability={}",
                id, stats.damage, stats.attackSpeed, stats.reach, stats.durability);
        }
        registerTestCommand();
    }

    private static void registerTestCommand() {
        CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) -> {
            dispatcher.register(Commands.literal("skyweapons")
                .then(Commands.literal("testkit").executes(context -> {
                    ServerPlayer player = context.getSource().getPlayerOrException();
                    for (String id : DEFAULTS.keySet()) {
                        Item item = ITEMS.get(id);
                        if (item != null) {
                            player.getInventory().add(new ItemStack(item));
                        }
                    }
                    player.sendSystemMessage(Component.literal("SkyCraft weapon test kit added: " + DEFAULTS.size() + " weapons across Iron, Steel, Dwarven and Orcish tiers."));
                    return 1;
                })));
        });
    }

    public static Item byId(String id) {
        return ITEMS.get(id);
    }

    private static Item register(String path, Stats stats) {
        Identifier id = Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, path);
        ResourceKey<Item> key = ResourceKey.create(Registries.ITEM, id);

        double attackDamageModifier = stats.damage - 1.0;   // player base damage is 1
        double attackSpeedModifier = stats.attackSpeed - 4.0; // player base attack speed is 4
        double reachModifier = stats.reach - 3.0;           // vanilla entity interaction range

        Item.Properties props = new Item.Properties()
            .setId(key)
            .stacksTo(1)
            .durability(stats.durability)
            .attributes(ItemAttributeModifiers.builder()
                .add(Attributes.ATTACK_DAMAGE,
                    new AttributeModifier(Item.BASE_ATTACK_DAMAGE_ID, attackDamageModifier, AttributeModifier.Operation.ADD_VALUE),
                    EquipmentSlotGroup.MAINHAND)
                .add(Attributes.ATTACK_SPEED,
                    new AttributeModifier(Item.BASE_ATTACK_SPEED_ID, attackSpeedModifier, AttributeModifier.Operation.ADD_VALUE),
                    EquipmentSlotGroup.MAINHAND)
                .add(Attributes.ENTITY_INTERACTION_RANGE,
                    new AttributeModifier(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, path + "_reach"), reachModifier, AttributeModifier.Operation.ADD_VALUE),
                    EquipmentSlotGroup.MAINHAND)
                .build());

        return Registry.register(BuiltInRegistries.ITEM, id, new Item(props));
    }

    private static Map<String, Stats> load() {
        if (!Files.exists(CONFIG)) {
            writeDefaults();
            return DEFAULTS;
        }
        try {
            JsonObject root = GSON.fromJson(Files.readString(CONFIG), JsonObject.class);
            LinkedHashMap<String, Stats> out = new LinkedHashMap<>();
            if (root != null && root.has("weapons")) {
                JsonObject weapons = root.getAsJsonObject("weapons");
                for (var entry : weapons.entrySet()) {
                    out.put(entry.getKey(), GSON.fromJson(entry.getValue(), Stats.class));
                }
            }
            return out;
        } catch (RuntimeException | IOException e) {
            SkyCraft.LOG.warn("SkyCraft weapon prototype: couldn't read {}; using defaults", CONFIG, e);
            return DEFAULTS;
        }
    }

    private static void writeDefaults() {
        try {
            Files.createDirectories(CONFIG.getParent());
            JsonObject root = new JsonObject();
            JsonObject weapons = new JsonObject();
            DEFAULTS.forEach((id, stats) -> weapons.add(id, GSON.toJsonTree(stats)));
            root.add("weapons", weapons);
            Files.writeString(CONFIG, GSON.toJson(root));
        } catch (IOException e) {
            SkyCraft.LOG.warn("SkyCraft weapon prototype: couldn't write default {}", CONFIG, e);
        }
    }

    private static Stats sanitize(Stats value, Stats fallback) {
        if (value == null) return fallback;
        return new Stats(
            clamp(value.damage, 0.5, 100.0),
            clamp(value.attackSpeed, 0.1, 8.0),
            clamp(value.reach, 1.0, 8.0),
            Math.max(1, Math.min(value.durability, 100000))
        );
    }

    private static double clamp(double v, double min, double max) {
        return Double.isFinite(v) ? Math.max(min, Math.min(max, v)) : min;
    }

    public static final class Stats {
        public double damage;
        public double attackSpeed;
        public double reach;
        public int durability;

        public Stats() {}

        public Stats(double damage, double attackSpeed, double reach, int durability) {
            this.damage = damage;
            this.attackSpeed = attackSpeed;
            this.reach = reach;
            this.durability = durability;
        }
    }
}
