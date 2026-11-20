# Mock EQEmu server (offline integration tests)

An in-process, loopback-only EQEmu stand-in for tests that drive a real `LoginClient`, `GameConnection` or `Session`. No game files, no server, no network beyond `127.0.0.1`. Everything runs in the normal `hailstone_tests` binary (`tests/mock_*_test.cpp`, about 4 s together).

| File | What it is |
|---|---|
| `soe_endpoint.{h,cpp}` | The server end of one session on UDP, built on the client's own codec (`net/session_codec.h`): session handshake, CRC and the compression/XOR passes, acks as EQEmu sends them (cumulative, out of order, again for a duplicate), OP_Packet/OP_Fragment/OP_Combined in both directions, unreliable packets, keepalive, disconnect. It does nothing by itself; `poll()` receives. |
| `mock_eqemu.{h,cpp}` | The scripts: login (handshake, DES login, server list, play), world (auth, guild list, character list, enter world, zone server info, zoning re-auth), zone (profile, own spawn, spawns, time, inventory, weather, new zone; then zone points, AA stats, the zone-in echoes). Plus triggers for gameplay traffic (chat, spawn add/remove/move, HP, items, zone requests). |
| `profile_builder.{h,cpp}` | OP_PlayerProfile bytes (the client has no encoder for it). Shared with `net_packets_test.cpp`. |
| `harness.h` | `Rig`: a `GameConnection` and a `MockEqServer` driven from one loop, and `pump()`. |

Packets are made with the client's own `encode()` where it has one ("server direction, for tests"), so a layout bug the client shares with its encoder would not show here; the layouts themselves are pinned byte for byte by `net_packets_test.cpp`. The rest are built from the documented layouts (`docs/protocol/rof2-packets.md`).

## Faults are scripted, not random

`script_outgoing({Fate::Drop, Fate::Duplicate, Fate::Hold, ...})` decides what happens to the next datagrams the server sends; `release_held(reverse)` delivers the held ones; `script_incoming(...)` does the same for the client's datagrams that carry a reliable packet; `drop_next_acks(n)` loses the server's acks. The server's own resends are explicit (`resend_unacked()`), as is every wait: tests pump until a condition holds (a 10 s deadline only catches a hang) and use a marker packet behind the one under test where something must *not* happen, because reliable delivery is in order.

## Timers

`GameConnection` reads `std::chrono::steady_clock` directly. `Session` does too unless `SessionOptions::clock` is set: `tests/manual_clock.h`'s `ManualClock` follows the steady clock until `hold()`, then moves only by `advance()`, which the idle-timeout tests use so a descheduled test thread cannot let the link die. `ConnectionOptions::session` (`SessionOptions`) shortens the session timers (multi-buffer hold, service pass, resend adjust, idle and connect timeouts): `harness.h`'s `quick_options()` does, and one test keeps the default timers. What is still wall clock: the 280 ms position interval (the cadence test runs ~1 s), the 500 ms zone-change retry. Not covered offline: the camp's 30 s timer and its OP_Logout, the 180 s zone-change wait, the 30 s stat request, the 180 s idle timeout of the default options at its real length (a held `ManualClock` covers it in `mock_session_test.cpp`; the GameConnection tests run the same code with a shortened limit).

## Not scripted

OP_SendMembership(+Details), tributes, OP_XTargetResponse at zone-in, doors and objects, character creation, mail, and the chat server's own protocol (the `ucs` socket only records what arrives).
