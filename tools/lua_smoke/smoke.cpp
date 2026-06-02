// Smoke test for LuaLoader T1.
// Self-contained: no SLSsteam internal headers, no libmem, no yaml-cpp.
// Inlines the same Lua VM setup logic as LuaLoader.cpp to verify that:
//   1. The Lua VM can be created.
//   2. The case-insensitive __index metamethod resolves mixed-case names.
//   3. All 14 stub bindings are reachable without "attempt to call a nil value".

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

// ── Minimal Lua VM matching LuaLoader.cpp logic ───────────────────────────

namespace SmokeLoader {

static lua_State* g_lua = nullptr;
static std::unordered_map<std::string, lua_CFunction> g_func_registry;

// Case-insensitive __index on _G: mirrors LuaLoader::case_insensitive_index.
static int case_insensitive_index(lua_State* L) {
    const char* name = lua_tostring(L, 2);
    if (!name) { lua_pushnil(L); return 1; }
    std::string lower;
    for (const char* p = name; *p; ++p)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    auto it = g_func_registry.find(lower);
    if (it != g_func_registry.end()) { lua_pushcfunction(L, it->second); return 1; }
    lua_pushnil(L);
    return 1;
}

// Register fn under lowercase_name in both _G and the registry.
static void register_func(lua_State* L, const char* name, lua_CFunction fn) {
    g_func_registry[name] = fn;
    lua_pushcfunction(L, fn);
    lua_setglobal(L, name);
}

// Generic stub: does nothing, returns 0 Lua values.
static int s_stub(lua_State*) { return 0; }

// Track whether any Lua runtime error occurred during script execution.
static bool g_had_error = false;

static void init(const std::string& scanDir) {
    g_lua = luaL_newstate();
    if (!g_lua) { fprintf(stderr, "[smoke] luaL_newstate failed\n"); return; }
    luaL_openlibs(g_lua);

    // Install case-insensitive __index on _G's metatable (mirrors LuaLoader::init).
    lua_getglobal(g_lua, "_G");
    if (!lua_getmetatable(g_lua, -1)) lua_newtable(g_lua);
    lua_pushcfunction(g_lua, case_insensitive_index);
    lua_setfield(g_lua, -2, "__index");
    lua_setmetatable(g_lua, -2);
    lua_pop(g_lua, 1); // Pop _G.

    // MANUAL MIRROR of the registration list in LuaLoader::init() — must be kept
    // in sync by hand whenever bindings are added or renamed there.
    // This smoke test inlines its own copy and will NOT catch drift automatically.
    register_func(g_lua, "addappid",                    s_stub);
    register_func(g_lua, "addtoken",                    s_stub);
    register_func(g_lua, "setmanifestid",               s_stub);
    register_func(g_lua, "fetch_manifest_code",         s_stub);
    register_func(g_lua, "fetch_manifest_code_ex",      s_stub);
    register_func(g_lua, "http_get",                    s_stub);
    register_func(g_lua, "http_post",                   s_stub);
    register_func(g_lua, "setappticket",                s_stub);
    register_func(g_lua, "seteticket",                  s_stub);
    register_func(g_lua, "setstat",                     s_stub);
    register_func(g_lua, "downloadapp",                 s_stub);
    register_func(g_lua, "addnonowneddepot",            s_stub);
    register_func(g_lua, "setnotifyondownloadcomplete", s_stub);
    register_func(g_lua, "setstpropertyforaccount",     s_stub);

    // Scan scanDir and execute every .lua file found.
    std::error_code ec;
    if (!std::filesystem::is_directory(scanDir, ec)) {
        fprintf(stderr, "[smoke] dir not found: %s\n", scanDir.c_str());
        g_had_error = true;
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(scanDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".lua") continue;

        const std::string path = entry.path().string();
        printf("[smoke] executing: %s\n", path.c_str());
        lua_settop(g_lua, 0);

        int rc = luaL_loadfile(g_lua, path.c_str());
        if (rc != LUA_OK) {
            fprintf(stderr, "[smoke] compile error in %s: %s\n",
                    path.c_str(), lua_tostring(g_lua, -1));
            lua_pop(g_lua, 1);
            g_had_error = true;
            continue;
        }
        if (lua_pcall(g_lua, 0, 0, 0) != LUA_OK) {
            fprintf(stderr, "[smoke] runtime error in %s: %s\n",
                    path.c_str(), lua_tostring(g_lua, -1));
            lua_pop(g_lua, 1);
            g_had_error = true;
        }
    }
}

} // namespace SmokeLoader

// ── Main ──────────────────────────────────────────────────────────────────

int main() {
    const char* tmp = std::getenv("TMPDIR");
    const std::string luaDir = std::string(tmp ? tmp : "/tmp") + "/sls_smoke_lua";

    printf("[smoke] LuaLoader T1 smoke test\n");
    printf("[smoke] script dir: %s\n", luaDir.c_str());

    // Write the test script that uses mixed-case binding names.
    std::filesystem::create_directories(luaDir);
    {
        std::ofstream f(luaDir + "/test.lua");
        if (!f) { fprintf(stderr, "[smoke] cannot write test.lua\n"); return 1; }

        // Each call uses a different capitalisation to exercise case-insensitive lookup.
        f << "-- test.lua: exercises all 14 stubs with mixed-case names\n";
        f << "AddAppId(12345)\n";                           // addappid
        f << "ADDTOKEN(12345, \"0\")\n";                   // addtoken
        f << "SetManifestId(12345, \"9999\")\n";           // setmanifestid
        f << "Fetch_Manifest_Code(1)\n";                    // fetch_manifest_code
        f << "fetch_manifest_code_ex(1, 2, 3)\n";          // exact lowercase
        f << "HTTP_GET(\"http://example.com\")\n";         // http_get
        f << "Http_Post(\"http://x\", \"body\")\n";        // http_post
        f << "SetAppTicket(1, \"deadbeef\")\n";            // setappticket
        f << "SETETICKET(1, \"deadbeef\")\n";              // seteticket
        f << "SetStat(1, \"76561198000000000\")\n";        // setstat
        f << "DownloadApp(1)\n";                           // downloadapp
        f << "AddNonOwnedDepot(1)\n";                      // addnonowneddepot
        f << "SetNotifyOnDownloadComplete(true)\n";        // setnotifyondownloadcomplete
        f << "SetStPropertyForAccount(1, \"k\", \"v\")\n"; // setstpropertyforaccount
        f << "print(\"[smoke] all bindings resolved OK\")\n";
    }

    SmokeLoader::init(luaDir);

    // Cleanup temp files.
    std::error_code ec;
    std::filesystem::remove_all(luaDir, ec);

    if (!SmokeLoader::g_had_error) {
        printf("[smoke] PASS\n");
        return 0;
    }
    fprintf(stderr, "[smoke] FAIL\n");
    return 1;
}
