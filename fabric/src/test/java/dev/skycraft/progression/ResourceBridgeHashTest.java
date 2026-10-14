package dev.skycraft.progression;

import static org.junit.jupiter.api.Assertions.assertEquals;
import org.junit.jupiter.api.Test;

final class ResourceBridgeHashTest {
	@Test
	void fnv1aRegistryIdsAreStable() {
		assertEquals(0x935D0568, ResourceBridge.itemHash("minecraft:iron_ingot"));
		assertEquals(0xF9ADF8A2, ResourceBridge.itemHash("minecraft:apple"));
		assertEquals(0xD3E6DE4B, ResourceBridge.itemHash("minecraft:leather"));
		assertEquals(0x20208F03, ResourceBridge.itemHash("minecraft:raw_iron"));
	}
}
