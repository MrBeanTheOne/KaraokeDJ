#include "app/kdj.h"

// Deck panels, the mixer strip and the top rhythm ribbon.
// Split out of kdj_ui.cpp — see the note there.

void drawDeck(App& a, Ui& ui, int d, const D2D1_RECT_F& r) {
    a.rcDeck[d] = r;
    const D2D1_COLOR_F ac = d == 0 ? cA : cB;
    const int act = a.mixer.activeDeck.load();
    ui.rect(r, cPanel, 10);
    if (act == d) { // soft glow around the live deck
        ui.frameRect(rc(r.left - 2, r.top - 2, r.right - r.left + 4,
                        r.bottom - r.top + 4),
                     D2D1_COLOR_F{ac.r, ac.g, ac.b, 0.28f}, 12, 3.f);
        ui.frameRect(r, ac, 10, 1.5f);
    } else {
        ui.frameRect(r, cBorder, 10);
    }
    const float x = r.left + 14, w = r.right - r.left - 28;

    // VDJ-style deck header: accent-tinted title bar with deck id/track/state.
    ui.rect(rc(r.left + 1.5f, r.top + 1.5f, r.right - r.left - 3, 48),
            mix(cPanel, ac, a.label[d].empty() ? 0.05f : 0.16f), 9);
    ui.text(rc(x, r.top + 6, w, 14), d == 0 ? L"DECK A" : L"DECK B", 10, ac, 0, true);
    const DeckState st = a.decks[d]->state();
    const bool cued = !a.label[d].empty() && act != d && st == DeckState::Ready &&
                      a.pendingFade != d;
    std::wstring stateTxt = a.label[d].empty() ? L"" :
        st == DeckState::Loading ? L"LOADING" :
        st == DeckState::Error   ? L"ERROR" :
        a.decks[d]->paused.load() ? L"PAUSED" :
        act == d ? L"PLAYING" : cued ? L"CUED — NEXT" : L"READY";
    ui.text(rc(x, r.top + 6, w, 14), stateTxt, 10,
            st == DeckState::Error ? cRed
            : stateTxt == L"PLAYING" ? cGreen
            : cued ? cText : cDim, 2, true);

    ui.text(rc(x, r.top + 22, w, 24),
            a.label[d].empty() ? L"— drop a track —" : a.label[d], 15,
            a.label[d].empty() ? cDim : cText, 0, true);
    std::wstring media = a.hasCdg[d] ? L"MP3+G KARAOKE" : a.hasVid[d] ? L"VIDEO" :
                         a.label[d].empty() ? L"" : L"AUDIO";
    const float ag = a.mixer.autoGain[d].load();
    if (ag != 1.f) { // auto-level trim in effect
        wchar_t agb[32];
        swprintf(agb, 32, L"   ·  auto %+.1f dB", 20.0 * log10(double(ag)));
        media += agb;
    }
    ui.text(rc(x, r.top + 56, w, 18), media, 11, cDim, 0, false);

    const float pk = a.mixer.deckPeak[d].load(std::memory_order_relaxed);
    ui.rect(rc(r.right - 90, r.top + 60, 76, 6), col(0x2A2A30), 3);
    if (pk > 0.001f)
        ui.rect(rc(r.right - 90, r.top + 60, 76 * (std::min)(pk * 1.2f, 1.f), 6),
                pk > 0.9f ? cRed : ac, 3);

    const double played = double(a.decks[d]->framesPlayed.load()) / kRate;
    const double total = double(a.decks[d]->totalFrames.load()) / kRate;
    const float tw = w - 150; // right side belongs to the jog platter
    ui.text(rc(x, r.top + 78, tw / 2, 34), fmtTime(played), 26, cText, 0, true);
    ui.text(rc(x + tw / 2, r.top + 78, tw / 2, 34),
            total > 0 ? L"-" + fmtTime(total - played) : L"", 26, cDim, 2, true);

    // Jog platter (VDJ look): spins with the track, accent ring when live.
    // ponytail: visual only — scrubbing lives on the waveform strip.
    {
        const float jx = r.right - 86, jy = r.top + 146, jr = 56;
        ui.circle(jx, jy, jr, cInset);
        ui.circle(jx, jy, jr, act == d ? ac : cBorder, false, act == d ? 2.f : 1.f);
        ui.circle(jx, jy, jr * 0.42f, col(0x18181C));
        // VDJ signature: crimson center cap on both platters.
        ui.circle(jx, jy, jr * 0.30f,
                  a.label[d].empty() ? col(0x3A2026) : cAccent);
        if (!a.label[d].empty()) {
            const float ang = float(played * 6.2831853 / 1.8) - 1.5707963f;
            const float mx = jx + cosf(ang) * jr * 0.72f;
            const float my = jy + sinf(ang) * jr * 0.72f;
            ui.line(jx + cosf(ang) * jr * 0.46f, jy + sinf(ang) * jr * 0.46f,
                    jx + cosf(ang) * jr * 0.94f, jy + sinf(ang) * jr * 0.94f,
                    col(0x323238), 3);
            ui.circle(mx, my, 4, act == d ? cText : cDim);
        }
    }

    // Waveform strip with click-to-scrub (fills in as the background scan runs).
    const D2D1_RECT_F wf = rc(x, r.top + 124, w - 160, 46); // handles live above
    ui.rect(wf, cInset, 6);
    const float frac = total > 0 ? float(played / total) : 0.f;
    const int readyN = a.wave[d].readyBins();
    const float bw = (wf.right - wf.left) / float(WaveformScanner::kBins);
    const float midY = (wf.top + wf.bottom) / 2;
    // The played-so-far accent means "the playhead got here", so it is drawn
    // for a deck that is on air (pausing keeps it — the track is still the one
    // on the deck, just halted) AND for one sitting mid-track, which is how a
    // crash-restored or scrubbed deck comes back. A freshly CUED deck rests
    // exactly on its start marker and stays grey: colouring that would read as
    // "already played" when all it did was seek. Bins outside the start/end
    // markers fade back, so the part that will really be heard stands out.
    const uint64_t cueFr =
        uint64_t((std::max)(int64_t(0), a.cueIn[d])) * kRate / 1000;
    const bool onAir = a.decks[d]->playing.load() ||
                       a.decks[d]->framesPlayed.load() > cueFr + kRate / 2;
    const float inFrac = total > 0 ? float(double(a.cueIn[d]) / (total * 1000.0)) : 0.f;
    const float outFrac = total > 0 && a.cueOut[d] > 0
                              ? float(double(a.cueOut[d]) / (total * 1000.0))
                              : 1.f;
    for (int i = 0; i < readyN; ++i) {
        float v = a.wave[d].bin(i);
        v = (std::min)(1.f, sqrtf(v) * 1.15f);
        const float bh = (std::max)(1.5f, v * 40.f);
        const float bf = float(i) / WaveformScanner::kBins;
        const bool done = onAir && bf <= frac;
        const bool outside = bf < inFrac || bf > outFrac;
        D2D1_COLOR_F bc = done ? ac : col(0x3A3A40);
        bc.a = outside ? 0.22f : done ? 0.9f : 1.f;
        ui.rect(rc(wf.left + i * bw, midY - bh / 2, (std::max)(1.f, bw - 0.5f), bh), bc,
                1);
    }
    a.rcWave[d] = wf;
    // Start/end markers: ALWAYS drawn (defaults: 0 and track end). Smart mode
    // moves them from the scan; drag the handles above the strip to correct.
    if (total > 0) {
        const double durMs = total * 1000.0;
        const double shown[2] = {double(a.cueIn[d]),
                                 a.cueOut[d] > 0 ? double(a.cueOut[d]) : durMs};
        const D2D1_COLOR_F mc[2] = {cGreen, cRed};
        for (int k = 0; k < 2; ++k) {
            const float fx = wf.left + float(shown[k] / durMs) *
                                           (wf.right - wf.left);
            ui.rect(rc(fx - 1, wf.top + 1, 2, wf.bottom - wf.top - 2), mc[k], 1);
            // Grab handle above the strip: markers move ONLY from here, so a
            // stray press on the waveform body can't drag them.
            const D2D1_RECT_F hnd = rc(fx - 6, wf.top - 10, 12, 12);
            ui.rect(hnd, mc[k], 3);
            ui.frameRect(hnd, cText, 3);
            if (ui.in.pressed && hit(hnd, ui.in.pressX, ui.in.pressY) &&
                !a.label[d].empty()) {
                a.markerDeck = d;
                a.markerWhich = k;
            }
        }
        if (a.markerDeck == d) { // dragging a handle
            const int k = a.markerWhich;
            const double ms = std::clamp(
                double((ui.in.mx - wf.left) / (wf.right - wf.left)), 0.0, 1.0) *
                durMs;
            if (k == 0) a.cueIn[d] = int64_t((std::min)(ms, shown[1] - 1000.0));
            else a.cueOut[d] = int64_t(std::clamp(ms, double(a.cueIn[d]) + 1000.0,
                                                  durMs));
            if (a.cueIn[d] < 0) a.cueIn[d] = 0;
            ui.text(rc(ui.in.mx + 8, wf.top - 12, 70, 14),
                    fmtTime((k == 0 ? a.cueIn[d] : a.cueOut[d]) / 1000.0), 11,
                    mc[k], 0, true);
            if (!ui.in.down) { // release: persist (library tracks)
                if (a.deckMatch[d].id) {
                    a.cueFromDb[d] = true; // manual correction wins over smart
                    Db::Stmt q;
                    a.db.prepare(q, "UPDATE media_item SET cue_in_ms=?2, "
                                    "cue_out_ms=?3 WHERE id=?1");
                    const int64_t outSave = // end marker at track end = unset
                        a.cueOut[d] >= int64_t(durMs) - 200 ? 0 : a.cueOut[d];
                    q.bind(1, a.deckMatch[d].id).bind(2, a.cueIn[d]).bind(3, outSave);
                    q.step();
                }
                // The deck follows the START marker straight away — saving it
                // without re-cueing is what made a post-load move look ignored.
                if (a.markerWhich == 0) applyCueIn(a, d);
                a.markerDeck = a.markerWhich = -1;
            }
        }
    }
    if (ui.in.rpressed && hit(wf, ui.in.rX, ui.in.rY) && !a.label[d].empty())
        a.menu = {MenuReq::DeckWave, d, ui.in.rX, ui.in.rY};
    if (total > 0)
        ui.rect(rc(wf.left + frac * (wf.right - wf.left) - 1, wf.top + 2, 2,
                   wf.bottom - wf.top - 4),
                cText, 1);
    // Scrub: press to jump, drag to sweep. The top 12 px belong to the marker
    // handles, so a press there never moves the playhead.
    if (ui.in.pressed && hit(wf, ui.in.pressX, ui.in.pressY) &&
        ui.in.pressY > wf.top + 12 && a.markerDeck < 0 && total > 0)
        a.scrubDeck = d;
    if (a.scrubDeck == d) {
        // Ghost playhead tracks the mouse at full frame rate; the actual seek
        // (which briefly blocks) is committed once, on release.
        const float sf =
            std::clamp((ui.in.mx - wf.left) / (wf.right - wf.left), 0.f, 1.f);
        ui.rect(rc(wf.left + sf * (wf.right - wf.left) - 1, wf.top + 2, 2,
                   wf.bottom - wf.top - 4),
                cAccent, 1);
        ui.text(rc(wf.left + sf * (wf.right - wf.left) + 6, wf.top, 60, 16),
                fmtTime(sf * total), 11, cAccent, 0, true);
        if (!ui.in.down) {
            seekFrac(a, d, sf);
            a.scrubDeck = -1;
        }
    }

    const float by = r.bottom - 44;
    if (cued) {
        if (ui.button(110 + d, rc(x, by, 92, 32), L"PLAY ▸", ac, true)) {
            a.pendingFade = d;
            a.pendingDur = act < 0 ? 0.3 : (a.automixOn ? a.fadeSec : 0.08);
        }
    } else {
        const bool paused = a.decks[d]->paused.load();
        if (ui.button(110 + d, rc(x, by, 92, 32), paused ? L"RESUME" : L"PAUSE", ac))
            a.decks[d]->paused.store(!paused);
    }
    // Labeled CLEAR: it empties the deck (and hands over when it was live),
    // it doesn't halt-in-place — that's PAUSE.
    if (ui.button(120 + d, rc(x + 100, by, 72, 32), L"CLEAR", cRed)) stopDeck(a, d);

    // Key change: semitones up/down, pitch only — the tempo (and with it the
    // lyric/video sync) is untouched. Costs nothing while it reads 0; the
    // shifter allocates the first time a key is dialled in, and every load
    // puts it back to 0. Click the readout to zero it.
    const int semi = a.decks[d]->key.semitones();
    // The big readout is the key the singer will actually hear: the track's
    // detected key transposed by the buttons. Until the analyzer has a key
    // for this track it falls back to the bare semitone offset.
    const std::wstring kname = keyName(int(a.deckMatch[d].musicKey), semi);
    // Centred under the jog platter (its centre is r.right - 86); the group
    // spans 128 px, so it starts 64 px left of that.
    const float kx = r.right - 86 - 64;
    if (ui.button(130 + d, rc(kx, by, 30, 32), L"−", cAccent))
        a.decks[d]->key.setSemitones(semi - 1, kCh);
    const D2D1_RECT_F kr = rc(kx + 32, by, 64, 32);
    ui.rect(kr, semi ? mix(cInset, cGreen, 0.14f) : cInset, 6);
    wchar_t off[8];
    swprintf(off, 8, L"%+d", semi);
    // Caption: KEY while we're at the original, how far off it once moved.
    ui.text(rc(kr.left, kr.top + 3, 64, 11), semi ? off : L"KEY", 9,
            semi ? cGreen : cDim, 1, true);
    ui.text(rc(kr.left, kr.top + 13, 64, 17),
            kname.empty() ? std::wstring(semi ? off : L"0") : kname, 15,
            semi ? cGreen : kname.empty() ? cDim : cText, 1, true);
    if (ui.in.pressed && hit(kr, ui.in.pressX, ui.in.pressY))
        a.decks[d]->key.setSemitones(0, kCh);
    if (ui.button(140 + d, rc(kx + 98, by, 30, 32), L"+", cAccent))
        a.decks[d]->key.setSemitones(semi + 1, kCh);
}

