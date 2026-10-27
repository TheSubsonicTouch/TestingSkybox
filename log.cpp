#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace {

// Opens TW_MenuLog.txt for atomic append and writes one line. Allocation-
// free by design so the VEH handler can use it.
void appendLine(const char* data, DWORD len) {
    HANDLE h = CreateFileW(L"TW_MenuLog.txt", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, data, len, &written, nullptr);
    CloseHandle(h);
}

}  // namespace

void TW_Log(const char* fmt, ...) {
    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int head = std::snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] ",
                              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (head < 0) return;
    if (head >= (int)sizeof line) head = (int)sizeof line - 1;

    va_list args;
    va_start(args, fmt);
    int body = std::vsnprintf(line + head, sizeof line - (size_t)head - 2, fmt, args);
    va_end(args);
    if (body < 0) body = 0;
    size_t total = (size_t)head + (size_t)body;
    if (total > sizeof line - 3) total = sizeof line - 3;
    line[total++] = '\n';
    appendLine(line, (DWORD)total);
}

void TW_VehLog(const char* line) {
    char buf[768];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = std::snprintf(buf, sizeof buf, "[%02d:%02d:%02d.%03d] %s\n", st.wHour,
                          st.wMinute, st.wSecond, st.wMilliseconds, line);
    if (n <= 0) return;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    appendLine(buf, (DWORD)n);
}
