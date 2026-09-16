#include "media/mf_decoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

bool MFDecoder::initMF() {
    return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}

bool MFDecoder::open(const std::wstring& path, uint32_t rate, uint32_t ch) {
    close();

    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 1);
    // Despite the name, this enables format conversion (resample, bit depth) for audio too.
    if (attrs) attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attrs, &reader_);
    if (attrs) attrs->Release();
    if (FAILED(hr)) { reader_ = nullptr; return false; }

    reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t))) { close(); return false; }
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * 4);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * ch * 4);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t);
    t->Release();
    if (FAILED(hr)) { close(); return false; }
    return true;
}

bool MFDecoder::readChunk(std::vector<float>& out) {
    if (!reader_) return false;
    DWORD flags = 0;
    IMFSample* sample = nullptr;
    HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                     nullptr, &flags, nullptr, &sample);
    if (FAILED(hr)) return false;
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        if (sample) sample->Release();
        return false;
    }
    if (!sample) return true; // stream tick, no data this call

    IMFMediaBuffer* buf = nullptr;
    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
        BYTE* data = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
            const float* f = reinterpret_cast<const float*>(data);
            out.insert(out.end(), f, f + len / sizeof(float));
            buf->Unlock();
        }
        buf->Release();
    }
    sample->Release();
    return true;
}

uint64_t MFDecoder::durationFrames(uint32_t rate) const {
    if (!reader_) return 0;
    PROPVARIANT var{};
    if (FAILED(reader_->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE),
                                                 MF_PD_DURATION, &var)))
        return 0;
    const uint64_t hns = var.uhVal.QuadPart; // 100 ns units
    PropVariantClear(&var);
    return hns * rate / 10000000ull;
}

void MFDecoder::seekTo(int64_t hns) {
    if (!reader_) return;
    PROPVARIANT var{};
    var.vt = VT_I8;
    var.hVal.QuadPart = hns;
    reader_->SetCurrentPosition(GUID_NULL, var);
}

void MFDecoder::close() {
    if (reader_) { reader_->Release(); reader_ = nullptr; }
}
