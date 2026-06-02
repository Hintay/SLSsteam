#include "manifest.hpp"

#include "../log.hpp"
#include "../lua/LuaLoader.hpp"


void Manifest::patchDepotInfo(CUtlVector<DepotEntry>* pDepotInfo)
{
	DepotEntry* entries = pDepotInfo->m_Memory.m_pMemory;
	if (!entries)
	{
		return;
	}

	for (uint32_t i = 0; i < pDepotInfo->m_Size; ++i)
	{
		DepotEntry& entry = entries[i];

		const LuaLoader::ManifestOverride* ov = LuaLoader::getManifest(entry.DepotId);
		if (!ov)
		{
			continue;
		}

		// A size of 0 in the override means "keep the original": it only affects
		// the download-size display, not the actual content pinned by the GID.
		const uint64_t newSize = ov->size ? ov->size : entry.ManifestSize;

		g_pLog->once
		(
			"Pinned manifest for depot %u: gid %llu -> %llu, size %llu -> %llu\n",
			entry.DepotId,
			(unsigned long long)entry.ManifestGid, (unsigned long long)ov->gid,
			(unsigned long long)entry.ManifestSize, (unsigned long long)newSize
		);

		entry.ManifestGid  = ov->gid;
		entry.ManifestSize = newSize;
	}
}
