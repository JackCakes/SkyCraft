#pragma once

#include "Link.h"

namespace skycraft::ResourceBridge
{
	// Registers the Skyrim inventory listener. No ESP, Papyrus script, or saved quest state is used.
	void Install();

	// A load/new game invalidates any in-flight request. Minecraft acknowledgements from the old
	// state are then ignored, so a quickload cannot remove an item from the newly loaded save.
	void OnGameLoaded();

	// Combat owns the Minecraft -> Skyrim event ring; it forwards resource acknowledgements here.
	// Returns true when the event belonged to this bridge.
	bool HandleEvent(const proto::McEvent& a_event);
}
