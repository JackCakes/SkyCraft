package dev.skycraft.landon;

import dev.skycraft.SkyCraft;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.minecraft.commands.Commands;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.EquipmentSlotGroup;
import net.minecraft.world.entity.ai.attributes.AttributeModifier;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.component.ItemAttributeModifiers;

/** Small, isolated showcase items made for Landon. */
public final class LandonItems {
    public static Item LANDON_COOKIE;
    public static Item HIDDEN_LEAF_KUNAI;

    private LandonItems() {}

    public static void init() {
        LANDON_COOKIE = registerSimple("landon_cookie", 16);
        HIDDEN_LEAF_KUNAI = registerKunai();
        registerCommand();
        SkyCraft.LOG.info("SkyCraft Landon set loaded: Landon Cookie + Hidden Leaf Kunai");
    }

    private static Item registerSimple(String path, int stackSize) {
        Identifier id = Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, path);
        ResourceKey<Item> key = ResourceKey.create(Registries.ITEM, id);
        return Registry.register(BuiltInRegistries.ITEM, id,
            new Item(new Item.Properties().setId(key).stacksTo(stackSize)));
    }

    private static Item registerKunai() {
        String path = "hidden_leaf_kunai";
        Identifier id = Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, path);
        ResourceKey<Item> key = ResourceKey.create(Registries.ITEM, id);

        Item.Properties props = new Item.Properties()
            .setId(key)
            .stacksTo(1)
            .durability(180)
            .attributes(ItemAttributeModifiers.builder()
                .add(Attributes.ATTACK_DAMAGE,
                    new AttributeModifier(Item.BASE_ATTACK_DAMAGE_ID, 4.0, AttributeModifier.Operation.ADD_VALUE),
                    EquipmentSlotGroup.MAINHAND)
                .add(Attributes.ATTACK_SPEED,
                    new AttributeModifier(Item.BASE_ATTACK_SPEED_ID, -1.8, AttributeModifier.Operation.ADD_VALUE),
                    EquipmentSlotGroup.MAINHAND)
                .add(Attributes.ENTITY_INTERACTION_RANGE,
                    new AttributeModifier(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, path + "_reach"), -0.2, AttributeModifier.Operation.ADD_VALUE),
                    EquipmentSlotGroup.MAINHAND)
                .build());

        return Registry.register(BuiltInRegistries.ITEM, id, new Item(props));
    }

    private static void registerCommand() {
        CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) ->
            dispatcher.register(Commands.literal("landon")
                .then(Commands.literal("gift").executes(context -> {
                    ServerPlayer player = context.getSource().getPlayerOrException();
                    player.getInventory().add(new ItemStack(LANDON_COOKIE, 8));
                    player.getInventory().add(new ItemStack(HIDDEN_LEAF_KUNAI));
                    player.sendSystemMessage(Component.literal("A little SkyCraft gift for Landon ♥"));
                    return 1;
                }))));
    }
}
