// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.9.5 (zip 5): the sounds of the menu, made by the program itself (no sound files, nothing to
// license): short effects for moving, choosing, going back, changing tab and starting a game, and a calm
// ambient loop (slow chords with a few bell notes and an echo) for the background.
//
// Everything here is plain arithmetic on arrays, so it is tested on a PC (tests/menu_test.cpp checks the
// lengths, the levels, that no sound starts or ends with a click and that the loop joins itself without a
// jump). The console side (rom_menu.cpp, MenuAudio) only opens an audio port and calls Mixer::Render.
//
// Samples are 48 kHz, 16 bit, stereo, interleaved (left, right).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Menu {

/// What the menu can say with a sound.
enum class SoundId : std::uint8_t {
    Move,   // the highlight moved, or a value stepped
    Select, // something was chosen or switched
    Back,   // back out of a screen
    Tab,    // another tab
    Play,   // a game was started
};
inline constexpr int SoundCount = 5;
inline constexpr int SampleRate = 48000;
/// How the menu audio is switched (settings.txt: menu_audio): music and sounds, sounds only, nothing.
enum class AudioMode : std::uint8_t { All, Sounds, Off };

using Samples = std::vector<std::int16_t>;

namespace SoundDetail {

inline constexpr double TwoPi = 6.283185307179586;

/// A sine oscillator by recurrence (two multiplications per sample, no sin() call): y[n] = 2cos(w) y[n-1] - y[n-2].
struct Osc {
    double k = 0, y1 = 0, y2 = 0;
    Osc(double hz, double phase = 0.0) {
        const double w = TwoPi * hz / SampleRate;
        k = 2.0 * std::cos(w);
        y1 = std::sin(phase - w);
        y2 = std::sin(phase - 2.0 * w);
    }
    double Next() {
        const double y = k * y1 - y2;
        y2 = y1;
        y1 = y;
        return y;
    }
};

/// A stereo mix in floating point, `frames` long. Notes are added into it; with `wrap` a note that runs past the end
/// continues at the start, so a loop has no seam.
struct Mix {
    int frames;
    bool wrap;
    std::vector<float> l, r; // float: a 32 s loop in doubles would take 3 x 24 MiB of the console's memory while it is made
    Mix(int frames_, bool wrap_) : frames(frames_), wrap(wrap_), l(size_t(frames_), 0.0f), r(size_t(frames_), 0.0f) {}

    void Add(long frame, double left, double right) {
        if (wrap) {
            frame = ((frame % frames) + frames) % frames;
        } else if (frame < 0 || frame >= frames) {
            return;
        }
        l[size_t(frame)] += float(left);
        r[size_t(frame)] += float(right);
    }
};

inline double Smooth(double x) { // 0..1 -> 0..1, flat at both ends
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

/// A bell: a sine with a quieter octave and a quieter, higher partial that dies faster; it starts in 3 ms and fades
/// out by itself (`decay` is the time constant, in seconds) and is cut with a fade over its last 20 ms.
inline void Bell(Mix& mix, double start_s, double hz, double length_s, double gain, double pan, double decay) {
    const long start = long(start_s * SampleRate);
    const int n = int(length_s * SampleRate);
    Osc a(hz), b(2.0 * hz), c(2.76 * hz);
    const int attack = std::max(1, int(0.003 * SampleRate)), release = std::max(1, int(0.02 * SampleRate));
    // pan 0 = left, 1 = right; equal power, so the middle is not louder than the sides
    const double left_gain = std::sqrt(1.0 - std::clamp(pan, 0.0, 1.0)), right_gain = std::sqrt(std::clamp(pan, 0.0, 1.0));
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / SampleRate;
        double env = std::min(1.0, double(i) / attack) * std::exp(-t / decay);
        if (i > n - release) {
            env *= double(n - i) / release;
        }
        const double v = a.Next() + 0.30 * b.Next() * std::exp(-t / (0.5 * decay)) + 0.10 * c.Next() * std::exp(-t / (0.25 * decay));
        mix.Add(start + i, v * env * gain * left_gain, v * env * gain * right_gain);
    }
}

/// A pad note: sines of the note and its first two overtones, a slow rise and a slow fall, the right channel a hair
/// sharper than the left, which makes the slow shimmer of a chorus.
inline void Pad(Mix& mix, double start_s, double hz, double length_s, double rise_s, double fall_s, double gain) {
    const long start = long(start_s * SampleRate);
    const int n = int(length_s * SampleRate);
    const int rise = std::max(1, int(rise_s * SampleRate)), fall = std::max(1, int(fall_s * SampleRate));
    Osc l1(hz * 0.9988), l2(2.0 * hz * 0.9988), l3(3.0 * hz * 0.9988);
    Osc r1(hz * 1.0012, 1.3), r2(2.0 * hz * 1.0012, 0.7), r3(3.0 * hz * 1.0012, 2.1);
    for (int i = 0; i < n; ++i) {
        const double env = Smooth(double(i) / rise) * Smooth(double(n - i) / fall);
        const double left = l1.Next() + 0.38 * l2.Next() + 0.14 * l3.Next();
        const double right = r1.Next() + 0.38 * r2.Next() + 0.14 * r3.Next();
        mix.Add(start + i, left * env * gain, right * env * gain);
    }
}

/// The mix as 16 bit samples, scaled so its loudest sample is `peak` (0..1) of full scale.
inline Samples Finish(const Mix& mix, double peak) {
    float loudest = 1e-9f;
    for (int i = 0; i < mix.frames; ++i) {
        loudest = std::max({loudest, std::fabs(mix.l[size_t(i)]), std::fabs(mix.r[size_t(i)])});
    }
    const double scale = peak * 32767.0 / double(loudest);
    Samples out(size_t(mix.frames) * 2);
    for (int i = 0; i < mix.frames; ++i) {
        out[size_t(i) * 2] = std::int16_t(std::lround(double(mix.l[size_t(i)]) * scale));
        out[size_t(i) * 2 + 1] = std::int16_t(std::lround(double(mix.r[size_t(i)]) * scale));
    }
    return out;
}

} // namespace SoundDetail

