// Borderless ("windowed fullscreen") mode for TrackMania Forever, implemented as
// a Direct3D 9 proxy:
//
//   * standalone: the copy named "d3d9.dll" in the game folder (the game imports
//     Direct3DCreate9 from it, so our export is the entry point);
//   * ModLoader: loaded under our own name; installIatHook() reroutes the game's
//     Direct3DCreate9 import through us.
//
// Every video-mode change goes through two D3D9 chokepoints, both at vtable slot
// 16 (offset 0x40): CreateDevice for the first device, and Reset for every later
// switch (the Alt+Enter toggle) - so both are hooked. Each hook forces
// Windowed=TRUE at the monitor size, and the window is dressed borderless
// (WS_POPUP). The rules and traps each live with the code that implements them:
//
//   * which modes are taken over - requestsFullResolution()
//   * why Reset does not touch the window - resetHook()
//   * the borderless dressing, incl. WM_EXITSIZEMOVE - applyBorderless()
//   * keeping the game alive while unfocused - gameWndProc()
//   * re-asserting the style the game reverts - watchdogThread()
//
// The game's internal fullscreen flag (this+0x29C) is deliberately left alone.
#include "proxy.h"

#include "log.h"

#include <d3d9.h>

#include <cstdint>
#include <cstring>

// Our d3d9 export, used by the ModLoader IAT hook (installIatHook). Defined at
// the bottom of this file; forward-declared here so the hook can reference it.
extern "C" IDirect3D9* WINAPI TMWFS_Direct3DCreate9(UINT sdkVersion);

namespace wfs {
namespace proxy {

namespace {

HWND g_hwnd = nullptr;

// Whether the game's *current* mode is the monitor's full resolution. The
// watchdog only dresses the window borderless while this is set, so a game
// running at a smaller resolution (e.g. the user picked 720p and the window is
// small) is left exactly as it is - the mod must not resize it to the monitor.
bool g_applyBorderless = false;

// Whether the mode we are dressed for is a *windowed* request from the game;
// only then is the cached client size synced (see applyBorderless).
bool g_gameWindowed = false;

// ---------------------------------------------------------------------------
// Keep the game running while it is not the foreground window.
//
// TrackMania stops its update/render loop the moment its window is deactivated
// (the frame freezes until it is clicked back), which defeats a borderless
// window. While the mod is dressed (g_applyBorderless) the game window is
// subclassed and the deactivation is hidden from it, so it never learns it lost
// activation and keeps simulating; re-activation still reaches it. Because it no
// longer hears the messages that release the pointer, those are handled here
// instead. The filter is gated on g_applyBorderless, so a smaller windowed mode
// pauses on focus loss as usual.
WNDPROC g_origGameProc = nullptr;
HWND g_subclassedHwnd = nullptr;

LRESULT CALLBACK gameWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_applyBorderless) {
        switch (msg) {
            case WM_ACTIVATEAPP:
                if (wp == FALSE) {
                    ClipCursor(nullptr);
                    logf("wnd: hid WM_ACTIVATEAPP(FALSE) - keeping the game running");
                    return 0;
                }
                break;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) {
                    ClipCursor(nullptr);
                    logf("wnd: hid WM_ACTIVATE(WA_INACTIVE) - keeping the game running");
                    return 0;
                }
                break;
            case WM_SYSCOMMAND:
                if ((wp & 0xFFF0) == SC_MINIMIZE) {
                    logf("wnd: hid SC_MINIMIZE");
                    return 0;
                }
                break;
            case WM_KILLFOCUS:
                // The message the client actually pauses on; hide it and release
                // the pointer state it would normally reset.
                ClipCursor(nullptr);
                ReleaseCapture();
                logf("wnd: hid WM_KILLFOCUS - keeping the game running");
                return 0;
            case WM_SETFOCUS:
                logf("wnd: WM_SETFOCUS (forwarded)");
                break;
            default:
                break;
        }
    }
    return CallWindowProcW(g_origGameProc, hwnd, msg, wp, lp);
}

