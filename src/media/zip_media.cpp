#include "media/zip_media.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "media/media_paths.h"
#include "miniz.h"

// Extracted entries get fixed names (audio.<ext> / lyrics.cdg) — this
// sidesteps zip-internal filename encodings (CP437 vs UTF-8) entirely.
static const wchar_t* kAudioExtW[] = {L".mp3", L".wav", L".m4a", L".flac", L".ogg"};
static const char* kAudioExtA[] = {".mp3", ".wav", ".m4a", ".flac", ".ogg"};

static bool extLike(const char* name, const char* ext) {
    const size_t n = strlen(name), e = strlen(ext);
    return n >= e && _stricmp(name + n - e, ext) == 0;
}

static std::wstring cacheDirFor(const std::wstring& zipPath, uint64_t size) {
    uint64_t h = 1469598103934665603ull; // FNV-1a over path chars + size, so a
    for (wchar_t c : zipPath) {          // replaced zip re-extracts
        h ^= uint64_t(c);
        h *= 1099511628211ull;
    }
    h ^= size;
    h *= 1099511628211ull;
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = std::wstring(buf) + L"\\KaraokeDJ";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\zipcache";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t hex[24];
    swprintf(hex, 24, L"\\%016llx", (unsigned long long)h);
    dir += hex;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
// ponytail: the cache only grows (a zip is a few MB, a long gig some hundreds);
// an LRU sweep at startup is the upgrade if disk ever matters.

bool extractKaraokeZip(const std::wstring& zipPath, std::wstring& audio,
                       std::wstring& cdgPath) {
    audio.clear();
    cdgPath.clear();
    HANDLE f = CreateFileW(zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    const std::wstring dir = cacheDirFor(zipPath, uint64_t(sz.QuadPart));
    const std::wstring cachedCdg = dir + L"\\lyrics.cdg";

    for (const wchar_t* e : kAudioExtW) { // cached from an earlier load?
        const std::wstring cand = dir + L"\\audio" + e;
        if (fileExists(cand)) {
            CloseHandle(f);
            audio = cand;
            if (fileExists(cachedCdg)) cdgPath = cachedCdg;
            return true;
        }
    }

    // Karaoke zips are a few MB: read whole file (wide-path safe), unzip from
    // memory — miniz's own file API is narrow fopen and would mangle accents.
    if (sz.QuadPart <= 0 || sz.QuadPart > (256ll << 20)) {
        CloseHandle(f);
        return false;
    }
    std::vector<uint8_t> data(size_t(sz.QuadPart));
    DWORD rd = 0;
    const bool readOk =
        ReadFile(f, data.data(), DWORD(data.size()), &rd, nullptr) && rd == data.size();
    CloseHandle(f);
    if (!readOk) return false;

    mz_zip_archive za{};
    if (!mz_zip_reader_init_mem(&za, data.data(), data.size(), 0)) return false;

    auto writeEntry = [&](mz_uint idx, const std::wstring& outPath) -> bool {
        size_t n = 0;
        void* p = mz_zip_reader_extract_to_heap(&za, idx, &n, 0);
        if (!p) return false;
        HANDLE o = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool ok = false;
        if (o != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ok = WriteFile(o, p, DWORD(n), &written, nullptr) && written == n;
            CloseHandle(o);
        }
        mz_free(p);
        return ok;
    };

    const mz_uint count = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < count && (audio.empty() || cdgPath.empty()); ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory) continue;
        if (cdgPath.empty() && extLike(st.m_filename, ".cdg")) {
            if (writeEntry(i, cachedCdg)) cdgPath = cachedCdg;
        } else if (audio.empty()) {
            for (size_t e = 0; e < 5; ++e) {
                if (!extLike(st.m_filename, kAudioExtA[e])) continue;
                const std::wstring out = dir + L"\\audio" + kAudioExtW[e];
                if (writeEntry(i, out)) audio = out;
                break;
            }
        }
    }
    mz_zip_reader_end(&za);
    return !audio.empty(); // a zip without .cdg still plays as audio
}
