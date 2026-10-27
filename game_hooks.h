// game_hooks.h - Triad Wars (x64) memory-backed actions for the debug menu.
// All RVAs are relative to the image base of TriadWars.exe.
#pragma once

#include <string>
#include <vector>

namespace tw {
namespace game {

// Asset/property-set names the menu can pick from.
const std::vector<std::string>& VehicleNames();
const std::vector<std::string>& NpcNames();
const std::vector<std::string>& WeaponNames();
const std::vector<std::string>& WeatherPresets();

// Spawns a property set at the player's transform. Returns the status line,
// e.g. "Spawn: done" or "Spawn failed: game system not ready". Never crashes:
// failures come back as a message (backstop: VEH in win_menu.cpp).
std::string SpawnAsset(const char* verb, const std::string& propSet);

// Sets the weather amount (0..2): Sunny/Cloudy/Light rain/Heavy rain.
std::string SetWeather(const std::string& preset);

// ReShade shader presets found in the game's Shaders/ folder.
const std::vector<std::string>& ShaderPresets();
// Activates the named ReShade preset (copies it to the game folder).
std::string SetShader(const std::string& name);

// Level targets the menu can set: Enforcer / Rackets / Triad / Rivals.
const std::vector<std::string>& LevelTargets();

// Sets a level (1..50) for the chosen target by editing state.json. The local
// server keeps the profile in memory, so the menu also touches its handler
// module to make the running server re-read the file. Returns the status line.
std::string SetLevel(const std::string& target, int level);

// Teleports player (or their vehicle) to the active waypoint.
std::string TeleportToWaypoint();

// Unlocks all content: vehicles, weapons, clothing, enforcer upgrades,
// favours, and maxes levels. Edits state.json and LocalServer/state.json.
std::string UnlockAllContent();

// True when this thread has the game's per-thread allocator block in TLS
// (required before any spawn). Publishes it via game function 0x4ba220 on
// first use. Also called from the VEH crash handler as a retry fixup.
bool EnsureThreadAllocator();

// True if rva falls inside a known function of the spawn call tree; used by
// the VEH backstop to decide whether a fault belongs to our spawn call.
bool IsSpawnRip(uintptr_t rva);

}  // namespace game
}  // namespace tw
