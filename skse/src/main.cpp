#include "Dig.h"
#include "ResourceBridge.h"
#include "Game.h"

namespace
{
	void SetupLog()
	{
		auto dir = SKSE::log::log_directory();
		if (!dir) {
			return;
		}
		auto path = *dir / "SkyCraft.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kInputLoaded:
			skycraft::Input::InstallSink();  // before other mods' sinks, so Minecraft's keys can be kept from them
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			if (!skycraft::Link::Get().Create()) {
				logger::error("SkyCraft disabled: could not create shared memory");
				return;
			}
			skycraft::Game::Install();
			skycraft::Input::Install();
			skycraft::Overlay::Install();
			skycraft::WorldRender::Install();
			skycraft::PathAvoid::Install();
			skycraft::Dig::Install();
			skycraft::LockOn::Install();
			skycraft::ResourceBridge::Install();
			skycraft::CrashLog::Install();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			skycraft::ResourceBridge::OnGameLoaded();
			skycraft::Game::OnGameLoaded();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .trampoline = true, .trampolineSize = 1024 });
	SetupLog();
	skycraft::CrashLog::Install();
	logger::info("SkyCraft {} loading (runtime {})", "0.1.2-Sunp22-adventure", a_skse->RuntimeVersion().string());
	logger::info("patch revision 52 (links: the scratch map CreateDuplicateForm takes has its four virtual functions defined here, CommonLib has none; revision 51: the custom stand-in NPC and race are made with the engine's own CreateDuplicateForm, the real CommonLib API, and the race copy is checked against the stock race before use; revision 50: the mob collision boxes are no longer sent to Minecraft as Skyrim walls: a pillager's arrow stuck in its own box and no hit reached a mob through it; revision 49: the plugin builds again: the CopyFrom / skeletonModels checks are template-safe (Compat.h), a creature record is cloned with CopyFrom or the engine's CreateDuplicateForm, whichever this CommonLib has; revision 48: the custom skeletons are real actor skeletons: NPC Root/COM/Head nodes and a keyframed body on the biped layer, not the static block box; revision 47: stand-in actors get a skeleton of our own per mob size, a box, with the NPC bounds set to it; the first one is a trial that turns custom skeletons off if the actor gets no 3D; revision 46: stand-in actors are made from a custom NPC record per mob size, built at runtime from the library's sizes, and kept in a pool per size; the box body is the mob's exact shape; revision 45: round one of the custom stand-in actors: a probe at the first mob, logged as 'Mob actor probe', that reads how Skyrim's creature skeletons are laid out and tries making an NPC and race record at runtime; nothing else changes; revision 44: the stand-in actors are back alongside the mob bodies, bStandInActors in SkyCraft.ini, because Skyrim's NPCs can only fight an actor; revision 43: mob bodies: every Minecraft mob is a collision box the size of its bounding box in Skyrim's world, made from a mob catalogue Minecraft writes at start (modded mobs included), replacing the stand-in actors; revision 42: damage diagnostics merged in: every blow that reaches a mob's stand-in is logged with who threw it, and Skyrim hits the Minecraft side cannot find a live mob for are logged; revision 41: only doors, trapdoors and fence gates lose their collision when open (a barrel has an open state too and stays solid); every change of a section that Skyrim takes up is logged; revision 40: a section sent again with the same collision no longer makes Skyrim rebuild its collision model, so the sections beside a door stop being rebuilt many times a second and a door change shows at once; revision 39: open doors, trapdoors and fence gates have no collision in Skyrim; each open/close is logged on the Minecraft side; revision 38: mob stand-ins restored to the earlier, steadier version (health 1:1, damage guard, revival, 20 Hz positions from Fabric) on top of the collision and lock-on work; lock-on stays LockOn.cpp, with the stand-ins on its never-lock-on list; revision 37: mob stand-ins are back: invisible Skyrim actors for Minecraft mobs so NPCs fight them and spells hurt the mob; they sit exactly on the mob's feet at its height, stay in every control mode, and are on True Directional Movement's never-lock-on list; revision 36: doors, trapdoors and fence gates send their section to Skyrim again the moment their state changes, by any route, and the Fabric side watches every openable block each frame; walking up stairs, slabs and carpets lifts the player onto the step as Minecraft does, bStepUp in SkyCraft.ini; revision 35: the collision model's blocks are now in Skyrim's own order, the shapes before the list that holds them: the list was built before its children existed, a null read at load in every section model; the body bytes now match Skyrim's own statics: the second collision response and callback delay were zero, which is response type INVALID, and the rolling friction was 1; revision 34: the plugin makes the shape library models at load and places odd blocks as references to them once a section model is proven; collision models on by default, the crates stay under you until a model is seen working; tools/make_collision_library.py builds the shape library from the catalogue, which the Fabric side now writes by itself; ShapeKey in Game.h; revision 33: the Fabric side can list every distinct collision shape in the game with -Dskycraft.dumpShapes=true, ShapeCatalog.java; revision 32: collision test models are now loaded one at a time, smallest first, with a LOADING line logged before each; the byte-shifted variants are gone; revision 31: collision shapes: the array headers inside the list and convex shapes are empty like Skyrim's own, which was a null read at load; revision 30: collision model body now matches Skyrim's own statics: fixed motion system, flags 129 and 130; revision 29: collision models are off by default, a copy of vanilla models is made to see their layout; revision 28: section models are written into files made at plugin load, because Skyrim only loads model files that existed when it started; revision 27: smaller test models and copies of loose models tried too; revision 26: block model variants tried side by side, the first Skyrim loads is used; revision 25: exact block shapes: no padding on doors, stairs and other non-cube blocks, up to 16 boxes per block; a probe in the log tells a rejected .nif from a form that can't load models; revision 24: block collision models checked in the Havok world, crates kept until they work; lock-on survives a missing frame; revision 23: section models of merged boxes loaded as invisible statics over the whole area Minecraft sent; revision 22: crates only, the box simulation that held the Skyrim player is gone; Minecraft's mobs, passive ones too, are lock-on targets again, handed to True Directional Movement as virtual targets; revision 21: mob stand-ins removed)");
	skycraft::BlockCollision::PrepareFiles();
	skycraft::MobBodies::PrepareFiles();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	// As early as possible: Minecraft takes about as long to start as Skyrim does to reach its menu.
	skycraft::Launcher::StartMinecraft();
	return true;
}
