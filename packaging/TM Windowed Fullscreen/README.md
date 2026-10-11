# TM Windowed Fullscreen

A TrackMania Forever mod that runs the game in a **borderless ("windowed
fullscreen") window** covering the monitor: no exclusive fullscreen, no
display-mode change, instant alt-tab for the ability to resize the window to any size
and instant switch to other monitors or windows by pressing the Win key or moving the
mouse cursor to something else.

Works with **TrackMania Nations Forever** and **TrackMania United Forever**. It's
a single 32-bit DLL, usable two ways:

* **ModLoader** — install it and tick **TM Windowed Fullscreen**.
* **Standalone** — drop the same file into the game folder renamed to `d3d9.dll`.

## What it does

* **Borderless fullscreen.** When the game runs at the monitor's full resolution,
  the mod presents it in a borderless window covering the monitor - visually identical
  to exclusive fullscreen.
* **Keeps running when unfocused.** Move the mouse to a second monitor (or
  press the Win key) and the game keeps running instead of freezing on the last frame.
* **Respects a smaller resolution.** If the game is set below the monitor's
  resolution (e.g. 720p on a 1440p monitor) it is left exactly as it is — the mod
  does not stretch or resize that window. The choice survives Alt+Enter toggles.
* **If the game is on full resolution and you press Alt+Enter instead of moving the
  cursor or pressing the Win key, the game will be resized to a smaller window.**
  You can resize the window to any size.
* **Always on, no configuration.** There is no dedicated hotkey. To stop using
  it, unload the mod (see [Uninstall](#uninstall)).

Alt+Enter is TrackMania's own fullscreen toggle — not a feature of this mod.
We hook this hotkey function to fix all the issues by the default behaviour.

## Install

### ModLoader

Take the generated release folder `packaging\TM Windowed Fullscreen\` and run the
installer (from the project root or from that folder):

```powershell
powershell -ExecutionPolicy Bypass -File .\install-modloader.ps1
```

It copies the descriptors and the DLL to
`%LOCALAPPDATA%\TMLoader\database\TmForever\products\TM Windowed Fullscreen\`.
Open the ModLoader, tick **TM Windowed Fullscreen** and start the game; the
window is borderless from the first frame.

### Standalone (without ModLoader)

Copy `TMWindowedFullscreen.dll` into the game folder next to `TmForever.exe` and
rename it to **`d3d9.dll`**:

```
<game folder>\
    TmForever.exe
    d3d9.dll                   <- TMWindowedFullscreen.dll renamed
```

`d3d9.dll` is not a KnownDLL and TrackMania imports it by name, so the copy in
the game folder wins over the system one. The DLL forwards every d3d9 call to the
real system `d3d9.dll` and adds the borderless hook.

## Uninstall

Unload the mod (untick it in the ModLoader) or delete the `d3d9.dll` copy from
the game folder. No game files are touched.

## Verifying

Logging is compiled out by default. Set `WFS_LOG_ENABLED` to `1` in `src/log.h`
and recompile to enable it; `TMWindowedFullscreen.log` is then written next to
the installed DLL.

If you have placed the DLL beside the TmForever.exe, the path of the log might be:

* `C:\Users\USER\AppData\Local\VirtualStore\Program Files (x86)\TmUnitedForever`
* `C:\Users\USER\AppData\Local\VirtualStore\Program Files (x86)\TmNationsForever`

-> This is some kind of symlink in simplified terms

Useful lines:

```
CreateDevice(request): Windowed=1 0x0 ... client=1280x960
CreateDevice(applied): Windowed=1 2560x1440 ... client=2560x1440
CreateDevice OK: borderless fullscreen on 2560x1440, hr=0x00000000
Reset #1 ... req Windowed=0 2560x1440 -> applied 2560x1440 fullRes=1
Reset #2 ... req Windowed=1 1280x960  -> applied 1280x960  fullRes=0
wnd: hid WM_KILLFOCUS - keeping the game running
```

`req` is what the game asked for, `applied` what actually reached D3D9
(`fullRes=1` means the mod took over). The `Reset #` timestamps should be seconds
apart; a ~60–80 ms cadence means the `WM_EXITSIZEMOVE` sync (below) is not taking
effect and the game is fighting the mod.

## Technical details

### The two mode-change chokepoints

TrackMania Forever is a Direct3D 9 game and funnels every video-mode change
through two functions, both at vtable slot 16 (offset `+0x40`):

* `IDirect3D9::CreateDevice` — the first device; and
* `IDirect3DDevice9::Reset` — every later switch, including the Alt+Enter
  fullscreen↔windowed toggle (the game's `FUN_0095c340` calls
  `(**(code**)(*device + 0x40))(...)` when a device already exists).

Both build one inline `D3DPRESENT_PARAMETERS` (`this+0x9C0`). The mod does not
patch the game; it is a **Direct3D 9 proxy** that hooks both functions and

1. **forces `Windowed=TRUE`** at the monitor resolution when the game is asking
   for the monitor's full resolution, and passes anything smaller through
   untouched (windowed requests with `BackBufferWidth/Height = 0` are judged by
   the window's client size); then
2. **dresses the window borderless** (`WS_POPUP` covering the monitor). The game
   still thinks it is fullscreen, which keeps its own input handling intact.

Hooking only `CreateDevice` is not enough — after the first Alt+Enter the mode
change goes through `Reset`. A watchdog re-asserts the window style every 250 ms,
because the game reverts it when it re-applies settings or toggles fullscreen
(its `SC_MAXIMIZE` handler).

### Keeping the game running when it is not the foreground window

TrackMania gates its update/render loop on window activation: deactivate the
borderless window (move the mouse to a second monitor, press the Win key, or
otherwise send it to the background) and it freezes on the last frame until
clicked back. While the
mod is dressed it subclasses the game window (`SetWindowLongPtrW(GWLP_WNDPROC)`)
and hides the deactivation from it:

| message | action | why |
|---|---|---|
| `WM_ACTIVATE(WA_INACTIVE)` | swallowed | app-style deactivation |
| `WM_ACTIVATEAPP(FALSE)` | swallowed | app-style deactivation |
| `WM_KILLFOCUS` | swallowed | the message the client actually pauses on |
| `SC_MINIMIZE` | swallowed | don't minimize on focus loss |

Re-activation (`WM_SETFOCUS`, …) still reaches the game. Because the game no
longer hears the message that normally releases the pointer, the mod calls
`ClipCursor(NULL)` / `ReleaseCapture()` when it swallows. The filter is only
active while the mod is dressed, so at a smaller windowed resolution the game
pauses on focus loss as usual.

### Why the mod sends `WM_EXITSIZEMOVE`

TrackMania sizes its windowed back buffer from a cached client size
(`this+0x148`/`this+0x14C`) that it only updates in its `WM_EXITSIZEMOVE` handler
(`FUN_00528b40`, case `0x232`). A plain `SetWindowPos` fires `WM_SIZE` (a
different field) but never `WM_EXITSIZEMOVE`, so the cache would stay at the
game's old size while the mod forces the monitor size — a device-reset feedback
loop at ~14 resets/second that presents as severe stutter. The mod therefore
sends `WM_EXITSIZEMOVE` after resizing.

It only sends it while the game itself is in **windowed** mode. That cache is
also what the game restores on the next Alt+Enter *back* to windowed, so syncing
it while converting an exclusive-fullscreen request would make the toggle return
at the monitor size and discard the player's own resolution.

### Why a proxy instead of a binary patch

| | proxy (this mod) | binary patch |
|---|---|---|
| addresses | none — the game only imports `Direct3DCreate9` | needs the exact sites per build |
| builds | every Nations/United build | per-executable, breaks on updates |
| persistence | re-asserted continuously | reverts whenever the game re-applies its style |
| window focus | game's fullscreen flag untouched; window subclassed only to hide deactivation | depends on how the flag is coerced |

A binary patch would have to coax the game's internal fullscreen flag
(`this+0x29C`) into "windowed" and its present-parameter filler (`this+0x9C0`)
into windowed-at-desktop, then fight the game re-asserting its own state across
*both* `CreateDevice` and `Reset`. The proxy achieves the same visible result
between the game and the driver with zero code patches, which is why it is
build-agnostic.

### Ghidra findings (reference)

The game imports only `Direct3DCreate9` and `D3DPERF_SetOptions` from `d3d9.dll`.

| executable | `CreateDevice` | `Reset` | fullscreen flag | present params | `Windowed` |
|---|---|---|---|---|---|
| `TmForever.exe` (United) | `FUN_00960c60` | `FUN_0095c340` | `this+0x29C` | `this+0x9C0` | `this+0x9E0` |
| `TmNationsForever.exe` | `FUN_00960a40` | same | same | same | same |

`FUN_00960c60` is the app's CreateDevice method; `FUN_0095c340` calls `Reset`
whenever a device exists and only falls back to CreateDevice when there is none.
Both build the present parameters via `FUN_0095ea80`, whose windowed branch uses
the cached client size. Supporting cast: `StartFullscreen` `FUN_00960790`,
`SwitchToWindowed` `FUN_00960950`, WndProc `FUN_00528b40` (maps `SC_MAXIMIZE` to
the fullscreen toggle and caches the windowed size in its `WM_EXITSIZEMOVE`
case), config tables at `0x00b314f4` / `0x00b55a40`. None of these are used
directly — they explain why the proxy is the robust choice.

### Safety

* **No binary patches.** Only the `CreateDevice`/`Reset` vtable slots of objects
  the mod itself creates are rerouted, plus — in ModLoader mode — the process's
  import-table slot for `Direct3DCreate9`. While borderless, the game window's
  `WndProc` is subclassed to hide deactivation, always chaining to the previous
  proc via `CallWindowProc`, so the game and any other ModLoader mod still see
  every other message.
* The real device and swap chain are the system `d3d9.dll`, so rendering,
  shaders and performance are unchanged.
* If the desktop-sized windowed back buffer is rejected, the hook degrades
  gracefully: first a windowed swap chain at the game's own size, then the game's
  original parameters.

## Development

```
CMakeLists.txt                 Win32 DLL project
description.yaml               mod metadata (source)
version.yaml                   version metadata / changelog (source)
install-modloader.ps1          ModLoader installer (copied into packaging)
src/dllmain.cpp                entry point -> background thread
src/proxy.cpp/.h               D3D9 proxy: CreateDevice/Reset hooks, borderless
                               dressing, window subclass, watchdog, IAT hook,
                               d3d9 exports
src/exports.def                d3d9 export names / forwarding
src/log.cpp/.h                 logging, compiled out by default
packaging/TM Windowed Fullscreen/   GENERATED by the build (do not edit)
```

Build with Visual Studio (or Build Tools) with the C++ workload and CMake:

```powershell
cmake -B build -A Win32
cmake --build build --config Release
```

This produces `build\Release\TMWindowedFullscreen.dll` (or
`build\TMWindowedFullscreen.dll` with a single-config generator such as Ninja)
and regenerates `packaging\TM Windowed Fullscreen\` — the descriptors, README,
installer and the DLL, with the version descriptor written as `description.yaml`.
TrackMania Forever is 32-bit: a 64-bit configuration will not load and is
rejected by `CMakeLists.txt`.

## Credits

* https://github.com/cheatoskar/TM-Vibrant-Shaders - for implementing a single DLL that supports Mod Loader and the standalone method by proxying Direct3D9 calls
* https://tomashu.dev/software/tmloader/ for The Mod Loader. An OpenMod might be built by me, as I don't like closed-source software that is embedded in .exe files

Shoutout to the following projects that helped me out for another project of mine:

* https://github.com/pixeltris/ModTMNF for the dumps of TMNF and it's capability to interact with TrackMania on a low level
* https://github.com/BigBang1112/gbx-net - for the inner working of GBX files
* https://explorer.gbx.tools/ and it's built-in IDE
