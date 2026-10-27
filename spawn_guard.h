// spawn_guard.h - crash-guarded call into the game's spawn functions.
//
// tw_spawn_guarded() calls the function pointer passed to it (with NULL
// arg5/arg6 on the stack, matching the game's own usage of SpawnAtXform)
// while protecting the caller:
//   - saves/restores all Windows x64 non-volatile registers (rbx, rbp, rsi,
//     rdi, r12-r15, xmm6, xmm15, mxcsr), so a game crash in between cannot
//     corrupt the C++ frame,
//   - exposes tw_in_spawn so the VEH handler can recognize an access
//     violation inside the spawn tree,
//   - on an unrecoverable fault the VEH redirects RIP to tw_spawn_fail,
//     which returns NULL to the C++ caller instead of crashing the game.
#pragma once

extern "C" {

// Calls tw_spawn_target(idOut, hashPtr, matrix, flags) with stack args 5/6
// forced to NULL. Returns whatever the target returns (ignored by callers).
void tw_spawn_guarded(void* idOut, void* hashPtr, const void* matrix, int flags);

// Landing pad the VEH redirects to on an unrecoverable spawn fault.
void tw_spawn_fail(void);

// Set to 1 by the VEH when it redirected to tw_spawn_fail.
extern volatile long g_spawnFaulted;

// Retry budget for the VEH TLS fixup; reset to 0 at every guarded call.
// (Used as a compare-exchange latch: 0 = one retry still available.)
extern volatile long g_spawnTries;

// 1 while tw_spawn_guarded is inside the game function.
extern volatile int tw_in_spawn;

// Target of tw_spawn_guarded; set by the C++ caller before each call.
extern void* tw_spawn_target;

}  // extern "C"