void drawMixer(App& a, Ui& ui, const D2D1_RECT_F& r) {
    ui.rect(r, cPanel, 10);
    ui.frameRect(r, cBorder, 10);
    const float cx = (r.left + r.right) / 2;
    const int act = a.mixer.activeDeck.load();

    // Live per-deck video preview panes (VDJ center): the operator sees both
    // feeds without opening a preview window.
    const float pw = (r.right - r.left - 28 - 4) / 2;
    for (int d = 0; d < 2; ++d) {
        const D2D1_RECT_F pane = rc(r.left + 14 + d * (pw + 4), r.top + 8, pw, 82);
        ui.rect(pane, col(0x000000), 4);
        if (a.cur[d]) { // cur[] resets on load/stop, so no stale-track frames
            if (a.shownSerial[d] != a.frameSerial[d]) {
                ui.setImage(d, a.cur[d]->bgra.data(), a.cur[d]->width,
                            a.cur[d]->height);
                a.shownSerial[d] = a.frameSerial[d];
            }
            ui.clipPush(pane);
            if (!ui.image(d, pane)) { // device loss dropped the cache: re-upload
                ui.setImage(d, a.cur[d]->bgra.data(), a.cur[d]->width,
                            a.cur[d]->height);
                ui.image(d, pane);
            }
            ui.clipPop();
        } else if (!a.label[d].empty() && !a.hasVid[d] && !a.hasCdg[d]) {
            ui.text(pane, L"AUDIO", 10, cDim, 1, true);
        }
        ui.frameRect(pane, act == d ? (d ? cB : cA) : cBorder, 4);
    }

    float gA = a.mixer.deckGain[0].load(), gB = a.mixer.deckGain[1].load();
    if (ui.sliderV(200, rc(cx - 70, r.top + 96, 40, 64), gA, cA))
        a.mixer.deckGain[0].store(gA);
    if (ui.sliderV(201, rc(cx + 30, r.top + 96, 40, 64), gB, cB))
        a.mixer.deckGain[1].store(gB);
    ui.text(rc(cx - 70, r.top + 160, 40, 14), L"A", 11, cA, 1, true);
    ui.text(rc(cx + 30, r.top + 160, 40, 14), L"B", 11, cB, 1, true);

    // Crossfader: deck-colored center fill (A blue left, B red right). During
    // a transition the thumb ANIMATES across at the fade's own pace — display
    // only, so the audio fade gains are never doubled up.
    float xf = a.mixer.crossfader.load();
    const int ftd = a.mixer.fadeTo.load();
    const float ftt = a.mixer.fadeT.load();
    const bool fading = ftd >= 0 && ftt >= 0.f;
    float shown = fading ? (ftd == 1 ? ftt : 1.f - ftt) : xf;
    if (ui.sliderH(202, rc(r.left + 16, r.top + 176, r.right - r.left - 32, 26),
                   shown, cB, true, &cA) &&
        !fading)
        a.mixer.crossfader.store(shown);

    const std::wstring amLabel =
        !a.automixOn ? L"AUTOMIX: CUT"
        : a.mixMode == MixMode::Smart ? L"AUTOMIX: SMART" : L"AUTOMIX: FADE";
    const D2D1_RECT_F amr = rc(r.left + 16, r.bottom - 38, 136, 28);
    if (ui.toggle(203, amr, amLabel, a.automixOn, cGreen)) a.automixOn = !a.automixOn;
    if (ui.in.rpressed && hit(amr, ui.in.rX, ui.in.rY))
        a.menu = {MenuReq::Automix, -1, ui.in.rX, ui.in.rY};

    if (ui.button(204, rc(r.right - 116, r.bottom - 38, 28, 28), L"−", cAccent))
        a.fadeSec = (std::max)(0.5, a.fadeSec - 0.5);
    wchar_t fb[24];
    swprintf(fb, 24, L"%.1fs", a.fadeSec);
    ui.text(rc(r.right - 86, r.bottom - 38, 42, 28), fb, 13, cText, 1, true);
    if (ui.button(205, rc(r.right - 42, r.bottom - 38, 28, 28), L"+", cAccent))
        a.fadeSec = (std::min)(10.0, a.fadeSec + 0.5);
}

