#include "steamui.hpp"

#include "apps.hpp"

#include "../hooks.hpp"
#include "../config.hpp"
#include "../ownership.hpp"
#include "../sdk/CSteamApp.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../globals.hpp"
#include "../log.hpp"

#include <atomic>

namespace {
    std::atomic<void*> g_controller{nullptr};
    std::atomic<void*> g_appChangeSource{nullptr};
}

namespace SteamUI {

void setController(void* p)       { g_controller.store(p, std::memory_order_release); }
void setAppChangeSource(void* p)  { g_appChangeSource.store(p, std::memory_order_release); }

// CONCURRENCY: called from Package::pumpOnSteamThread() after queued package
// removals have been drained on a Steam hook thread. FileWatcher/Lua/config
// threads must not call this directly; they only enqueue/signals pending deltas.
// The Steam calls below reach the REAL originals through detour trampolines
// (.tramp.fn), so they bypass our capture hooks and do not re-enter Package code.
void removeAppAndSendChange(uint32_t appId)
{
    void* ctrl = g_controller.load(std::memory_order_acquire);
    void* src  = g_appChangeSource.load(std::memory_order_acquire);
    if (!ctrl || !src) return;
    // Don't clear ownership of apps proven to be genuinely subscribed.
    if (Ownership::isGenuinelyOwned(appId)) return;
    void* app = Hooks::CSteamUIAppController_GetAppByID.tramp.fn(ctrl, appId, false);
    if (!app) return;
    CSteamApp::clearOwnership(app);                                // [app+0x18] = 0
    Hooks::CUpdateManager_MarkAppChange.tramp.fn(src, appId, CSteamApp::kEAppChange_AddedOrCreated); // refresh library UI
    g_pLog->debug("SteamUI: removed+marked app %u\n", appId);
}

// Stamps the configured PurchasedTime into the source CSteamApp BEFORE the original
// FillInAppOverview runs, relying on the original COPYING +0x28 into the overview
// instead of recomputing/overwriting it. This hook fires 0 times on the Deck
// (CEF), so this ordering assumption must be validated on DESKTOP classic UI.
void stampPurchaseTimeIfControlled(void** ppHolder)
{
    if (!g_config.packageInjection.get()) return;   // feature opt-in: inert when off
    if (!ppHolder) return;
    void* app = *ppHolder;                       // §7.8: arg3 is void**, deref once
    if (!app) return;
    uint32_t appId = CSteamApp::appId(app);      // [app+0x0c]
    if (!Ownership::isControlledApp(appId)) return;
    uint32_t t = Ownership::getPurchaseTime(appId);
    if (t) CSteamApp::setPurchaseTime(app, t);   // [app+0x28] = t
}

} // namespace SteamUI