void installWindowSubclass(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd) || g_subclassedHwnd == hwnd) {
        return;
    }
    SetLastError(0);
    const LONG_PTR orig = SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                            reinterpret_cast<LONG_PTR>(&gameWndProc));
    if (orig == 0 && GetLastError() != 0) {
        logf("wnd: SetWindowLongPtr(GWLP_WNDPROC) failed (%lu)", GetLastError());
        return;
    }
    g_origGameProc = reinterpret_cast<WNDPROC>(orig);
    g_subclassedHwnd = hwnd;
    logf("wnd: subclassed game window %p (orig proc %p)", hwnd, g_origGameProc);
}

using CreateDeviceFn =
    HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
CreateDeviceFn g_realCreateDevice = nullptr;

// IDirect3DDevice9::Reset, vtable slot 16 - the chokepoint for every switch after
// the first device (see the file header). Without it the borderless override
// would not re-apply after Alt+Enter.
using ResetFn =
    HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
ResetFn g_realReset = nullptr;

// ---------------------------------------------------------------------------
// Window dressing
// ---------------------------------------------------------------------------

bool monitorRect(HWND hwnd, RECT& out) {
    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon != nullptr && GetMonitorInfoW(mon, &mi)) {
        out = mi.rcMonitor;
        return true;
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        out.left = 0;
        out.top = 0;
        out.right = static_cast<LONG>(dm.dmPelsWidth);
        out.bottom = static_cast<LONG>(dm.dmPelsHeight);
        return true;
    }
    out.left = 0;
    out.top = 0;
    out.right = GetSystemMetrics(SM_CXSCREEN);
    out.bottom = GetSystemMetrics(SM_CYSCREEN);
    return out.right > 0 && out.bottom > 0;
}

bool isBorderlessAt(HWND hwnd, const RECT& rc) {
    if (!IsWindow(hwnd)) {
        return false;
    }
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_POPUP) == 0 || (style & WS_CAPTION) != 0) {
        return false;
    }
    RECT cur{};
    if (!GetWindowRect(hwnd, &cur)) {
        return false;
    }
    return cur.left == rc.left && cur.top == rc.top &&
           (cur.right - cur.left) == (rc.right - rc.left) &&
           (cur.bottom - cur.top) == (rc.bottom - rc.top);
}

// True when the mode the game is asking for is the monitor's full resolution.
// Windowed requests pass BackBufferWidth/Height = 0 and let D3D9 size the back
// buffer from the client area, so the intended size is the client rect. Only
// such full-resolution modes get the borderless treatment; a smaller mode (the
// user's 720p window) is passed through untouched.
bool requestsFullResolution(HWND target, const D3DPRESENT_PARAMETERS& pp,
                            const RECT& monitor) {
    LONG w = static_cast<LONG>(pp.BackBufferWidth);
    LONG h = static_cast<LONG>(pp.BackBufferHeight);
    if (w <= 0 || h <= 0) {
        RECT cr{};
        if (target == nullptr || !IsWindow(target) || !GetClientRect(target, &cr)) {
            return false;
        }
        w = cr.right - cr.left;
        h = cr.bottom - cr.top;
    }
    return w == (monitor.right - monitor.left) &&
           h == (monitor.bottom - monitor.top);
}

void applyBorderless(HWND hwnd, bool gameWindowed) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    RECT rc;
    if (!monitorRect(hwnd, rc)) {
        return;
    }
    g_hwnd = hwnd;
    installWindowSubclass(hwnd);
    if (isBorderlessAt(hwnd, rc)) {
        return;  // already dressed - don't spam SetWindowPos
    }
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
               WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
    style |= WS_POPUP | WS_VISIBLE;
    ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
    ex |= WS_EX_APPWINDOW;
    SetWindowLongW(hwnd, GWL_STYLE, style);
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // TrackMania caches the windowed client size (this+0x148/this+0x14C) only in
    // its WM_EXITSIZEMOVE handler (FUN_00528b40 case 0x232), and sizes the
    // windowed back buffer from that cached value. SetWindowPos alone does not
    // update it, so the game keeps resetting toward its old (small) windowed size
    // while we force the monitor size -> a ~14/s Reset feedback loop (stutter).
    // Feeding it WM_EXITSIZEMOVE makes it adopt the monitor client size.
    //
    // Only do this while the game itself is in windowed mode. That cache is
    // exactly what the game restores on the next Alt+Enter back to windowed, so
    // updating it while we converted an exclusive-fullscreen request would make
    // the toggle come back at the monitor size and lose the player's own
    // resolution. In the fullscreen case the sizes already agree, so no sync is
    // needed to avoid the reset loop anyway.
    if (gameWindowed) {
        SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    }
}

