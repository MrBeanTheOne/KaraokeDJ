#include "app/kdj.h"

// ------------------------------------------------------------ engine control

bool loadTo(App& a, int d, const Match& m) {
    std::wstring audio, cdgPath;
    if (!resolveMedia(m.path, audio, cdgPath)) {
        a.status = L"NO AUDIO FILE (skipped): " + m.label;
        return false;
    }
    a.vdec[d].close();
    a.cur[d].reset();
    a.hasVid[d] = a.hasCdg[d] = false;
    if (!a.decks[d]->load(audio, false)) {
        a.status = L"LOAD FAILED (skipped): " + m.label;
        return false;
    }
    if (!cdgPath.empty()) a.hasCdg[d] = a.cdg[d].load(cdgPath);
    if (!a.hasCdg[d]) a.hasVid[d] = a.vdec[d].open(audio);
    a.wave[d].start(audio);
    a.mixer.autoGain[d].store(1.f);
    a.gainSet[d] = false;
    a.cueIn[d] = a.cueOut[d] = 0;
    a.cueFromDb[d] = false;
    a.smartCueSet[d] = false;
    if (m.id) { // song start/end markers (set via waveform right-click)
        Db::Stmt q;
        a.db.prepare(q, "SELECT cue_in_ms, cue_out_ms FROM media_item WHERE id=?1");
        q.bind(1, m.id);
        if (q.step()) {
            a.cueIn[d] = q.colInt(0);
            a.cueOut[d] = q.colInt(1);
        }
        a.cueFromDb[d] = a.cueIn[d] > 0 || a.cueOut[d] > 0;
        if (a.cueIn[d] > 500) {
            a.decks[d]->seek(double(a.cueIn[d]) / 1000.0);
            if (a.hasVid[d]) a.vdec[d].seek(a.cueIn[d] * 10000);
        }
    }
    a.label[d] = m.label;
    a.deckMatch[d] = m;
    return true;
}

// Queue everything selected in the browser (multi-select aware).
void queueSelected(App& a) {
    size_t n = 0;
    if (a.selRows.size() > 1) {
        for (int k : a.selRows)
            if (k >= 0 && k < int(a.results.size())) {
                a.queue.push_back(a.results[k]);
                ++n;
            }
    } else if (a.selLib >= 0 && a.selLib < int(a.results.size())) {
        a.queue.push_back(a.results[a.selLib]);
        n = 1;
    }
    if (n > 1) a.status = L"queued " + std::to_wstring(n) + L" tracks";
}

// A manual action is about to replace an auto-cued track: return it to the
// front of the queue so it is not silently lost.
void rescueAutoCue(App& a, int d) {
    if (a.autoCued[d] && !a.label[d].empty()) {
        a.queue.push_front(a.deckMatch[d]);
        if (a.selQueue >= 0) ++a.selQueue;
    }
    a.autoCued[d] = false;
}

void playNow(App& a, const Match& m) {
    if (a.mixer.fadeTo.load() >= 0 || a.pendingFade >= 0) {
        a.status = L"transition in progress";
        return;
    }
    const int act = a.mixer.activeDeck.load();
    int idle = act < 0 ? (a.lastFreed >= 0 ? 1 - a.lastFreed : 0) : 1 - act;
    rescueAutoCue(a, idle);
    if (loadTo(a, idle, m) && a.decks[idle]->state() != DeckState::Error) {
        a.pendingFade = idle;
        a.pendingDur = act < 0 ? 0.3 : (a.automixOn ? a.fadeSec : 0.08);
        a.status = L"loading: " + m.label;
    }
}

// Drop on a deck: cue it there without playing (it becomes the next song);
// if that deck is busy, it goes to the front of the queue instead.
void dropOnDeck(App& a, int d, const Match& m) {
    const int act = a.mixer.activeDeck.load();
    if (act == d || a.pendingFade == d || a.mixer.fadeTo.load() == d) {
        a.queue.push_front(m);
        a.status = L"queued next: " + m.label;
        return;
    }
    rescueAutoCue(a, d);
    loadTo(a, d, m);
    a.status = L"cued on deck " + std::wstring(d ? L"B" : L"A") + L": " + m.label;
}

