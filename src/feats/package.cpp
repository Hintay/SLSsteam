#include "package.hpp"

#include "../hooks.hpp"
#include "../lua/LuaLoader.hpp"
#include "../log.hpp"

#include <atomic>
#include <mutex>

namespace {
    std::atomic<bool>  g_active{false};
    std::atomic<void*> g_pkg{nullptr};
    std::atomic<void*> g_cuser{nullptr};
    std::mutex         g_injectMtx;   // serialize notify vs init (FileWatcher thread)
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

void setInjectedPackage(void* pkg) { g_pkg.store(pkg, std::memory_order_release); }
void setCUser(void* cuser)         { g_cuser.store(cuser, std::memory_order_release); }

// Mark package 0 changed + process pending license updates so Steam re-evaluates
// ownership without a restart. Caller holds g_injectMtx. Returns false if deps not ready.
static bool markAndProcess()
{
    void* cuser = g_cuser.load(std::memory_order_acquire);
    if (!cuser || !Hooks::oMarkLicenseAsChanged || !Hooks::oProcessPendingLicenseUpdates)
        return false;
	// §8 (proven 2026-06-03): Mark/Process mutate the CUser license table that
	// Steam's own threads walk. This is safe ONLY because we run inside the full
	// SLSsteam hook set (CheckAppOwnership/GetSubscribedApps/IPC spoofs all live),
	// which makes the re-evaluation consistent — an ISOLATED inject crashes Steam.
	// notifyLicenseChanged runs on the FileWatcher thread; if Deck validation shows
	// cross-thread instability, post markAndProcess onto a Steam thread (the
	// PackageInjection config gate is the kill switch for now).
    Hooks::oMarkLicenseAsChanged(cuser, 0 /*pkgId*/, 1 /*bChanged*/);
    Hooks::oProcessPendingLicenseUpdates(cuser);
    return true;
}

void tryInitFakeLicenseOnce()
{
    if (g_active.load(std::memory_order_acquire)) return;
    void* pkg = g_pkg.load(std::memory_order_acquire);
    if (!pkg || !g_cuser.load(std::memory_order_acquire)) return;
    if (PackageInfo::status(pkg) != 0) return; // not Available

    std::lock_guard<std::mutex> lock(g_injectMtx);
    if (g_active.load(std::memory_order_acquire)) return; // double-checked

    auto* vec = PackageInfo::appIdVec(pkg);
    const auto ids = LuaLoader::getAllDepotIds();
    size_t added = 0, dropped = 0;
    for (uint32_t id : ids) {
        findAndFastRemove(vec, id);                  // drop any existing copy (de-dup)
        if (appendAppIdInPlace(vec, id)) ++added; else ++dropped;
    }
    if (dropped) g_pLog->warn("Package: %zu ids dropped — pkg0 AppIdVec out of spare capacity\n", dropped);
    g_active.store(true, std::memory_order_release);
    if (!markAndProcess())
        g_pLog->warn("Package: markAndProcess skipped at init (deps not ready)\n");
    g_pLog->once("Package: injected %zu appIds into pkg0, license re-evaluated\n", added);
}

void notifyLicenseChanged()
{
    void* pkg = g_pkg.load(std::memory_order_acquire);
    if (!pkg || !g_active.load(std::memory_order_acquire)) { tryInitFakeLicenseOnce(); return; }

    std::lock_guard<std::mutex> lock(g_injectMtx);
    auto* vec = PackageInfo::appIdVec(pkg);
    size_t changed = 0;
    for (uint32_t id : LuaLoader::takePendingRemovals())  if (findAndFastRemove(vec, id)) ++changed;
    for (uint32_t id : LuaLoader::takePendingAdditions()) if (appendAppIdInPlace(vec, id)) ++changed;
    if (changed) {
        if (!markAndProcess())
            g_pLog->warn("Package: markAndProcess skipped on live change (deps not ready)\n");
        g_pLog->info("Package: live license change applied (%zu)\n", changed);
    }
}

} // namespace Package
