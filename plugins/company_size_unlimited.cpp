// Company Size Unlimited -- more company vehicles, as plugins\mt_plugin_CompanySizeUnlimited.dll.
//
// Two separate caps:
//
// Owned: a company buys vehicle slots (ServerAddCompanyVehicleSlot), counted in
// MTCompany.AddedVehicleSlots. The add refuses at 15 -- `cmp ecx, 0xf; jge` right after
// reading +0x78 -- and the company screen has its own copy of the test that shows the
// "max" message instead of the price. Both constants are rewritten to MaxBoughtSlots.
// The price keeps rising per slot exactly as vanilla's formula does.
//
// Usable: slots = 5 + min(bought, 15), inlined at five more sites: the two server checks
// that add a vehicle, the screen's check, the "n / max" counter, and the save load, which
// stops reading vehicles at that number. Their 15 is rewritten too, so the counter shows
// 5 + what was really bought.
//
// Ids: each vehicle takes the first free id from 1 to 20, and the save load skips any id
// outside that. Both are raised to the same total, or vehicle 21 is refused and lost on
// reload. The list itself is a ListView, so the screen needs no change.
//
// Spawned: MotorTownGameState.Net_ServerConfig (MTServerRuntimeConfig) holds the spawn
// caps the game enforces. In single player / as host there is no dedicated-server json,
// so they are raised here to at least the ini values. A dedicated server sets its own
// in DedicatedServerConfig.json (this plugin on the server raises them the same way).
//
// Settings: Binaries\Win64\company_size_unlimited.ini, [CompanySlots].
//
// shortcut: the owned cap is checked by whoever runs the company -- single player, the
// host, or the dedicated server. A client of a vanilla server keeps vanilla's 15.

#include <mutex>
#include <vector>
#include "../include/mt_plugin.hpp"

using namespace mt;

struct Patch { const char* what; const char* sig; int at; bool required; int plus; };  // writes MaxBoughtSlots + plus
static const Patch PATCHES[] = {
    // mov ecx,[rax+78h]; mov qword [rax+?],0; cmp ecx,0Fh; jge  -- the add itself
    {"buy slot (server)", "8B 48 78 48 C7 80 ?? ?? 00 00 00 00 00 00 83 F9 0F 7D", 16, true},
    // test rax,rax; jz; cmp dword [rax+78h],0Fh; jl; lea rdx,[msg]  -- the company screen
    {"buy slot (screen)", "48 85 C0 0F 84 ?? ?? ?? ?? 83 78 78 0F 7C ?? 48 8D 15", 12, false},
    // mov r,0Fh; cmp [r+78h],r; cmovl r,[r+78h]  -- the 5 + min(bought, 15) sites
    {"add vehicle (server)", "B9 0F 00 00 00 39 48 78 0F 4C 48 78", 1, true},
    {"add vehicle (server 2)", "BA 0F 00 00 00 39 50 78 0F 4C 50 78", 1, true},
    {"add vehicle (screen)", "41 B8 0F 00 00 00 41 8B D4 44 39 40 78", 2, false},
    {"counter", "B9 0F 00 00 00 48 89 5C 24 30 39 4F 78", 1, false},
    {"save load", "B9 0F 00 00 00 48 89 9C 24 ?? ?? 00 00 48 89 B4 24 ?? ?? 00 00 33 F6 41 39 4D 78", 1, true},
    // inc rsi; cmp rsi,14h; jg  -- the first free id, 1 to 20
    {"vehicle id", "48 FF C6 48 83 FE 14 7F ?? 48 8B C2", 6, true, 5},
    // lea rax,[r8-1]; cmp rax,13h; ja  -- the save load drops an id outside 1 to 20
    {"vehicle id (save load)", "49 8D 40 FF 48 83 F8 13 0F 87", 7, true, 4},
};

static const char* SPAWN_FIELDS[] = {"MaxSpawnedCompanyVehiclesPerCompany",
                                     "MaxSpawnedCorporationVehiclesPerCorporation",
                                     "MaxSpawnedManagedFleetVehiclesTotal"};
