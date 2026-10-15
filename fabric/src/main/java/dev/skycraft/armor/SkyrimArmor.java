package dev.skycraft.armor;
import dev.skycraft.SkyCraft;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.item.equipment.ArmorMaterials;
import net.minecraft.world.item.equipment.ArmorType;
import net.minecraft.world.item.Item;
public final class SkyrimArmor {
 private SkyrimArmor() {}
 public static void init() {
  register("skyrim_iron_helmet", ArmorType.HELMET);
  register("skyrim_iron_chestplate", ArmorType.CHESTPLATE);
  register("skyrim_iron_gauntlets", ArmorType.LEGGINGS);
  register("skyrim_iron_boots", ArmorType.BOOTS);
  SkyCraft.LOG.info("SkyCraft armor prototype: Iron set registered");
 }
 private static void register(String path, ArmorType type) {
  Identifier id=Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID,path);
  ResourceKey<Item> key=ResourceKey.create(Registries.ITEM,id);
  Item.Properties props=new Item.Properties().setId(key).stacksTo(1).humanoidArmor(ArmorMaterials.IRON,type).durability(type.getDurability(15));
  Registry.register(BuiltInRegistries.ITEM,id,new Item(props));
 }
}