void logPresentParams(const char* tag, HWND target, const D3DPRESENT_PARAMETERS& pp) {
    RECT wr{};
    RECT cr{};
    if (target != nullptr && IsWindow(target)) {
        GetWindowRect(target, &wr);
        GetClientRect(target, &cr);
    }
    logf("%s: Windowed=%ld %lux%lu fmt=%lu msaa=%lu/%lu swap=%lu interval=0x%08lX",
         tag, static_cast<long>(pp.Windowed),
         static_cast<unsigned long>(pp.BackBufferWidth),
         static_cast<unsigned long>(pp.BackBufferHeight),
         static_cast<unsigned long>(pp.BackBufferFormat),
         static_cast<unsigned long>(pp.MultiSampleType),
         static_cast<unsigned long>(pp.MultiSampleQuality),
         static_cast<unsigned long>(pp.SwapEffect),
         static_cast<unsigned long>(pp.PresentationInterval));
    logf("%s: window=%ld,%ld %ldx%ld  client=%ldx%ld",
         tag, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
         cr.right - cr.left, cr.bottom - cr.top);
}

// ---------------------------------------------------------------------------
// IDirect3DDevice9::Reset hook
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE resetHook(IDirect3DDevice9* self,
                                    D3DPRESENT_PARAMETERS* pp) {
    ResetFn real = g_realReset;
    if (real == nullptr || pp == nullptr) {
        return D3DERR_INVALIDCALL;
    }

    const HWND target = (pp->hDeviceWindow != nullptr) ? pp->hDeviceWindow : g_hwnd;
    D3DPRESENT_PARAMETERS patched = *pp;
    // Snapshot what the game asked for, before the real Reset gets a chance to
    // touch the struct, so the log shows the untampered request.
    const long reqWindowed = static_cast<long>(pp->Windowed);
    const unsigned long reqW = pp->BackBufferWidth;
    const unsigned long reqH = pp->BackBufferHeight;
    RECT rc{};
    const bool fullRes = monitorRect(target, rc) &&
                         requestsFullResolution(target, *pp, rc);

    if (fullRes) {
        // Force windowed at the monitor size, but do NOT touch the window here:
        // TrackMania reacquires often, and SetWindowPos/SetWindowLong inside the
        // reset caused a WM_SIZE -> viewport -> Reset feedback loop. The watchdog
        // dresses the window instead.
        patched.Windowed = TRUE;
        patched.FullScreen_RefreshRateInHz = 0;
        patched.BackBufferWidth = rc.right - rc.left;
        patched.BackBufferHeight = rc.bottom - rc.top;
        g_hwnd = target;
    }
    // Stop the watchdog re-dressing a windowed mode we are no longer managing.
    g_applyBorderless = fullRes;
    // Record the game's own mode, not the Windowed=TRUE override we apply.
    g_gameWindowed = fullRes && pp->Windowed != FALSE;

    HRESULT hr = real(self, fullRes ? &patched : pp);

    // Log only the first few resets. Per-reset logging (file open+close and
    // OutputDebugStringA) inside the present path is itself a stutter source, so
    // cap it. Every request is logged, not just the ones we take over, so it can
    // be verified that the game's own resolution survives an Alt+Enter
    // round-trip: `req` is what the game asked for, `applied` what reached D3D9.
    static int logged = 0;
    if (logged < 64) {
        ++logged;
        logf("Reset #%d t=%lu hr=0x%08lX req Windowed=%ld %lux%lu -> applied %lux%lu fullRes=%d",
             logged, static_cast<unsigned long>(GetTickCount()),
             static_cast<unsigned long>(hr), reqWindowed, reqW, reqH,
             static_cast<unsigned long>(patched.BackBufferWidth),
             static_cast<unsigned long>(patched.BackBufferHeight), fullRes ? 1 : 0);
    }
    return hr;
}

