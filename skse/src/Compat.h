#pragma once

// Helpers for making runtime copies of Skyrim's records (the stand-in actors' own NPC and race, see Combat.cpp).
// Written against this CommonLibSSE-NG's TESForm.h / TESNPC.h / TESRace.h: TESForm::CreateDuplicateForm(bool, NiTPointerMap<TESForm*, TESForm*>*)
// is the engine's own "duplicate this record" and gives the copy a new dynamic form id. (TESForm::Copy is a no-op for a race, which does
// not override it, so Copy into a blank form is not used.)

// CommonLib declares NiTMapBase's four virtual functions for a <TESForm*, TESForm*> map but never defines them, so a map of that type can't be
// built (LNK2001 at revision 51). CreateDuplicateForm wants one only as a scratch "old form -> new form" map that nothing here reads, so these
// are the simplest definitions that are right for that: a pointer hash and pointer equality; stored values are not needed.
namespace RE
{
	template <>
	inline std::uint32_t NiTMapBase<NiTPointerAllocator<std::uint64_t>, TESForm*, TESForm*>::hash_function(TESForm* a_key) const
	{
		return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_key) >> 4);
	}

	template <>
	inline bool NiTMapBase<NiTPointerAllocator<std::uint64_t>, TESForm*, TESForm*>::key_eq(TESForm* a_lhs, TESForm* a_rhs) const
	{
		return a_lhs == a_rhs;
	}

	template <>
	inline void NiTMapBase<NiTPointerAllocator<std::uint64_t>, TESForm*, TESForm*>::assign_value(NiTMapItem<TESForm*, TESForm*>*, TESForm*, TESForm*)
	{}

	template <>
	inline void NiTMapBase<NiTPointerAllocator<std::uint64_t>, TESForm*, TESForm*>::clear_value(NiTMapItem<TESForm*, TESForm*>*)
	{}
}

namespace skycraft::compat
{
	// A new form of the same kind as a_src, with a_src's data; nullptr if the engine made none.
	template <class T>
	T* CloneForm(T* a_src, bool& a_copied)
	{
		a_copied = false;
		if (!a_src) {
			return nullptr;
		}
		RE::NiTPointerMap<RE::TESForm*, RE::TESForm*> copyMap;
		auto*                                         dup = a_src->CreateDuplicateForm(false, &copyMap);
		if (!dup) {
			return nullptr;
		}
		a_copied = true;
		return static_cast<T*>(dup);
	}

	// Does a duplicated race really hold the stock race's data? (Checked before it is used: a blank race would crash the actor.)
	inline bool RaceLooksCopied(const RE::TESRace* a_copy, const RE::TESRace* a_stock)
	{
		return a_copy && a_stock && a_copy != a_stock && a_copy->data.height[0] == a_stock->data.height[0] &&
		       a_copy->data.startingHealth == a_stock->data.startingHealth && a_copy->data.flags.underlying() == a_stock->data.flags.underlying() &&
		       a_copy->bodyPartData == a_stock->bodyPartData && a_copy->behaviorGraphs[0].GetModel() != nullptr;
	}

	// Points a race's skeleton model(s) at a_path (relative to Data\Meshes).
	inline bool SetSkeleton(RE::TESRace* a_race, const char* a_path)
	{
		if (!a_race || !a_path || !*a_path) {
			return false;
		}
		for (auto& model : a_race->skeletonModels) {
			model.SetModel(a_path);
		}
		return true;
	}
}
