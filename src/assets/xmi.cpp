#include "assets/xmi.h"

#include <cstring>
#include <stdexcept>
#include <string_view>

namespace hailstone::assets {

namespace {

struct Chunk {
    std::string_view id;
    std::span<const std::byte> body;
};

std::uint32_t be32(std::span<const std::byte> d, std::size_t at) {
    return (std::to_integer<std::uint32_t>(d[at]) << 24) | (std::to_integer<std::uint32_t>(d[at + 1]) << 16) |
           (std::to_integer<std::uint32_t>(d[at + 2]) << 8) | std::to_integer<std::uint32_t>(d[at + 3]);
}

std::string_view tag(std::span<const std::byte> d, std::size_t at) {
    return {reinterpret_cast<const char*>(d.data() + at), 4};
}

// The chunks of an IFF body, each {id, body}; sizes padded to even.
std::vector<Chunk> chunks(std::span<const std::byte> d) {
    std::vector<Chunk> out;
    std::size_t at = 0;
    while (at + 8 <= d.size()) {
        std::uint32_t size = be32(d, at + 4);
        if (size > d.size() - at - 8) throw std::runtime_error("xmi: chunk past the end of the file");
        out.push_back({tag(d, at), d.subspan(at + 8, size)});
        at += 8 + size + (size & 1u);
    }
    return out;
}

class EventReader {
public:
    explicit EventReader(std::span<const std::byte> d) : d_(d) {}
    bool done() const { return pos_ >= d_.size(); }
    std::uint8_t peek() const { return std::to_integer<std::uint8_t>(d_[pos_]); }
    std::uint8_t byte() {
        if (pos_ >= d_.size()) throw std::runtime_error("xmi: event past the end of EVNT");
        return std::to_integer<std::uint8_t>(d_[pos_++]);
    }
    // A MIDI data byte: 7 bits. A forged file's high bit would take a pitch wheel past 14 bits or a velocity past
    // 127 (the player divides it by 127); it is dropped, as a synth reading the byte as data would.
    std::uint8_t data() { return static_cast<std::uint8_t>(byte() & 0x7fu); }
    std::uint32_t vlq() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint8_t b = byte();
            v = (v << 7) | (b & 0x7fu);
            if (!(b & 0x80u)) return v;
        }
        throw std::runtime_error("xmi: variable-length quantity too long");
    }
    void skip(std::size_t n) {
        if (n > d_.size() - pos_) throw std::runtime_error("xmi: event past the end of EVNT");
        pos_ += n;
    }

private:
    std::span<const std::byte> d_;
    std::size_t pos_ = 0;
};

XmiSequence parse_events(std::span<const std::byte> evnt) {
    XmiSequence seq;
    EventReader r(evnt);
    std::uint32_t delay = 0;
    while (!r.done()) {
        if (r.peek() < 0x80) {
            delay += r.byte();
            continue;
        }
        XmiEvent e;
        e.delay = delay;
        delay = 0;
        e.status = r.byte();
        switch (e.status & 0xf0u) {
        case 0x90:
            e.data1 = r.data();
            e.data2 = r.data();
            e.duration = r.vlq();
            break;
        case 0x80:
        case 0xa0:
        case 0xb0:
        case 0xe0:
            e.data1 = r.data();
            e.data2 = r.data();
            break;
        case 0xc0:
        case 0xd0:
            e.data1 = r.data();
            break;
        default:
            if (e.status == 0xff) {
                e.meta_type = r.byte();
                r.skip(r.vlq());
            } else if (e.status == 0xf0 || e.status == 0xf7) {
                r.skip(r.vlq());
            } else {
                throw std::runtime_error("xmi: unknown event status");
            }
            break;
        }
        seq.events.push_back(e);
        if (e.status == 0xff && e.meta_type == 0x2f) break; // end of track
    }
    return seq;
}

void collect_forms(std::span<const std::byte> body, std::vector<XmiSequence>& out) {
    for (const Chunk& c : chunks(body)) {
        if (c.id != "FORM" || c.body.size() < 4 || tag(c.body, 0) != "XMID") continue;
        for (const Chunk& inner : chunks(c.body.subspan(4)))
            if (inner.id == "EVNT") {
                out.push_back(parse_events(inner.body));
                break;
            }
    }
}

} // namespace

std::vector<XmiSequence> parse_xmi(std::span<const std::byte> data) {
    std::vector<XmiSequence> out;
    for (const Chunk& top : chunks(data)) {
        if (top.body.size() < 4) continue;
        std::string_view type = tag(top.body, 0);
        if (top.id == "CAT " && type == "XMID") {
            collect_forms(top.body.subspan(4), out);
        } else if (top.id == "FORM" && type == "XMID") {
            // A file with one sequence and no XDIR header.
            collect_forms(data.subspan(static_cast<std::size_t>(top.body.data() - data.data()) - 8,
                                       top.body.size() + 8),
                          out);
        }
    }
    if (out.empty()) throw std::runtime_error("xmi: no XMIDI sequences");
    return out;
}

} // namespace hailstone::assets
