#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// Logging switch
//
//   WFS_LOG_ENABLED 1 -> write TMWindowedFullscreen.log (next to the DLL) and
//                        mirror every line to OutputDebugString.
//   WFS_LOG_ENABLED 0 -> logging is compiled out: no file is created, nothing is
//                        written, and every log call becomes a no-op.
//
// Flip this one value to re-enable logging; the call sites never change.
// ---------------------------------------------------------------------------
#define WFS_LOG_ENABLED 0

namespace wfs {

#if WFS_LOG_ENABLED

// Records the DLL's own module so the log file can be placed next to it.
void logInit(HMODULE self);

// printf-style logging to OutputDebugString and to TMWindowedFullscreen.log next
// to the DLL.
void logf(const char* fmt, ...);

#else

// No-op stubs so the call sites stay identical.
inline void logInit(HMODULE) {}
inline void logf(const char*, ...) {}

#endif

} // namespace wfs