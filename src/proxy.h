#pragma once

#include <windows.h>

struct IDirect3D9;

namespace wfs {
namespace proxy {

// The system's d3d9.dll, loaded by full path (in standalone mode this DLL itself
// is named "d3d9.dll", so LoadLibrary("d3d9.dll") would resolve back to us).
HMODULE systemD3D9();

// Creates the real IDirect3D9 object and reroutes its CreateDevice vtable slot
// (slot 16, +0x40) to our borderless hook.
IDirect3D9* createHookedD3D9(UINT sdkVersion);

// ModLoader mode: the DLL is loaded under its own name, so the game still talks
// to the system d3d9.dll. This patches the game's import table entry for
// d3d9.Direct3DCreate9 so it goes through us as well. Safe no-op in standalone
// mode (the import already resolves to our export).
void installIatHook();

// Background thread (watchdog re-asserting the borderless style).
void startThreads();

} // namespace proxy
} // namespace wfs