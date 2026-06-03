#include "steamui.hpp"

#include "../hooks.hpp"
#include "../config.hpp"
#include "../lua/LuaLoader.hpp"
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

// CONCURRENCY: called from notifyLicenseChanged with g_injectMtx HELD. The Steam
// calls below all reach the REAL originals: isSubscribed goes through CUser's
// trampoline; GetAppByID/MarkAppChange go through THEIR detour trampolines
// (.tramp.fn) — i.e. they bypass our own capture hooks. None of these take a
// mutex or re-enter Package code, so there is no same-thread re-lock of the
// non-recursive g_injectMtx. (Steam's own threads reacting to MarkAppChange may
// contend for g_injectMtx cross-thread — a brief wait, never a deadlock.)
void removeAppAndSendChange(uint32_t appId)
{
    void* ctrl = g_controller.load(std::memory_order_acquire);
    void* src  = g_appChangeSource.load(std::memory_order_acquire);
    if (!ctrl || !src) return;
    // Don't clear ownership of a genuinely-subscribed app (mirror OST IsOwned guard).
    auto* u = g_pSteamEngine ? g_pSteamEngine->getUser(0) : nullptr;
    if (u && u->isSubscribed(appId)) return;
    void* app = Hooks::CSteamUIAppController_GetAppByID.tramp.fn(ctrl, appId, false);
    if (!app) return;
    CSteamApp::clearOwnership(app);                                // [app+0x18] = 0
    Hooks::CUpdateManager_MarkAppChange.tramp.fn(src, appId, CSteamApp::kEAppChange_AddedOrCreated); // refresh library UI
    g_pLog->debug("SteamUI: removed+marked app %u\n", appId);
}

// Stamps the lua PurchasedTime into the source CSteamApp BEFORE the original
// FillInAppOverview runs, relying on the original COPYING +0x28 into the overview
// (not recomputing/overwriting it) — same as OST. This hook fires 0 times on the
// Deck (CEF), so this ordering assumption must be validated on DESKTOP classic UI.
void stampPurchaseTimeIfControlled(void** ppHolder)
{
    if (!g_config.packageInjection.get()) return;   // feature opt-in: inert when off
    if (!ppHolder) return;
    void* app = *ppHolder;                       // §7.8: arg3 is void**, deref once
    if (!app) return;
    uint32_t appId = CSteamApp::appId(app);      // [app+0x0c]
    if (!g_config.isAddedAppId(appId)) return;   // only controlled apps
    uint32_t t = LuaLoader::getPurchaseTime(appId);
    if (t) CSteamApp::setPurchaseTime(app, t);   // [app+0x28] = t
}

} // namespace SteamUI
