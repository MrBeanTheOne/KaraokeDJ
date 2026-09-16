#pragma once
#include <windows.h>

#include <string>

inline bool fileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

inline std::wstring replaceExt(const std::wstring& p, const wchar_t* ext) {
    const size_t dot = p.find_last_of(L'.');
    return (dot == std::wstring::npos ? p : p.substr(0, dot)) + ext;
}

inline bool hasExt(const std::wstring& p, const wchar_t* ext) {
    const size_t dot = p.find_last_of(L'.');
    return dot != std::wstring::npos && _wcsicmp(p.c_str() + dot, ext) == 0;
}

// Karaoke ZIP (FR-006): defined in zip_media.cpp; extracts into the managed
// cache and returns the audio/.cdg paths inside it.
bool extractKaraokeZip(const std::wstring& zipPath, std::wstring& audio,
                       std::wstring& cdgPath);

// MP3+G pairing: given any media path, resolve the audio file to load and the
// .cdg to render (empty when none). Returns false when no audio exists.
// A .zip resolves through the extraction cache.
inline bool resolveMedia(const std::wstring& given, std::wstring& audio,
                         std::wstring& cdgPath) {
    audio.clear();
    cdgPath.clear();
    if (hasExt(given, L".zip")) return extractKaraokeZip(given, audio, cdgPath);
    if (hasExt(given, L".cdg")) {
        cdgPath = given;
        for (const wchar_t* e : {L".mp3", L".wav", L".m4a", L".flac", L".ogg"}) {
            std::wstring cand = replaceExt(given, e);
            if (fileExists(cand)) { audio = cand; return true; }
        }
        return false;
    }
    audio = given;
    std::wstring sib = replaceExt(given, L".cdg");
    if (fileExists(sib)) cdgPath = sib;
    return true;
}
