#include "LuaLoader.hpp"

#include "../config.hpp"
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
#include <cstdlib>
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

    // ── Stub binding implementations ───────────────────────────────────────
    // Each returns 0 (no Lua values pushed) and does nothing.
    // Real implementations come in later tasks (T2/T6/T7/T8).

    static int stub_addappid(lua_State*)               { return 0; }
    static int stub_addtoken(lua_State*)               { return 0; }
    static int stub_setmanifestid(lua_State*)          { return 0; }
    static int stub_fetch_manifest_code(lua_State*)    { return 0; }
    static int stub_fetch_manifest_code_ex(lua_State*) { return 0; }
    static int stub_http_get(lua_State*)               { return 0; }
    static int stub_http_post(lua_State*)              { return 0; }
    static int stub_setappticket(lua_State*)           { return 0; }
    static int stub_seteticket(lua_State*)             { return 0; }
    static int stub_setstat(lua_State*)                { return 0; }
    static int stub_downloadapp(lua_State*)            { return 0; }
    static int stub_addnonowneddepot(lua_State*)       { return 0; }
    static int stub_setnotifyondownloadcomplete(lua_State*) { return 0; }
    static int stub_setstpropertyforaccount(lua_State*)     { return 0; }

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

        // Register all binding stubs up front so scripts never see nil.
        register_func(g_lua, "addappid",                    stub_addappid);
        register_func(g_lua, "addtoken",                    stub_addtoken);
        register_func(g_lua, "setmanifestid",               stub_setmanifestid);
        register_func(g_lua, "fetch_manifest_code",         stub_fetch_manifest_code);
        register_func(g_lua, "fetch_manifest_code_ex",      stub_fetch_manifest_code_ex);
        register_func(g_lua, "http_get",                    stub_http_get);
        register_func(g_lua, "http_post",                   stub_http_post);
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
        // the real binding implementations will respect last-write semantics.

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

        g_pLog->info("LuaLoader: init complete\n");
    }

} // namespace LuaLoader