void hookDevice(IDirect3DDevice9* device) {
    if (device == nullptr) {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(device);
    if (vtable == nullptr) {
        return;
    }
    if (vtable[16] == reinterpret_cast<void*>(&resetHook)) {
        return;  // already hooked
    }
    if (g_realReset == nullptr) {
        g_realReset = reinterpret_cast<ResetFn>(vtable[16]);
        if (g_realReset == nullptr) {
            logf("hook: IDirect3DDevice9 has no Reset slot");
            return;
        }
    }
    DWORD old = 0;
    if (!VirtualProtect(&vtable[16], sizeof(void*), PAGE_READWRITE, &old)) {
        logf("hook: VirtualProtect(Reset slot) failed (%lu)", GetLastError());
        return;
    }
    vtable[16] = reinterpret_cast<void*>(&resetHook);
    VirtualProtect(&vtable[16], sizeof(void*), old, &old);
    logf("hook: IDirect3DDevice9::Reset -> %p (real %p)", &resetHook, g_realReset);
}

// ---------------------------------------------------------------------------
// IDirect3D9::CreateDevice hook
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE createDeviceHook(IDirect3D9* self, UINT adapter,
                                           D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** outDevice) {
    CreateDeviceFn real = g_realCreateDevice;
    if (real == nullptr || pp == nullptr || outDevice == nullptr) {
        return E_POINTER;
    }

    const HWND target = (pp->hDeviceWindow != nullptr) ? pp->hDeviceWindow : focus;
    D3DPRESENT_PARAMETERS patched = *pp;
    RECT rc{};
    const bool fullRes = monitorRect(target, rc) &&
                         requestsFullResolution(target, *pp, rc);

    if (fullRes) {
        // Windowed fullscreen: windowed swap chain sized to the monitor.
        patched.Windowed = TRUE;
        patched.FullScreen_RefreshRateInHz = 0;
        patched.BackBufferWidth = rc.right - rc.left;
        patched.BackBufferHeight = rc.bottom - rc.top;
    }

    logPresentParams("CreateDevice(request)", target, *pp);
    HRESULT hr = real(self, adapter, type, focus, flags, fullRes ? &patched : pp, outDevice);
    if (FAILED(hr) && fullRes) {
        // Some cards reject the desktop-sized windowed back buffer (format).
        // Degrade gracefully: first windowed at the game's own size (stretched
        // presentation), then, if that fails too, the game's original request.
        logf("CreateDevice with borderless params failed (0x%08lX), retrying windowed at the game's size",
             static_cast<unsigned long>(hr));
        D3DPRESENT_PARAMETERS windowedSize = *pp;
        windowedSize.Windowed = TRUE;
        windowedSize.FullScreen_RefreshRateInHz = 0;
        hr = real(self, adapter, type, focus, flags, &windowedSize, outDevice);
        if (FAILED(hr)) {
            logf("windowed retry failed too (0x%08lX), using the game's own parameters",
                 static_cast<unsigned long>(hr));
            hr = real(self, adapter, type, focus, flags, pp, outDevice);
        }
    }

    if (SUCCEEDED(hr) && *outDevice != nullptr) {
        // Keep hooking Reset even for non-fullscreen modes: a later Alt+Enter or
        // in-game change to the full resolution must still be picked up.
        hookDevice(*outDevice);
        g_applyBorderless = fullRes;
        g_gameWindowed = fullRes && pp->Windowed != FALSE;
        if (fullRes) {
            g_hwnd = target;
            applyBorderless(target, g_gameWindowed);
            logPresentParams("CreateDevice(applied)", target, patched);
            logf("CreateDevice OK: borderless fullscreen on %lux%lu, hr=0x%08lX",
                 static_cast<unsigned long>(patched.BackBufferWidth),
                 static_cast<unsigned long>(patched.BackBufferHeight),
                 static_cast<unsigned long>(hr));
        }
    }
    return hr;
}

void hookD3D9(IDirect3D9* d3d9) {
    if (d3d9 == nullptr) {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(d3d9);
    if (vtable == nullptr) {
        return;
    }
    if (vtable[16] == reinterpret_cast<void*>(&createDeviceHook)) {
        return;  // already hooked
    }
    if (g_realCreateDevice == nullptr) {
        g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(vtable[16]);
        if (g_realCreateDevice == nullptr) {
            logf("hook: IDirect3D9 has no CreateDevice slot");
            return;
        }
    }
    DWORD old = 0;
    if (!VirtualProtect(&vtable[16], sizeof(void*), PAGE_READWRITE, &old)) {
        logf("hook: VirtualProtect(CreateDevice slot) failed (%lu)", GetLastError());
        return;
    }
    vtable[16] = reinterpret_cast<void*>(&createDeviceHook);
    VirtualProtect(&vtable[16], sizeof(void*), old, &old);
    logf("hook: IDirect3D9::CreateDevice -> %p (real %p)", &createDeviceHook,
         g_realCreateDevice);
}

} // namespace

HMODULE systemD3D9() {
    static HMODULE module = []() -> HMODULE {
        wchar_t path[MAX_PATH] = {};
        // SysWOW64 for this 32-bit process, System32 on 32-bit Windows.
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16) {
            logf("proxy: GetSystemDirectoryW failed");
            return nullptr;
        }
        wcscat_s(path, L"\\d3d9.dll");
        HMODULE m = LoadLibraryW(path);
        if (!m) {
            logf("proxy: failed to load system d3d9.dll (%lu)", GetLastError());
        } else {
            logf("proxy: loaded system d3d9.dll from %ls", path);
        }
        return m;
    }();
    return module;
}

