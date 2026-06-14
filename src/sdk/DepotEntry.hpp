#pragma once

#include <cstddef>
#include <cstdint>


// In-memory layout of Valve's CUtlMemory/CUtlVector and the DepotEntry
// records BuildDepotDependency emits, so the manifest hook can read and patch
// the depot vector in place.

template<typename T>
struct CUtlMemory
{
	T*       m_pMemory;          // 0x00 — element storage base
	uint32_t m_nAllocationCount; // 0x04
	uint32_t m_nGrowSize;        // 0x08
};

template<typename T>
struct CUtlVector
{
	CUtlMemory<T> m_Memory; // 0x00 — base at m_Memory.m_pMemory
	int32_t       m_Size;   // 0x0c — element count (signed in Valve's CUtlVector;
	                        //        a negative sentinel yields zero iterations, not an overrun)
};

// Single depot manifest entry (0x20 bytes) produced by BuildDepotDependency.
struct DepotEntry
{
	uint32_t DepotId;       // 0x00
	uint32_t AppId;         // 0x04
	uint64_t ManifestGid;   // 0x08 — from depots/<id>/manifests/<branch>/gid
	uint64_t ManifestSize;  // 0x10 — from depots/<id>/manifests/<branch>/size
	uint32_t DlcAppId;      // 0x18 — associated DLC AppID, 0 if none
	uint8_t  LcsRequired;   // 0x1C — branches/<branch>/lcsrequired
	uint8_t  bNotNewTarget; // 0x1D — carried over from active list (not newly activated this call)
	uint8_t  SharedInstall; // 0x1E — sharedinstall / depotfromapp redirect
	uint8_t  Padding;       // 0x1F
};
static_assert(sizeof(DepotEntry) == 0x20, "DepotEntry must be 32 bytes");
// Guard the offsets the manifest hook reads/writes: a future field reorder that
// preserved total size would otherwise silently shift these into Steam-owned memory.
static_assert(offsetof(DepotEntry, DepotId)      == 0x00, "DepotId offset");
static_assert(offsetof(DepotEntry, ManifestGid)  == 0x08, "ManifestGid offset");
static_assert(offsetof(DepotEntry, ManifestSize) == 0x10, "ManifestSize offset");
static_assert(offsetof(DepotEntry, DlcAppId)     == 0x18, "DlcAppId offset");
