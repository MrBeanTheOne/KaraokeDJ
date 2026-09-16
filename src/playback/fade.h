#pragma once
#include <cmath>

enum class FadeCurve { EqualPower, Linear, FastCut, SlowBlend };

// Crossfader law ("smooth"/full-cut): p in [0,1], 0 = deck A only, 1 = deck B
// only, and the middle region leaves both decks at unity so a centered fader
// doesn't duck the active deck.
inline void xfadeGains(float p, float& gA, float& gB) {
    if (p < 0.f) p = 0.f;
    if (p > 1.f) p = 1.f;
    gA = 2.f * (1.f - p); if (gA > 1.f) gA = 1.f;
    gB = 2.f * p;         if (gB > 1.f) gB = 1.f;
}

// t in [0,1]: gain of the incoming deck (in) and outgoing deck (out).
inline void fadeGains(FadeCurve c, float t, float& in, float& out) {
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    switch (c) {
    case FadeCurve::Linear:
        in = t; out = 1.f - t;
        break;
    case FadeCurve::FastCut: // both stay loud, quick swap
        in = std::pow(t, 0.25f); out = std::pow(1.f - t, 0.25f);
        break;
    case FadeCurve::SlowBlend: { // smoothstep
        const float s = t * t * (3.f - 2.f * t);
        in = s; out = 1.f - s;
        break;
    }
    default: // EqualPower
        in = std::sin(t * 1.57079632679f);
        out = std::cos(t * 1.57079632679f);
        break;
    }
}
