#pragma once

#include <cstdint>
#include <string>
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

    // ── Query APIs (T2) ───────────────────────────────────────────────────────

    // Returns a pointer to the 32-byte depot decryption key for depotId,
    // or nullptr if no key was registered via addappid(..., "hexkey").
    const std::vector<uint8_t>* getKey(uint32_t depotId);

    // Returns a pointer to the manifest GID+size override for depotId,
    // or nullptr if setmanifestid was not called for that depot.
    const ManifestOverride* getManifest(uint32_t depotId);

    // Returns the Steam ID associated with appId via setstat (T8).
    // Falls back to 76561198028121353 when not set.
    uint64_t getStatSteamId(uint32_t appId);

} // namespace LuaLoader
