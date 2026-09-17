cmake_minimum_required(VERSION 3.21)
project(karaoke-dj VERSION 1.1.9 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the installer then needs no VC++ redistributable on the target
# laptop â€” the exes are self-contained.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>"
                    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

add_executable(harness
  src/app/harness_main.cpp
  src/media/mf_decoder.cpp
  src/playback/deck.cpp
  src/playback/mixer.cpp
  src/playback/pitch_shifter.cpp
  src/audio/wasapi_out.cpp)
target_include_directories(harness PRIVATE src)
target_link_libraries(harness PRIVATE mfplat mfreadwrite mfuuid ole32 avrt psapi)

add_executable(video_harness
  external/qrcodegen/qrcodegen.cpp
  src/app/video_harness_main.cpp
  src/karaoke/cdg_renderer.cpp
  src/media/zip_media.cpp
  src/media/mf_decoder.cpp
  src/media/mf_video_decoder.cpp
  src/playback/deck.cpp
  src/playback/mixer.cpp
  src/playback/pitch_shifter.cpp
  src/audio/wasapi_out.cpp
  src/video/video_window.cpp)
target_include_directories(video_harness PRIVATE src external/qrcodegen)
target_link_libraries(video_harness PRIVATE miniz mfplat mfreadwrite mfuuid ole32 avrt d2d1 d3d11
  dwrite windowscodecs)

add_executable(gen_test_video tools/gen_test_video.cpp)
target_link_libraries(gen_test_video PRIVATE mfplat mfreadwrite mfuuid ole32)

# Key-detector tuning rig (see src/media/key_detect.cpp).
add_executable(dump_chroma tools/dump_chroma.cpp
  src/media/key_detect.cpp
  src/media/mf_decoder.cpp)
target_include_directories(dump_chroma PRIVATE src)
target_link_libraries(dump_chroma PRIVATE mfplat mfreadwrite mfuuid ole32)

add_library(miniz STATIC external/miniz/miniz.c)
target_include_directories(miniz PUBLIC external/miniz)

add_library(sqlite3 STATIC external/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC external/sqlite)

add_executable(librarian
  src/app/librarian_main.cpp
  src/library/db.cpp
  src/library/media_query.cpp
  src/library/scanner.cpp)
target_include_directories(librarian PRIVATE src)
target_link_libraries(librarian PRIVATE sqlite3 shell32 propsys ole32 normaliz)

add_executable(karaoke_dj WIN32
  src/app/karaoke_dj_main.cpp
  src/app/kdj_data.cpp
  src/app/kdj_engine.cpp
  src/app/kdj_ui.cpp
  src/app/kdj_menus.cpp
  assets/app.rc
  src/ui/im.cpp
  src/ui/lang.cpp
  src/library/db.cpp
  src/library/media_query.cpp
  src/library/scanner.cpp
  src/karaoke/cdg_renderer.cpp
  src/media/mf_decoder.cpp
  src/media/mf_video_decoder.cpp
  src/media/key_detect.cpp
  src/media/waveform.cpp
  src/media/zip_media.cpp
  src/playback/deck.cpp
  src/playback/mixer.cpp
  src/playback/pitch_shifter.cpp
  src/audio/wasapi_out.cpp
  src/video/video_window.cpp
  src/web/request_server.cpp
  external/qrcodegen/qrcodegen.cpp)
target_include_directories(karaoke_dj PRIVATE src external/httplib external/qrcodegen)
target_compile_definitions(karaoke_dj PRIVATE KDJ_VERSION="${PROJECT_VERSION}")
target_link_libraries(karaoke_dj PRIVATE sqlite3 miniz normaliz psapi mfplat mfreadwrite mfuuid ole32 avrt d3d11
  d2d1 dwrite shell32 propsys ws2_32 iphlpapi windowscodecs winhttp)

add_executable(test_core tests/unit/test_core.cpp src/karaoke/cdg_renderer.cpp
  src/playback/pitch_shifter.cpp src/media/key_detect.cpp)
target_include_directories(test_core PRIVATE src)

# ------------------------------------------------------------------ installer
# `cpack -G NSIS -C Release` in build/ produces KaraokeDJ-<ver>-win64.exe:
# installs the operator app + librarian CLI to Program Files, Start-menu and
# desktop shortcuts, standard uninstaller. User data (%APPDATA%\KaraokeDJ) is
# created at runtime and never touched by the uninstaller.
install(TARGETS karaoke_dj librarian RUNTIME DESTINATION .)
# Bundled tools (redist/ is not committed â€” populate it before packaging):
# yt-dlp for YouTube playback, ffmpeg so yt-dlp can merge best-quality
# streams. OPTIONAL: building without them still works, they just aren't
# packaged.
install(PROGRAMS redist/yt-dlp.exe redist/ffmpeg.exe DESTINATION . OPTIONAL)
# License + third-party attributions ship with every install (the MIT
# components require their notices to accompany distributed binaries).
install(FILES LICENSE THIRD-PARTY-NOTICES.txt DESTINATION .)

set(CPACK_GENERATOR NSIS)
set(CPACK_PACKAGE_NAME "Karaoke DJ")
set(CPACK_PACKAGE_VENDOR "Karaoke DJ")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Two-deck karaoke / video DJ")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Karaoke DJ")
# Version-less registry key: every version shares one identity, so a newer
# installer auto-uninstalls the older one and Apps never shows duplicates.
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "Karaoke DJ")
set(CPACK_PACKAGE_FILE_NAME "KaraokeDJ-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_EXECUTABLES "karaoke_dj;Karaoke DJ")
set(CPACK_CREATE_DESKTOP_LINKS "karaoke_dj")
set(CPACK_NSIS_DISPLAY_NAME "Karaoke DJ")
set(CPACK_NSIS_PACKAGE_NAME "Karaoke DJ")
set(CPACK_NSIS_INSTALLED_ICON_NAME "karaoke_dj.exe")
set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/assets/karaoke_dj.ico")
set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/assets/karaoke_dj.ico")
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_COMPRESSOR "/SOLID lzma") # ffmpeg squeezes ~3x
# Inbound firewall rule for the phone-request server (silent if the user
# never enables the feature; removed again on uninstall).
# NOTE: these strings are parsed TWICE (CMakeLists -> CPackConfig -> cpack),
# so quotes and backslashes are double-escaped.
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
    "ExecWait 'netsh advfirewall firewall delete rule name=\\\"Karaoke DJ\\\"'\nExecWait 'netsh advfirewall firewall add rule name=\\\"Karaoke DJ\\\" dir=in action=allow program=\\\"$INSTDIR\\\\karaoke_dj.exe\\\"'")
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
    "ExecWait 'netsh advfirewall firewall delete rule name=\\\"Karaoke DJ\\\"'")
include(CPack)
