#pragma once
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

// Start only after an explicit, confirmed process exit. Normal cleanup should
// finish first; a stuck OS capture/codec call must not leave an invisible owner
// of the device identity indefinitely. Never terminate individual Qt threads.
inline void startExitDeadline() {
    static std::atomic_bool started{false};
    if(started.exchange(true)) return;
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(8));
        std::_Exit(70); // Distinguish fallback termination from a clean exit.
    }).detach();
}
