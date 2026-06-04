#include "package.hpp"
#include "steamui.hpp"

#include "../hooks.hpp"
#include "../config.hpp"
#include "apps.hpp"
#include "../lua/LuaLoader.hpp"
#include "../ownership.hpp"
#include "../log.hpp"

#include <atomic>
#include <mutex>
#include <vector>

namespace {
    std::atomic<bool>  g_active{false};
    std::atomic<void*> g_pkg{nullptr};
    std::atomic<void*> g_cuser{nullptr};
    std::mutex         g_injectMtx;   // guards pkg0 mutation and cross-thread pending-change queues
    // Set by notifyLicenseChanged (lua FileWatcher thread); drained by
    // pumpOnSteamThread (Steam thread) so the actual pkg0 mutation + Mark/Process
    // never runs on the foreign FileWatcher thread (§8 cross-thread race → crash).
    std::atomic<bool>  g_pendingChange{false};
    std::vector<uint32_t> g_pendingConfigAdditions;
    std::vector<uint32_t> g_pendingConfigRemovals;

    // Grow AppIdVec in batches to amortise realloc calls. 16 exceeds the typical lua
    // set size (~10 ids) so a single grow is almost always enough.
    static constexpr int kAppIdVecGrowBatch = 16;

    struct ThreadPumpGuard {
        bool& active;
        bool entered;

        explicit ThreadPumpGuard(bool& active) : active(active), entered(!active)
        {
            if (entered) active = true;
        }

        ~ThreadPumpGuard()
        {
            if (entered) active = false;
        }
    };

    static void appendUnique(std::vector<uint32_t>& dst, const std::vector<uint32_t>& src)
    {
        for (uint32_t id : src) {
            bool found = false;
            for (uint32_t existing : dst) {
                if (existing == id) {
                    found = true;
                    break;
                }
            }
            if (!found) dst.push_back(id);
        }
    }
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

void queueAppIdChanges(const std::vector<uint32_t>& additions, const std::vector<uint32_t>& removals)
{
    if (!g_config.packageInjection.get()) return;
    if (additions.empty() && removals.empty()) return;

    {
        std::lock_guard<std::mutex> lock(g_injectMtx);
        appendUnique(g_pendingConfigAdditions, additions);
        appendUnique(g_pendingConfigRemovals, removals);
    }
    g_pendingChange.store(true, std::memory_order_release);
}

// Mark package 0 changed + process pending license updates so Steam re-evaluates
// ownership without a restart. Do not call while holding g_injectMtx: Steam may
// re-enter our hooks during ProcessPendingLicenseUpdates, and re-taking the mutex
// would deadlock. Returns false if deps are not ready.
static bool markAndProcess()
{
    void* cuser = g_cuser.load(std::memory_order_acquire);
    if (!cuser || !Hooks::oMarkLicenseAsChanged || !Hooks::oProcessPendingLicenseUpdates)
        return false;
	// §8 (proven 2026-06-03 Task 8): Mark/Process mutate the CUser license table
	// that Steam's own threads walk. Doing this from the foreign FileWatcher thread
	// corrupted Steam and crashed the IPC thread (SIGSEGV). Both injection paths now
	// run on a Steam thread via pumpOnSteamThread. The PackageInjection config gate
	// remains the kill switch.
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

    size_t added = 0, dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_injectMtx);
        if (g_active.load(std::memory_order_acquire)) return; // double-checked

        auto* vec = PackageInfo::appIdVec(pkg);
        const auto ids = Ownership::getControlledAppIds();
        for (uint32_t id : ids) {
            findAndFastRemove(vec, id);                  // drop any existing copy (de-dup)
            if (appendAppIdGrowing(vec, id)) ++added; else ++dropped;
        }
        // The final config set above already includes yaml AdditionalApps plus lua
        // contributions. Drain boot-time queues so the first live apply only handles
        // true post-injection deltas instead of redundantly re-applying the boot set.
        LuaLoader::takePendingAdditions();
        LuaLoader::takePendingRemovals();
        g_pendingConfigAdditions.clear();
        g_pendingConfigRemovals.clear();
        g_active.store(true, std::memory_order_release);
    }

    if (dropped) g_pLog->warn("Package: %zu ids dropped — pkg0 AppIdVec out of spare capacity\n", dropped);
    if (!markAndProcess())
        g_pLog->warn("Package: markAndProcess skipped at init (deps not ready)\n");
    g_pLog->once("Package: injected %zu appIds into pkg0, license re-evaluated\n", added);
}

// Apply queued config/lua hot-reload add/removes to pkg0 + live re-evaluate. MUST
// run on a Steam thread (§8): doing this from the foreign FileWatcher thread races
// Steam's license/IPC threads and corrupts them (proven SIGSEGV, Task 8). Only
// reached via pumpOnSteamThread after g_active, so pkg0 is captured and injected.
static void applyPendingChanges()
{
    void* pkg = g_pkg.load(std::memory_order_acquire);
    if (!pkg) return;

    size_t changed = 0;
    std::vector<uint32_t> removals;
    std::vector<uint32_t> additions;
    std::vector<uint32_t> removed;
    {
        std::lock_guard<std::mutex> lock(g_injectMtx);
        auto* vec = PackageInfo::appIdVec(pkg);
        appendUnique(removals, LuaLoader::takePendingRemovals());
        appendUnique(removals, g_pendingConfigRemovals);
        appendUnique(additions, LuaLoader::takePendingAdditions());
        appendUnique(additions, g_pendingConfigAdditions);
        g_pendingConfigRemovals.clear();
        g_pendingConfigAdditions.clear();

        for (uint32_t id : removals) {
            if (Ownership::isControlledApp(id)) continue;
            if (findAndFastRemove(vec, id)) { ++changed; removed.push_back(id); }
        }
        for (uint32_t id : additions) {
            if (!Ownership::isControlledApp(id)) continue;
            findAndFastRemove(vec, id);                 // drop any existing copy first (de-dup, matches tryInit)
            if (appendAppIdGrowing(vec, id)) ++changed;
        }
    }

    for (uint32_t id : removals)
        if (!Ownership::isControlledApp(id)) Ownership::unmarkGenuinelyOwned(id);

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

// Runs on a Steam thread. Performs the one-shot initial injection, then drains
// pending lua/yaml hot-reload changes — all Steam-thread-side so nothing mutates
// Steam's tables from a foreign thread. Guard against same-thread re-entry because
// ProcessPendingLicenseUpdates may call back into hooks that also pump.
void pumpOnSteamThread()
{
    static thread_local bool s_inPump = false;
    ThreadPumpGuard guard(s_inPump);
    if (!guard.entered) return;

    tryInitFakeLicenseOnce();
    if (!g_active.load(std::memory_order_acquire)) return;   // not injected yet; pending stays set
    if (g_pendingChange.exchange(false, std::memory_order_acq_rel))
        applyPendingChanges();
}

} // namespace Package
