#include "LuaLoader.hpp"

#include "../config.hpp"
#include "../curl.hpp"
#include "../globals.hpp"
#include "../log.hpp"

// Use the raw Lua C API — same approach as OST's LuaConfig.cpp.
// Do NOT use sol2 or any wrapper.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace LuaLoader {

    // ── Module state tables (definitions) ─────────────────────────────────
    std::unordered_map<uint32_t, std::vector<uint8_t>> depotKeys;
    std::unordered_map<uint32_t, ManifestOverride>      manifestOverrides;
    std::unordered_set<uint32_t>                        ownedAppIds;
    std::unordered_map<uint32_t, uint64_t>              appTokens;
    std::unordered_map<uint32_t, uint64_t>              statSteamIds;
    std::unordered_map<uint32_t, LuaTicket>             appTickets;
    std::unordered_map<uint32_t, LuaTicket>             encTickets;

    // ── Internal state ─────────────────────────────────────────────────────
    static lua_State* g_lua = nullptr;

    // Case-insensitive function registry: lowercase name → C function.
    // Mirrors OST LuaConfig.cpp:g_func_registry.
    static std::unordered_map<std::string, lua_CFunction> g_func_registry;

    // Lua registry references for callback functions captured via luaL_ref.
    // Initialised to LUA_NOREF (-1) so callers can test before invoking.
    static int g_fetchCodeRef   = LUA_NOREF;
    static int g_fetchCodeExRef = LUA_NOREF;

    // ── Case-insensitive global __index metamethod ─────────────────────────
    // When a Lua script references an unknown global (e.g. AddAppId),
    // we lower-case the name and look it up in g_func_registry so that
    // any capitalisation variant resolves to the registered function.
    // Mirrors OST LuaConfig.cpp lines ~117-134.
    static int case_insensitive_index(lua_State* L) {
        const char* name = lua_tostring(L, 2);
        if (!name) {
            lua_pushnil(L);
            return 1;
        }
        std::string lower;
        lower.reserve(std::strlen(name));
        for (const char* p = name; *p; ++p) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        }
        auto it = g_func_registry.find(lower);
        if (it != g_func_registry.end()) {
            lua_pushcfunction(L, it->second);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    // Register fn in _G under lowercase_name AND in g_func_registry so any
    // case variant also resolves via the __index metamethod.
    static void register_func(lua_State* L,
                              const char* lowercase_name,
                              lua_CFunction fn)
    {
        g_func_registry[lowercase_name] = fn;
        lua_pushcfunction(L, fn);
        lua_setglobal(L, lowercase_name);
    }

    // ── Hex helpers ────────────────────────────────────────────────────────

    // Convert a single ASCII hex character to its nibble value.
    // Returns -1 on invalid input.
    static int hexNibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // Parse a 64-character hex string into a 32-byte vector.
    // Returns false (and leaves out unchanged) on malformed input.
    static bool parseHex64(const char* hex, std::vector<uint8_t>& out) {
        if (std::strlen(hex) != 64) return false;
        std::vector<uint8_t> result;
        result.reserve(32);
        for (int i = 0; i < 64; i += 2) {
            int hi = hexNibble(hex[i]);
            int lo = hexNibble(hex[i + 1]);
            if (hi < 0 || lo < 0) return false;
            result.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        out = std::move(result);
        return true;
    }

    // Parse a decimal uint64 string (as used by addtoken / setmanifestid).
    // Returns false on empty string, overflow, or non-digit characters.
    static bool parseU64Decimal(const char* str, uint64_t& out) {
        if (!str || *str == '\0') return false;
        errno = 0;
        char* end = nullptr;
        unsigned long long v = strtoull(str, &end, 10);
        if (errno != 0 || end == str || *end != '\0') return false;
        out = static_cast<uint64_t>(v);
        return true;
    }

    // ── Real binding implementations ───────────────────────────────────────

    // addappid(id [, dlc_flag [, hex_key]])
    // Mirrors OST LuaConfig.cpp::lua_addappid, adapted for SLSsteam tables.
    //
    // Arg 1 (required): integer app/depot ID
    // Arg 2 (optional): integer DLC flag (accepted but not stored — kept for
    //                   script compatibility; the flag is Steam-internal info)
    // Arg 3 (optional): 64-hex-char depot decryption key string
    //
    // On success: inserts id into ownedAppIds; if key present and valid,
    // inserts 32-byte parsed key into depotKeys[id].
    static int impl_addappid(lua_State* L) {
        int argc = lua_gettop(L);
        if (argc == 0 || !lua_isinteger(L, 1))
            return luaL_error(L, "addappid: arg1 must be integer app/depot id");

        lua_Integer raw = lua_tointeger(L, 1);
        if (raw < 0 || raw > static_cast<lua_Integer>(UINT32_MAX))
            return luaL_error(L, "addappid: id out of uint32 range");

        uint32_t id = static_cast<uint32_t>(raw);
        ownedAppIds.insert(id);

        // Arg 3: optional 64-hex-char depot key (arg 2 is the DLC flag).
        if (argc >= 3) {
            if (!lua_isstring(L, 3)) {
                g_pLog->warn("LuaLoader: addappid(%u): arg3 must be hex string, ignoring key\n", id);
            } else {
                const char* hexStr = lua_tostring(L, 3);
                std::vector<uint8_t> keyBytes;
                if (parseHex64(hexStr, keyBytes)) {
                    depotKeys[id] = std::move(keyBytes);
                    g_pLog->debug("LuaLoader: addappid(%u): depot key stored\n", id);
                } else {
                    g_pLog->warn("LuaLoader: addappid(%u): invalid key (need 64 hex chars), ignoring\n", id);
                }
            }
        }

        g_pLog->debug("LuaLoader: addappid(%u)\n", id);
        return 0;
    }

    // addtoken(appid, "decimal_u64_str")
    // Mirrors OST LuaConfig.cpp::lua_addtoken.
    static int impl_addtoken(lua_State* L) {
        int argc = lua_gettop(L);
        if (argc < 1 || !lua_isinteger(L, 1))
            return luaL_error(L, "addtoken: arg1 must be integer appid");

        lua_Integer raw = lua_tointeger(L, 1);
        if (raw < 0 || raw > static_cast<lua_Integer>(UINT32_MAX))
            return luaL_error(L, "addtoken: appid out of uint32 range");
        uint32_t appId = static_cast<uint32_t>(raw);

        if (argc < 2)
            return 0; // token arg omitted — accept silently like OST does

        if (!lua_isstring(L, 2))
            return luaL_error(L, "addtoken: arg2 must be decimal uint64 string");

        const char* tokenStr = lua_tostring(L, 2);
        uint64_t token = 0;
        if (!parseU64Decimal(tokenStr, token))
            return luaL_error(L, "addtoken: arg2 is not a valid uint64 decimal string");

        appTokens[appId] = token;
        g_pLog->debug("LuaLoader: addtoken(%u, ...)\n", appId);
        return 0;
    }

    // setmanifestid(depotId, "gid_str" [, size])
    // Mirrors OST LuaConfig.cpp::lua_setManifestid.
    // Size omitted → stored as 0 (keep original size later).
    static int impl_setmanifestid(lua_State* L) {
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setmanifestid: need depotId, gid_str [, size]");

        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setmanifestid: arg1 (depotId) must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setmanifestid: arg2 (gid) must be decimal string");

        lua_Integer raw = lua_tointeger(L, 1);
        if (raw < 0 || raw > static_cast<lua_Integer>(UINT32_MAX))
            return luaL_error(L, "setmanifestid: depotId out of uint32 range");
        uint32_t depotId = static_cast<uint32_t>(raw);

        const char* gidStr = lua_tostring(L, 2);
        uint64_t gid = 0;
        if (!parseU64Decimal(gidStr, gid))
            return luaL_error(L, "setmanifestid: gid must be all decimal digits");

        uint64_t size = 0;
        if (argc >= 3) {
            if (lua_isstring(L, 3)) {
                // Tolerate size passed as a numeric string.
                const char* sizeStr = lua_tostring(L, 3);
                if (!parseU64Decimal(sizeStr, size)) {
                    g_pLog->warn("LuaLoader: setmanifestid(%u): invalid size string, using 0\n", depotId);
                    size = 0;
                }
            } else if (lua_isinteger(L, 3)) {
                lua_Integer rawSize = lua_tointeger(L, 3);
                size = (rawSize >= 0) ? static_cast<uint64_t>(rawSize) : 0;
            } else if (lua_isnumber(L, 3)) {
                double rawSize = lua_tonumber(L, 3);
                size = (rawSize >= 0) ? static_cast<uint64_t>(rawSize) : 0;
            }
        }

        manifestOverrides[depotId] = ManifestOverride{ gid, size };
        g_pLog->debug("LuaLoader: setmanifestid(%u, gid=%llu, size=%llu)\n",
                      depotId, static_cast<unsigned long long>(gid),
                      static_cast<unsigned long long>(size));
        return 0;
    }

    // fetch_manifest_code(fn)
    // Captures a Lua function reference in the registry for later invocation (T6).
    static int impl_fetch_manifest_code(lua_State* L) {
        if (lua_gettop(L) < 1 || !lua_isfunction(L, 1))
            return luaL_error(L, "fetch_manifest_code: arg1 must be a function");

        // Release any previously stored reference first.
        if (g_fetchCodeRef != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, g_fetchCodeRef);
        }

        // Push the function to the top and anchor it in the registry.
        lua_pushvalue(L, 1);
        g_fetchCodeRef = luaL_ref(L, LUA_REGISTRYINDEX);
        g_pLog->debug("LuaLoader: fetch_manifest_code captured (ref=%d)\n", g_fetchCodeRef);
        return 0;
    }

    // fetch_manifest_code_ex(fn)
    // Same as fetch_manifest_code but for the extended variant (T6).
    static int impl_fetch_manifest_code_ex(lua_State* L) {
        if (lua_gettop(L) < 1 || !lua_isfunction(L, 1))
            return luaL_error(L, "fetch_manifest_code_ex: arg1 must be a function");

        if (g_fetchCodeExRef != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, g_fetchCodeExRef);
        }

        lua_pushvalue(L, 1);
        g_fetchCodeExRef = luaL_ref(L, LUA_REGISTRYINDEX);
        g_pLog->debug("LuaLoader: fetch_manifest_code_ex captured (ref=%d)\n", g_fetchCodeExRef);
        return 0;
    }

    // http_get(url) → (body: string, status: integer)
    // Wraps Curl::getString which performs a GET and returns a CURLcode.
    // On success: pushes (body_string, 0). On failure: pushes (nil, error_code).
    static int impl_http_get(lua_State* L) {
        if (lua_gettop(L) < 1 || !lua_isstring(L, 1))
            return luaL_error(L, "http_get: arg1 must be URL string");

        const char* url = lua_tostring(L, 1);
        std::string body;
        int rc = Curl::getString(url, body);
        if (rc == 0 /* CURLE_OK */) {
            lua_pushlstring(L, body.data(), body.size());
            lua_pushinteger(L, 0);
        } else {
            lua_pushnil(L);
            lua_pushinteger(L, rc);
        }
        return 2;
    }

    // http_post(url, body [, headers_table]) → (response: string|nil, status: integer)
    // TODO (T6): full POST support and custom headers require extending Curl::getString.
    // For now returns nil + a descriptive message so scripts can detect the limitation.
    static int impl_http_post(lua_State* L) {
        // TODO (T6): implement POST with body and optional headers table once
        // Curl is extended with curl_easy_setopt(CURLOPT_POST/CURLOPT_POSTFIELDS).
        lua_pushnil(L);
        lua_pushstring(L, "http_post: not yet implemented (T6)");
        return 2;
    }

    // ── Stub binding implementations (T7/T8/T9 — do not implement here) ───
    static int stub_setappticket(lua_State*)           { return 0; }
    static int stub_seteticket(lua_State*)             { return 0; }
    static int stub_setstat(lua_State*)                { return 0; }
    static int stub_downloadapp(lua_State*)            { return 0; }
    static int stub_addnonowneddepot(lua_State*)       { return 0; }
    static int stub_setnotifyondownloadcomplete(lua_State*) { return 0; }
    static int stub_setstpropertyforaccount(lua_State*)     { return 0; }

    // ── Merge lua tables into g_config ────────────────────────────────────
    // Called at the end of init() after all Lua files have been executed.
    //
    // ownedAppIds  → g_config.addedAppIds  (plain union; yaml entries kept)
    // appTokens    → g_config.appTokens    (yaml wins on conflict; lua fills gaps)
    //
    // depotKeys and manifestOverrides are lua-only (no yaml counterpart).
    static void mergeIntoConfig() {
        // Merge ownedAppIds (plain union — add every lua-registered id).
        {
            auto existing = g_config.addedAppIds.get();
            for (uint32_t id : ownedAppIds) {
                existing.insert(id);
            }
            g_config.addedAppIds.set(existing);
            g_pLog->info("LuaLoader: merged %zu ownedAppIds into g_config.addedAppIds\n",
                         ownedAppIds.size());
        }

        // Merge appTokens: yaml wins on conflict, lua fills gaps.
        {
            auto existing = g_config.appTokens.get();
            size_t added = 0;
            for (const auto& [appId, token] : appTokens) {
                if (existing.find(appId) == existing.end()) {
                    existing[appId] = token;
                    ++added;
                }
            }
            g_config.appTokens.set(existing);
            g_pLog->info("LuaLoader: merged %zu new appTokens into g_config.appTokens\n", added);
        }
    }

    // ── Derive the Steam root directory ───────────────────────────────────
    // The authoritative source is g_modSteamClient.path (set when steamclient.so
    // loads). steamclient.so lives at <steam_root>/ubuntu12_32/steamclient.so,
    // so we go two levels up.
    // If the module path is not yet populated we fall back to ~/.local/share/Steam.
    // TODO: wire the real path once g_modSteamClient is guaranteed to be set
    //       before LuaLoader::init() is called (T9).
    static std::string getSteamRoot() {
        // g_modSteamClient.path is a char[] set by libmem when steamclient.so loads.
        if (g_modSteamClient.path[0] != '\0') {
            std::filesystem::path p(g_modSteamClient.path);
            // steamclient.so → ubuntu12_32/ → <steam_root>/
            auto root = p.parent_path().parent_path();
            if (!root.empty() && std::filesystem::is_directory(root)) {
                return root.string();
            }
        }
        // Fallback: standard Steam install location.
        // TODO: replace with a cleaner source of truth when available (T9).
        const char* home = std::getenv("HOME");
        if (home) {
            return std::string(home) + "/.local/share/Steam";
        }
        return "";
    }

    // ── Directory scanner ─────────────────────────────────────────────────
    // Runs every .lua file in dir through the Lua VM.
    // Silently skips missing directories (they may not exist yet).
    static void scanDirectory(const std::string& dir) {
        if (dir.empty()) return;

        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            g_pLog->debug("LuaLoader: skipping non-existent dir: %s\n", dir.c_str());
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".lua") continue;

            const std::string filePath = entry.path().string();
            g_pLog->info("LuaLoader: loading %s\n", filePath.c_str());

            lua_settop(g_lua, 0);
            int rc = luaL_loadfile(g_lua, filePath.c_str());
            if (rc != LUA_OK) {
                g_pLog->warn("LuaLoader: compile error in %s: %s\n",
                             filePath.c_str(), lua_tostring(g_lua, -1));
                lua_pop(g_lua, 1);
                continue;
            }
            if (lua_pcall(g_lua, 0, 0, 0) != LUA_OK) {
                g_pLog->warn("LuaLoader: runtime error in %s: %s\n",
                             filePath.c_str(), lua_tostring(g_lua, -1));
                lua_pop(g_lua, 1);
            }
        }
    }

    // ── Public entry point ────────────────────────────────────────────────
    void init() {
        if (g_lua) return; // Already initialised.

        // Create Lua VM and open all standard libraries.
        g_lua = luaL_newstate();
        if (!g_lua) {
            g_pLog->warn("LuaLoader: failed to create lua_State\n");
            return;
        }
        luaL_openlibs(g_lua);

        // Install case-insensitive __index on _G's metatable.
        // Mirrors OST LuaConfig.cpp:Initialize() lines ~386-393.
        lua_getglobal(g_lua, "_G");
        if (!lua_getmetatable(g_lua, -1)) {
            lua_newtable(g_lua); // Push a fresh metatable.
        }
        lua_pushcfunction(g_lua, case_insensitive_index);
        lua_setfield(g_lua, -2, "__index");
        lua_setmetatable(g_lua, -2);
        lua_pop(g_lua, 1); // Pop _G.

        // Register all bindings. T2 functions are real; others remain stubs
        // until their respective tasks (T6 for http_post, T7/T8 for tickets/stat).
        register_func(g_lua, "addappid",                    impl_addappid);
        register_func(g_lua, "addtoken",                    impl_addtoken);
        register_func(g_lua, "setmanifestid",               impl_setmanifestid);
        register_func(g_lua, "fetch_manifest_code",         impl_fetch_manifest_code);
        register_func(g_lua, "fetch_manifest_code_ex",      impl_fetch_manifest_code_ex);
        register_func(g_lua, "http_get",                    impl_http_get);
        register_func(g_lua, "http_post",                   impl_http_post);
        register_func(g_lua, "setappticket",                stub_setappticket);
        register_func(g_lua, "seteticket",                  stub_seteticket);
        register_func(g_lua, "setstat",                     stub_setstat);
        register_func(g_lua, "downloadapp",                 stub_downloadapp);
        register_func(g_lua, "addnonowneddepot",            stub_addnonowneddepot);
        register_func(g_lua, "setnotifyondownloadcomplete", stub_setnotifyondownloadcomplete);
        register_func(g_lua, "setstpropertyforaccount",     stub_setstpropertyforaccount);

        g_pLog->info("LuaLoader: VM ready, scanning lua dirs\n");

        // ── Scan directories in priority order ────────────────────────────
        // Later directories override earlier ones (for the same depot) because
        // the binding implementations use map assignment (last write wins).

        // 1. System-wide plugin dir: {steam_root}/config/stplug-in/*.lua
        const std::string steamRoot = getSteamRoot();
        if (!steamRoot.empty()) {
            scanDirectory(steamRoot + "/config/stplug-in");
        }

        // 2. User config dir: <config>/SLSsteam/lua/*.lua
        //    Reuse g_config.getDir() which already handles XDG_CONFIG_HOME → ~/.config/SLSsteam.
        {
            std::string userLuaDir = g_config.getDir() + "/lua";
            scanDirectory(userLuaDir);
        }

        // 3. Extra directories from yaml lua.paths[] — stubbed empty for T1;
        //    full YAML wiring is a later task.
        // TODO (T_yaml): read g_config yaml node "lua.paths" and scan each.

        // ── Union lua tables into g_config ────────────────────────────────
        mergeIntoConfig();

        g_pLog->info("LuaLoader: init complete\n");
    }

    // ── Query API implementations ─────────────────────────────────────────

    const std::vector<uint8_t>* getKey(uint32_t depotId) {
        auto it = depotKeys.find(depotId);
        if (it == depotKeys.end()) return nullptr;
        return &it->second;
    }

    const ManifestOverride* getManifest(uint32_t depotId) {
        auto it = manifestOverrides.find(depotId);
        if (it == manifestOverrides.end()) return nullptr;
        return &it->second;
    }

    // Default Steam ID used when setstat has not been called for an app.
    // Matches the value specified in the T2 task description.
    static constexpr uint64_t kDefaultStatSteamId = 76561198028121353ULL;

    uint64_t getStatSteamId(uint32_t appId) {
        auto it = statSteamIds.find(appId);
        if (it == statSteamIds.end()) return kDefaultStatSteamId;
        return it->second;
    }

} // namespace LuaLoader
