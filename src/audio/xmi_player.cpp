#include "audio/xmi_player.h"

#include <algorithm>
#include <cmath>

#include <tsf.h>

#include "core/log.h"

namespace hailstone::audio {

namespace {
// The most events one tick may run. Counted loops nest four deep with counts up to 127, so a body that takes no time
// could run ~127^4 events in one tick on the audio thread. A real track plays a handful a tick (32 note slots), so a
// guard of ours far above that ends such a sequence instead (not the Miles sequencer's rule).
constexpr std::size_t kMaxEventsPerTick = 4096;
// The most one tick appends: one per event run, plus the 32 slots' note-offs for the durations and for each of at
// most three restarts (two in the loop, one in stop()), plus End.
static_assert(XmiSequencer::kMaxTickOutput == kMaxEventsPerTick + 32 * 4 + 1);
} // namespace

std::shared_ptr<SoundFont> SoundFont::load(std::span<const std::byte> sf2) {
    tsf* f = tsf_load_memory(sf2.data(), static_cast<int>(sf2.size()));
    if (!f) return nullptr;
    return std::shared_ptr<SoundFont>(new SoundFont(f));
}

SoundFont::~SoundFont() { tsf_close(font_); }

XmiSequencer::XmiSequencer(assets::XmiSequence seq, std::int32_t loops) : seq_(std::move(seq)), loops_(loops) {
    wait_ = seq_.events.empty() ? 0 : seq_.events.front().delay;
}

void XmiSequencer::restart(std::vector<Event>& out) {
    for (auto& s : slots_)
        if (s.channel >= 0) {
            out.push_back({Event::Kind::NoteOff, static_cast<std::uint8_t>(0x80 | s.channel), s.key, 0});
            s.channel = -1;
        }
    for (auto& l : for_) l = {};
    next_ = 0;
    wait_ = seq_.events.empty() ? 0 : seq_.events.front().delay;
}

void XmiSequencer::tick(std::vector<Event>& out) {
    if (finished_) return;
    // Durations first, releases in slot order.
    for (auto& s : slots_) {
        if (s.channel < 0) continue;
        if (--s.remaining <= 0) {
            out.push_back({Event::Kind::NoteOff, static_cast<std::uint8_t>(0x80 | s.channel), s.key, 0});
            s.channel = -1;
        }
    }
    if (wait_ > 0) --wait_;
    // Every event this loop runs is at the same tick. A second restart of the
    // track, or a second jump of an endless loop (CC117 on a count of 0), in
    // one tick means the body takes no time and would spin forever on the
    // audio thread: a guard of ours ends the sequence there instead.
    bool restarted = false, jumped = false;
    auto stop = [&] {
        restart(out);
        finished_ = true;
        out.push_back({Event::Kind::End});
    };
    std::size_t steps = 0;
    while (wait_ == 0 && !finished_) {
        if (++steps > kMaxEventsPerTick) {
            // Logged by the source's owner (~XmiSource, on the game thread): the log writes and flushes a file.
            overran_ = true;
            stop();
            return;
        }
        if (next_ >= seq_.events.size()) {
            // Ran off the end without an end-of-track event: treat it as one.
            if (loops_ == 1 || restarted) {
                stop();
                return;
            }
            restarted = true;
            if (loops_ > 1) --loops_;
            restart(out);
            if (seq_.events.empty()) { finished_ = true; return; }
            continue;
        }
        const assets::XmiEvent& e = seq_.events[next_++];
        wait_ = next_ < seq_.events.size() ? seq_.events[next_].delay : 0;
        std::uint8_t type = e.status & 0xf0u;
        if (e.status == 0xff) {
            if (e.meta_type == 0x2f) {
                if (loops_ == 1 || restarted) {
                    stop();
                    return;
                }
                restarted = true;
                if (loops_ > 1) --loops_;
                restart(out);
            }
            continue;
        }
        if (e.status >= 0xf0) continue;
        if (type == 0x90) {
            if (e.data2 == 0) continue;
            out.push_back({Event::Kind::NoteOn, e.status, e.data1, e.data2});
            auto free = std::find_if(std::begin(slots_), std::end(slots_), [](const Slot& s) { return s.channel < 0; });
            if (free == std::end(slots_)) {
                // The Miles sequencer reports an error and stops the sequence.
                stop();
                return;
            }
            free->channel = e.status & 0x0f;
            free->key = e.data1;
            free->remaining = e.duration;
            continue;
        }
        if (type == 0xb0 && e.data1 == 116) {
            // Loop start: the first free slot keeps the count and the cursor.
            for (auto& l : for_)
                if (!l.used) {
                    l = {e.data2, next_, true};
                    break;
                }
            continue;
        }
        if (type == 0xb0 && e.data1 == 117) {
            // Loop end: the highest used slot.
            for (int i = 3; i >= 0; --i) {
                Loop& l = for_[i];
                if (!l.used) continue;
                if (e.data2 < 64) {
                    l.used = false;
                } else if (l.count == 0) {
                    if (jumped) {
                        stop();
                        return;
                    }
                    jumped = true;
                    next_ = l.start;
                    wait_ = next_ < seq_.events.size() ? seq_.events[next_].delay : 0;
                } else if (--l.count > 0) {
                    next_ = l.start;
                    wait_ = next_ < seq_.events.size() ? seq_.events[next_].delay : 0;
                } else {
                    l.used = false;
                }
                break;
            }
            continue;
        }
        out.push_back({Event::Kind::Channel, e.status, e.data1, e.data2});
    }
}

namespace {

class XmiSource final : public Source {
public:
    XmiSource(std::shared_ptr<SoundFont> font, assets::XmiSequence seq, std::int32_t loops)
        : font_(std::move(font)), seq_(std::move(seq), loops) {
        synth_ = tsf_copy(font_->get());
        if (synth_) {
            tsf_set_max_voices(synth_, 128);
            // All 16 channels made here at their defaults (8192 is the centre of the pitch wheel): TinySoundFont
            // grows its channel array to the highest one used, which on the audio thread would reallocate.
            tsf_channel_set_pitchwheel(synth_, 15, 8192);
            tsf_channel_set_bank_preset(synth_, 9, 128, 0);
        }
        events_.reserve(XmiSequencer::kMaxTickOutput);
    }
    ~XmiSource() override {
        if (seq_.overran())
            log::warn("music: an XMI sequence ran over {} events in one tick (loops that take no time); stopped",
                      kMaxEventsPerTick);
        if (synth_) tsf_close(synth_);
    }

