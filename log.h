#pragma once
// Appends timestamped lines to TW_MenuLog.txt in the game folder.
// Both functions are allocation-free (CreateFileW/WriteFile per line), so
// they are safe to call from the vectored exception handler.
void TW_Log(const char* fmt, ...);
// Appends one pre-formatted line (a timestamp prefix is added).
void TW_VehLog(const char* line);