// Free a deck slot: engine unload plus all per-deck video/UI state. Every
// error path must leave a deck either playing or fully cleared — half-states
// starve the preloader.
void clearDeckSlot(App& a, int d) {
    a.decks[d]->stopAndUnload();
    a.mixer.autoGain[d].store(1.f);
    a.gainSet[d] = false;
    a.cueIn[d] = a.cueOut[d] = 0;
    a.cueFromDb[d] = a.smartCueSet[d] = false;
    a.vdec[d].close();
    a.wave[d].cancel();
    a.cur[d].reset();
    a.hasVid[d] = a.hasCdg[d] = false;
    a.label[d].clear();
    a.autoCued[d] = false;
}

void stopDeck(App& a, int d) {
    if (a.mixer.fadeTo.load() >= 0) return;
    const bool wasOnAir = a.mixer.activeDeck.load() == d;
    a.decks[d]->playing.store(false);
    clearDeckSlot(a, d);
    a.lastFreed = d;
    if (a.mixer.activeDeck.load() == d) a.mixer.activeDeck.store(-1);
    if (a.pendingFade == d) a.pendingFade = -1;
    // STOP on the live deck = move on: the other deck's cue starts — MANUAL
    // cues included (the operator asked twice) — and because this sets the
    // pending fade, the preloader cannot pull a fresh queue track first.
    // With no cue anywhere, the queue feeds the empty deck as usual.
    const int other = 1 - d;
    if (wasOnAir && !a.label[other].empty() && a.pendingFade < 0 &&
        a.decks[other]->state() != DeckState::Error) {
        a.pendingFade = other;
        a.pendingDur = 0.3;
    }
}

void seekFrac(App& a, int d, float frac) {
    const uint64_t total = a.decks[d]->totalFrames.load();
    if (!total) return;
    const double t = frac * double(total) / kRate;
    a.decks[d]->seek(t);
    if (a.hasVid[d]) a.vdec[d].seek(int64_t(t * 10000000.0));
}

void setSingerStatus(App& a, int64_t itemId, const char* status) {
    Db::Stmt q;
    a.db.prepare(q, "UPDATE singer_queue_item SET status=?2 WHERE id=?1");
    q.bind(1, itemId).bind(2, std::string(status));
    q.step();
    a.navDirty = true;
}

void singNow(App& a, const SingerRow& row) {
    if (a.singingItemId >= 0) setSingerStatus(a, a.singingItemId, "completed");
    playNow(a, row.song);
    setSingerStatus(a, row.itemId, "singing");
    a.singingItemId = row.itemId;
    a.status = L"NOW SINGING: " + row.singer;
}

void addToRotationAs(App& a, const Match& m, const std::wstring& singer) {
    if (!m.id) { a.status = L"song is not in the library yet (import it first)"; return; }
    Db::Stmt q;
    a.db.prepare(q, "INSERT INTO singer_queue_item(singer,media_id,position) "
                    "VALUES(?1,?2,(SELECT IFNULL(MAX(position),0)+1 FROM "
                    "singer_queue_item))");
    q.bind(1, utf8(singer.empty() ? L"Guest" : singer)).bind(2, m.id);
    q.step();
    a.navDirty = true;
    a.status = singer + L" -> " + m.label;
}

// Every singer the rotation has seen, for the add-to-rotation submenu.
std::vector<std::wstring> rotationSingers(App& a) {
    std::vector<std::wstring> names;
    Db::Stmt q;
    a.db.prepare(q, "SELECT DISTINCT singer FROM singer_queue_item ORDER BY singer");
    while (q.step()) names.push_back(wide(q.colText(0)));
    return names;
}

