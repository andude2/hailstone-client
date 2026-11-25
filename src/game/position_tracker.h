#pragma once

// The position history the client reports with OP_FloatListThing (0x46c6; EQEmu's Handle_OP_MovementHistoryList
// feeds its cheat checks, and flags a client that sends none for 70 s). From the disassembly of the original:
//   PositionTracker_GetInstance @0x56e7f0     a 0x34-byte singleton: a node pool (grown by 100), the recorded
//                                             nodes (+0x14 head, +0x18 tail), +0x1c the sample count, +0x1e the
//                                             entry count, +0x20 enabled, +0x21 the last moving sample, +0x2d
//                                             whether there is one, +0x30 the last sample's time
//   PositionTracker_RecordPosition @0x56e570  records a sample (below)
//   0x56e680                              writes the oldest samples as 17-byte entries
//   0x56e510                              forgets every sample; +0x1c = +0x1e
//   Send_OP_FloatListThing @0x58c9c0          sends them (GameConnection::send_position_history)
//   PlayerClient_RecordPositionSample @0x58ff80  the local player's position (or a given one) as a sample
// Independent of the network; the times are __EQGetTime milliseconds.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace hailstone::game {

// The sample reasons the original records (EQEmu's UpdateMovementType names in brackets).
enum PositionReason : std::uint32_t {
    kPositionMove = 1,       // the last good position saved after a move (Collision)
    kPositionUpdate = 2,     // before an OP_ClientUpdate (TeleportB)
    kPositionTeleport = 3,   // a teleport the server or a GM made (TeleportA)
    kPositionZoneLine = 4,   // a zone line or teleport region was entered (ZoneLine)
    kPositionNoTeleport = 5, // the region's teleport failed
    kPositionHit = 7,        // a knock-back (SpellA)
    kPositionSpell = 9,      // a spell moved the player (SpellB)
};

class PositionTracker {
public:
    struct Sample {
        std::array<float, 3> position{}; // the spawn's +0x64.. order: Y, X, Z (world x, y, the spawn's z)
        std::uint32_t reason = 0;
        std::uint32_t time = 0;
    };
    // What RecordPosition checks before it records anything.
    struct Gate {
        bool in_game = false;        // CEverQuest GameState (+0x5c8) is 5
        bool zone_connected = false; // dword 0xdd25dc
        bool controlled = false;     // pinstControlledPlayer is set and not a corpse (+0x125 < 2)
        bool main_loop = false;      // byte 0xddf644: DoMainLoop is running (1 from its start until it returns)
    };

    // DoMainLoop's start (@0x53d461): a tracker that was off forgets its samples, then it is on.
    void enable();
    bool enabled() const { return enabled_; }

    // RecordPosition: true when the sample must be sent at once (reasons 3, 4, 5 and 8: Send_OP_FloatListThing(1)).
    // Outside the main loop moving samples (reason 1) are not taken; in it, a moving sample at the same position as
    // the last one less than 1 s after the last sample is not taken.
    bool record(const std::array<float, 3>& position, std::uint32_t reason, std::uint32_t now, const Gate& gate);

    // 0x56e680(buffer, force): the oldest samples, at most 10000, each as {f32 Y, f32 X, f32 Z, u8 reason, u16
    // the sample time's low word, u16 the entry count (counting up)}; without `force` it stops at a sample not older
    // than 2 s. Returns the payload (count x 17 + 1 bytes; the last byte is not written by the original: 0 here),
    // empty when no sample was taken.
    std::vector<std::byte> take(bool force, std::uint32_t now);

    // 0x56e510.
    void flush();

    bool empty() const { return samples_.empty(); }
    std::size_t size() const { return samples_.size(); }
    // +0x1c: copied into every OP_ClientUpdate's sequence field (DoMainLoop, DoTeleportB).
    std::uint16_t sequence() const { return sequence_; }
    std::uint16_t entries() const { return entries_; }
    // Continue the monotonic OP_ClientUpdate / OP_FloatListThing counters across a control-lease handoff so the
    // server does not drop post-swap position updates as out-of-order.
    void restore(std::uint16_t sequence, std::uint16_t entries) {
        sequence_ = sequence;
        entries_ = entries;
    }

private:
    std::deque<Sample> samples_;
    std::uint16_t sequence_ = 0;  // +0x1c
    std::uint16_t entries_ = 0;   // +0x1e
    bool enabled_ = false;        // +0x20
    std::array<float, 3> last_{}; // +0x21
    bool has_last_ = false;       // +0x2d
    std::uint32_t last_time_ = 0; // +0x30
};

} // namespace hailstone::game