/// One effect, at most about a second long, its loudest sample at 0.7 of full scale.
inline Samples MakeEffect(SoundId id) {
    using namespace SoundDetail;
    const double length = id == SoundId::Play ? 1.4 : id == SoundId::Select ? 0.55 : 0.4;
    Mix mix(int(length * SampleRate), false);
    switch (id) {
    case SoundId::Move: // a soft tick
        Bell(mix, 0.0, 880.0, 0.12, 0.8, 0.5, 0.03);
        break;
    case SoundId::Select: // two notes going up
        Bell(mix, 0.0, 659.26, 0.35, 0.7, 0.4, 0.09);
        Bell(mix, 0.075, 987.77, 0.45, 0.8, 0.6, 0.13);
        break;
    case SoundId::Back: // two notes going down
        Bell(mix, 0.0, 783.99, 0.25, 0.7, 0.6, 0.07);
        Bell(mix, 0.065, 523.25, 0.3, 0.8, 0.4, 0.09);
        break;
    case SoundId::Tab: // a small step up
        Bell(mix, 0.0, 523.25, 0.2, 0.6, 0.4, 0.06);
        Bell(mix, 0.05, 783.99, 0.25, 0.7, 0.6, 0.07);
        break;
    case SoundId::Play: // an arpeggio that opens up, over a low note
        Pad(mix, 0.0, 130.81, 1.3, 0.04, 0.9, 0.35);
        Bell(mix, 0.0, 523.25, 0.9, 0.6, 0.4, 0.30);
        Bell(mix, 0.09, 659.26, 0.9, 0.6, 0.6, 0.30);
        Bell(mix, 0.18, 783.99, 0.9, 0.6, 0.4, 0.30);
        Bell(mix, 0.27, 1046.5, 1.0, 0.7, 0.6, 0.38);
        Bell(mix, 0.36, 1318.5, 1.0, 0.5, 0.5, 0.40);
        break;
    }
    return Finish(mix, 0.7);
}

/// The background: `seconds` (a multiple of 4, 32 by default) of four slow chords (C, Am, F, G) with bell notes and an
/// echo, its loudest sample at 0.8 of full scale. The last frame leads straight into the first: notes that run past
/// the end continue at the start and the echo is circular, so the loop has no seam.
inline Samples MakeAmbient(int seconds = 32) {
    using namespace SoundDetail;
    seconds = std::max(4, seconds - seconds % 4);
    const double chord_s = seconds / 4.0;
    struct Chord {
        double pad[5];
        double bells[4];
    };
    static const Chord chords[4] = {
        {{130.81, 196.00, 329.63, 493.88, 587.33}, {659.26, 783.99, 587.33, 1046.5}},  // C major 9
        {{110.00, 164.81, 261.63, 392.00, 493.88}, {523.25, 659.26, 880.00, 783.99}},  // A minor 9
        {{87.31, 130.81, 220.00, 329.63, 392.00}, {880.00, 1046.5, 783.99, 659.26}},   // F major 9
        {{98.00, 146.83, 246.94, 329.63, 440.00}, {587.33, 783.99, 987.77, 880.00}},   // G six nine
    };
    static const double pad_gain[5] = {0.55, 0.45, 0.35, 0.22, 0.12};
    static const double bell_at[4] = {0.125, 0.375, 0.5625, 0.8125}; // fractions of a chord
    Mix pad(seconds * SampleRate, true), bells(seconds * SampleRate, true);
    for (int c = 0; c < 4; ++c) {
        const double begin = c * chord_s;
        for (int n = 0; n < 5; ++n) {
            // The note rises over 2.5 s and falls over 3 s, so each chord leans into the next one.
            Pad(pad, begin, chords[c].pad[n], chord_s + 2.5, 2.5, 3.0, pad_gain[n]);
        }
        for (int n = 0; n < 4; ++n) {
            Bell(bells, begin + bell_at[n] * chord_s, chords[c].bells[n], 3.2, 0.22, n % 2 == 0 ? 0.3 : 0.7, 1.4);
        }
    }
    // The echo: the bells come back 0.52 s and 1.04 s later, the first in the other ear, going round the loop.
    const int d1 = int(0.52 * SampleRate), d2 = int(1.04 * SampleRate), n = pad.frames;
    Mix all(n, true);
    for (int i = 0; i < n; ++i) {
        const size_t a = size_t((i - d1 + n) % n), b = size_t((i - d2 + n) % n), at = size_t(i);
        all.l[at] = pad.l[at] + bells.l[at] + 0.38 * bells.r[a] + 0.18 * bells.l[b];
        all.r[at] = pad.r[at] + bells.r[at] + 0.38 * bells.l[a] + 0.18 * bells.r[b];
    }
    return Finish(all, 0.8);
}

