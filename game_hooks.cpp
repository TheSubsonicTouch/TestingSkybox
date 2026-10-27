// game_hooks.cpp - Triad Wars memory-backed actions (spawn / weather /
// teleport) for the debug menu.
//
// Verified call chain for spawning (from TriadWars.exe disassembly, matched
// against the game's own respawn loop at 0x55fad0):
//
//   hash = 0x1b82a0(&hashOut, propSetName)          -> returns &hashOut
//   id   = 0x1c8a60(&table, &idOut, instanceId)     -> returns &idOut
//   0x63deb0(&idOut, &hashOut, matrix, 0)           -> IdToNameObj +
//                                                       SpawnAtXform
//
// Spawn safety: EnsureThreadAllocator() must find the per-thread allocator
// block in TLS (index at RVA 0x267B7E0) before entering the spawn tree --
// that is the exact fault from TW_MenuLog (game+0x1F3FEE dereferences
// [rax+0x58] with rax = NULL). If the block is missing we publish it by
// calling the game's thread-init function 0x4ba220 (which writes the root
// heap at RVA 0x23D73D8 into TLS). The call itself runs through the
// crash-guarded trampoline in spawn_guard.S as a second line of defense.
#include "game_hooks.h"

#include "json_util.h"
#include "tw_teleport.h"
#include "win/log.h"
#include "win/spawn_guard.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tw {
namespace game {

// --- verified RVAs -------------------------------------------------------
constexpr uintptr_t kHashFn           = 0x1b82a0;   // string -> id hash
constexpr uintptr_t kFindOrCreateId   = 0x1c8a60;   // (table, out, idStr)
constexpr uintptr_t kSpawnWrapper     = 0x63deb0;   // (idOut, hash, xform, flags)
constexpr uintptr_t kSpawnTable       = 0x23F2510;  // address of id table object
constexpr uintptr_t kTlsIndexGlobal   = 0x267B7E0;  // DWORD TlsAlloc index
constexpr uintptr_t kRootHeapGlobal   = 0x23D73D8;  // void* shared root heap
constexpr uintptr_t kThreadInitFn     = 0x4ba220;   // publish heap into this TLS
constexpr uintptr_t kWeatherGetterFn  = 0x71780;    // -> weather singleton*
constexpr uintptr_t kPlayerGlobal     = 0x23f25f8;  // void* local player

using HashFn      = void* (*)(void* out, const char* s);
using FindIdFn    = void* (*)(void* table, void* out, const char* s);
using SpawnFn     = void* (*)(void* idOut, void* hashPtr, const void* matrix,
                              int flags, void* a5, void* a6);
using ThreadInit  = void (*)();
using WeatherGet  = void* (*)();

// --- helpers -------------------------------------------------------------

bool Readable(const void* p, size_t n) {
    if (!p) return false;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(b, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    DWORD prot = mbi.Protect & 0xFF;
    switch (prot) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        break;
    default:
        return false;
    }
    // If the range crosses a page boundary, check the last byte too.
    const uint8_t* last = b + (n ? n - 1 : 0);
    if ((reinterpret_cast<uintptr_t>(b) & ~uintptr_t(0xFFF)) !=
        (reinterpret_cast<uintptr_t>(last) & ~uintptr_t(0xFFF))) {
        MEMORY_BASIC_INFORMATION m2{};
        if (VirtualQuery(last, &m2, sizeof m2) != sizeof m2) return false;
        if (m2.State != MEM_COMMIT || (m2.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    }
    return true;
}

void IdentityMatrix(float m[16]) {
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

bool MatrixSane(const float m[16]) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

// Current 4x4 transform of the local player (rows at node+0x80).
bool ReadPlayerMatrix(float m[16]) {
    const uintptr_t base = Base();
    void* player = *reinterpret_cast<void**>(base + kPlayerGlobal);
    if (!Readable(player, 0x60)) return false;
    void* node = *reinterpret_cast<void**>(static_cast<uint8_t*>(player) + 0x58);
    if (!Readable(node, 0xC0)) return false;
    std::memcpy(m, static_cast<uint8_t*>(node) + 0x80, 64);
    return MatrixSane(m);
}

// Weapon/character property sets: gamedb.json only has vehicles in
// plaintext, so these come from literal strings in TriadWars.exe.
// The last 3 are the player's equipped weapons from state.json
// (Cleaver, Resolver, Hi Cap Morelli) — their gamedb entries use
// sym: hashes which we cannot resolve to property-set names, so we
// use the sym hash string as the spawn identifier.
const char* const kNpcNames[] = {
    "object-physical-character-police",
    "object-physical-character-ThumbnailMan",
    "object-physical-character-taxi_driver_faceaction",
    "object-physical-character-player",
};
const char* const kWeaponNames[] = {
    "object-physical-weapon-firearm-pistol-9mm",
    "object-physical-weapon-firearm-rifle-assault_dlc",
    "object-physical-weapon-firearm-smg-45cal_gold",
    "object-physical-weapon-melee-CombatFish001",
    "object-physical-weapon-melee-cleaver_Gold",
    "object-physical-weapon-melee-tireiron",
    "object-physical-weapon-melee-umbrella",
    "sym:0x35f98f2d",
    "sym:0xe42540bd",
    "sym:0x5360631b",
};
const char* const kWeaponDisplayNames[] = {
    "9mm Pistol",
    "Assault Rifle",
    "SMG .45cal Gold",
    "Combat Fish",
    "Cleaver Gold",
    "Tire Iron",
    "Umbrella",
    "Cleaver",
    "Resolver",
    "Hi Cap Morelli",
};
const char* const kWeatherNames[] = {"Sunny", "Cloudy", "Light rain", "Heavy rain"};

std::vector<std::string> makeVector(const char* const* a, std::size_t n) {
    return std::vector<std::string>(a, a + n);
}


// Vehicle property sets generated from gamedb.json (asset_names.cpp).
extern const char* const kVehicleNames[];
extern const std::size_t kVehicleNamesCount;

const std::vector<std::string>& VehicleNames() {
    static const std::vector<std::string> v(kVehicleNames, kVehicleNames + kVehicleNamesCount);
    return v;
}

const std::vector<std::string>& NpcNames() {
    static const std::vector<std::string> v = makeVector(
        kNpcNames, sizeof(kNpcNames) / sizeof(kNpcNames[0]));
    return v;
}

const std::vector<std::string>& WeaponNames() {
    static const std::vector<std::string> v = makeVector(
        kWeaponNames, sizeof(kWeaponNames) / sizeof(kWeaponNames[0]));
    return v;
}

const std::vector<std::string>& WeatherPresets() {
    static const std::vector<std::string> v = makeVector(
        kWeatherNames, sizeof(kWeatherNames) / sizeof(kWeatherNames[0]));
    return v;
}

bool EnsureThreadAllocator() {
    const uintptr_t base = Base();
    const DWORD idx = *reinterpret_cast<const DWORD*>(base + kTlsIndexGlobal);
    if (idx == 0xFFFFFFFFu) return false;

    uint8_t* blk = static_cast<uint8_t*>(TlsGetValue(idx));
    if (blk) { TW_Log("thread-init: existing blk=%p (leave as-is)", blk); return true; }

    void* heap = *reinterpret_cast<void**>(base + kRootHeapGlobal);
    if (!heap) {
        TW_Log("thread-init: root heap global is null (nothing to publish)");
        return false;
    }
    TW_Log("thread-init: publishing allocator (tls_idx=%u heap=%p blk=%p)", idx, heap, blk);
    Fn<ThreadInit>(kThreadInitFn)();

    blk = static_cast<uint8_t*>(TlsGetValue(idx));
    const bool ok = blk != nullptr;
    TW_Log("thread-init: result blk=%p ok=%d", blk, ok ? 1 : 0);
    return ok;
}

bool IsSpawnRip(uintptr_t rva) {
    static const struct { uintptr_t lo, hi; } kRanges[] = {
        {0x063deb0, 0x063e120},   // spawn wrapper
        {0x1f3e70, 0x1f5600},     // observed fault: game+0x1F3FEE
        {0x229e70, 0x22a200},     // IdToNameObj
        {0x1b82a0, 0x1b8600},     // hash
        {0x1c8a60, 0x1c8c00},     // FindOrCreateId
        {0x246c30, 0x246e00},     // id/name resolution
        {0x06392d0, 0x0639900},   // SpawnAtXform inner executor
    };
    for (const auto& r : kRanges)
        if (rva >= r.lo && rva < r.hi) return true;
    return false;
}

std::string SpawnAsset(const char* verb, const std::string& propSet) {
    const std::string failed = std::string(verb) + " failed: ";
    if (propSet.empty()) return std::string(verb) + ": nothing selected";

    if (!EnsureThreadAllocator()) {
        TW_Log("%s: allocator not ready, refusing to spawn '%s'", verb, propSet.c_str());
        return failed + "game system not ready";
    }

    alignas(16) float m[16];
    const bool haveMtx = ReadPlayerMatrix(m);
    if (!haveMtx) IdentityMatrix(m);

    uint32_t hashVal = 0;
    uint32_t idVal = 0;
    static uint32_t s_counter = 0;
    char idStr[48];
    std::snprintf(idStr, sizeof idStr, "TWSpawn%u", ++s_counter);

    g_spawnFaulted = 0;
    // Hash + id creation can allocate (they may reach the observed TLS
    // allocator at game+0x1F3FEE), so they run under the same guard as the
    // spawn call itself; the VEH's tw_in_spawn backstop covers all three.
    tw_spawn_target = reinterpret_cast<void*>(Base() + kHashFn);
    tw_spawn_guarded(&hashVal, const_cast<char*>(propSet.c_str()), nullptr, 0);
    if (!g_spawnFaulted) {
        tw_spawn_target = reinterpret_cast<void*>(Base() + kFindOrCreateId);
        tw_spawn_guarded(reinterpret_cast<void*>(Base() + kSpawnTable), &idVal, idStr, 0);
    }

    TW_Log("%s: '%s' hash=%08X id=%08X matrix=%s", verb, propSet.c_str(), hashVal, idVal,
           haveMtx ? "player" : "identity");

    if (g_spawnFaulted) {
        TW_Log("%s: guarded id setup faulted (see VEH line above)", verb);
        return failed + "internal error (see TW_MenuLog.txt)";
    }

    tw_spawn_target = reinterpret_cast<void*>(Base() + kSpawnWrapper);
    tw_spawn_guarded(&idVal, &hashVal, m, 0);

    if (g_spawnFaulted) {
        TW_Log("%s: guarded spawn faulted (see VEH line above)", verb);
        return failed + "internal error (see TW_MenuLog.txt)";
    }
    return std::string(verb) + ": done";
}

std::string SetWeather(const std::string& preset) {
    float amt;
    if (preset == "Sunny") amt = 0.0f;
    else if (preset == "Cloudy") amt = 0.5f;
    else if (preset == "Light rain") amt = 1.2f;
    else if (preset == "Heavy rain") amt = 2.0f;
    else return "Weather failed: unknown preset";

    void* obj = Fn<WeatherGet>(kWeatherGetterFn)();
    if (!obj) return "Weather failed: not ready";

    // weather_set_amount (0x5766a0) clamps to [0,2] and writes these three.
    auto* p = static_cast<uint8_t*>(obj);
    if (amt < 0.0f) amt = 0.0f;
    if (amt > 2.0f) amt = 2.0f;
    *reinterpret_cast<float*>(p + 0x34) = amt;
    *reinterpret_cast<float*>(p + 0x38) = amt;
    *reinterpret_cast<float*>(p + 0x3c) = amt;
    TW_Log("Weather: '%s' amount=%.2f obj=%p", preset.c_str(), amt, obj);
    return "Weather: " + preset;
}

// --- ReShade shader presets ------------------------------------------------


// UTF-8 -> UTF-16 for Win32 file APIs.
std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, 0);
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], len);
    return out;
}

