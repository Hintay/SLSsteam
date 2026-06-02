#pragma once

#include <cstdint>


// In-memory layout of Valve's CUtlMemory/CUtlVector and the DepotEntry
// records BuildDepotDependency emits. Mirrors OpenSteamTool's Structs.h
// exactly so the manifest hook can read and patch the depot vector in place.

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
	uint32_t      m_Size;   // 0x0c — element count
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