// VDJ-style rhythm strip: both decks' waveforms scroll past a fixed center
// "now" line — deck A drawn upward, deck B downward, ±20 s window.
// ponytail: reuses the 400-bin full-track scan, so blocks are chunky on long
// tracks; a zoomed rescan around the playhead is the upgrade if it matters.
void drawRhythm(App& a, Ui& ui, const D2D1_RECT_F& r) {
    ui.rect(r, cInset, 8); // black well, VDJ-style
    ui.frameRect(r, cBorder, 8);
    const float midY = (r.top + r.bottom) / 2;
    ui.line(r.left + 6, midY, r.right - 6, midY, col(0x1B1B1F), 1);
    constexpr double win = 20.0; // seconds visible on each side of "now"
    const float cx = (r.left + r.right) / 2;
    const float pxPerSec = (cx - r.left - 6) / float(win);
    ui.clipPush(rc(r.left + 4, r.top + 3, r.right - r.left - 8, r.bottom - r.top - 6));
    for (int d = 0; d < 2; ++d) {
        const uint64_t total = a.decks[d]->totalFrames.load();
        const int readyN = a.wave[d].readyBins();
        if (!total || !readyN) continue;
        const double dur = double(total) / kRate;
        const double pos = double(a.decks[d]->framesPlayed.load()) / kRate;
        const D2D1_COLOR_F ac = d == 0 ? cA : cB;
        const float half = midY - r.top - 8;
        const double binSec = dur / WaveformScanner::kBins;
        const float bw = (std::max)(1.f, float(binSec * pxPerSec) - 0.5f);
        for (int i = 0; i < readyN; ++i) {
            const double t = (i + 0.5) * binSec;
            const float x = cx + float((t - pos) * pxPerSec) - bw / 2;
            if (x + bw < r.left + 4 || x > r.right - 4) continue;
            const float v = (std::min)(1.f, sqrtf(a.wave[d].bin(i)) * 1.15f);
            const float bh = (std::max)(1.5f, v * half);
            const D2D1_COLOR_F c = t <= pos ? ac : mix(ac, col(0x3A3A40), 0.6f);
            ui.rect(d == 0 ? rc(x, midY - bh, bw, bh) : rc(x, midY + 1, bw, bh), c,
                    0.5f);
        }
    }
    ui.clipPop();
    ui.rect(rc(cx - 1, r.top + 4, 2, r.bottom - r.top - 8), cText, 1); // now line
}

// Immediate children of dir among folders that contain media.
