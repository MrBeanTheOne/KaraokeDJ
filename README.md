# Karaoke DJ

A professional two-deck karaoke / video DJ app for Windows, built to run a
full night from a regular laptop — no dedicated GPU required. Dark
VirtualDJ-style interface, singer rotation management, and a second-screen
video output for the crowd.

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue)
![Language](https://img.shields.io/badge/C%2B%2B-20-red)
![License](https://img.shields.io/badge/license-private-lightgrey)

## Features

### Playback
- **Two decks + crossfader** — automix with configurable fade length, or mix
  by hand. The crossfader fills blue (deck A) / red (deck B) from center.
- **Hardware video decode** (Media Foundation + D3D11) — smooth video
  karaoke even on a weak laptop CPU.
- **Formats**: MP3+G (mp3/cdg pairs), karaoke ZIPs, video files (MP4 etc.),
  plain audio.
- **YouTube links** — paste a URL into the search box and press Enter; the
  track downloads and drops into the queue (yt-dlp + ffmpeg bundled).
- **Start/end markers** per song — set them manually (drag the handles above
  the waveform) or let smart automix skip silent intros/outros. Saved per
  track.
- **Auto volume leveling** — track loudness is measured during the waveform
  scan and the gain is matched so singers don't get blasted between songs.
- **BPM detection** — read from tags at import when present, otherwise
  detected in the background (onset autocorrelation) and filled into the
  browser as the analyzer works through the library.

### Library
- **Fast parallel import** with live progress; closing mid-import is safe and
  a rescan continues where it stopped. Importing mid-gig is fine too: while
  a deck is on air the scan threads drop to background priority so playback
  always wins.
- **Accent- and case-insensitive search** ("demo" finds "DÉMO").
- **Sortable, reorderable columns** — drag dividers to resize, right-click
  the header to reorder or hide columns (Title / Artist / Genre / Year /
  BPM / Time).
- **Playlists** — create in-app, drag to reorder, queue all with one
  right-click. Deleting a playlist never touches queued songs.
- **Artist/title swap** for mis-tagged files, played-tonight markers
  (red dot), zebra striping, multi-select (shift-click) drag to queue.

### Phone requests (optional)
- Flip **Allow phone requests** in Settings and singers on the venue Wi-Fi
  (or the laptop's mobile hotspot) get a mobile page to search the library
  and request songs under their name — a QR code appears on the waiting
  screen between songs.
- Phones only see **singable material**: CDG songs (mp3+cdg / karaoke ZIPs)
  and tracks with "karaoke" in the title or filename. Plain music videos and
  regular audio never show up — those stay the DJ's.
- Requests land in a **REQUESTS** inbox in the header; the DJ approves each
  one into the singer rotation or rejects it. Per-phone throttling keeps
  pranksters out.
- Optional **password**: set one in Settings and phones must enter it once
  (each phone remembers it afterwards). Change it mid-night and everyone is
  asked again. Blank = open.
- Strictly opt-in and fully isolated: off by default, and when off no server,
  thread, or port exists. Only song metadata is ever served.

### Running the night
- **Singer rotation** — right-click any track to add it under a singer;
  reorder the rotation by dragging; completed / skipped / no-show entries
  move to a per-singer history. Right-click clears the rotation or history
  for a new night.
- **Singer search** — look up any singer to see their upcoming songs and
  full history.
- **Tonight's history** — everything that hit the air, in order.
- **Session snapshots** — the session is checkpointed continuously; if the
  app (or the laptop) dies mid-gig, relaunching restores the night.
- **Audio device recovery** — pulling the USB interface mid-song recovers to
  the pinned or default device without a restart.
- **Second-screen output** — full-screen video window on a chosen monitor
  with fit modes (letterbox / fill / stretch), configured in Settings.

## Installation

Grab the latest `KaraokeDJ-<version>-win64.exe` installer from
[Releases](https://github.com/MrBeanTheOne/KaraokeDJ/releases). It is fully
self-contained (yt-dlp and ffmpeg included) and cleanly replaces any older
version. The library database lives in `%APPDATA%\KaraokeDJ\`.

## Building from source

Requirements: Visual Studio 2022+ (MSVC x64), CMake 3.20+.

```
cmake -B build
cmake --build build --config Release
build\Release\test_core.exe        # sanity tests
```

The installer target additionally expects `redist\yt-dlp.exe` and
`redist\ffmpeg.exe` (not in the repo — drop in current builds), then:

```
cd build && cpack -C Release
```

SQLite, miniz, cpp-httplib and qrcodegen are vendored (`external/`);
everything else is Windows SDK (Media Foundation, WASAPI,
Direct2D/DirectWrite, Winsock).

## Layout

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