// Confirm the "new singer" modal: the name becomes the current singer too.
void commitPrompt(App& a) {
    if (a.prompt == App::Prompt::Columns) { // Enter closes the editor
        a.prompt = App::Prompt::None;
        return;
    }
    if (a.prompt == App::Prompt::Confirm) {
        switch (a.confirmAction) {
        case App::ConfirmAction::RemoveFolder:
            performRemoveFolder(a, a.confirmPath);
            break;
        case App::ConfirmAction::DeletePlaylist: {
            Db::Stmt q; // playlist_item rows go via FK cascade
            a.db.prepare(q, "DELETE FROM playlist WHERE id=?1");
            q.bind(1, a.confirmId);
            q.step();
            if (a.nav == NavMode::Playlist && a.navPlaylist == a.confirmId) {
                a.nav = NavMode::Library;
                a.searchDirty = true;
            }
            a.navDirty = true;
            a.status = L"playlist deleted";
            break;
        }
        case App::ConfirmAction::CleanMissing:
            performCleanMissing(a);
            break;
        case App::ConfirmAction::ClearRotation:
            a.db.exec("DELETE FROM singer_queue_item");
            a.singingItemId = -1;
            a.navDirty = true;
            a.status = L"rotation cleared — ready for a new night";
            break;
        case App::ConfirmAction::ClearHistory:
            a.db.exec("DELETE FROM play_history WHERE started_at > "
                      "strftime('%s','now') - 43200");
            a.navDirty = true;
            a.status = L"tonight's history cleared";
            break;
        default:
            break;
        }
        a.confirmAction = App::ConfirmAction::None;
        a.prompt = App::Prompt::None;
        return;
    }
    if (a.prompt == App::Prompt::NewSinger) {
        if (!a.promptText.empty()) a.singerName = a.promptText;
        addToRotationAs(a, a.rotAddPending, a.promptText);
    } else if (a.prompt == App::Prompt::NewPlaylist && !a.promptText.empty()) {
        Db::Stmt q;
        a.db.prepare(q, "INSERT OR IGNORE INTO playlist(name,created_at,updated_at) "
                        "VALUES(?1, strftime('%s','now'), strftime('%s','now'))");
        q.bind(1, utf8(a.promptText));
        q.step();
        Db::Stmt f; // navigate to it (existing one if the name was taken)
        a.db.prepare(f, "SELECT id FROM playlist WHERE name=?1");
        f.bind(1, utf8(a.promptText));
        if (f.step()) {
            a.nav = NavMode::Playlist;
            a.navPlaylist = f.colInt(0);
            a.navPlaylistName = a.promptText;
            a.searchDirty = true;
        }
        a.navDirty = true;
        a.status = L"playlist created: " + a.promptText;
    }
    a.prompt = App::Prompt::None;
}

void presentVideo(App& a) {
    if (a.fullOut) a.fullOut->setFitMode(a.videoFit); // covers every create path
    VideoWindow* wins[1] = {a.fullOut.get()};
    for (int d = 0; d < 2; ++d) {
        const int64_t clk = int64_t(a.decks[d]->framesPlayed.load() * 10000000ull / kRate);
        bool fresh = false;
        if (a.hasVid[d]) {
            if (auto f = a.vdec[d].popDue(clk)) { a.cur[d] = std::move(f); fresh = true; }
        } else if (a.hasCdg[d]) {
            if (!a.cur[d]) a.cur[d] = std::make_unique<VideoFrame>();
            fresh = a.cdg[d].renderTo(*a.cur[d], double(clk) / 10000000.0);
        }
        if (fresh) {
            ++a.frameSerial[d]; // the mixer preview pane re-uploads on lag
            for (auto* w : wins)
                if (w) w->setFrame(d, *a.cur[d]);
        }
    }
    const int ft = a.mixer.fadeTo.load();
    const float t = a.mixer.fadeT.load();
    const int act = a.mixer.activeDeck.load();
    std::wstring nextUp;
    for (int d = 0; d < 2; ++d)
        if (a.autoCued[d] && !a.label[d].empty()) nextUp = L"NEXT UP:  " + a.label[d];
    if (nextUp.empty() && !a.queue.empty())
        nextUp = L"NEXT UP:  " + a.queue.front().label;
    for (auto* w : wins) {
        if (!w) continue;
        w->setIdleText(a.idleTitle, nextUp); // waiting screen (plan §6)
        if (w->bitmapsLost())
            for (int d = 0; d < 2; ++d)
                if (a.cur[d]) w->setFrame(d, *a.cur[d]);
        if (ft >= 0 && t >= 0.f) w->draw(1 - ft, ft, t);
        else w->draw(act >= 0 && (a.hasVid[act] || a.hasCdg[act]) ? act : -1, -1, 0.f);
    }
}

