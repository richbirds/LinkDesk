#pragma once
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Start only after an explicit, confirmed process exit. Normal cleanup should
// finish first; a stuck OS capture/codec call must not leave an invisible owner
// of the device identity indefinitely. Never terminate individual Qt threads.
inline void startExitDeadline() {
    static std::atomic_bool started{false};
    if(started.exchange(true)) return;
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(8));
        #ifdef Q_OS_WIN
        // ExitProcess/CRT detach can themselves stall in a broken DLL teardown.
        // This fallback terminates the entire already-stopping process only.
        TerminateProcess(GetCurrentProcess(),70);
#else
        std::_Exit(70);
#endif
    }).detach();
}
