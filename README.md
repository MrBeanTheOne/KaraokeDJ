<div align="center">

# 🎤 Karaoke DJ

**A professional two-deck karaoke & video DJ app for Windows — built to run a
full night from a regular laptop, no dedicated GPU required.**

[![Download](https://img.shields.io/github/v/release/MrBeanTheOne/KaraokeDJ?label=download&color=d22c36)](https://github.com/MrBeanTheOne/KaraokeDJ/releases/latest)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue)
![Language](https://img.shields.io/badge/C%2B%2B-20-red)
![Dependencies](https://img.shields.io/badge/runtime%20deps-none-4ade80)

<img src="docs/app.png" alt="Karaoke DJ" width="900">

</div>

## 📥 Install

Grab **`KaraokeDJ-<version>-win64.exe`** from the
[**latest release**](https://github.com/MrBeanTheOne/KaraokeDJ/releases/latest)
and run it. The installer is fully self-contained — yt-dlp and ffmpeg are
bundled, no runtimes to install — and cleanly replaces any older version.
Your library lives in `%APPDATA%\KaraokeDJ\` and survives every upgrade.
The app checks GitHub for newer releases on launch (and on demand in
Settings) and points you at the download when one exists.

## ✨ What it does

### 🎛️ Two decks, zero babysitting
- Automix with configurable fade length, or mix by hand — crossfader fills
  blue (deck A) / red (deck B) from center.
- **Hardware video decode** (Media Foundation + D3D11): smooth video karaoke
  on a weak laptop CPU.
- Plays **MP3+G** (mp3/cdg pairs), **karaoke ZIPs**, **video files**, plain
  audio — and **YouTube links** pasted straight into the search box.
- **Start/end markers** per song: drag the handles, or let smart automix skip
  silent intros and outros. Saved per track.
- **Auto volume leveling** — loudness is measured per track and matched, so
  nobody gets blasted between songs.
- **BPM everywhere**: read from tags on import, detected in the background
  for everything else.

### 📚 A library that keeps up
- Fast parallel import with live progress; closing mid-import is safe, and a
  mid-gig import drops to background priority so playback always wins.
- Built for **big collections**: 100k+ track libraries browse and search
  smoothly (pre-folded search text, debounced queries).
- **Stays in sync**: right-click a folder to rescan it, one button rescans
  everything, and an optional watcher imports new files by itself seconds
  after they land on disk.
- Accent- and case-insensitive search ("demo" finds "DÉMO").
- Sortable, resizable, reorderable columns — right-click the header to choose
  what shows.
- Playlists with drag-reorder, played-tonight dots, multi-select drag to
  queue.
- **Tag editor**: right-click any track to fix artist/title/genre/year — in
  the library only, or written straight into the file's real tags.

### 🖼️ Design your waiting screen
- Between songs the output shows a screen **you lay out in Settings**: a
  background image (auto-dimmed so text stays readable), logo (transparency
  kept), title, a free message line, "next up", the upcoming singer rotation,
  and the request QR.
- Every element gets a show/hide toggle, a 3×3 position grid and three
  sizes, with a **live preview** right in Settings — and the layout is
  proportional, so a cheap 720p bar TV shows the same design as a 4K panel.
- The whole designer collapses to one line when you're done.

### 🎤 Running the night
- **Singer rotation** with per-singer history — reorder by drag, right-click
  to clear for a new night, search any singer to see everything they've sung.
- **Session snapshots**: if the laptop dies mid-gig, relaunching restores the
  night exactly where it stopped.
- **Audio device recovery**: pulling the USB interface mid-song recovers
  without a restart.
- **Second-screen video output** with letterbox / fill / stretch fit modes.

### 📱 Phone requests <sub>(optional, off by default)</sub>

<img src="docs/waiting-screen.png" alt="Waiting screen with request QR" width="700">

- Singers scan a **QR code on the waiting screen** and get a mobile page to
  search the library and request songs under their name.
- Requests land in a **REQUESTS inbox** — you approve into the rotation or
  reject. Per-phone throttling keeps pranksters out.
- Only **singable material** is offered (CDG songs, karaoke ZIPs, tracks with
  "karaoke" in the name) — music videos stay yours.
- Optional **password**: phones enter it once and remember it; change it
  mid-night and everyone is asked again.
- Strictly isolated: when off, no server, thread, or port exists. Only song
  metadata is ever served.

## 🔨 Building from source

Requirements: Visual Studio 2022+ (MSVC x64), CMake 3.21+.

```
cmake -B build
cmake --build build --config Release
build\Release\test_core.exe        # sanity tests
```

The installer additionally expects `redist\yt-dlp.exe` and `redist\ffmpeg.exe`
(not committed — drop in current builds), then:

```
cd build && cpack -C Release
```

SQLite, miniz, cpp-httplib and qrcodegen are vendored (`external/`);
everything else is Windows SDK (Media Foundation, WASAPI,
Direct2D/DirectWrite, Winsock).

## 🗂️ Source layout

```
src/app/       app state, UI drawing, engine tick, menus, data/db glue
src/ui/        immediate-mode Direct2D widget kit
src/audio/     WASAPI output + device recovery
src/media/     Media Foundation decode, waveform/loudness/BPM analysis
src/playback/  deck engine (transport, mixing, markers)
src/karaoke/   CDG rendering + karaoke ZIP handling
src/video/     video windows (deck previews + fullscreen output)
src/web/       optional phone-request server (embedded page + JSON API)
src/library/   SQLite library, scanner/import, search
```
