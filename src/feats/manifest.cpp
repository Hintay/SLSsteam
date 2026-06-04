#include "manifest.hpp"

#include "../config.hpp"
#include "../log.hpp"
#include "../lua/LuaLoader.hpp"


void Manifest::patchDepotInfo(CUtlVector<DepotEntry>* pDepotInfo)
{
	DepotEntry* entries = pDepotInfo->m_Memory.m_pMemory;
	if (!entries)
	{
		return;
	}

	// Trust neither m_Size nor m_pMemory blindly: BuildDepotDependency can return
	// on the browse/verify path with the vector half-populated. m_Size is signed
	// (a negative sentinel already yields zero iterations), but guard the upper
	// bound against a stale positive count so we never walk past the real
	// allocation and read/write Steam-owned memory.
	const int32_t size = pDepotInfo->m_Size;
	if (size <= 0 || static_cast<uint32_t>(size) > pDepotInfo->m_Memory.m_nAllocationCount)
	{
		return;
	}
	if (!g_config.useLuaManifestOverrides.get())
	{
		return;
	}

	for (int32_t i = 0; i < size; ++i)
	{
		DepotEntry& entry = entries[i];

		const auto ov = LuaLoader::getManifest(entry.DepotId);
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
