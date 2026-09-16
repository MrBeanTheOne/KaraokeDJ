#pragma once
#include <atomic>
#include <string>

class Db;

struct ScanStats {
    int seen = 0, added = 0, updated = 0, unchanged = 0, unsupported = 0;
};

// Live progress for a running scan (poll from another thread). Set `cancel`
// to stop the scan promptly: work already committed stays, and a later scan
// of the same folder fast-skips it and continues where this one stopped.
struct ScanProgress {
    std::atomic<int> phase{0};  // 0 = discovering files, 1 = reading tags
    std::atomic<int> walked{0}; // media files discovered so far
    std::atomic<int> total{0};  // files that need tag reads (new/changed)
    std::atomic<int> done{0};   // tag reads finished
    std::atomic<bool> cancel{false};
    // While true, the walk and tag-read threads run in OS background mode so
    // a mid-gig import never competes with playback. The app keeps this
    // synced to "a deck is on air"; import runs full speed otherwise.
    std::atomic<bool> gentle{false};

    void reset() {
        phase = walked = total = done = 0;
        cancel = false;
        gentle = false;
    }
};

// Recursively scans root, classifies media per plan §7 and upserts into
// media_item. Unchanged files (same size + mtime) are skipped fast. Tag
// reading is parallel (it is the dominant cost on external drives), CDG
// pairing is resolved in memory from the walk itself (no extra disk hits),
// and commits happen in chunks so a crash keeps most of a long import.
// readFileTags=false: skip the per-file tag reads entirely (fastest possible
// import — titles come from filenames, durations show 0:00 until a rescan).
ScanStats scanDirectory(Db& db, const std::wstring& root,
                        ScanProgress* prog = nullptr, bool readFileTags = true);