// Thin scroll indicator on the right edge of a list viewport.
// Draggable scroll thumb on the right edge of a list viewport. Rows must
// ignore presses within the grab zone (list.right - 12).
void scrollbar(App& a, Ui& ui, int id, const D2D1_RECT_F& list, size_t count,
                      float rowH, float& scroll) {
    const float visible = (list.bottom - list.top) / rowH;
    if (count <= size_t(visible)) return;
    const float h = list.bottom - list.top;
    const float barH = (std::max)(24.f, visible / count * h);
    const float maxScroll = float(count) - visible;
    const float t = std::clamp(scroll / maxScroll, 0.f, 1.f);
    const float y = list.top + t * (h - barH);
    const D2D1_RECT_F track = rc(list.right - 12, list.top, 12, h);
    const D2D1_RECT_F thumb = rc(list.right - 8, y, 6, barH);
    if (ui.in.pressed && hit(track, ui.in.pressX, ui.in.pressY)) {
        a.scrollGrab = id; // thumb keeps its grip point; track-click centers
        a.scrollGrabOff = hit(thumb, ui.in.pressX, ui.in.pressY)
                              ? ui.in.pressY - y
                              : barH / 2;
    }
    if (!ui.in.down && a.scrollGrab == id) a.scrollGrab = 0;
    if (a.scrollGrab == id) {
        const float ny =
            std::clamp(ui.in.my - a.scrollGrabOff, list.top, list.bottom - barH);
        scroll = (ny - list.top) / (h - barH) * maxScroll;
    }
    const bool over = hit(track, ui.in.mx, ui.in.my);
    ui.rect(thumb, a.scrollGrab == id ? cAccent
                   : over             ? col(0x5A5A64)
                                      : col(0x3C3C44),
            3);
}

