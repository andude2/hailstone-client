#pragma once

// Helpers shared by the typed packet encoders and decoders
// (world_packets.cpp, zone_packets.cpp): opcode-tagged packet building,
// NUL-terminated string writing, exception-to-optional decoding, bitfield
// access and the EQ CRC.

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "core/byte_io.h"
#include "net/opcodes.h"
#include "net/packet.h"

namespace hailstone::net {

inline AppPacket make_packet(Op op, ByteWriter&& w) { return AppPacket(to_wire(op), w.take()); }

// A packet with no payload (OP_ReqClientSpawn, OP_ClientReady, OP_Logout, ...).
inline AppPacket make_empty(Op op) { return AppPacket(to_wire(op)); }

// True when the packet carries this opcode.
inline bool is_op(const AppPacket& p, Op op) { return to_wire(op) != 0 && p.opcode == to_wire(op); }

// The EQ CRC32: the standard reflected table (polynomial 0xedb88320), start
// value 0xffffffff and no final inversion, i.e. ~zlib_crc32(data). The client
// uses it as __MemChecker0 @005643f0 (file checksums, OP_ZoneEntry's name
// hash) and EQEmu as CRC32::SetEQChecksum (OP_PlayerProfile).
std::uint32_t eq_crc32(std::span<const std::byte> data);

namespace detail {

inline void write_cstring(ByteWriter& w, std::string_view s) {
    w.bytes({reinterpret_cast<const std::byte*>(s.data()), s.size()});
    w.le<std::uint8_t>(0);
}

// Runs a decoder body; a read past the end (or any range error) gives nullopt, and so does an allocation the
// packet's counts made too large: a decoder never throws into the handler loop.
template <typename F> auto decode_with(const AppPacket& p, F&& body) -> std::optional<decltype(body(std::declval<ByteReader&>()))> {
    try {
        ByteReader r(p.data);
        return body(r);
    } catch (const std::out_of_range&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

// Unsigned field of `bits` bits starting at bit `shift` of a little-endian word.
constexpr std::uint32_t bits_u(std::uint32_t word, unsigned shift, unsigned bits) {
    return (word >> shift) & ((bits == 32) ? 0xffffffffu : ((1u << bits) - 1u));
}

// Signed (two's complement) field.
constexpr std::int32_t bits_s(std::uint32_t word, unsigned shift, unsigned bits) {
    std::uint32_t v = bits_u(word, shift, bits);
    std::uint32_t sign = 1u << (bits - 1);
    return static_cast<std::int32_t>(v ^ sign) - static_cast<std::int32_t>(sign);
}

// Places the low `bits` bits of `value` at `shift`.
constexpr std::uint32_t put_bits(std::int64_t value, unsigned shift, unsigned bits) {
    std::uint32_t mask = (bits == 32) ? 0xffffffffu : ((1u << bits) - 1u);
    return (static_cast<std::uint32_t>(value) & mask) << shift;
}

} // namespace detail
} // namespace hailstone::net