IDirect3D9* createHookedD3D9(UINT sdkVersion) {
    using D3D9CreateFn = IDirect3D9*(WINAPI*)(UINT);
    HMODULE module = systemD3D9();
    auto fn = module ? reinterpret_cast<D3D9CreateFn>(GetProcAddress(module, "Direct3DCreate9"))
                     : nullptr;
    if (fn == nullptr) {
        logf("proxy: no Direct3DCreate9 in system d3d9.dll");
        return nullptr;
    }
    IDirect3D9* d3d9 = fn(sdkVersion);
    hookD3D9(d3d9);
    return d3d9;
}

// ---------------------------------------------------------------------------
// ModLoader mode: reroute the game's Direct3DCreate9 import table slot.
// ---------------------------------------------------------------------------

void installIatHook() {
    HMODULE game = GetModuleHandleW(L"TmForever.exe");
    if (game == nullptr) {
        game = GetModuleHandleW(nullptr);
    }
    const BYTE* base = reinterpret_cast<const BYTE*>(game);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const IMAGE_NT_HEADERS32* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    const DWORD sizeOfImage = nt->OptionalHeader.SizeOfImage;
    const auto inRange = [&](uintptr_t p) {
        return p >= reinterpret_cast<uintptr_t>(base) &&
               p < reinterpret_cast<uintptr_t>(base) + sizeOfImage;
    };

    const IMAGE_DATA_DIRECTORY& imp =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp.VirtualAddress == 0) {
        return;
    }
    const IMAGE_IMPORT_DESCRIPTOR* desc =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imp.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        if (!inRange(reinterpret_cast<uintptr_t>(base) + desc->Name)) {
            continue;
        }
        const char* moduleName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(moduleName, "d3d9.dll") != 0) {
            continue;
        }
        const IMAGE_THUNK_DATA32* names =
            desc->OriginalFirstThunk != 0
                ? reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + desc->OriginalFirstThunk)
                : reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + desc->FirstThunk);
        // The IAT slots themselves are writable - the surrounding image data is
        // only read, so treat the module bytes as non-const for the write below.
        IMAGE_THUNK_DATA32* iat =
            reinterpret_cast<IMAGE_THUNK_DATA32*>(const_cast<BYTE*>(base) + desc->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData != 0; ++i) {
            if ((names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) != 0) {
                continue;  // ordinal import - ignore
            }
            if (!inRange(reinterpret_cast<uintptr_t>(base) + names[i].u1.AddressOfData)) {
                continue;
            }
            const IMAGE_IMPORT_BY_NAME* ibn =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), "Direct3DCreate9") == 0) {
                DWORD old = 0;
                if (VirtualProtect(&iat[i].u1.Function, sizeof(DWORD), PAGE_READWRITE, &old)) {
                    iat[i].u1.Function =
                        reinterpret_cast<DWORD>(&TMWFS_Direct3DCreate9);
                    VirtualProtect(&iat[i].u1.Function, sizeof(DWORD), old, &old);
                    logf("iat: game's Direct3DCreate9 import -> %p", &TMWFS_Direct3DCreate9);
                } else {
                    logf("iat: VirtualProtect on import slot failed (%lu)", GetLastError());
                }
                return;
            }
        }
    }
    logf("iat: d3d9 import table not found (standalone mode, export used instead)");
}

