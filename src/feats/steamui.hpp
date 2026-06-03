#pragma once
#include <cstdint>
namespace SteamUI {
    void setController(void* p);       // GetAppByID arg0
    void setAppChangeSource(void* p);  // MarkAppChange arg0
    // Clear the steamui app object's ownership flag + nudge the library UI so a
    // removed (un-injected) app disappears live. No-op if captures not ready or
    // the app is genuinely subscribed. Call AFTER pkg removal + Mark/Process.
    void removeAppAndSendChange(uint32_t appId);
}
