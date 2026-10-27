#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Installs the vectored exception handler (startup load-crash fixups + spawn
// crash backstop). Idempotent; safe to call from DllMain.
void TW_VehInstall();

// Installs the D3D11 Present hook and input polling. Safe to call more than once.
void TW_MenuStart();
// Stops input handling. Hooks stay installed until the process exits.
void TW_MenuStop();

#ifdef __cplusplus
}
#endif
