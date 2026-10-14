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

    static {
        DEFAULTS.put("skyrim_iron_dagger", new Stats(4.0, 2.0, 1.8, 150));
        DEFAULTS.put("skyrim_iron_sword", new Stats(6.0, 1.6, 2.5, 200));
        DEFAULTS.put("skyrim_iron_greatsword", new Stats(11.0, 1.0, 3.0, 350));
        DEFAULTS.put("skyrim_iron_battleaxe", new Stats(12.0, 0.9, 3.0, 400));
        DEFAULTS.put("skyrim_steel_dagger", new Stats(4.5, 2.0, 1.8, 175));
        DEFAULTS.put("skyrim_steel_sword", new Stats(6.5, 1.6, 2.5, 225));
        DEFAULTS.put("skyrim_steel_war_axe", new Stats(7.5, 1.3, 2.4, 250));
        DEFAULTS.put("skyrim_steel_mace", new Stats(8.5, 1.1, 2.3, 275));
        DEFAULTS.put("skyrim_steel_greatsword", new Stats(11.5, 1.0, 3.0, 375));
        DEFAULTS.put("skyrim_steel_battleaxe", new Stats(12.5, 0.9, 3.0, 425));
        DEFAULTS.put("skyrim_steel_warhammer", new Stats(13.5, 0.8, 3.1, 450));
        DEFAULTS.put("skyrim_dwarven_dagger", new Stats(5.0, 2.0, 1.8, 225));
        DEFAULTS.put("skyrim_dwarven_sword", new Stats(7.0, 1.6, 2.5, 275));
        DEFAULTS.put("skyrim_dwarven_war_axe", new Stats(8.0, 1.3, 2.4, 300));
        DEFAULTS.put("skyrim_dwarven_mace", new Stats(9.0, 1.1, 2.3, 325));
        DEFAULTS.put("skyrim_dwarven_greatsword", new Stats(12.0, 1.0, 3.0, 425));
        DEFAULTS.put("skyrim_dwarven_battleaxe", new Stats(13.0, 0.9, 3.0, 475));
        DEFAULTS.put("skyrim_dwarven_warhammer", new Stats(14.0, 0.8, 3.1, 500));
        DEFAULTS.put("skyrim_dwarven_bow", new Stats(6.0, 1.2, 4.0, 350));
        DEFAULTS.put("skyrim_elven_dagger", new Stats(5.5, 2.0, 1.8, 275));
        DEFAULTS.put("skyrim_elven_sword", new Stats(7.5, 1.6, 2.5, 325));
        DEFAULTS.put("skyrim_elven_war_axe", new Stats(8.5, 1.3, 2.4, 350));
        DEFAULTS.put("skyrim_elven_mace", new Stats(9.5, 1.1, 2.3, 375));
        DEFAULTS.put("skyrim_elven_greatsword", new Stats(12.5, 1.0, 3.0, 475));
        DEFAULTS.put("skyrim_elven_battleaxe", new Stats(13.5, 0.9, 3.0, 525));
        DEFAULTS.put("skyrim_elven_warhammer", new Stats(14.5, 0.8, 3.1, 550));
        DEFAULTS.put("skyrim_elven_bow", new Stats(6.5, 1.2, 4.0, 400));
        DEFAULTS.put("skyrim_orcish_dagger", new Stats(6.0, 2.0, 1.8, 325));
        DEFAULTS.put("skyrim_orcish_sword", new Stats(8.0, 1.6, 2.5, 375));
        DEFAULTS.put("skyrim_orcish_war_axe", new Stats(9.0, 1.3, 2.4, 400));
        DEFAULTS.put("skyrim_orcish_mace", new Stats(10.0, 1.1, 2.3, 425));
        DEFAULTS.put("skyrim_orcish_greatsword", new Stats(13.0, 1.0, 3.0, 525));
        DEFAULTS.put("skyrim_orcish_battleaxe", new Stats(14.0, 0.9, 3.0, 575));
        DEFAULTS.put("skyrim_orcish_warhammer", new Stats(15.0, 0.8, 3.1, 600));
        DEFAULTS.put("skyrim_orcish_bow", new Stats(7.0, 1.2, 4.0, 450));
        DEFAULTS.put("skyrim_glass_dagger", new Stats(6.5, 2.0, 1.8, 375));
        DEFAULTS.put("skyrim_glass_sword", new Stats(8.5, 1.6, 2.5, 425));
        DEFAULTS.put("skyrim_glass_war_axe", new Stats(9.5, 1.3, 2.4, 450));
        DEFAULTS.put("skyrim_glass_mace", new Stats(10.5, 1.1, 2.3, 475));
        DEFAULTS.put("skyrim_glass_greatsword", new Stats(13.5, 1.0, 3.0, 575));
        DEFAULTS.put("skyrim_glass_battleaxe", new Stats(14.5, 0.9, 3.0, 625));
        DEFAULTS.put("skyrim_glass_warhammer", new Stats(15.5, 0.8, 3.1, 650));
        DEFAULTS.put("skyrim_glass_bow", new Stats(7.5, 1.2, 4.0, 500));
        DEFAULTS.put("skyrim_draugr_sword", new Stats(6.0, 1.6, 2.7, 200));
        DEFAULTS.put("skyrim_draugr_war_axe", new Stats(7.0, 1.3, 2.6, 225));
        DEFAULTS.put("skyrim_draugr_greatsword", new Stats(11.0, 1.0, 3.4, 350));
        DEFAULTS.put("skyrim_draugr_battleaxe", new Stats(12.0, 0.9, 3.5, 400));
        DEFAULTS.put("skyrim_draugr_bow", new Stats(5.5, 1.2, 4.5, 250));
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
                    player.sendSystemMessage(Component.literal("SkyCraft weapon test kit added: dagger, sword, greatsword, battleaxe."));
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