static int want[3], oField[3], oCfg = -1;
static uintptr_t cGameState;
static std::mutex lock;
static std::vector<uintptr_t> states;

// The loader owns the ini files: it merges every plugin's values into one
// file, so two plugins that set the same key do not overwrite each other.
static int setting(const wchar_t* key, int def) {
    int n = iniInt(L"company_size_unlimited.ini", L"CompanySlots", key, def);
    MT_LOG("setting %ls = %d", key, n);
    return n;
}

static void raise(void*) {
    std::lock_guard<std::mutex> g(lock);
    for (uintptr_t gs : states) {
        uintptr_t cfg = gs + oCfg;
        for (int i = 0; i < 3; i++) {
            int cur = get<int>(cfg + oField[i]);
            if (cur >= want[i]) continue;
            wr(cfg + oField[i], &want[i], 4);
            MT_LOG("%s %d -> %d", SPAWN_FIELDS[i], cur, want[i]);
        }
    }
}

static DWORD WINAPI run(LPVOID) {
    MT_LOG("Company Size Unlimited loaded");
    int bought = setting(L"MaxBoughtSlots", 95);  // vanilla 15
    want[0] = setting(L"MaxSpawnedPerCompany", 100);
    want[1] = setting(L"MaxSpawnedPerCorporation", 100);
    want[2] = setting(L"MaxSpawnedManagedFleetTotal", 500);
    if (bought > 122) { MT_LOG("MaxBoughtSlots %d is over the 122 the patches can hold -- using 122", bought); bought = 122; }

    for (auto& p : PATCHES) {
        auto hits = find_sig(p.sig);
        if (hits.size() != 1) {
            MT_LOG("%s: %s (%d matches) -- left vanilla", p.what, p.required ? "FAILED" : "not here", (int)hits.size());
            continue;
        }
        uint8_t b = (uint8_t)(bought + p.plus);
        bool ok = api->patch_bytes(hits[0] + p.at, &b, 1);
        MT_LOG("%s: %s at %p, now %d", p.what, ok ? "patched" : "FAILED to write", (void*)(hits[0] + p.at), bought + p.plus);
    }

    if (!api->wait_engine(INFINITE)) { MT_LOG("FAILED: no engine -- spawn caps stay as they are"); return 0; }
    cGameState = api->find_object("Class", "MotorTownGameState", "/Script/MotorTown");
    uintptr_t sCfg = api->find_object("ScriptStruct", "MTServerRuntimeConfig", "/Script/MotorTown");
    oCfg = off(cGameState, "Net_ServerConfig");
    bool ok = cGameState && sCfg && oCfg >= 0;
    for (int i = 0; i < 3; i++) ok = ok && (oField[i] = off(sCfg, SPAWN_FIELDS[i])) >= 0;
    if (!ok) { MT_LOG("FAILED: MotorTownGameState.Net_ServerConfig not found -- spawn caps stay as they are"); return 0; }
    MT_LOG("Net_ServerConfig at +%x (company +%x, corporation +%x, fleet +%x)", oCfg, oField[0], oField[1], oField[2]);

    int at = -1;
    for (;;) {  // the game state is rebuilt with every world load
        std::vector<uintptr_t> found;
        uintptr_t known = at >= 0 ? api->object(at) : 0;
        if (known && api->is_a(classOf(known), cGameState)) found.push_back(known);
        for (int i = 0, n = found.empty() ? api->num_objects() : 0; i < n; i++) {
            uintptr_t o = api->object(i);
            if (o && !isCDO(o) && api->is_a(classOf(o), cGameState)) { found.push_back(o); at = i; }
        }
        if (!found.empty() && api->game_thread_ready()) {
            { std::lock_guard<std::mutex> g(lock); states.swap(found); }
            api->run_on_game_thread(raise, nullptr);
        }
        Sleep(2000);
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    MoveFileW((gameDir() + L"company_slots.ini").c_str(), (gameDir() + L"company_size_unlimited.ini").c_str());  // settings from before the rename
    if (iniDisabled(L"company_size_unlimited.ini")) return 0;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