    std::size_t render(float* out, std::size_t frames, std::uint32_t rate) override {
        if (!synth_) return 0;
        if (rate != rate_) {
            rate_ = rate;
            tsf_set_output(synth_, TSF_STEREO_INTERLEAVED, static_cast<int>(rate), 0.0f);
        }
        const double per_tick = static_cast<double>(rate) / assets::kXmiTicksPerSecond;
        std::size_t done = 0;
        while (done < frames) {
            if (until_tick_ <= 0.0) {
                if (seq_.finished()) {
                    // Let the released notes ring out, then end.
                    if (tsf_active_voice_count(synth_) == 0 || ++tail_ticks_ > assets::kXmiTicksPerSecond * 2) return done;
                } else {
                    events_.clear();
                    seq_.tick(events_);
                    for (const auto& e : events_) play(e);
                }
                until_tick_ += per_tick;
            }
            auto n = static_cast<std::size_t>(std::ceil(until_tick_));
            n = std::clamp<std::size_t>(n, 1, frames - done);
            tsf_render_float(synth_, out + done * 2, static_cast<int>(n), 0);
            done += n;
            until_tick_ -= static_cast<double>(n);
        }
        return done;
    }

private:
    void play(const XmiSequencer::Event& e) {
        int ch = e.status & 0x0f;
        // Data bytes are 7 bits (assets/xmi.cpp masks them as it reads; kept here for any other source of events).
        const int d1 = e.data1 & 0x7f, d2 = e.data2 & 0x7f;
        switch (e.kind) {
        case XmiSequencer::Event::Kind::NoteOn: tsf_channel_note_on(synth_, ch, d1, static_cast<float>(d2) / 127.0f); break;
        case XmiSequencer::Event::Kind::NoteOff: tsf_channel_note_off(synth_, ch, d1); break;
        case XmiSequencer::Event::Kind::Channel:
            switch (e.status & 0xf0) {
            case 0x80: tsf_channel_note_off(synth_, ch, d1); break;
            case 0xb0: tsf_channel_midi_control(synth_, ch, d1, d2); break;
            case 0xc0: tsf_channel_set_presetnumber(synth_, ch, d1, ch == 9); break;
            case 0xe0: tsf_channel_set_pitchwheel(synth_, ch, (d2 << 7) | d1); break;
            default: break; // aftertouch: not supported by TinySoundFont
            }
            break;
        case XmiSequencer::Event::Kind::End: break;
        }
    }

    std::shared_ptr<SoundFont> font_;
    XmiSequencer seq_;
    tsf* synth_ = nullptr;
    std::uint32_t rate_ = 0;
    double until_tick_ = 0.0;
    int tail_ticks_ = 0;
    std::vector<XmiSequencer::Event> events_;
};

} // namespace

std::unique_ptr<Source> make_xmi_source(std::shared_ptr<SoundFont> font, assets::XmiSequence seq, std::int32_t loops) {
    if (!font) return nullptr;
    return std::make_unique<XmiSource>(std::move(font), std::move(seq), loops);
}

} // namespace hailstone::audio