// ---------------------------------------------------------------------------
// Runtime: watchdog
// ---------------------------------------------------------------------------

DWORD WINAPI watchdogThread(LPVOID) {
    // The game resets the window style when it toggles its own
    // fullscreen/windowed mode (SC_MAXIMIZE handler) or re-applies settings.
    // Re-assert the borderless dressing every 250 ms so the window stays
    // borderless for as long as this DLL is loaded.
    for (;;) {
        Sleep(250);
        HWND hwnd = g_hwnd;
        if (g_applyBorderless && hwnd != nullptr && IsWindow(hwnd) &&
            IsWindowVisible(hwnd)) {
            applyBorderless(hwnd, g_gameWindowed);
        }
    }
}

void startThreads() {
    HANDLE t = CreateThread(nullptr, 0, watchdogThread, nullptr, 0, nullptr);
    if (t) {
        CloseHandle(t);
    }
}

} // namespace proxy
} // namespace wfs

// ---------------------------------------------------------------------------
// Exports (standalone d3d9.dll mode)
// ---------------------------------------------------------------------------

using namespace wfs;

namespace {

// Functions forwarded verbatim: resolved once, then jumped to with the caller's
// arguments (and registers) untouched.
#define WFS_FORWARDS(X)                            \
    X(D3DPERF_BeginEvent)                          \
    X(D3DPERF_EndEvent)                            \
    X(D3DPERF_GetStatus)                           \
    X(D3DPERF_QueryRepeatFrame)                    \
    X(D3DPERF_SetMarker)                           \
    X(D3DPERF_SetOptions)                          \
    X(D3DPERF_SetRegion)                           \
    X(DebugSetLevel)                               \
    X(DebugSetMute)                                \
    X(Direct3D9EnableMaximizedWindowedModeShim)    \
    X(Direct3DCreate9On12)                         \
    X(Direct3DCreate9On12Ex)                       \
    X(Direct3DShaderValidatorCreate9)              \
    X(PSGPError)                                   \
    X(PSGPSampleTexture)

#define WFS_DECLARE_POINTER(name) void* g_##name = nullptr;
WFS_FORWARDS(WFS_DECLARE_POINTER)

extern "C" void __cdecl wfsResolveForwards() {
    static bool resolved = false;
    if (resolved) {
        return;
    }
    HMODULE module = proxy::systemD3D9();
#define WFS_RESOLVE(name) \
    g_##name = module ? reinterpret_cast<void*>(GetProcAddress(module, #name)) : nullptr;
    WFS_FORWARDS(WFS_RESOLVE)
    resolved = true;
}

} // namespace

extern "C" {

IDirect3D9* WINAPI TMWFS_Direct3DCreate9(UINT sdkVersion) {
    return proxy::createHookedD3D9(sdkVersion);
}

HRESULT WINAPI TMWFS_Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out) {
    using Fn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    HMODULE module = proxy::systemD3D9();
    auto fn = module ? reinterpret_cast<Fn>(GetProcAddress(module, "Direct3DCreate9Ex")) : nullptr;
    return fn ? fn(sdkVersion, out) : D3DERR_NOTAVAILABLE;
}

// Naked stubs: preserve every register and stack argument, resolve once, then jump.
#pragma warning(push)
#pragma warning(disable : 4100)
#define WFS_DEFINE_STUB(name)                      \
    __declspec(naked) void TMWFS_##name() {        \
        __asm pushad                               \
        __asm call wfsResolveForwards              \
        __asm popad                                \
        __asm jmp dword ptr [g_##name]             \
    }
WFS_FORWARDS(WFS_DEFINE_STUB)
#pragma warning(pop)

} // extern "C"