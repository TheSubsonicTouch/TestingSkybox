// tw_teleport.h  --  "Teleport to waypoint" for Triad Wars (TriadWars.exe, x64)
//
// Ported from the logic in sneakyevil's Sleeping Dogs: DE mod menu
// (callback 0x180078e50 + waypoint query in the Teleport menu).
//
// STATUS: UNTESTED. Written from static analysis only (no way to run the game
// where this was produced). Confidence per address is noted below.
//
// USAGE
//   #include "tw_teleport.h"
//   tw::TeleportToWaypoint();      // call from the GAME thread (your per-frame
//                                  // hook), not from the render/ImGui thread.
//
// Offsets are RVAs relative to the TriadWars.exe image base (ASLR safe, we
// resolve the base at runtime).

#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace tw {

struct Vec3 { float x, y, z; };

// ---------------------------------------------------------------------------
// Triad Wars RVAs   (SD = Sleeping Dogs DE RVA these were derived from)
// ---------------------------------------------------------------------------
// Data globals (each holds a POINTER; verified: used identically inside
// byte-identical copies of the same functions in both executables)
constexpr uintptr_t RVA_WaypointMgr  = 0x2605950; // SD 0x24310a0  (+0x07 byte = active, +0x30 Vec3 = position)
constexpr uintptr_t RVA_WorldQuery   = 0x23e0e40; // SD 0x2174a50  (world / ground-probe object)
constexpr uintptr_t RVA_LocalPlayer  = 0x23f25f8; // SD 0x235c488  (local player object; +0x58 -> transform node)

// Functions
constexpr uintptr_t RVA_FindByUid    = 0x1c8ae0;  // SD 0x190ad0   byte-identical            [tier A]
constexpr uintptr_t RVA_GroundProbe  = 0xeeae0;   // SD 0xd6e30    byte-identical            [tier A]
constexpr uintptr_t RVA_GroundSolve  = 0xeed60;   // SD 0xd70b0    byte-identical            [tier A]
constexpr uintptr_t RVA_SetTransform = 0x5f5ab0;  // SD 0x556870   SAME FUNCTION, but Triad Wars
                                                  //   added a 3rd parameter (bool, r8b). The Sleeping
                                                  //   Dogs menu hardcodes that slot to 0, so we pass false.

// Component UID the menu uses to find "what the player is riding/driving".
constexpr uint32_t UID_RideComponent = 0x96000001;

// ---------------------------------------------------------------------------
// Function pointer types (Microsoft x64 ABI)
// ---------------------------------------------------------------------------
using FindByUid_t    = void* (__fastcall*)(void* self, uint32_t uid);
using GroundProbe_t  = void* (__fastcall*)(void* world, const Vec3* pos, uintptr_t a, uintptr_t b, uintptr_t c);
using GroundSolve_t  = void  (__fastcall*)(void* probe, Vec3* out, const Vec3* pos, uintptr_t a);
using SetTransform_t = void  (__fastcall*)(void* target, const float* matrix4x4 /*64 bytes*/, bool flag);

inline uintptr_t Base() {
    static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return b;
}
template <class T> inline T Fn(uintptr_t rva)  { return reinterpret_cast<T>(Base() + rva); }
inline void* ReadPtr(uintptr_t rva) {
    return *reinterpret_cast<void**>(Base() + rva);
}
template <class T> inline T* At(void* p, uintptr_t off) {
    return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(p) + off);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Reads the active waypoint. Returns false if there is none.
inline bool GetWaypoint(Vec3& out) {
    void* wp = ReadPtr(RVA_WaypointMgr);
    if (!wp) return false;
    if (*At<uint8_t>(wp, 0x07) == 0) return false;     // not active
    out = *At<Vec3>(wp, 0x30);
    return true;
}

// Resolves the ground position under (x,y,z) using the game's own query.
inline bool ResolveGround(const Vec3& in, Vec3& out) {
    void* world = ReadPtr(RVA_WorldQuery);
    if (!world) return false;
    Vec3 p = in;
    void* probe = Fn<GroundProbe_t>(RVA_GroundProbe)(world, &p, 0, 0, 0);
    if (!probe) return false;
    out = Vec3{0, 0, 0};
    Fn<GroundSolve_t>(RVA_GroundSolve)(probe, &out, &p, 0);
    return true;
}

// Moves the player (or the vehicle they are in) to 'pos'.
inline bool TeleportTo(const Vec3& pos) {
    void* player = ReadPtr(RVA_LocalPlayer);
    if (!player) return false;

    // Same selection logic as the menu: if the ride component exists, move
    // that object (vehicle); otherwise move the player itself.
    void* target = player;
    if (void* comp = Fn<FindByUid_t>(RVA_FindByUid)(player, UID_RideComponent)) {
        if (void* p = *At<void*>(comp, 0x70)) {
            if (void* c = *At<void*>(p, 0x28)) target = c;
        }
    }
    void* node = *At<void*>(target, 0x58);               // transform node
    if (!node) return false;

    alignas(16) float m[16];
    std::memcpy(m, At<void>(node, 0x80), sizeof(m));     // current 4x4 matrix (rows at +0x80..+0xbf)
    m[12] = pos.x; m[13] = pos.y; m[14] = pos.z; m[15] = 1.0f;   // overwrite translation row

    Fn<SetTransform_t>(RVA_SetTransform)(target, m, /*flag*/ false);
    return true;
}

// Waypoint -> ground -> teleport.   zLift: extra height (the menu's free-cam
// path uses 1.5; the normal path uses 0). Z is the up axis.
inline bool TeleportToWaypoint(float zLift = 0.0f) {
    Vec3 wp;
    if (!GetWaypoint(wp)) return false;
    Vec3 ground;
    if (!ResolveGround(wp, ground)) return false;
    ground.z += zLift;
    return TeleportTo(ground);
}

} // namespace tw
