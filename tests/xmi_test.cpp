#include <doctest/doctest.h>

#include <string>

#include "assets/xmi.h"
#include "audio/xmi_player.h"
#include "game_files.h"

using namespace hailstone;
using namespace hailstone::assets;
using Ev = audio::XmiSequencer::Event;

namespace {

void append(std::vector<std::byte>& d, std::string_view s) {
    for (char c : s) d.push_back(static_cast<std::byte>(c));
}
void append_be32(std::vector<std::byte>& d, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) d.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xffu));
}
std::vector<std::byte> chunk(std::string_view id, const std::vector<std::byte>& body) {
    std::vector<std::byte> d;
    append(d, id);
    append_be32(d, static_cast<std::uint32_t>(body.size()));
    d.insert(d.end(), body.begin(), body.end());
    if (body.size() & 1u) d.push_back(std::byte{0});
    return d;
}
std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> d;
    for (int b : v) d.push_back(static_cast<std::byte>(b));
    return d;
}

// FORM XDIR + CAT XMID with the given EVNT bodies.
std::vector<std::byte> xmi_file(const std::vector<std::vector<std::byte>>& events) {
    std::vector<std::byte> info = bytes({static_cast<int>(events.size()), 0});
    std::vector<std::byte> xdir;
    append(xdir, "XDIR");
    auto c = chunk("INFO", info);
    xdir.insert(xdir.end(), c.begin(), c.end());
    std::vector<std::byte> cat;
    append(cat, "XMID");
    for (const auto& e : events) {
        std::vector<std::byte> form;
        append(form, "XMID");
        auto t = chunk("TIMB", bytes({0, 0}));
        form.insert(form.end(), t.begin(), t.end());
        auto ev = chunk("EVNT", e);
        form.insert(form.end(), ev.begin(), ev.end());
        auto f = chunk("FORM", form);
        cat.insert(cat.end(), f.begin(), f.end());
    }
    auto out = chunk("FORM", xdir);
    auto cc = chunk("CAT ", cat);
    out.insert(out.end(), cc.begin(), cc.end());
    return out;
}

std::vector<std::vector<Ev>> run(audio::XmiSequencer& s, int ticks) {
    std::vector<std::vector<Ev>> out;
    for (int i = 0; i < ticks; ++i) {
        out.emplace_back();
        s.tick(out.back());
    }
    return out;
}

} // namespace

TEST_CASE("XMI: sequences, delays, note durations, end of track") {
    // Program change, note C4 for 2 ticks, delay 3 (two bytes 1+2), CC7, end.
    auto evnt = bytes({0xc0, 5, 0x90, 60, 100, 2, 1, 2, 0xb0, 7, 90, 0xff, 0x2f, 0});
    auto seqs = parse_xmi(xmi_file({evnt, bytes({0xff, 0x2f, 0})}));
    REQUIRE(seqs.size() == 2);
    const auto& e = seqs[0].events;
    REQUIRE(e.size() == 4);
    CHECK(e[0].status == 0xc0);
    CHECK(e[1].status == 0x90);
    CHECK(e[1].duration == 2);
    CHECK(e[2].delay == 3);
    CHECK(e[2].data2 == 90);
    CHECK(e[3].meta_type == 0x2f);
}

TEST_CASE("XMI: a long variable-length duration") {
    auto seqs = parse_xmi(xmi_file({bytes({0x91, 64, 1, 0x81, 0x00, 0xff, 0x2f, 0})}));
    CHECK(seqs[0].events[0].duration == 128);
    CHECK_THROWS(parse_xmi(bytes({'F', 'O', 'R', 'M', 0, 0, 0, 50})));
}

TEST_CASE("XMI: data bytes are 7 bits, whatever a forged file holds") {
    auto seqs = parse_xmi(xmi_file({bytes({0x90, 0xbc, 0xff, 1, 0xe0, 0xff, 0xff, 0xc0, 0x85, 0xff, 0x2f, 0})}));
    const auto& e = seqs[0].events;
    REQUIRE(e.size() == 4);
    CHECK(e[0].data1 == 0x3c);
    CHECK(e[0].data2 == 0x7f);
    CHECK(e[1].data1 == 0x7f);
    CHECK(e[1].data2 == 0x7f);
    CHECK(e[2].data1 == 0x05);
}

TEST_CASE("XMI sequencer: releases before events, one-tick minimum, end stops notes") {
    // tick 1: note A (dur 2) and note B (dur 0); 2 ticks later C; end 3 ticks after that.
    auto evnt = bytes({0x90, 60, 100, 2, 0x90, 62, 100, 0, 2, 0x90, 64, 100, 10, 3, 0xff, 0x2f, 0});
    auto seq = parse_xmi(xmi_file({evnt}))[0];
    audio::XmiSequencer s(seq, 1);
    auto t = run(s, 7);
    REQUIRE(t[0].size() == 2);
    CHECK(t[0][0].kind == Ev::Kind::NoteOn);
    CHECK(t[1].size() == 1);                // B released after one tick
    CHECK(t[1][0].kind == Ev::Kind::NoteOff);
    CHECK(t[1][0].data1 == 62);
    REQUIRE(t[2].size() == 2);              // A's release comes before C's note-on
    CHECK(t[2][0].kind == Ev::Kind::NoteOff);
    CHECK(t[2][0].data1 == 60);
    CHECK(t[2][1].kind == Ev::Kind::NoteOn);
    CHECK(t[2][1].data1 == 64);
    REQUIRE(t[5].size() == 2);              // the end releases C (still held) and ends
    CHECK(t[5][0].kind == Ev::Kind::NoteOff);
    CHECK(t[5][1].kind == Ev::Kind::End);
    CHECK(s.finished());
    CHECK(t[6].empty());
}