/// Mixes the background loop and the effects into the grains the audio port is fed with. Trigger() and the setters are
/// called from the menu's thread, Render() from the audio thread.
class Mixer {
public:
    Mixer() {
        for (int i = 0; i < SoundCount; ++i) {
            effects_[size_t(i)] = MakeEffect(SoundId(i));
        }
    }

    /// The background loop (made on the audio thread, since it takes a moment). Until it is set there is no music.
    void SetAmbient(Samples loop) {
        std::lock_guard lock{mutex_};
        ambient_ = std::move(loop);
        ambient_pos_ = 0;
    }
    void SetMusic(bool on) { music_.store(on); }
    void SetEffects(bool on) { effects_on_.store(on); }
    /// 0 to 1.
    void SetVolume(float volume) { volume_.store(std::clamp(volume, 0.0f, 1.0f)); }
    /// Everything goes down to silence over `seconds` (the menu is about to close the port). After it, Quiet() is true.
    void FadeOut(float seconds) { fade_step_.store(1.0f / (std::max(seconds, 0.01f) * SampleRate)); fading_.store(true); }
    bool Quiet() const { return fading_.load() && master_.load() <= 0.0f; }

    /// Queues an effect; up to 8 play at once, the next one cuts the oldest.
    void Trigger(SoundId id) {
        if (!effects_on_.load()) {
            return;
        }
        std::lock_guard lock{mutex_};
        if (pending_.size() < 8) {
            pending_.push_back(id);
        }
    }

    /// How many effects are playing or waiting (for the tests).
    int Active() const {
        std::lock_guard lock{mutex_};
        return int(voices_.size() + pending_.size());
    }

    /// `frames` stereo frames into `out` (2 * frames samples).
    void Render(std::int16_t* out, int frames) {
        std::lock_guard lock{mutex_};
        for (const SoundId id : pending_) {
            if (voices_.size() >= 8) {
                voices_.erase(voices_.begin());
            }
            voices_.push_back({size_t(id), 0});
        }
        pending_.clear();
        const float volume = volume_.load();
        const bool fading = fading_.load();
        const float fade_step = fade_step_.load();
        float master = master_.load();
        const float target = music_.load() && !ambient_.empty() ? 1.0f : 0.0f;
        const float step = 1.0f / (0.6f * SampleRate); // the music comes in and goes out over 0.6 s
        for (int i = 0; i < frames; ++i) {
            float l = 0.0f, r = 0.0f;
            if (music_level_ != target) {
                music_level_ = target > music_level_ ? std::min(target, music_level_ + step) : std::max(target, music_level_ - step);
            }
            if (music_level_ > 0.0f && !ambient_.empty()) {
                const float g = music_level_ * 0.42f;
                l += float(ambient_[ambient_pos_]) * g;
                r += float(ambient_[ambient_pos_ + 1]) * g;
                ambient_pos_ += 2;
                if (ambient_pos_ >= ambient_.size()) {
                    ambient_pos_ = 0;
                }
            }
            for (Voice& v : voices_) {
                const Samples& e = effects_[v.effect];
                if (v.pos + 1 < e.size()) {
                    l += float(e[v.pos]) * 0.85f;
                    r += float(e[v.pos + 1]) * 0.85f;
                    v.pos += 2;
                }
            }
            if (fading && master > 0.0f) {
                master = std::max(0.0f, master - fade_step);
            }
            out[size_t(i) * 2] = Clip(l * volume * master);
            out[size_t(i) * 2 + 1] = Clip(r * volume * master);
        }
        master_.store(master);
        voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                     [&](const Voice& v) { return v.pos + 1 >= effects_[v.effect].size(); }),
                      voices_.end());
    }

private:
    struct Voice {
        size_t effect;
        size_t pos;
    };
    static std::int16_t Clip(float x) {
        return std::int16_t(std::clamp(x, -32767.0f, 32767.0f));
    }

    mutable std::mutex mutex_;
    std::array<Samples, SoundCount> effects_;
    Samples ambient_;
    size_t ambient_pos_ = 0;
    float music_level_ = 0.0f;
    std::vector<Voice> voices_;
    std::vector<SoundId> pending_;
    std::atomic<bool> music_{true}, effects_on_{true};
    std::atomic<float> volume_{0.7f};
    std::atomic<bool> fading_{false};
    std::atomic<float> fade_step_{0.0f}, master_{1.0f};
};

} // namespace Menu
