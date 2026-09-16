#include "audio/wasapi_out.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <chrono>
#include <cstring>

#include "playback/mixer.h"

using namespace std::chrono_literals;

std::vector<std::pair<std::wstring, std::wstring>> listAudioOutputs() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)))
        return out;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            LPWSTR id = nullptr;
            IPropertyStore* ps = nullptr;
            std::wstring name = L"(unknown)";
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) &&
                    v.vt == VT_LPWSTR)
                    name = v.pwszVal;
                PropVariantClear(&v);
                ps->Release();
            }
            if (SUCCEEDED(d->GetId(&id))) {
                out.push_back({id, name});
                CoTaskMemFree(id);
            }
            d->Release();
        }
        col->Release();
    }
    en->Release();
    return out;
}

namespace {
// Minimal IMMNotificationClient: flags the device changes that matter to the
// render loop — the default output moving (when following the default), or
// the pinned endpoint changing state (so we can return to it when it comes
// back). Lives on the render thread's stack, so refcounting is a formality.
struct DevNotify : IMMNotificationClient {
    std::atomic<bool>* changed = nullptr;
    const wchar_t* pinnedId = nullptr; // empty/null = following the default
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR) override {
        if (flow == eRender && role == eConsole && (!pinnedId || !pinnedId[0]))
            changed->store(true); // pinned outputs ignore default moves
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD) override {
        if (pinnedId && pinnedId[0] && id && _wcsicmp(id, pinnedId) == 0)
            changed->store(true); // pinned device (re)appeared or vanished
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,
                                                     const PROPERTYKEY) override {
        return S_OK;
    }
};
} // namespace

bool WasapiOut::start(uint32_t rate, uint32_t ch, Mixer& mixer,
                      const std::wstring& deviceId) {
    stop(); // restart-safe: settings changes reopen on the new endpoint
    deviceId_ = deviceId;
    status_.store(0);
    quit_.store(false);
    th_ = std::thread(&WasapiOut::threadMain, this, rate, ch, &mixer);
    while (status_.load() == 0) std::this_thread::sleep_for(10ms);
    return status_.load() == 1;
}

void WasapiOut::stop() {
    quit_.store(true);
    if (th_.joinable()) th_.join();
}

bool WasapiOut::runDevice(uint32_t rate, uint32_t ch, Mixer* mixer,
                          IMMDeviceEnumerator* en, std::atomic<bool>& devChanged) {
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr;
    HANDLE evt = nullptr;
    HANDLE mmcss = nullptr;
    bool started = false;

    do {
        // Pinned endpoint first; fall back to the default while it is absent
        // (the state-change notification brings us back when it returns).
        if (!deviceId_.empty()) en->GetDevice(deviceId_.c_str(), &dev);
        if (!dev && FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)))
            break;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 (void**)&ac)))
            break;

        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = WORD(ch);
        fmt.nSamplesPerSec = rate;
        fmt.wBitsPerSample = 32;
        fmt.nBlockAlign = WORD(ch * 4);
        fmt.nAvgBytesPerSec = rate * fmt.nBlockAlign;

        if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  0, 0, &fmt, nullptr)))
            break;

        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt || FAILED(ac->SetEventHandle(evt))) break;

        UINT32 bufFrames = 0;
        if (FAILED(ac->GetBufferSize(&bufFrames))) break;
        mixer->prepare(size_t(bufFrames) * ch);

        if (FAILED(ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc))) break;

        BYTE* p = nullptr; // prefill with silence
        if (SUCCEEDED(rc->GetBuffer(bufFrames, &p))) {
            std::memset(p, 0, size_t(bufFrames) * fmt.nBlockAlign);
            rc->ReleaseBuffer(bufFrames, 0);
        }

        DWORD taskIdx = 0;
        mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

        if (FAILED(ac->Start())) break;
        started = true;
        status_.store(1);

        while (!quit_.load(std::memory_order_relaxed) && !devChanged.load()) {
            if (WaitForSingleObject(evt, 2000) != WAIT_OBJECT_0) continue;
            UINT32 pad = 0;
            if (FAILED(ac->GetCurrentPadding(&pad))) break; // device invalidated
            const UINT32 frames = bufFrames - pad;
            if (!frames) continue;
            BYTE* data = nullptr;
            if (FAILED(rc->GetBuffer(frames, &data))) break;
            mixer->render(reinterpret_cast<float*>(data), frames, ch);
            rc->ReleaseBuffer(frames, 0);
        }
        ac->Stop();
    } while (false);

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (rc) rc->Release();
    if (ac) ac->Release();
    if (dev) dev->Release();
    if (evt) CloseHandle(evt);
    return started;
}

void WasapiOut::threadMain(uint32_t rate, uint32_t ch, Mixer* mixer) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en))) {
        status_.store(-1);
        CoUninitialize();
        return;
    }
    std::atomic<bool> devChanged{false};
    DevNotify notify;
    notify.changed = &devChanged;
    notify.pinnedId = deviceId_.c_str();
    en->RegisterEndpointNotificationCallback(&notify);

    while (!quit_.load()) {
        devChanged.store(false);
        const bool ran = runDevice(rate, ch, mixer, en, devChanged);
        if (status_.load() == 0 && !ran) {
            status_.store(-1); // no device at launch: start() reports failure
            break;
        }
        if (quit_.load()) break;
        // Default-device switches reopen immediately; failures (unplug, driver
        // reset) poll for a usable device.
        // ponytail: fixed 500 ms retry — decks keep decoding, playback resumes
        // from where the ring got to.
        if (!devChanged.load()) std::this_thread::sleep_for(500ms);
    }

    en->UnregisterEndpointNotificationCallback(&notify);
    en->Release();
    CoUninitialize();
}
