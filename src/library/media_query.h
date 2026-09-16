#pragma once
#include <cstdint>
#include <string>
#include <vector>

class Db;

struct Match {
    int64_t id = 0;
    std::wstring label; // "artist - title" (or just title)
    std::wstring artist, title, genre;
    int64_t year = 0, bpm = 0;
    std::wstring path;
    std::string type; // audio | mp3g | video
    int64_t durMs = 0;
};

// Playable library items matching term in title/artist/path; empty term = all.
// pathPrefix restricts results to one folder subtree when non-empty.
// sortCol: 0 artist, 1 title, 2 genre, 3 year, 4 bpm, 5 duration.
std::vector<Match> searchMedia(Db& db, const std::wstring& term, int limit = 50,
                               const std::wstring& pathPrefix = L"", int sortCol = 0,
                               bool sortAsc = true);
