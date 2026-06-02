#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// LuaLoader: embedded Lua 5.4 scripting layer for SLSsteam.
// T1 (skeleton): state tables declared; all bindings are empty stubs.
// Real implementations come in later tasks.
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

    // State tables — populated by real binding implementations in later tasks.
    extern std::unordered_map<uint32_t, std::vector<uint8_t>> depotKeys;
    extern std::unordered_map<uint32_t, ManifestOverride>      manifestOverrides;
    extern std::unordered_set<uint32_t>                        ownedAppIds;
    extern std::unordered_map<uint32_t, uint64_t>              appTokens;
    extern std::unordered_map<uint32_t, uint64_t>              statSteamIds;
    extern std::unordered_map<uint32_t, LuaTicket>             appTickets;
    extern std::unordered_map<uint32_t, LuaTicket>             encTickets;

    // Initialize the Lua VM, register all stub bindings, and execute
    // every .lua file found in the three scan directories.
    // Call once after the Steam root is known (i.e. after steamclient.so loads).
    void init();

} // namespace LuaLoader
