#include "package.hpp"
#include "steamui.hpp"

#include "../hooks.hpp"
#include "../config.hpp"
#include "../lua/LuaLoader.hpp"
#include "../log.hpp"

#include <atomic>
#include <mutex>
#include <vector>

namespace {
    std::atomic<bool>  g_active{false};
    std::atomic<void*> g_pkg{nullptr};
    std::atomic<void*> g_cuser{nullptr};
    std::mutex         g_injectMtx;   // guards pkg0 mutation; only taken on a Steam thread now
    // Set by notifyLicenseChanged (lua FileWatcher thread); drained by
    // pumpOnSteamThread (Steam thread) so the actual pkg0 mutation + Mark/Process
    // never runs on the foreign FileWatcher thread (§8 cross-thread race → crash).
    std::atomic<bool>  g_pendingChange{false};

    // Grow AppIdVec in batches to amortise realloc calls. 16 exceeds the typical lua
    // set size (~10 ids) so a single grow is almost always enough.
    static constexpr int kAppIdVecGrowBatch = 16;
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

// Append into spare capacity; if full, grow via Steam's CUtlMemory::Grow then retry.
// Caller holds g_injectMtx. Returns false only if grow is unavailable or still fails.
static bool appendAppIdGrowing(CUtlVector<uint32_t>* vec, uint32_t appId)
{
    if (!vec) return false;
    if (appendAppIdInPlace(vec, appId)) return true;   // spare slot existed
    if (!Hooks::oCUtlMemoryGrow) return false;
    // NOTE: Grow triggers a realloc that frees the old m_pMemory; Steam threads
    // reading AppIdVec lock-free during this window risk a use-after-free — an
    // escalation over the §8 in-place-write race (which only risks a stale value).
    // Accepted per architecture; grow only fires when the lua set exceeds the
    // initial spare (~10). Monitor the grow path during Deck validation (Task 8).
    Hooks::oCUtlMemoryGrow(&vec->m_Memory, kAppIdVecGrowBatch); // grow by a batch (amortize)
    return appendAppIdInPlace(vec, appId);              // retry after grow
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
	// §8 (proven 2026-06-03 Task 8): Mark/Process mutate the CUser license table
	// that Steam's own threads walk. Doing this from the foreign FileWatcher thread
	// corrupted Steam and crashed the IPC thread (SIGSEGV). Both injection paths now
	// run on a Steam thread (pumpOnSteamThread, from hkUser_CheckAppOwnership), which
	// is synchronised with Steam's own execution — the same path that was stable at
	// boot. The PackageInjection config gate remains the kill switch.
    Hooks::oMarkLicenseAsChanged(cuser, 0 /*pkgId*/, 1 /*bChanged*/);
    Hooks::oProcessPendingLicenseUpdates(cuser);
    return true;
}

void tryInitFakeLicenseOnce()
{
    if (!g_config.packageInjection.get()) return;
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
        if (appendAppIdGrowing(vec, id)) ++added; else ++dropped;
    }
    if (dropped) g_pLog->warn("Package: %zu ids dropped — pkg0 AppIdVec out of spare capacity\n", dropped);
    // getAllDepotIds() above already injected the full owned set, including the
    // additions queued during the boot-time lua load. Drain (discard) the pending
    // queues so the first applyPendingChanges only processes true post-boot deltas
    // instead of redundantly re-adding the boot set.
    LuaLoader::takePendingAdditions();
    LuaLoader::takePendingRemovals();
    g_active.store(true, std::memory_order_release);
    if (!markAndProcess())
        g_pLog->warn("Package: markAndProcess skipped at init (deps not ready)\n");
    g_pLog->once("Package: injected %zu appIds into pkg0, license re-evaluated\n", added);
}

// Apply queued lua hot-reload add/removes to pkg0 + live re-evaluate. MUST run on
// a Steam thread (§8): doing this from the foreign FileWatcher thread races Steam's
// license/IPC threads and corrupts them (proven SIGSEGV, Task 8). Only reached via
// pumpOnSteamThread after g_active, so pkg0 is captured and injected.
static void applyPendingChanges()
{
    void* pkg = g_pkg.load(std::memory_order_acquire);
    if (!pkg) return;

    std::lock_guard<std::mutex> lock(g_injectMtx);
    auto* vec = PackageInfo::appIdVec(pkg);
    size_t changed = 0;
    std::vector<uint32_t> removed;
    for (uint32_t id : LuaLoader::takePendingRemovals())
        if (findAndFastRemove(vec, id)) { ++changed; removed.push_back(id); }
    for (uint32_t id : LuaLoader::takePendingAdditions()) {
        findAndFastRemove(vec, id);                 // drop any existing copy first (de-dup, matches tryInit)
        if (appendAppIdGrowing(vec, id)) ++changed;
    }
    bool processed = false;
    if (changed) {
        processed = markAndProcess();
        if (!processed) g_pLog->warn("Package: markAndProcess skipped on live change (deps not ready)\n");
        g_pLog->info("Package: live license change applied (%zu)\n", changed);
    }
    if (processed)
        for (uint32_t id : removed) SteamUI::removeAppAndSendChange(id);
}

// onDepotsChanged callback — runs on the lua FileWatcher thread. Do NOT touch pkg0
// / Mark/Process here (§8 cross-thread race → crash). Just signal; pumpOnSteamThread
// drains it on a Steam thread.
void notifyLicenseChanged()
{
    if (!g_config.packageInjection.get()) return;
    g_pendingChange.store(true, std::memory_order_release);
}

// Runs on a Steam thread (from hkUser_CheckAppOwnership, fired frequently). Performs
// the one-shot initial injection, then drains any pending lua hot-reload changes —
// all Steam-thread-side so nothing mutates Steam's tables from a foreign thread.
void pumpOnSteamThread()
{
    tryInitFakeLicenseOnce();
    if (!g_active.load(std::memory_order_acquire)) return;   // not injected yet; pending stays set
    if (g_pendingChange.exchange(false, std::memory_order_acq_rel))
        applyPendingChanges();
}

} // namespace Package
