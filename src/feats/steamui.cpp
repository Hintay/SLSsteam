#include "steamui.hpp"

#include "../hooks.hpp"
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
// calls below (isSubscribed via CUser's trampoline, oGetAppByID/oMarkAppChange via
// resolved original pointers) all BYPASS SLSsteam's own hooks, so they never
// re-enter notifyLicenseChanged/tryInitFakeLicenseOnce on this thread and never
// try to re-lock the non-recursive g_injectMtx. Keep it that way: if any of these
// is ever routed through a *hooked* path, this becomes a same-thread deadlock.
// (Steam's own threads reacting to MarkAppChange contend for g_injectMtx cross-
// thread — a brief wait, not a deadlock.)
void removeAppAndSendChange(uint32_t appId)
{
    void* ctrl = g_controller.load(std::memory_order_acquire);
    void* src  = g_appChangeSource.load(std::memory_order_acquire);
    if (!ctrl || !src || !Hooks::oGetAppByID || !Hooks::oMarkAppChange) return;
    // Don't clear ownership of a genuinely-subscribed app (mirror OST IsOwned guard).
    auto* u = g_pSteamEngine ? g_pSteamEngine->getUser(0) : nullptr;
    if (u && u->isSubscribed(appId)) return;
    void* app = Hooks::oGetAppByID(ctrl, appId, false);
    if (!app) return;
    CSteamApp::clearOwnership(app);                                // [app+0x18] = 0
    Hooks::oMarkAppChange(src, appId, kEAppChange_AddedOrCreated); // refresh library UI
    g_pLog->debug("SteamUI: removed+marked app %u\n", appId);
}

} // namespace SteamUI