std::string GameDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    std::wstring p(path);
    auto slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return "";
    p = p.substr(0, slash + 1);
    int len = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, 0);
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, &out[0], len, nullptr, nullptr);
    return out;
}

bool ReadFileText(const std::string& path, std::string& out) {
    HANDLE h = CreateFileW(ToWide(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
    CloseHandle(h);
    return true;
}

bool WriteFileText(const std::string& path, const std::string& text) {
    HANDLE h = CreateFileW(ToWide(path).c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
    return true;
}

// Updates the last-write time so the running local server re-reads state.json.
void Touch(const std::string& path) {
    HANDLE h = CreateFileW(ToWide(path).c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft;
    SYSTEMTIME st;
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

json::Value* member(json::Value& v, const std::string& key) {
    if (!v.isObject()) return nullptr;
    return v.find(key);
}

void setAllRackets(json::Value& turfBag, int level) {
    json::Value* turf = member(turfBag, "Turf");
    if (turf && turf->isObject()) {
        for (const char* key : {"Safehouse", "Warehouse", "Temple"}) {
            json::Value* b = member(*turf, key);
            if (b && b->isObject()) (*b)["RacketLevel"].setInt(level);
        }
    }
    json::Value* list = member(turfBag, "Rackets");
    if (list && list->isArray())
        for (auto& r : list->array)
            if (r.isObject()) r["RacketLevel"].setInt(level);
}

void setOwner(json::Value& owner, const std::string& target, int level, long long xp) {
    json::Value* pdb = member(owner, "PlayerDataBag");
    json::Value* tb = member(owner, "TurfBag");
    if (target == "Enforcer") {
        if (pdb) (*pdb)["EnforcerXP"].setInt(xp);
    } else if (target == "Rackets") {
        if (tb) setAllRackets(*tb, level);
    } else if (target == "Triad") {
        if (pdb) (*pdb)["TriadLevel"].setInt(level);
    }
}



std::vector<std::string> g_shaderPresets;
bool g_shaderPresetsLoaded = false;

std::string ShadersDir() {
    const std::string dir = GameDir();
    if (dir.empty()) return "";
    return dir + "Shaders";
}

void LoadShaderPresets() {
    if (g_shaderPresetsLoaded) return;
    g_shaderPresetsLoaded = true;
    g_shaderPresets.clear();
    const std::string dir = ShadersDir();
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((ToWide(dir) + L"\\*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char name[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, MAX_PATH, nullptr, nullptr);
            g_shaderPresets.push_back(name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (g_shaderPresets.empty()) g_shaderPresets.push_back("(none found)");
}


const std::vector<std::string>& ShaderPresets() {
    LoadShaderPresets();
    return g_shaderPresets;
}

std::string SetShader(const std::string& name) {
    if (name.empty() || name == "(none found)") return "Shader: no presets available";
    const std::string dir = ShadersDir();
    if (dir.empty()) return "Shader failed: game dir not found";
    const std::string src = dir + "\\" + name;
    const std::string dst = GameDir() + "ReShadePreset.ini";
    // Copy the selected preset to the active ReShade preset path.
    HANDLE hSrc = CreateFileW(ToWide(src).c_str(), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hSrc == INVALID_HANDLE_VALUE) return "Shader failed: cannot open " + name;
    LARGE_INTEGER sz;
    GetFileSizeEx(hSrc, &sz);
    std::vector<char> buf(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    ReadFile(hSrc, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
    CloseHandle(hSrc);
    if (got != buf.size()) return "Shader failed: read error";
    if (!WriteFileText(dst, std::string(buf.data(), buf.size())))
        return "Shader failed: cannot write preset";
    TW_Log("Shader: activated '%s' -> %s", name.c_str(), dst.c_str());
    return "Shader: " + name + " activated (restart may be needed)";
}

std::string TeleportToWaypoint() {
    // tw_teleport.h RVAs are ported from Sleeping Dogs DE and untested.
    // Mark as not wired until verified on Triad Wars.
    return "Teleport: not wired yet (untested RVAs)";
}


// Unlocks all content: vehicles, weapons, clothing, enforcer upgrades,
// favours, and maxes levels. Edits state.json and LocalServer/state.json.
// Unlocks all content: vehicles, weapons, clothing, enforcer upgrades,
// favours, and maxes levels. Edits state.json and LocalServer/state.json.
std::string UnlockAllContent() {
    const std::string dir = GameDir();
    if (dir.empty()) return "UnlockAll failed: game directory not found";

    const char* files[] = {"state.json", "LocalServer/state.json"};
    int edited = 0;

    // Load gamedb for UIDs
    std::string gamedbPath = dir + "LocalServer/devdata/gamedb.json";
    std::string gamedbText;
    if (!ReadFileText(gamedbPath, gamedbText)) {
        return "UnlockAll failed: gamedb.json not found";
    }
    json::Value gamedb;
    if (!json::parse(gamedbText, gamedb) || !gamedb.isObject()) {
        return "UnlockAll failed: gamedb.json parse error";
    }

    // Collect all UIDs from gamedb
    std::vector<std::string> enforcerUpgradeUIDs;
    std::vector<std::string> storeVehicleUIDs;
    std::vector<std::string> storeWeaponUIDs;
    std::vector<std::string> storeClothingUIDs;

    json::Value* eu = member(gamedb, "UFG::EnforcerUpgrade");
    if (eu && eu->isArray()) {
        for (auto& item : eu->array) {
            json::Value* uid = member(item, "$uid");
            if (uid && uid->isString()) enforcerUpgradeUIDs.push_back(uid->str);
        }
    }

    json::Value* sv = member(gamedb, "UFG::StoreVehicle");
    if (sv && sv->isArray()) {
        for (auto& item : sv->array) {
            json::Value* uid = member(item, "$uid");
            if (uid && uid->isString()) storeVehicleUIDs.push_back(uid->str);
        }
    }

    json::Value* sw = member(gamedb, "UFG::StoreWeapon");
    if (sw && sw->isArray()) {
        for (auto& item : sw->array) {
            json::Value* uid = member(item, "$uid");
            if (uid && uid->isString()) storeWeaponUIDs.push_back(uid->str);
        }
    }

    json::Value* sc = member(gamedb, "UFG::StoreClothing");
    if (sc && sc->isArray()) {
        for (auto& item : sc->array) {
            json::Value* uid = member(item, "$uid");
            if (uid && uid->isString()) storeClothingUIDs.push_back(uid->str);
        }
    }

    TW_Log("UnlockAll: %zu enforcer upgrades, %zu vehicles, %zu weapons, %zu clothing",
           enforcerUpgradeUIDs.size(), storeVehicleUIDs.size(),
           storeWeaponUIDs.size(), storeClothingUIDs.size());

    for (const char* rel : files) {
        const std::string path = dir + rel;
        std::string text;
        if (!ReadFileText(path, text)) continue;
        json::Value root;
        if (!json::parse(text, root) || !root.isObject()) continue;

        bool changed = false;
        json::Value* player = member(root, "player");
        if (!player || !player->isObject()) continue;

        // 1. EnforcerUpgradesBag - add all enforcer upgrades
        json::Value* eub = member(*player, "EnforcerUpgradesBag");
        if (!eub) {
            eub = &(*player)["EnforcerUpgradesBag"];
            eub->type = json::Value::Type::Array;
        }
        if (eub->isArray()) {
            eub->array.clear();
            for (const auto& uid : enforcerUpgradeUIDs) {
                json::Value entry;
                entry.type = json::Value::Type::Object;
                entry["Item1"].setString(uid);
                entry["Item2"].setInt(0);
                eub->array.push_back(std::move(entry));
            }
            changed = true;
        }

        // 2. InventoryBag - add all vehicles and weapons
        json::Value* ib = member(*player, "InventoryBag");
        if (!ib) {
            ib = &(*player)["InventoryBag"];
            ib->type = json::Value::Type::Array;
        }
        if (ib->isArray()) {
            std::set<std::string> existingStoreItemIDs;
            for (auto& item : ib->array) {
                if (item.isObject()) {
                    json::Value* sid = member(item, "StoreItemID");
                    if (sid && sid->isString()) existingStoreItemIDs.insert(sid->str);
                }
            }
            int nextInvID = 7018;
            for (const auto& uid : storeVehicleUIDs) {
                if (existingStoreItemIDs.find(uid) == existingStoreItemIDs.end()) {
                    json::Value entry;
                    entry.type = json::Value::Type::Object;
                    entry["SecondsElapsed"].setInt(0);
                    entry["TotalSeconds"].setInt(0);
                    entry["StoreItemID"].setString(uid);
                    entry["InventoryItemID"].setString(std::to_string(nextInvID++));
                    entry["AccountID"].setString("1");
                    ib->array.push_back(std::move(entry));
                    changed = true;
                }
            }
            for (const auto& uid : storeWeaponUIDs) {
                if (existingStoreItemIDs.find(uid) == existingStoreItemIDs.end()) {
                    json::Value entry;
                    entry.type = json::Value::Type::Object;
                    entry["SecondsElapsed"].setInt(0);
                    entry["TotalSeconds"].setInt(0);
                    entry["StoreItemID"].setString(uid);
                    entry["InventoryItemID"].setString(std::to_string(nextInvID++));
                    entry["AccountID"].setString("1");
                    ib->array.push_back(std::move(entry));
                    changed = true;
                }
            }
        }

        // 3. CustomizationBag - add all clothing
        json::Value* cb = member(*player, "CustomizationBag");
        if (!cb) {
            cb = &(*player)["CustomizationBag"];
            cb->type = json::Value::Type::Object;
        }
        if (cb->isObject()) {
            json::Value* csiu = member(*cb, "ClothingStoreItemUIDs");
            if (!csiu) {
                csiu = &(*cb)["ClothingStoreItemUIDs"];
                csiu->type = json::Value::Type::Array;
            }
            if (csiu->isArray()) {
                std::set<std::string> existing;
                for (auto& item : csiu->array) {
                    if (item.isString()) existing.insert(item.str);
                }
                for (const auto& uid : storeClothingUIDs) {
                    if (existing.find(uid) == existing.end()) {
                        json::Value entry;
                        entry.setString(uid);
                        csiu->array.push_back(std::move(entry));
                        changed = true;
                    }
                }
            }
            changed = true;
        }

        // 4. FavoursBag - mark all as available
        json::Value* fb = member(*player, "FavoursBag");
        if (fb && fb->isArray()) {
            for (auto& favour : fb->array) {
                if (favour.isObject()) {
                    favour["SecondsElapsed"].setInt(0);
                    favour["TotalSeconds"].setInt(0);
                    favour["IsNew"].setInt(1);
                }
            }
            changed = true;
        }

        // 5. Max levels (Enforcer, Rackets, Triad)
        const long long maxXP = 17177050LL; // Level 50
        json::Value* pdb = member(*player, "PlayerDataBag");
        if (pdb && pdb->isObject()) {
            pdb->operator[]("EnforcerXP").setInt(maxXP);
            pdb->operator[]("TriadLevel").setInt(50);
            changed = true;
        }
        json::Value* tb = member(*player, "TurfBag");
        if (tb && tb->isObject()) {
            json::Value* turf = member(*tb, "Turf");
            if (turf && turf->isObject()) {
                for (const char* key : {"Safehouse", "Warehouse", "Temple"}) {
                    json::Value* b = member(*turf, key);
                    if (b && b->isObject()) b->operator[]("RacketLevel").setInt(10);
                }
            }
            json::Value* rackets = member(*tb, "Rackets");
            if (rackets && rackets->isArray()) {
                for (auto& r : rackets->array) {
                    if (r.isObject()) r["RacketLevel"].setInt(10);
                }
            }
            changed = true;
        }

        // Also apply to opponents
        json::Value* opps = member(root, "opponents");
        if (opps && opps->isArray()) {
            for (auto& o : opps->array) {
                if (!o.isObject()) continue;
                json::Value* data = member(o, "data");
                if (data && data->isObject()) {
                    json::Value* opp_pdb = member(*data, "PlayerDataBag");
                    if (opp_pdb && opp_pdb->isObject()) {
                        opp_pdb->operator[]("EnforcerXP").setInt(maxXP);
                        opp_pdb->operator[]("TriadLevel").setInt(50);
                    }
                    json::Value* opp_tb = member(*data, "TurfBag");
                    if (opp_tb && opp_tb->isObject()) {
                        json::Value* opp_turf = member(*opp_tb, "Turf");
                        if (opp_turf && opp_turf->isObject()) {
                            for (const char* key : {"Safehouse", "Warehouse", "Temple"}) {
                                json::Value* b = member(*opp_turf, key);
                                if (b && b->isObject()) b->operator[]("RacketLevel").setInt(10);
                            }
                        }
                        json::Value* opp_rackets = member(*opp_tb, "Rackets");
                        if (opp_rackets && opp_rackets->isArray()) {
                            for (auto& r : opp_rackets->array) {
                                if (r.isObject()) r["RacketLevel"].setInt(10);
                            }
                        }
                    }
                    json::Value* opp_fb = member(*data, "FavoursBag");
                    if (opp_fb && opp_fb->isArray()) {
                        for (auto& favour : opp_fb->array) {
                            if (favour.isObject()) {
                                favour["SecondsElapsed"].setInt(0);
                                favour["TotalSeconds"].setInt(0);
                                favour["IsNew"].setInt(1);
                            }
                        }
                    }
                    changed = true;
                }
            }
        }

        if (changed && WriteFileText(path, json::dump(root, 1))) ++edited;
    }

    if (edited == 0) return "UnlockAll failed: state.json not found or unreadable";

    Touch(dir + "LocalServer/handlers.py");
    return "UnlockAll: done (" + std::to_string(edited) + " file" + (edited == 1 ? "" : "s") + ")";
}


// --- level editing (state.json) --------------------------------------------


// XP needed to reach each enforcer level (game XPDefinitions.mAuthoredXPLevels).
// Index 0 = level 1, index 49 = level 50 (the cap).
const long long kXPLevels[50] = {
    0, 175, 1175, 3650, 8200, 16750, 31150, 50050, 78725, 115475, 161300,
    213800, 271175, 336175, 409050, 490050, 579425, 677425, 784300, 902300,
    1034050, 1180300, 1341800, 1519300, 1713550, 1925300, 2155300, 2404300,
    2673050, 2962300, 3272800, 3605300, 3972800, 4376800, 4818800, 5300300,
    5822800, 6387800, 6996800, 7651300, 8352800, 9102800, 9902800, 10754300,
    11658800, 12617800, 13632800, 14705300, 15873300, 17177050,
};

const int kMaxRacketLevel = 10;   // RacketType mLevels has 11 entries (0..10)

const std::vector<std::string>& LevelTargets() {
    static const std::vector<std::string> v = {"Enforcer", "Rackets", "Triad", "Rivals"};
    return v;
}

std::string SetLevel(const std::string& target, int level) {
    if (level < 1) level = 1;
    if (level > 50) level = 50;
    const long long xp = kXPLevels[level - 1];
    const int racketLevel = std::min(level, kMaxRacketLevel);

    const std::string dir = GameDir();
    if (dir.empty()) return "Level failed: game directory not found";

    const char* files[] = {"state.json", "LocalServer/state.json"};
    int edited = 0;
    for (const char* rel : files) {
        const std::string path = dir + rel;
        std::string text;
        if (!ReadFileText(path, text)) continue;
        json::Value root;
        if (!json::parse(text, root) || !root.isObject()) continue;

        bool changed = false;
        if (target == "Rivals") {
            json::Value* opps = member(root, "opponents");
            if (opps && opps->isArray()) {
                for (auto& o : opps->array) {
                    if (!o.isObject()) continue;
                    json::Value* data = member(o, "data");
                    if (data && data->isObject()) {
                        setOwner(*data, "Enforcer", racketLevel, xp);
                        json::Value* tb = member(*data, "TurfBag");
                        if (tb) setAllRackets(*tb, racketLevel);
                        changed = true;
                    }
                }
            }
        } else {
            json::Value* player = member(root, "player");
            if (player && player->isObject()) {
                setOwner(*player, target, racketLevel, xp);
                changed = true;
            }
        }

        if (changed && WriteFileText(path, json::dump(root, 1))) ++edited;
    }

    if (edited == 0) return "Level failed: state.json not found or unreadable";

    // The local server caches the profile in memory; touch its handler module so
    // the next request reloads state.json.
    Touch(dir + "LocalServer/handlers.py");
    return "Level set: " + target + " -> " + std::to_string(level) + " (" +
           std::to_string(edited) + " file" + (edited == 1 ? "" : "s") + ")";
}

}  // namespace game
}  // namespace tw