void engineTick(App& a) {
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - a.lastTick).count();
    a.lastTick = now;

    if (a.pendingFade >= 0 &&
        a.decks[a.pendingFade]->state() == DeckState::Empty) {
        a.pendingFade = -1; // watchdog: a fade can never fire from an empty deck
    }
    if (a.pendingFade >= 0) {
        const DeckState s = a.decks[a.pendingFade]->state();
        if (s == DeckState::Ready) {
            const int pf = a.pendingFade;
            a.retireAfterFade = a.mixer.activeDeck.load(); // outgoing deck, if any
            MixCommand c{pf, uint64_t(a.pendingDur * kRate), FadeCurve::EqualPower};
            a.mixer.cmds.push(&c, 1);
            a.pendingFade = -1;
            a.status = L"playing: " + a.label[pf];
            if (a.deckMatch[pf].id) { // tonight's history: every track on air
                std::wstring who;
                for (const auto& sg : a.singers)
                    if (sg.status == "singing" && sg.song.id == a.deckMatch[pf].id)
                        who = sg.singer;
                Db::Stmt h;
                a.db.prepare(h, "INSERT INTO play_history(media_id,singer,started_at) "
                                "VALUES(?1,?2,strftime('%s','now'))");
                h.bind(1, a.deckMatch[pf].id).bind(2, utf8(who));
                h.step();
                a.playedTonight.insert(a.deckMatch[pf].id);
                a.navDirty = true;
            }
        } else if (s == DeckState::Error) {
            a.status = L"LOAD ERROR: " + a.label[a.pendingFade];
            a.label[a.pendingFade].clear();
            a.pendingFade = -1;
        }
    }
    const uint64_t f = a.mixer.fadesCompleted.load();
    if (f != a.lastFades) {
        a.lastFades = f;
        const int act = a.mixer.activeDeck.load();
        // Retire ONLY the deck the fade came from — a fade that started from
        // silence has no outgoing deck, and the other deck may hold a fresh
        // cue (manual, or a crash-restored one) that must survive.
        const int other = a.retireAfterFade;
        a.retireAfterFade = -1;
        if (act >= 0) {
            if (other >= 0 && other != act) {
                if (a.repeatOn && !a.label[other].empty())
                    a.queue.push_back(a.deckMatch[other]); // repeat: to the tail
                clearDeckSlot(a, other);
            }
            // Park the crossfader on the winning side: the freed deck is now
            // silent in the master, so a track can be cued and scrubbed there.
            a.mixer.crossfader.store(act == 0 ? 0.f : 1.f);
        }
    }
    // Preload: keep the idle deck holding the next queued track from the
    // moment the current one starts — a bad file fails NOW, minutes early,
    // not at the transition (and the operator can see/scrub the cue).
    if (a.pendingFade < 0 && a.mixer.fadeTo.load() < 0) {
        const int act = a.mixer.activeDeck.load();
        int idle;
        if (act >= 0) {
            idle = 1 - act;
        } else {
            // Nothing on air (e.g. just stopped): cue onto an EMPTY deck,
            // preferring the opposite of the one that was stopped, so the
            // next track never reloads the deck the operator just killed.
            idle = a.lastFreed >= 0 ? 1 - a.lastFreed : 0;
            if (!a.label[idle].empty() && a.label[1 - idle].empty()) idle = 1 - idle;
        }
        // Any errored idle deck (auto-cued or manual) frees itself so the
        // queue keeps flowing; the status line keeps the report.
        if (!a.label[idle].empty() && a.decks[idle]->state() == DeckState::Error) {
            a.status = (a.autoCued[idle] ? L"PRELOAD FAILED (skipped): "
                                         : L"LOAD ERROR (cleared): ") +
                       a.label[idle];
            clearDeckSlot(a, idle);
        }
        if (a.label[idle].empty() && !a.queue.empty()) {
            const Match m = a.queue.front();
            a.queue.pop_front();
            if (a.selQueue >= 0) --a.selQueue;
            if (loadTo(a, idle, m)) { // failure self-heals: next item next tick
                a.autoCued[idle] = true;
                a.status = L"next up on deck " + std::wstring(idle ? L"B" : L"A") +
                           L": " + m.label;
            }
        }
    }
    // Queue / cued-deck advance.
    if (a.pendingFade < 0 && a.mixer.fadeTo.load() < 0) {
        const int act = a.mixer.activeDeck.load();
        if (act >= 0) {
            const int idle = 1 - act;
            const bool cued = !a.label[idle].empty() &&
                              a.decks[idle]->state() == DeckState::Ready;
            // Repeat with nothing else queued: the playing track is its own
            // successor (crossfades into a fresh load of itself).
            const bool selfRepeat =
                !cued && a.queue.empty() && a.repeatOn && !a.label[act].empty();
            if (cued || !a.queue.empty() || selfRepeat) {
                double rem = a.decks[act]->remainingSec();
                bool trigger = a.decks[act]->eos.load();
                if (a.cueOut[act] > 0) { // manual END marker wins
                    const double remCue =
                        double(a.cueOut[act]) / 1000.0 -
                        double(a.decks[act]->framesPlayed.load()) / kRate;
                    if (rem < 0 || remCue < rem) rem = remCue;
                    if (remCue <= 0.05) trigger = true; // cut mode hits the mark
                }
                if (a.automixOn && rem >= 0 && rem <= a.fadeSec + 0.25)
                    trigger = true; // smart mode acts through the end marker
                if (trigger) {
                    if (cued) { // deck-cued track wins: it is "the next song"
                        a.pendingFade = idle;
                        a.pendingDur = a.automixOn ? a.fadeSec : 0.08;
                    } else if (selfRepeat) {
                        playNow(a, a.deckMatch[act]);
                    } else {
                        const Match m = a.queue.front();
                        a.queue.pop_front();
                        if (a.selQueue >= 0) --a.selQueue;
                        playNow(a, m);
                    }
                }
            }
        } else { // nothing playing: auto-start a queue-cued deck (manual cues wait)
            for (int d = 0; d < 2; ++d) {
                if (a.autoCued[d] && !a.label[d].empty() &&
                    a.decks[d]->state() == DeckState::Ready) {
                    a.pendingFade = d;
                    a.pendingDur = 0.3;
                    break;
                }
            }
        }
    }
    // Smart automix: when the scan lands, derive start/end markers from the
    // waveform bins (first/last audible) — VISIBLE and correctable by drag,
    // instead of the old invisible silence skipping. Saved markers win.
    for (int d = 0; d < 2; ++d) {
        if (a.smartCueSet[d] || a.cueFromDb[d] || a.label[d].empty()) continue;
        if (!(a.automixOn && a.mixMode == MixMode::Smart)) continue;
        if (a.wave[d].loudness() <= 0.f) continue; // scan still running
        const uint64_t totalFr = a.decks[d]->totalFrames.load();
        if (!totalFr) continue;
        const double dur = double(totalFr) / kRate;
        int first = -1, last = -1;
        for (int i = 0; i < WaveformScanner::kBins; ++i)
            if (a.wave[d].bin(i) > 0.02f) {
                if (first < 0) first = i;
                last = i;
            }
        if (first >= 0) {
            const double binSec = dur / WaveformScanner::kBins;
            const double inSec = (std::max)(0.0, first * binSec - 0.3);
            const double outSec = (std::min)(dur, (last + 1) * binSec + 0.5);
            if (inSec > 1.0) a.cueIn[d] = int64_t(inSec * 1000);
            if (dur - outSec > 1.5) a.cueOut[d] = int64_t(outSec * 1000);
            // Reposition a deck that is not on air — or one that IS playing
            // but still inside the silent intro (the jump is inaudible),
            // which happens when a track auto-starts right after a stop.
            const bool inSilentIntro =
                a.decks[d]->framesPlayed.load() <
                uint64_t(a.cueIn[d]) * kRate / 1000;
            if (a.cueIn[d] > 500 &&
                (!a.decks[d]->playing.load() || inSilentIntro) &&
                a.pendingFade != d && a.mixer.fadeTo.load() != d) {
                a.decks[d]->seek(double(a.cueIn[d]) / 1000.0);
                if (a.hasVid[d]) a.vdec[d].seek(a.cueIn[d] * 10000);
            }
        }
        a.smartCueSet[d] = true;
    }

    // Detected BPM write-back: the scan measures tempo; store it once so the
    // browser's BPM column fills in as tracks get cued/played.
    for (int d = 0; d < 2; ++d) {
        const int bpm = a.wave[d].bpm();
        if (bpm <= 0 || a.label[d].empty() || !a.deckMatch[d].id) continue;
        if (a.deckMatch[d].bpm > 0) continue; // already known (tag or earlier)
        Db::Stmt q; // bpm<=0 lets the full-track scan refine an analyzer miss
        a.db.prepare(q, "UPDATE media_item SET bpm=?2 WHERE id=?1 AND bpm<=0");
        q.bind(1, a.deckMatch[d].id).bind(2, int64_t(bpm));
        q.step();
        a.deckMatch[d].bpm = bpm; // don't re-write every tick
        a.searchDirty = true;     // column refreshes on next reload
    }

    // Auto gain: once the waveform scan has measured a track's loudness,
    // trim the deck toward a common level. Applied under the user's slider.
    for (int d = 0; d < 2; ++d) {
        if (a.gainSet[d] || a.label[d].empty()) continue;
        const float loud = a.wave[d].loudness();
        if (loud <= 0.f) continue; // scan still running
        float g = 1.f;
        if (a.autoGainOn) {
            constexpr float kTargetRms = 0.18f; // ~-15 dBFS in the loud parts
            g = std::clamp(kTargetRms / loud, 0.4f, 2.0f); // -8..+6 dB
        }
        a.mixer.autoGain[d].store(g);
        a.gainSet[d] = true;
    }
    if (a.fullOut && !a.fullOut->pump()) { a.fullOut.reset(); a.outMonitor = -1; }
    if (now - a.lastSnapAt >= 5s) { // session snapshot for crash recovery
        a.lastSnapAt = now;
        saveSnapshot(a);
    }
    presentVideo(a);
}
