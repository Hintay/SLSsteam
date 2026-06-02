#pragma once

#include "../sdk/DepotEntry.hpp"


namespace Manifest
{
	// Patches each DepotEntry in the output vector with a Lua-provided manifest
	// GID (and content size, when the override specifies a non-zero size).
	// Called from the BuildDepotDependency hook after the original runs, so the
	// vector is already populated. Entries without an override are left intact.
	void patchDepotInfo(CUtlVector<DepotEntry>* pDepotInfo);
}
