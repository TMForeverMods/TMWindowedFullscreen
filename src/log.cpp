#include "log.h"

// The whole implementation is compiled only when logging is enabled (see
// WFS_LOG_ENABLED in log.h). With it at 0 this file contributes nothing and no
// TMWindowedFullscreen.log is ever created or written.
#if WFS_LOG_ENABLED

#include <cstdarg>
#include <cstdio>
#include <string>

namespace wfs {

namespace {
wchar_t        g_path[MAX_PATH] = {};
CRITICAL_SECTION g_lock;
bool           g_ready = false;
} // namespace

void logInit(HMODULE self) {
    InitializeCriticalSection(&g_lock);

    wchar_t dll[MAX_PATH] = {};
    if (GetModuleFileNameW(self, dll, MAX_PATH) == 0) {
        wcscpy_s(g_path, L"TMWindowedFullscreen.log");
        g_ready = true;
        return;
    }

    std::wstring path(dll);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
    const std::wstring file = dir + L"\\TMWindowedFullscreen.log";
    wcsncpy_s(g_path, file.c_str(), _TRUNCATE);
    g_ready = true;
}

void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    OutputDebugStringA("[TMWindowedFullscreen] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    if (!g_ready) {
        return;
    }

    EnterCriticalSection(&g_lock);
    FILE* f = _wfopen(g_path, L"a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
    LeaveCriticalSection(&g_lock);
}

} // namespace wfs

#endif // WFS_LOG_ENABLED