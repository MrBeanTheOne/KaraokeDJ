// Encodes tests/media-fixtures/test_video.mp4: 8 s of 640x360 H.264 with a
// moving white block over color bands plus an AAC tone that beeps once per
// second, so motion, audio and A/V sync are all verifiable by eye and ear.
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static constexpr UINT32 W = 640, H = 360, FPS = 30, SECS = 8;
static constexpr LONGLONG FRAME_HNS = 10000000LL / FPS;
static constexpr UINT32 ARATE = 48000, ACH = 2;

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = argc > 1 ? argv[1] : L"tests/media-fixtures/test_video.mp4";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 1;

    IMFSinkWriter* sw = nullptr;
    if (FAILED(MFCreateSinkWriterFromURL(path, nullptr, nullptr, &sw))) {
        wprintf(L"cannot create %ls\n", path);
        return 1;
    }

    IMFMediaType* outT = nullptr;
    MFCreateMediaType(&outT);
    outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outT->SetUINT32(MF_MT_AVG_BITRATE, 1500000);
    outT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outT, MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(outT, MF_MT_FRAME_RATE, FPS, 1);
    MFSetAttributeRatio(outT, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    HRESULT hr = sw->AddStream(outT, &stream);
    outT->Release();
    if (FAILED(hr)) { wprintf(L"AddStream failed\n"); return 1; }

    IMFMediaType* inT = nullptr;
    MFCreateMediaType(&inT);
    inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inT, MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(inT, MF_MT_FRAME_RATE, FPS, 1);
    hr = sw->SetInputMediaType(stream, inT, nullptr);
    inT->Release();
    if (FAILED(hr)) { wprintf(L"SetInputMediaType failed (no H264 encoder?)\n"); return 1; }

    IMFMediaType* aOutT = nullptr;
    MFCreateMediaType(&aOutT);
    aOutT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    aOutT->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    aOutT->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, ARATE);
    aOutT->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ACH);
    aOutT->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    aOutT->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000); // ~128 kbps AAC
    DWORD aStream = 0;
    hr = sw->AddStream(aOutT, &aStream);
    aOutT->Release();
    if (FAILED(hr)) { wprintf(L"audio AddStream failed\n"); return 1; }

    IMFMediaType* aInT = nullptr;
    MFCreateMediaType(&aInT);
    aInT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    aInT->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    aInT->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, ARATE);
    aInT->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ACH);
    aInT->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    aInT->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ACH * 2);
    aInT->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, ARATE * ACH * 2);
    hr = sw->SetInputMediaType(aStream, aInT, nullptr);
    aInT->Release();
    if (FAILED(hr)) { wprintf(L"audio SetInputMediaType failed (no AAC encoder?)\n"); return 1; }

    if (FAILED(sw->BeginWriting())) { wprintf(L"BeginWriting failed\n"); return 1; }

    { // Whole tone track in one sample: quiet 220 Hz with a loud 880 Hz beep
      // during the first 150 ms of every second.
        const UINT32 total = ARATE * SECS;
        std::vector<int16_t> pcm(size_t(total) * ACH);
        for (UINT32 i = 0; i < total; ++i) {
            const bool beep = (i % ARATE) < ARATE * 3 / 20;
            const double f = beep ? 880.0 : 220.0;
            const double amp = beep ? 0.4 : 0.08;
            const int16_t v = int16_t(amp * 32767.0 * sin(6.2831853 * f * i / ARATE));
            pcm[size_t(i) * ACH] = v;
            pcm[size_t(i) * ACH + 1] = v;
        }
        const UINT32 bytes = UINT32(pcm.size() * 2);
        IMFMediaBuffer* buf = nullptr;
        MFCreateMemoryBuffer(bytes, &buf);
        BYTE* dst = nullptr;
        buf->Lock(&dst, nullptr, nullptr);
        std::memcpy(dst, pcm.data(), bytes);
        buf->Unlock();
        buf->SetCurrentLength(bytes);
        IMFSample* s = nullptr;
        MFCreateSample(&s);
        s->AddBuffer(buf);
        s->SetSampleTime(0);
        s->SetSampleDuration(LONGLONG(SECS) * 10000000LL);
        hr = sw->WriteSample(aStream, s);
        s->Release();
        buf->Release();
        if (FAILED(hr)) { wprintf(L"audio WriteSample failed\n"); return 1; }
    }

    std::vector<uint8_t> px(size_t(W) * H * 4);
    for (UINT32 i = 0; i < FPS * SECS; ++i) {
        for (UINT32 y = 0; y < H; ++y) {
            for (UINT32 x = 0; x < W; ++x) {
                uint8_t* p = &px[(size_t(y) * W + x) * 4];
                p[0] = uint8_t((x * 255) / W);            // B ramp
                p[1] = uint8_t((y * 255) / H);            // G ramp
                p[2] = uint8_t((i * 4) & 0xFF);           // R shifts per frame
                p[3] = 0;
            }
        }
        const UINT32 bx = (i * (W - 60)) / (FPS * SECS); // moving white block
        for (UINT32 y = H / 2 - 30; y < H / 2 + 30; ++y)
            std::memset(&px[(size_t(y) * W + bx) * 4], 0xFF, 60 * 4);

        IMFMediaBuffer* buf = nullptr;
        MFCreateMemoryBuffer(UINT32(px.size()), &buf);
        BYTE* dst = nullptr;
        buf->Lock(&dst, nullptr, nullptr);
        std::memcpy(dst, px.data(), px.size());
        buf->Unlock();
        buf->SetCurrentLength(UINT32(px.size()));

        IMFSample* s = nullptr;
        MFCreateSample(&s);
        s->AddBuffer(buf);
        s->SetSampleTime(LONGLONG(i) * FRAME_HNS);
        s->SetSampleDuration(FRAME_HNS);
        hr = sw->WriteSample(stream, s);
        s->Release();
        buf->Release();
        if (FAILED(hr)) { wprintf(L"WriteSample failed at %u\n", i); return 1; }
    }
    sw->Finalize();
    sw->Release();
    MFShutdown();
    wprintf(L"wrote %ls\n", path);
    return 0;
}
