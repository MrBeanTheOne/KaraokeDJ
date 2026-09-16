#pragma once
#include <string>

// Karaoke ZIP support (FR-006): extract the audio track (and .cdg when
// present) from zipPath into the managed cache under
// %APPDATA%\KaraokeDJ\zipcache\<hash-of-path+size>, and return the extracted
// file paths. Extraction happens once — later calls for the same zip return
// the cached files instantly. False when the zip is unreadable or contains no
// audio entry.
bool extractKaraokeZip(const std::wstring& zipPath, std::wstring& audio,
                       std::wstring& cdgPath);
