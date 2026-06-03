#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cstdio>

// NOTE: This smoke validates accessor LOGIC only, and is self-consistent with the
// host's pointer size (it writes and reads m_Size through the same host struct
// layout). It does NOT validate the real 32-bit on-target offset of m_Size (+0x44):
// that is enforced by PackageInfo.hpp's static_assert when the Deck builds the
// 32-bit .so. On a 64-bit host, m_Size physically lands at buf+0x38+0x10, which is
// fine because both the write and the read go through this same mirrored struct.

// Self-contained mirror of the CUtlMemory/CUtlVector layout used by PackageInfo.hpp.
// NOT compiled for 32-bit target here — this is a host-native logic smoke test only.
// 32-bit layout assertions live in PackageInfo.hpp and are validated by the Deck build.
template<typename T> struct CUtlMemory { T* m_pMemory; uint32_t m_nAllocationCount; uint32_t m_nGrowSize; };
template<typename T> struct CUtlVector { CUtlMemory<T> m_Memory; int32_t m_Size; };

namespace PackageInfo {
    static constexpr size_t kStatusOff   = 0x18;
    static constexpr size_t kAppIdVecOff = 0x38;
    inline uint32_t status(void* p) { return *reinterpret_cast<uint32_t*>((char*)p + kStatusOff); }
    inline CUtlVector<uint32_t>* appIdVec(void* p) {
        return reinterpret_cast<CUtlVector<uint32_t>*>((char*)p + kAppIdVecOff);
    }
}

int main() {
    alignas(16) unsigned char buf[0x100] = {0};
    *reinterpret_cast<uint32_t*>(buf + 0x18) = 0;     // Status = Available
    uint32_t storage[4] = {5, 7, 70, 0};
    auto* vec = PackageInfo::appIdVec(buf);
    vec->m_Memory.m_pMemory = storage;
    vec->m_Memory.m_nAllocationCount = 4;
    vec->m_Size = 3;

    assert(PackageInfo::status(buf) == 0);
    assert(PackageInfo::appIdVec(buf)->m_Size == 3);
    assert(PackageInfo::appIdVec(buf)->m_Memory.m_pMemory[2] == 70);
    printf("pkg_smoke: PackageInfo accessors OK\n");

    // ── Task 2: append-into-spare + find-and-fast-remove ──
    auto appendInPlace = [](CUtlVector<uint32_t>* v, uint32_t id) -> bool {
        if (v->m_Size < 0) return false;
        if ((uint32_t)v->m_Size >= v->m_Memory.m_nAllocationCount) return false;
        v->m_Memory.m_pMemory[v->m_Size] = id;
        v->m_Size += 1;
        return true;
    };
    auto fastRemove = [](CUtlVector<uint32_t>* v, uint32_t id) -> bool {
        for (int32_t i = 0; i < v->m_Size; ++i) {
            if (v->m_Memory.m_pMemory[i] == id) {
                v->m_Memory.m_pMemory[i] = v->m_Memory.m_pMemory[v->m_Size - 1];
                v->m_Size -= 1;
                return true;
            }
        }
        return false;
    };
    uint32_t st2[4] = {5, 7, 70, 0};
    CUtlVector<uint32_t> v2{}; v2.m_Memory.m_pMemory = st2;
    v2.m_Memory.m_nAllocationCount = 4; v2.m_Size = 3;
    assert(appendInPlace(&v2, 1139900) == true);   // uses the 1 spare slot
    assert(v2.m_Size == 4 && st2[3] == 1139900);
    assert(appendInPlace(&v2, 999) == false);       // full -> no realloc
    assert(v2.m_Size == 4);
    assert(fastRemove(&v2, 7) == true);             // remove middle; last fills it
    assert(v2.m_Size == 3 && st2[1] == 1139900);
    assert(fastRemove(&v2, 424242) == false);       // absent
    printf("pkg_smoke: append/remove OK\n");

    return 0;
}
