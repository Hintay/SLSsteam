#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// LuaLoader: embedded Lua 5.4 scripting layer for SLSsteam.
// T1 (skeleton): state tables declared; all bindings are empty stubs.
// T2: real implementations for download-related bindings + query APIs.
namespace LuaLoader {

    // Manifest GID + content size override for a depot.
    struct ManifestOverride {
        uint64_t gid;
        uint64_t size;
    };

    // App/eticket blob associated with a Steam ID.
    struct LuaTicket {
        uint32_t steamId;
        std::vector<uint8_t> bytes;
    };

    // State tables — populated by real binding implementations.
    // depotKeys mirrors OST DepotKeySet membership semantics: every addappid(id)
    // creates an entry; an empty vector means the id is configured but no depot
    // decryption key was supplied.
    extern std::unordered_map<uint32_t, std::vector<uint8_t>> depotKeys;
    extern std::unordered_map<uint32_t, ManifestOverride>      manifestOverrides;
    extern std::unordered_set<uint32_t>                        ownedAppIds;
    extern std::unordered_map<uint32_t, uint64_t>              appTokens;
    extern std::unordered_map<uint32_t, uint64_t>              statSteamIds;
    extern std::unordered_map<uint32_t, LuaTicket>             appTickets;
    extern std::unordered_map<uint32_t, LuaTicket>             encTickets;

    // Initialize the Lua VM, register all bindings, and execute
    // every .lua file found in the three scan directories.
    // After scanning, merges ownedAppIds and appTokens into g_config.
    // Call once after the Steam root is known (i.e. after steamclient.so loads).
    // Note: the internal lua_State is intentionally process-lifetime — created once
    // here, never lua_close()'d; it lives until the injected process exits.
    void init();

    // True once init() has finished building all lua tables. The FileWatcher
    // hot-reload path (CConfig::loadSettings) checks this before calling
    // reconcileIntoConfig() so it never races init()'s table construction.
    bool initDone();

    // Union the lua-provided ownedAppIds/appTokens into g_config. Called by init()
    // and again at the end of CConfig::loadSettings() so a yaml hot-reload (which
    // overwrites those fields wholesale) does not drop lua-defined apps. Safe to
    // call before init() — operates on the (then empty) tables.
    void reconcileIntoConfig();

    void onDepotsChanged();
    void setOnDepotsChanged(void (*fn)());

    void unloadFile(const std::string& path);
    std::vector<uint32_t> takePendingAdditions();
    std::vector<uint32_t> takePendingRemovals();

    // True when a live Lua contribution registered this app via addappid().
    // Used to let Lua take precedence over yaml AdditionalApps for download policy.
    bool hasOwnedAppId(uint32_t appId);

    // ── Query APIs (T2) ───────────────────────────────────────────────────────

    // Returns the 32-byte depot decryption key for depotId, or an empty vector
    // if no key was supplied. Note: depotKeys may still contain an empty marker
    // for ids registered via addappid(id) without a key.
    std::vector<uint8_t> getKey(uint32_t depotId);

    // Returns a pointer to the manifest GID+size override for depotId,
    // or nullptr if setmanifestid was not called for that depot.
    std::optional<ManifestOverride> getManifest(uint32_t depotId);

    // Returns the Steam ID associated with appId via setstat (T8).
    // Falls back to 76561198028121353 when not set.
    uint64_t getStatSteamId(uint32_t appId);

    // Returns a pointer to the in-memory app ownership ticket for appId
    // registered via setappticket(), or nullptr if not present.
    std::optional<LuaTicket> getAppTicket(uint32_t appId);

    // Returns a pointer to the in-memory encrypted ticket for appId
    // registered via seteticket(), or nullptr if not present.
    std::optional<LuaTicket> getEncTicket(uint32_t appId);

    // Fetch the manifest request code for (appId, depotId, gid).
    // Selection order (mirrors OST FetchManifestRequestCode):
    //   1. g_fetchCodeExRef != LUA_NOREF  → call fetch_manifest_code_ex(appId, depotId, gid)
    //   2. g_fetchCodeRef   != LUA_NOREF  → call fetch_manifest_code(gid)
    //   3. fallback                        → ManifestProvider::fetchFromProvider(gid, outCode)
    // Returns true and writes outCode on success; outCode is validated non-zero.
    bool fetchManifestCode(uint32_t appId, uint32_t depotId, uint64_t gid, uint64_t& outCode);

} // namespace LuaLoader
