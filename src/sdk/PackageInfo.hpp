#pragma once
#include "DepotEntry.hpp"   // CUtlVector<T> / CUtlMemory<T>
#include <cstddef>
#include <cstdint>

// In-memory accessors for Steam's PackageInfo (steamclient.so). We do NOT model
// the full struct (intermediate fields unknown); only the offsets observed on
// build 64b86a11 are exposed. Re-derive offsets on a new build.

#if defined(__i386__) || defined(_M_IX86)
// Offsets above assume Valve's 32-bit CUtlMemory layout (m_pMemory@0, alloc@4, grow@8 = 12 bytes).
// Guarded to __i386__ so a 64-bit host compile of this header (where a pointer is 8 bytes) does not
// hard-error; validated for real when the Deck builds the 32-bit .so.
static_assert(sizeof(CUtlMemory<uint32_t>) == 12, "CUtlMemory<uint32> must be 12 bytes on the 32-bit target");
#endif

namespace PackageInfo {
    static constexpr size_t kStatusOff     = 0x18; // EPackageStatus: Available==0 (Invalid==3)
    static constexpr size_t kAppIdVecOff   = 0x38; // CUtlVector<uint32> (m_Size @ +0x44)
    static constexpr size_t kDepotIdVecOff = 0x48; // offset verified; accessor omitted (no current caller)

    inline uint32_t status(void* pkg) {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(pkg) + kStatusOff);
    }
    inline CUtlVector<uint32_t>* appIdVec(void* pkg) {
        return reinterpret_cast<CUtlVector<uint32_t>*>(reinterpret_cast<char*>(pkg) + kAppIdVecOff);
    }
}
