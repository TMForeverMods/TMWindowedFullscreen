#include <windows.h>

#include "log.h"
#include "proxy.h"

namespace {

// Runs on its own thread so no work happens under the loader lock in DllMain.
DWORD WINAPI boot(LPVOID) {
    wfs::logf("boot: TMWindowedFullscreen starting");

    // ModLoader mode: the game talks to the system d3d9.dll, so reroute its
    // Direct3DCreate9 import through us. No-op in standalone mode, where the
    // import already resolves to our export.
    wfs::proxy::installIatHook();

    // Watchdog thread re-asserting the borderless style if the game resets it.
    wfs::proxy::startThreads();
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            wfs::logInit(module);

            HANDLE thread = CreateThread(nullptr, 0, boot, module, 0, nullptr);
            if (thread) {
                CloseHandle(thread);
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}