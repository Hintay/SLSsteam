#pragma once

#include <cstdint>
#include <cstddef>

// Accessors for steamui.so CSteamUIAppController's app object (returned by
// GetAppByID). Offsets proven via Deck frida 2026-06-03 (NOT OST's Windows
// packed layout). Re-derive on a new build.
namespace CSteamApp {
    static constexpr size_t kAppIdOff     = 0x0c; // AppId_t
    static constexpr size_t kOwnershipOff = 0x18; // EAppOwnershipFlags (OwnsLicense = 0x1)
    static constexpr size_t kPurchaseOff  = 0x28; // PurchasedTime (Unix seconds)

    inline uint32_t appId(void* app) {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(app) + kAppIdOff);
    }
    inline void clearOwnership(void* app) {
        *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(app) + kOwnershipOff) = 0;
    }
    inline void setPurchaseTime(void* app, uint32_t t) {
        *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(app) + kPurchaseOff) = t;
    }

    // EAppChangeFlags::AddedOrCreated — the change flag Steam itself uses on app add
    // (proven 2026-06-03). Used by SteamUI::removeAppAndSendChange.
    static constexpr uint32_t kEAppChange_AddedOrCreated = 0x0001;
}
