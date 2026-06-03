#include "package.hpp"

#include "../log.hpp"

#include <atomic>

namespace {
    std::atomic<bool> g_active{false};
}

namespace Package {

bool isActive() { return g_active.load(std::memory_order_acquire); }

bool appendAppIdInPlace(CUtlVector<uint32_t>* vec, uint32_t appId)
{
    if (!vec || vec->m_Size < 0 || !vec->m_Memory.m_pMemory) return false;
    if (static_cast<uint32_t>(vec->m_Size) >= vec->m_Memory.m_nAllocationCount)
        return false; // no spare capacity; first version never reallocs
    vec->m_Memory.m_pMemory[vec->m_Size] = appId;
    vec->m_Size += 1;
    return true;
}

bool findAndFastRemove(CUtlVector<uint32_t>* vec, uint32_t appId)
{
    if (!vec || !vec->m_Memory.m_pMemory) return false;
    for (int32_t i = 0; i < vec->m_Size; ++i) {
        if (vec->m_Memory.m_pMemory[i] == appId) {
            vec->m_Memory.m_pMemory[i] = vec->m_Memory.m_pMemory[vec->m_Size - 1];
            vec->m_Size -= 1;
            return true;
        }
    }
    return false;
}

// Stubs filled in Task 5.
void setInjectedPackage(void*) {}
void setCUser(void*) {}
void notifyLicenseChanged() {}
void tryInitFakeLicenseOnce() {}

} // namespace Package