TEST_CASE("XMI sequencer: loop controllers 116/117 and looping the whole sequence") {
    // CC116 count 2, note, delay 1, CC117 127, end.
    auto evnt = bytes({0xb0, 116, 2, 0x90, 60, 100, 1, 1, 0xb0, 117, 127, 0xff, 0x2f, 0});
    auto seq = parse_xmi(xmi_file({evnt}))[0];
    audio::XmiSequencer once(seq, 1);
    int notes = 0;
    for (const auto& tick : run(once, 20))
        for (const auto& e : tick) notes += e.kind == Ev::Kind::NoteOn;
    CHECK(notes == 2);
    CHECK(once.finished());
    audio::XmiSequencer forever(seq, 0);
    notes = 0;
    for (const auto& tick : run(forever, 40))
        for (const auto& e : tick) notes += e.kind == Ev::Kind::NoteOn;
    CHECK(notes > 4);
    CHECK_FALSE(forever.finished());
}

TEST_CASE("XMI sequencer: a loop that takes no time ends instead of spinning") {
    // Only an end of track, looping forever: every restart lands on the same tick.
    auto seq = parse_xmi(xmi_file({bytes({0xff, 0x2f, 0})}))[0];
    audio::XmiSequencer only_end(seq, 0);
    auto t = run(only_end, 2);
    CHECK(only_end.finished());
    REQUIRE_FALSE(t[0].empty());
    CHECK(t[0].back().kind == Ev::Kind::End);
    // An endless CC116/CC117 loop with nothing between them.
    seq = parse_xmi(xmi_file({bytes({0xb0, 116, 0, 0xb0, 117, 127, 1, 0xff, 0x2f, 0})}))[0];
    audio::XmiSequencer empty_loop(seq, 1);
    run(empty_loop, 2);
    CHECK(empty_loop.finished());
    // A whole sequence of zero-time events looped a few times still ends.
    seq = parse_xmi(xmi_file({bytes({0xb0, 7, 100, 0xff, 0x2f, 0})}))[0];
    audio::XmiSequencer counted(seq, 1000000);
    run(counted, 2);
    CHECK(counted.finished());
    CHECK_FALSE(counted.overran());
}

TEST_CASE("XMI sequencer: nested counted loops that take no time end within the tick") {
    // Four loops of 127 around a controller, nothing taking time: ~127^4 events in one tick without the cap.
    std::vector<std::byte> body;
    for (int i = 0; i < 4; ++i)
        for (auto b : bytes({0xb0, 116, 127})) body.push_back(b);
    for (auto b : bytes({0xb0, 7, 100})) body.push_back(b);
    for (int i = 0; i < 4; ++i)
        for (auto b : bytes({0xb0, 117, 127})) body.push_back(b);
    for (auto b : bytes({0xff, 0x2f, 0})) body.push_back(b);
    auto seq = parse_xmi(xmi_file({body}))[0];
    audio::XmiSequencer nested(seq, 1);
    auto t = run(nested, 1);
    CHECK(nested.finished());
    REQUIRE_FALSE(t[0].empty());
    CHECK(t[0].back().kind == Ev::Kind::End);
    CHECK(t[0].size() <= audio::XmiSequencer::kMaxTickOutput); // what the source reserves
    CHECK(nested.overran()); // logged by the source on the game thread, not from tick()
    // Two loops of 3 around a note that takes time still play every pass.
    seq = parse_xmi(xmi_file({bytes({0xb0, 116, 3, 0xb0, 116, 3, 0x90, 60, 100, 1, 1, 0xb0, 117, 127, 0xb0, 117, 127,
                                      0xff, 0x2f, 0})}))[0];
    audio::XmiSequencer counted(seq, 1);
    int notes = 0;
    for (const auto& tick : run(counted, 40))
        for (const auto& e : tick) notes += e.kind == Ev::Kind::NoteOn;
    CHECK(notes == 9);
}

TEST_CASE("XMI files of the installed game") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    auto gfay = game->read("gfaydark.xmi");
    REQUIRE(gfay);
    CHECK(parse_xmi(*gfay).size() == 6);
    auto qeynos = game->read("qeynos.xmi");
    REQUIRE(qeynos);
    auto seqs = parse_xmi(*qeynos);
    CHECK(seqs.size() == 13);
    // Every sequence ends with an end-of-track event.
    for (const auto& s : seqs) {
        REQUIRE_FALSE(s.events.empty());
        CHECK(s.events.back().meta_type == 0x2f);
    }
    if (auto sf2 = game->read("synthusr.sf2")) CHECK(audio::SoundFont::load(*sf2) != nullptr);
}
