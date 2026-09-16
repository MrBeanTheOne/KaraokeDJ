#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class Mixer;
struct IMMDeviceEnumerator;

// Active render endpoints as {device id, friendly name} — for the settings UI.
std::vector<std::pair<std::wstring, std::wstring>> listAudioOutputs();

// Shared-mode event-driven WASAPI render loop on its own MMCSS "Pro Audio"
// thread. AUTOCONVERTPCM lets us always run float/rate/ch of our choosing.
// Device recovery (FR-011): the thread reopens after a device failure or a
// relevant device change, so playback survives unplugs. With an empty
// deviceId it follows the Windows default output; with a pinned id it opens
// that endpoint (falling back to the default while the device is absent, and
// returning to it when it comes back).
class WasapiOut {
public:
    bool start(uint32_t rate, uint32_t channels, Mixer& mixer,
               const std::wstring& deviceId = L""); // blocks until running or failed
    void stop();
    ~WasapiOut() { stop(); }

private:
    void threadMain(uint32_t rate, uint32_t ch, Mixer* mixer);
    // One device session: render until quit, failure, or a device change.
    // Returns false when setup never reached Start().
    bool runDevice(uint32_t rate, uint32_t ch, Mixer* mixer, IMMDeviceEnumerator* en,
                   std::atomic<bool>& devChanged);

    std::wstring deviceId_; // empty = follow the system default
    std::atomic<int> status_{0}; // 0 pending, 1 running, -1 failed
    std::atomic<bool> quit_{false};
    std::thread th_;
};
