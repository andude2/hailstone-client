# External resources for Hailstone

Survey of public projects, specs and data sources that can help build Hailstone. Everything listed here was checked on **2026-10-20**. GitHub repositories were confirmed through the GitHub API (licence, language and last push date). Web pages were fetched or confirmed to respond. A few community sites (eqinterface.com, sourceforge.net, brewall.com) block scripted access, and the report says so where that applies.

## How to read the licence column

Hailstone is GPL-2.0-or-later (NOTICE.md), its plugin SDK (`sdk/`) MIT.

| Licence class | What we may do |
|---|---|
| **MIT / BSD / ISC / zlib / CC0 / Unlicense** | We may port or vendor the code with credit in `THIRD_PARTY.md`. |
| **GPL-2.0 / GPL-2.0-or-later / LGPL-2.1** | We may port the code with credit in `THIRD_PARTY.md`, keeping its notice; a file with GPL-2.0-only code (MacroQuest) says so in its SPDX line. Never into `sdk/`. |
| **Apache-2.0 (only) / GPL-3.0 / AGPL / MPL file-level** | **Reference only.** Their terms are not compatible with GPL version 2, so copying them would bind the client to version 3. Read them to learn the format or behaviour, then write our own code from the facts (a separate Apache-2.0 library beside the binary, such as MoltenVK, is not copied code). File formats and protocol facts can't be copyrighted, so learning them this way is fine. |
| **No licence (none on GitHub)** | All rights reserved by default. We may **read** it, including its docs, for facts. Don't copy code or text. |
| **Restricted custom licence** | Same as no licence, or stricter (see EQ: Requiem). |

"Last activity" means the date of the last push to the repository.

Usefulness ratings: **High** means we should use it on the critical path of a milestone. **Medium** means a second source or cross-check. **Low** means niche or historical.

---

## 1. Asset formats

### 1.1 PFS / S3D / WLD (classic zones and models)

| Name | URL | Licence | Lang | Last activity | What it covers | Use | Caveats |
|---|---|---|---|---|---|---|---|
| **libeq** (cjab) | https://github.com/cjab/libeq | MIT | Rust | 2026-03 | `libeq_pfs` (read and write S3D/PFS, CRC) and `libeq_wld`, a full WLD parser with **49 fragment-type modules** that round-trip (parse and serialise). Also the `s3d` and `wld-cli` tools. Two other EQ clients (eq-client, eqoxide) are built on it. | **High (M1, M4)**. This is the cleanest permissive WLD implementation and our best port source for fragment layouts. `wld-cli` can dump fragments for testing. | No EQG support upstream. Later-era zone WLDs fail its strict parsing; see the djhenry fork below. |
| libeq fork (djhenry) | https://github.com/djhenry/libeq | MIT (fork) | Rust | 2026-10 | Adds `libeq_eqg` (zone, mesh and lighting modules), plus a `fix/tolerant-zone-wld-parsing` branch that accepts later-era zone WLDs. | Medium (M1, M6). It shows which RoF2-era WLDs break a strict parser. | Young code with a narrow scope. |
| **LanternExtractor** (LanternEQ) | https://github.com/LanternEQ/LanternExtractor | MIT | C# | 2025-12 | Trilogy-era S3D/WLD extraction to a text intermediate format, OBJ and glTF. Covers zone meshes, collision, vertex colours, **BSP regions**, ambient and placed lights, **music and sound instances**, object vertex animation, **skeletal animation**, skins and equipment. `ClientData/animationsources.txt` is a table of which races borrow animations from which model (e.g. `HUM,ELM`). | **High (M1, M4, M8)**. These are the most battle-tested semantics for what the fragments *mean*, not only their layout. The animation-source table is essential for M4. | Trilogy scope only, with no Luclin or EQG. Its "post-Velious" support is listed as future work. |
| LanternUnityTools | https://github.com/LanternEQ/LanternUnityTools | MIT | C# | 2026-09 | Imports Lantern's intermediate files into Unity: shaders for EQ blend modes, sky, doors and instancing. | Medium (M1). Shows how one renderer port mapped EQ's material types. | Unity-specific. |
| LanternGoExtract | https://github.com/tmyhres/LanternGoExtract | MIT | Go | 2026-01 | Go port of LanternExtractor, including a `sound` package with an `_sndbnk.eff` parser. | Low to medium. A second reading of Lantern in another language. | Single maintainer. |
| **WLD by Windcatcher** (EQEmu docs) | https://docs.eqemu.dev/client/guides/file-formats/wld-by-windcatcher/ (source: https://github.com/EQEmu/eqemu-docs-v2 `docs/client/guides/file-formats/wld-by-windcatcher.md`) | No licence on the docs repo | Markdown | docs repo 2026-05 | Transcription of Windcatcher's 2004 `wlddoc.pdf`. Covers fragments 0x03–0x37 field by field, with notes and the differences between old and new 0x36 formats. | **High (M1)**. The canonical spec that every other implementation cites. | Old. It has errors that libeq and Lantern later corrected, so cross-check against them. |
| WCEmu definitions (EQEmu docs) | https://docs.eqemu.dev/client/wcemu/latest/ | None stated | Markdown | 2026-05 | Text form of every WLD fragment using the tool keywords (`ACTORDEF`, `DMSPRITEDEF2`, `HIERARCHICALSPRITEDEF` …), with each field's printf format. This is quail's `.wce` format. | Medium (M1, M4). Gives names for fields, which helps naming in our code. | Describes text form, not byte layout. |
| wld-doc.github.io | https://wld-doc.github.io/ (repo https://github.com/Wld-Doc/wld-doc.github.io) | None | JS + Kaitai | 2025-12 | Documents the True3D/`.wld` family (an early beta variant, Tanarus, Return to Krondor) with **Kaitai Struct** definitions and JS parsers. | Low. Interesting for Kaitai-generated fuzz tests, and for origin history. | Pre-release variant, and marked WIP. |
| OpenEQ wiki (old) | https://github.com/daeken/OpenEQ/wiki/WLD | n/a | wiki | old | Header and fragment notes from the earlier OpenEQ generation. | Low. We already have OpenEQ's newer docs. | Superseded. |
| **EQEmu zone-utilities** | https://github.com/EQEmu/zone-utilities | GPL-2.0 | C++ | 2024-10 | `pfs`, `s3d_loader`, `wld_fragment`, `s3d_bsp`, `s3d_skeleton_track`, `eqg_loader`, `eqg_model_loader`, **`eqg_v4_loader`** (v4 text zones and terrain tiles), `eqg_terrain`, `eqg_water_sheet`, `eqg_invis_wall`, `eqg_region`. Its tools are `azone` (writes `.map`), `awater` (writes `.wtr`) and `map_edit` (writes `.nav`). | **High as a reference (M1, M6, nav)**. It is the only C++ code that loads S3D, EQG v1–3 *and* v4 zones together. | **GPL**, so read it but don't copy. Its geometry handling is aimed at server collision, not rendering, so it skips materials and shaders. |
| eqsage (knervous) | https://github.com/knervous/eqsage | **None** | JS | 2025-03 | Browser converter, viewer and zone editor. `sage/lib/s3d/*` covers WLD, BSP, animation, skeleton, track, particle sprite, sound and lights. `sage/lib/eqg/*` covers the EQG decoder and encoder, model, skeleton, weight buffer, `v4-zone.js`, `eco`, `tog` and glTF export for v3 and v4. | Medium (M4, M6). A readable cross-check for EQG v4 zones and skeletal export. | No licence, so read only. |
| EQZip (Shendare) | https://github.com/Shendare/EQZip | None | C# | 2022-01 | Full S3D/EQG archive manager (read and write). | Low (tooling). Useful for inspecting archives by hand. | No licence. |
| s3d / s3d_extract (kevr) | https://github.com/kevrgithub/s3d | None | C++ | 2013 | Minimal PFS reader. | Low. | Old, no licence. |
| S3DModelExtracter, ZEQ-API, ZEQClient (Zaela) | https://github.com/Zaela/S3DModelExtracter, https://github.com/Zaela/ZEQ-API, https://github.com/Zaela/ZEQClient | None | C / C++ | 2014–2016 | WLD model extraction, plus OpenGL drawing of EQ models and an Irrlicht client experiment. | Low. Historical. | No licence. |
| eq-js-zoneloader | https://github.com/wesparish/eq-js-zoneloader | None | JS | 2026-07 | Dependency-free WebGL2 viewer: PFS, inflate, WLD, BMP/DDS decoding and collision, written in about 25 minutes with Claude Code. | Low. It shows that the M1 core is small. | No licence, and a toy. |

### 1.2 EQG family (MOD, MDS, TER, ZON, ANI, LIT, LOD, PRT/PTS, EDD, ECO, TOG, LAY, v4 zones)

| Name | URL | Licence | Lang | Last activity | What it covers | Use | Caveats |
|---|---|---|---|---|---|---|---|
| **quail** (xackery) | https://github.com/xackery/quail | MIT | Go | 2025-07 (**archived**) | The widest EQG coverage we found. `raw/` holds read and write code for **ani, bmp/dds/png/tga/jpg, dat (fe, iw, wtr, zon), def, eco, edd, lay, lit, lod, mds, mod, obg, prt, pts, rfd, ter, tog, wld, zon (including `zon_read_v4`)**. `raw/rawfrag/` holds every WLD fragment. Also a `.wce` text round-trip and tests. | **High (M6, also M1, M4)**. MIT, so we can port the readers directly. It is our primary EQG source. | Archived, and was "early preview" with known gaps (its checklist marks v4 zones and parts of S3D incomplete). Several active forks exist (e.g. `dannuic/quail`) but none is clearly the successor. |
| quail-addon | https://github.com/xackery/quail-addon (archived), https://github.com/dariusuknuis/quail-addon (active fork, 2026-09) | MIT | Python | 2025 / 2026 | Blender import of S3D and EQG files through quail. | Low (tooling). Useful for viewing models when debugging. | — |
| eqgzi (xackery) | https://github.com/xackery/eqgzi (archived) | MIT | C++ | 2025-04 | Zone importer evolved from Zaela's EQGZoneImporter. Writes EQG v1–3 zones (TER, ZON, MOD). | Medium (M6). Permissive C++ that *writes* EQG, so its structs mirror the readers. | Archived. |
| EQGZoneImporter, EQGModelImporter, EQGWeaponModelImporter (Zaela) | https://github.com/Zaela/EQGZoneImporter (and siblings) | None | C++ | 2014–2021 | The first EQG writers. | Low. | No licence. |
| **EQGZI docs: shader list** | https://docs.eqemu.dev/client/eqgzi/shader-list/ | None | md | 2026-05 | **72 EQG `.fx` shader names**, with example zones and material properties (`e_TextureDiffuse0`, `e_TextureNormal0`, `e_TextureEnvironment0`, `e_fSlide1X/Y`, `e_fFresnelPower`, `e_fWaterColor1` …). | **High (M6)**. This is the checklist for our EQG shader families. | Gives names and properties, not the shader maths. |
| ZON versions table (EQEmu docs) | https://docs.eqemu.dev/client/zon-versions/ | None | md | 2026-05 | Each zone's `.zon` version (1, 2 or 4). | Medium (M6). A ready test matrix. | Hand-made list. |
| EQ Particle Editor (Zaela) | https://github.com/Zaela/EQEmuParticleEditor | None | C | 2014 | `EDD` (spell emitter definitions) record layout, with about 60 partly named fields (`density`, `emission_delay`, `fadeout_time`, tints, `image_segments`, `rotation_speed` …). | Medium (M9 spell effects). | No licence. Many fields unnamed. quail's `edd_read.go` (MIT) covers the same format. |
| EQRaceInventory (Shendare) | https://github.com/Shendare/EQRaceInventory | None | C++ | 2024-04 | Program and **released HTML tables for RoF2**: every race ID mapped to model code and source file (global, `_chr`, EQG), and which zones load each one. | **High (M4)**. Ground truth for the race-to-model table for RoF2. | No licence on the code. The tables can be regenerated from a RoF2 asset set. |
| Race Files / IT model files / Model Loading (EQEmu docs) | https://docs.eqemu.dev/client/race-files/, https://docs.eqemu.dev/client/it-model-files/, https://docs.eqemu.dev/client/guides/model-loading/ | None | md | 2026-05 | About 725 race rows (ID, model codes, `_chr` file). IT item model numbers. The `<zone>_chr.txt` and global load-list format (count line, then `model,file`). | Medium (M4). | Gaps (races 35–60 incomplete), and no EQG column. |

### 1.3 Rendering behaviour docs (EQEmu docs, mostly from LanternEQ research)

| Page | URL | What it gives | Use |
|---|---|---|---|
| Fog System and Clip Plane | https://docs.eqemu.dev/client/guides/fog-system-and-clip-plane/ | Per-level fog start and end formulas for the 21-step clip slider, fog colour scaled by an hourly blend table, the MinFog == MaxFog special case, and FogClipMin ≥ 30. | **High (M1)**. |
| Sky System | https://docs.eqemu.dev/client/guides/sky-system/ | 5 sky variants with textures, layer pan speeds and directions, render order, no depth writes, and camera-locked placement. | High (M1/M9). |
| Sounds Reference | https://docs.eqemu.dev/client/guides/sounds/ | Catalogue of WAV names by `snd*.pfs` archive (UI, player, environment, mob). | Medium (M8). |

---

## 2. Network protocol

| Name | URL | Licence | Lang | Last activity | What it covers | RoF2? | Use | Caveats |
|---|---|---|---|---|---|---|---|---|
| **EQEmu server: reliable stream** | https://github.com/EQEmu/EQEmu `common/net/reliable_stream_connection.{h,cpp}`, `reliable_stream_structs.h`, `crc32.cpp`, `eqstream.cpp`, `common/eq_stream_ident.cpp` | GPL-3.0 | C++ | 2026-10 | The server side of the reliable-UDP session protocol: session request and response, sequencing, acks, fragments, combined packets, compression, CRC keys and stream identification.  | Yes (client-agnostic) | **High (M2)**. It is what our client must interoperate with. | GPL, so reference only. Use it to check behaviour. |
| EQEmu RoF2 patch + loginserver | `common/patches/rof2.cpp`, `rof2_structs.h`, `rof2_ops.h`, `rof2_limits.*`, `utils/patches/patch_RoF2.conf`; `loginserver/encryption.cpp`, `login_types.h`, `client.cpp` | GPL-3.0 | C++ | 2026-10 | Wire contract for RoF2; login packets and DES credential handling. | Yes | Already in our plan (listed for completeness). | GPL. |
| **eq-network** (eq-p99-tools) | https://github.com/eq-p99-tools/eq-network | **MIT** | Rust | 2026-10 (active daily) | Crates: `eq-network-transport` (session framing, sequencing, fragmentation, compression, keyed CRC, retransmit, stats, in `soe.rs`, `fragment.rs`, `combined.rs`, `legacy.rs`, `modern.rs`, `crc.rs`), `eq-network-login` (login packets, **DES-CBC with zero key and IV**, server list), `eq-network-game` (world and zone codecs) and `eq-network` (full session client: login → server select → char select → zone, reconnect, typed events and commands). | **No.** Titanium (EQEmu), P99, TAKP/EQMac. | **High (M2)**. The only MIT, actively maintained, complete EQ client network stack. The transport and login layers don't depend on client version, so we can port them almost directly. | The game layer is Titanium/TAKP, so its packet structs don't apply. A capability table documents what has been tested live. |
| **eqoxide** (djhenry) | https://github.com/djhenry/eqoxide | **None** | Rust (wgpu) | 2026-10 (active daily) | A standalone **RoF2** EQEmu client: login, char select, zone in, movement, combat, casting, merchants, loot, trade, quests, inventory. Renders zones and skinned characters with armour textures, using libeq. Docs: `docs/protocol-notes.md` ("This client speaks RoF2", with a provenance tag per section), `docs/rof2-migration.md` (how EQEmu identifies a RoF2 stream: `OP_SendLoginInfo` 0x7a09 at 464 bytes, `OP_ZoneEntry` 0x5089 at **76** bytes), `character-models.md`, `equipment-textures-findings.md`, `collision-system.md`, `zone-rendering.md`. | **Yes** | **High as reading (M2–M4, M7)**. The only other RoF2 client we found. Its notes list the traps they hit (dead `#if 0` structs in `rof2_structs.h`, ENCODE vs `zone/` serialiser). | **No licence, so read only.** It needs its own asset server (`eqoxide_asset_server`, Rust, no licence), which converts S3D to glTF. Its targets are AI agents, not people playing. |
| scry-eq (ShowEQ revival) | https://github.com/scry-eq/showeq (GPL-2.0, C), https://github.com/scry-eq/scry-cpp (GPL-2.0), https://github.com/scry-eq/scry-decoder-rs (GPL-2.0, Rust), https://github.com/scry-eq/scry-qt (MIT) | mixed | C / C++ / Rust | 2026-06 to 2026-09 | ShowEQ split into a capture daemon, decoder and Qt/web front ends, using protobuf. | Live servers, not RoF2 | Low to medium. Session-layer decode logic and opcode-hunting methods. | GPL core, and it follows the live game. |
| ShowEQ (classic) | http://www.showeq.net/, mirror https://github.com/lucianposton/showeq | GPL-2.0 | C++ | mirror 2020 | Packet analyser. Its SourceForge SVN tags hold `worldopcodes.xml` / `zoneopcodes.xml` per live patch. | Tags around 2013 are close to RoF2's era | Low. EQEmu's `patch_RoF2.conf` already covers this. | GPL. sourceforge.net blocks scripted access. |
| EQExtractor (EQEmu) | https://github.com/EQEmu/EQExtractor | None | C# | 2014 | Converts pcaps to SQL. Has **per-patch decoders and opcode configs for 2010–2014 live patches** (`patch_March13-2013`, `April15-2013` … `May19-2014`), the era RoF2 comes from. | Close to RoF2 | Medium (M3). Independent decoders for spawn, door, zone-point and object packets from RoF2's time. | No licence. Its live patch dates don't match RoF2's exactly. |
| EQPacketParser | https://github.com/daerath/EQPacketParser | MIT | C# | 2019 | Decompresses captures and splits combined and oversized packets. | n/a | Low to medium. Useful for a pcap debugging tool. | — |
| EQNet (Zaela) | https://github.com/Zaela/EQNet | None | C++ | 2015 | Client networking library for EQEmu: ack manager, compression, CRC, login, world, chat, combat, inventory, with opcode translation tables for several clients. | Older clients | Low. | No licence. |
| Sanctuary.SoeProtocol | https://github.com/PS2Sanctuary/Sanctuary.SoeProtocol | ISC | C# / Zig | 2026-05 | **Written docs** (`docs/`) plus an implementation of the related SOE protocol v3, as used by PlanetSide 2, H1Z1, Landmark and Free Realms. Links to further implementations (LibSOE, soe-dissector, h1z1-server, swg). | EQ's protocol is from the same family, but its variant differs | Medium (M2). The best written explanation of the session state machine and of reliable-channel windowing. | Not EQ-specific: CRC, encoding and opcode details must be checked against EQEmu. |
| soe-protocol (FabledRealms) | https://github.com/FabledRealmsProject/soe-protocol | MIT | Rust | 2026-07 | A pure state machine with no I/O, ported from Sanctuary. Documents a **16-bit sequence-wraparound bug** in the ack-all throttle. | Same family | Medium (M2). A good design pattern (time passed in, buffers in and out) and a bug to avoid. | AI-assisted port of a PS2 variant. |
| soe-dissector | https://github.com/misterupkeep/soe-dissector | GPL-2.0 | Lua | 2020 | Wireshark dissector for the related reliable UDP protocol. | Same family | Low to medium. Debug aid: could be adapted for EQ captures. | GPL (tooling only, so not an issue). |
| Packet and OpCode Analysis (EQEmu docs) | https://docs.eqemu.dev/developer/packet-and-opcode-analysis/ | — | md | — | How to capture and decode packets, and how to find opcodes. | — | Low. | No session-layer spec. |

**Finding:** there is no complete standalone written spec of the RoF2 session protocol variant. The fullest sources are EQEmu's `reliable_stream_connection` (GPL, server side), eq-network-transport (MIT client) and the Sanctuary docs (generic v3).

---

## 3. Other open-source EQ clients and viewers

| Name | URL | Licence | Lang / engine | Last activity | Protocol | State | Lessons and use |
|---|---|---|---|---|---|---|---|
| **eqoxide** | see section 2 | None | Rust, wgpu | 2026-10 | **RoF2** | Plays real characters. Renders zones, skinned races with armour, weapons and doors. EQG zones not yet converted. | Closest to our target protocol. Its docs are worth reading (see section 2). |
| **eq-client** (eq-p99-tools) | https://github.com/eq-p99-tools/eq-client | **MIT** | Rust, Bevy | 2026-10 (daily) | Titanium / TAKP / P99 | Native top-down client with an offline viewer, login UI, char select, spellbook and inventory windows, max-FPS handling from `eqclient.ini`, and screenshot tests. `eq-client-axes` defines EQ's mirrored-handedness conversion **in one place**. `eq-client-assets` reads **SIDL** (`sidl.rs`, 89 KB: `Ui2DAnimation`, `WindowDrawTemplate`, pieces), sounds, maps, regions and `eqlsstr_us.txt` strings at runtime from the player's files. | **High (M1, M5)**. MIT, so the SIDL reader and axis-convention module can be ported. Its "read skin data at runtime, never copy it" design matches ours. |
| **WillEQ** | https://github.com/wcassis/willeq | GPL-3.0 | C++17, Irrlicht (software, GL 2.1, GLES2) | 2026-10 | Titanium | Login through zoning, S3D/WLD zones (PVS, frustum, portal culling), animated textures, sky (60+ types), weather, skeletal models with equipment and tints, inventory, merchants, trading, groups, **spatial audio** (miniaudio + TinySoundFont for XMI, minimp3, dr_wav), `.eff` emitters, creature, door and player sounds, Recast navmesh. Runs on ARM SBCs. Has a `CLAUDE.md`, and the author describes it as "mostly vibe-coded". | **High as reference (M1, M4, M8)**. The closest C++ analogue to Hailstone. Its `docs/reference/eff_sound_format.md` (84-byte `_sounds.eff` records) and `eq_audio_assets_catalog.md` are good facts. Its audio stack choice validates ours. | 
| WillEQ sibling "whatever" | https://github.com/wcassis/whatever | None | — | 2025-12 | Titanium | Earlier repo of the same project. | Skip. |
| open-norrath | https://github.com/dashkto/open-norrath | GPL-3.0 | Scala | 2026-03 | EQEmu | Experimental. | Low. |
| **EQ: Requiem** (knervous) | https://github.com/knervous/eqrequiem | **Restricted custom licence** (no reuse, no AI training) | TS, Babylon.js, WebTransport, Go | 2026-08 (archived as read-only) | Its own server | An EQ-style browser MMO using EQ assets, with WebGPU paths. | **Avoid.** The licence explicitly forbids reuse and ML use. Mentioned only so nobody mines it by accident. |
| requiemfiles | https://github.com/knervous/requiemfiles | GPL-2.0 | JS | 2024-12 | — | Asset manifest for Requiem. | Low. |
| OpenEQ | https://github.com/daeken/OpenEQ | MIT OR Apache-2.0 (Cargo.toml; no LICENSE file) | Rust, wgpu | 2026-10 | EQEmu | Already our main secondary source. | — |
| EQWOWConverter | https://github.com/NathanHandley/EQWOWConverter | Custom ("other") | C# | 2026-10 | n/a | Converts EQ zones, creatures and items into WoW 3.3.5 formats, using LanternExtractor output. Highly active. | Low to medium. Another, independent semantic reading of zones (regions, liquids, sounds, music). |
| eql-atlas | https://github.com/foraern/eql-atlas | None | JS | 2026-09 | — | 3D viewer for EQ map files. | Low. |

The **most complete** projects, in order: eqoxide (RoF2, gameplay), WillEQ (Titanium, broadest features, C++), eq-client plus eq-network (Titanium/TAKP, cleanest design, MIT), and OpenEQ.

---

## 4. UI (SIDL, STML, skins)

| Name | URL | Licence | What it covers | Use | Caveats |
|---|---|---|---|---|---|
| | eq-client `sidl.rs` | https://github.com/eq-p99-tools/eq-client/blob/main/crates/eq-client-assets/src/sidl.rs | MIT | Runtime SIDL parser for `Ui2DAnimation`, `WindowDrawTemplate`, borders, gauges, labels, images and screen pieces, read from the player's skin. | **High (M5)**. A permissive port source. | Covers only the windows that client draws. |
| eqinterface.com | https://www.eqinterface.com/ (e.g. https://www.eqinterface.com/patches/live_3.14.07/EQUI_InspectWnd.html) | — | Historical archive of EQUI files per patch, skin downloads, and forum threads on SIDL attributes such as `EQType`. | Medium (M5). Explains what an attribute is *for*. | Blocks scripted fetches (403), so open it in a browser. Mostly 2002–2010. |
| SIDL Fiddle | https://github.com/mbarbre1/Fiddle | GPL-3.0 | Java EQ UI designer (a SIDL renderer). | Low to medium. A second SIDL layout implementation. | GPL, and last active 2019. |
| EQUIEditor | https://github.com/anotheregostar/EQUIEditor | GPL-3.0 | C# graphical editor for EQ UI XML. | Low. | GPL. |
| DiaWind (EQEmu docs) | https://docs.eqemu.dev/quest-api/diawind/ | — | Server-side markup for popup/dialogue windows; relevant to the STML the server sends. | Low (M7). | — |

**Finding:** apart from eq-client's and OpenEQ's partial readers, there is no open, complete SIDL or STML renderer, and no written STML spec.

---

## 5. Audio

| Name | URL | Licence | Lang | Last activity | What it covers | Use | Caveats |
|---|---|---|---|---|---|---|---|
| **xmi2mid (Corsix)** | https://github.com/CorsixTH/CorsixTH `CorsixTH/Src/xmi2mid.cpp` | **MIT** (file header, © 2009 Peter Cawley) | C++ | 2026-10 | XMI (multi-sequence) to Standard MIDI conversion in about 400 lines. | **High (M8)**. Vendor it. | — |
| xmi_to_midi (knervous) | https://github.com/knervous/xmi_to_midi | None at repo level, but `xmi.cpp` is Corsix's MIT code | C++ / WASM | 2024-09 | Corsix's converter, adapted through EQEmu Tools (KLS) to **emit every sequence in an EQ `.xmi` as its own MIDI file** (EQ zone XMIs hold several tracks, selected by `xmiIndex`). Example: `qeytoqrg.xmi`. | **High (M8)**. Shows the EQ-specific multi-track handling. | Take the code from CorsixTH, and treat this repo's changes as reference. |
| **TinySoundFont + tml.h** | https://github.com/schellingb/TinySoundFont | MIT | C (single header) | 2026-07 | SF2 synthesiser plus a MIDI file loader. WillEQ uses it for EQ music. | **High (M8)**. | `tml.h` has no XMI support, so convert first. |
| miniaudio | https://github.com/mackron/miniaudio | Public domain / MIT-0 (dual) | C | 2026-10 | Cross-platform audio output, mixing, 3D spatialisation (attenuation, doppler, cones), and decoders for WAV, MP3 and FLAC. | **High (M8)**. One dependency covers output, 3D sound and decoding on Linux, Windows and macOS. | Its spatialiser is simple, which suits EQ's simple model. |
| minimp3 / dr_libs | https://github.com/lieff/minimp3 (CC0), https://github.com/mackron/dr_libs (PD/MIT-0) | CC0 / PD | C | 2026 | MP3, WAV and FLAC decoding. | Medium (M8). Only needed if we don't use miniaudio's built-in decoders. | — |
| OpenAL Soft | https://github.com/kcat/openal-soft | LGPL-2.0+ | C++ | 2026-10 | Full 3D audio with HRTF and EFX reverb. | Medium (M8). An option if we want the EAX-style echo that `.emt` `EchoLevel` implies. | LGPL, so dynamic linking only. |
| Steam Audio | https://github.com/ValveSoftware/steam-audio | Apache-2.0 | C++ | 2026-03 | HRTF, occlusion and reverb. | Low (later improvement). | Heavy. |
| FluidSynth | https://github.com/FluidSynth/fluidsynth | LGPL-2.1 | C | 2026-10 | Reference-quality SF2 synth. | Medium (M8). An alternative to TSF. | LGPL. |
| WildMIDI | https://github.com/Mindwerks/wildmidi | LGPLv3 (library) / GPLv3 (player) | C | 2026-08 | GUS-patch MIDI synth that **loads XMI directly** (via Ryan Nunn's LGPL code). | Low. | LGPL, and GUS patches instead of SF2. |
| libADLMIDI | https://github.com/Wohlstand/libADLMIDI | GPL-3.0 / LGPL | C++ | 2026-10 | OPL3 FM synth that reads XMI. | Low. EQ's music targets General MIDI, not OPL. | — |
| **GeneralUser GS** | https://github.com/mrbumpy409/GeneralUser-GS | Custom permissive (v2.0): "use without restriction … feel free to use it in your software projects" | SF2 (about 30 MB) | 2026-02 | A General MIDI soundfont with a small memory footprint. | **High (M8)**. A good default soundfont to download on first run. | The author can't vouch for every sample's origin, and asks that sites not hot-link his downloads. |
| FluidR3_GM | Debian `fluid-soundfont` (https://metadata.ftp-master.debian.org/changelogs/main/f/fluid-soundfont/unstable_copyright) | MIT | SF2 (about 140 MB) | — | General MIDI soundfont by Frank Wen. | Medium (M8). The cleanest licence, but large. | — |
| **Eff2Emt** (Shendare) | https://github.com/Shendare/Eff2Emt (also Eff2EmtGUI) | **CC0** (stated in README) | C++ | 2015 | Converts classic `_sounds.eff` (binary) plus `_sndbnk.eff` (text) into `.emt`. The code fully decodes the binary records. | **High (M8)**. CC0, so we can port it freely. | — |
| `.emt` field list (EQGZI sound how-to) | https://docs.eqemu.dev/client/eqgzi/sound-howto/ (source: `eqemu-docs-v2/docs/client/eqgzi/sound-howto.md`) | — | md | 2026-05 | The CSV columns: `type, SoundFile, unk, WhenActive (0 always, 1 day, 2 night), Volume, FadeInMS, FadeOutMS, WavLoopType, X, Y, Z, FullVolRadius, MaxAudibleDist, RandomizeLocation, ActivationRange, MinRepeatDelay, MaxRepeatDelay, xmiIndex, EchoLevel, IsEnvSound`, with examples. | **High (M8)**. | Community-derived. Check against the server's `.emt` files. |
| WillEQ `eff_sound_format.md` | https://github.com/wcassis/willeq/blob/main/docs/reference/eff_sound_format.md | GPL-3.0 (repo) | md | 2026 | `_sndbnk.eff` sections (`EMIT`, `LOOP`, `RAND`) and the **84-byte `_sounds.eff` record**. | Medium (M8). | Facts only. Don't copy the text. |
| Eff→Emt forum thread | https://www.eqemulator.org/forums/showthread.php?t=39747 | — | forum | 2015 | Shendare's notes on the format. | Medium. | — |

**3D sound model:** miniaudio's spatialiser (per-sound min/max distance with simple rolloff) is a close fit for the `.emt` fields `FullVolRadius` and `MaxAudibleDist`.

---

## 6. Data files

| Item | URL | Licence | What it covers | Use | Caveats |
|---|---|---|---|---|---|
| **EQEmu docs: spells** | https://docs.eqemu.dev/server/spells/ with sub-pages `spell-effect-fields/`, `spell-effect-ids/`, `client-spell-id-limitations/`; schema https://docs.eqemu.dev/schema/spells/spells_new/ | — | Spell effect (SPA) IDs and the base/limit/max semantics of each. `spells_new` has **237 columns** (`id` … `field236`), many marked "Unknown". | **High (M7)**. | The schema page doesn't state that column order matches `spells_us.txt`. That is implied by the exporter (next row); confirm against the exporter. |
| EQEmu client file exporter | https://github.com/EQEmu/EQEmu `client_files/export/main.cpp` (and `client_files/import/main.cpp`) | GPL-3.0 | Writes `spells_us.txt` (from `spells_new`, `^`-separated), `SkillCaps.txt`, `BaseData.txt` and `dbstr_us.txt` in the exact layouts RoF2 reads. | **High (M7)**. Shows the exact line format, and the server and client must agree on it. | GPL, but the formats are facts. |
| DBStr types | https://docs.eqemu.dev/server/operation/dbstr-types/ | — | `dbstr_us.txt` type IDs 0–20 (AA title, AA description, spell description, race, class, deity, start city, augment, currency, expansion …) plus the RoF additions 21–26 and 71. | **High (M5, M7)**. | Line format (`id^type^text^…`) isn't on that page; read the exporter. |
| base_data / skill_caps / db_str schema | https://docs.eqemu.dev/schema/client-files/base_data/ (and `skill_caps/`, `db_str/`) | — | Column meanings for BaseData and SkillCaps. | Medium (M7). | — |
| EQEmu common constants | `common/races.{h,cpp}`, `classes.*`, `eq_constants.h`, `emu_constants.*`, `textures.*`, `light_source.*`, `say_link.*` (item link body format), `skill_caps.*`, `spdat.{h,cpp}` | GPL-3.0 | Race and class tables, equipment texture slots, the **RoF2 item-link body layout** in `say_link.cpp`, and SPA enums. | High (M4, M7). | GPL, so take facts and tables only. |
| Race List / Client Race Inventory / Client Version Bitmasks | https://docs.eqemu.dev/server/npc/race-list/, `…/client-race-inventory/`, `…/player/client-version-bitmasks/` | — | Race IDs and names, and which client has which race. | Medium (M4). | — |
| eqspellparser (rumstil) | https://github.com/rumstil/eqspellparser | Apache-2.0 | C# parser for `spells_us.txt` plus `spells_us_str.txt`, and SPA-to-English descriptions. | Medium (M7). Its spell-description generator is a portable reference (Apache). | It follows the **live** layout, which differs from RoF2 (live split strings into a second file). Map the fields with care. |
| Lucy / spells.eqresource.com / raidloot | https://lucy.allakhazam.com/, https://spells.eqresource.com/, https://www.raidloot.com/spells | — | Browsable spell data with named fields, across history. | Low to medium. Cross-checking field meanings. | Live data, not RoF2. |
| EQEmu Spire | https://github.com/EQEmu/spire | None | TS/Go server editor. `frontend/src/app/constants/eq-race*-constants.ts` and other constants; spell, item and NPC editors with field help. | Medium (M7). The field descriptions in its editors are handy. | No licence. |

---

## 7. Maps and navigation

| Name | URL | Licence | What it covers | Use | Caveats |
|---|---|---|---|---|---|
| In-game map format (`maps/*.txt`, `_1`, `_2` layers) | the `maps/` folder of the asset set; reader: https://github.com/martinlindhe/eqformat_map | reader is MIT (Rust, 2021) | Lines `L x0, y0, z0, x1, y1, z1, r, g, b` and points `P x, y, z, r, g, b, size, label`. X and Y are sign-flipped relative to `/loc`. | **High (M5 map window)**. The format is trivial, and the MIT reader is a test oracle. | — |
| Brewall's maps | https://github.com/RedGuides/brewall-maps (also the RedGuides resource page) | No licence (map data) | The most-used third-party map pack, with more accurate geometry than the default maps. | Low to medium. Players install it themselves; we only need to read the folder. | Don't bundle it. |
| EQEmu `.map` / `.wtr` / `.nav` | Loaders in EQEmu `zone/map.cpp` (V1 and zlib-compressed V2), `zone/water_map{,_v1,_v2}.cpp`, `zone/pathfinder_nav_mesh.cpp` (Detour navmesh); writers in zone-utilities `azone`, `awater`, `map_edit`. Data: https://github.com/EQEmu/maps (Perl tooling, 2026-01), https://github.com/Valorith/eqemu-maps, https://github.com/darkhue/EQEmuMaps | GPL (code); data unlicensed | Server-side collision, line of sight, water regions and navmesh. Documented at https://docs.eqemu.dev/server/maps/. | Medium (M3, M7). Optional client-side collision or pathing cache, and a cross-check of our BSP-region water against the server's. | GPL loaders, so use the format facts only. |
| Recast/Detour | https://github.com/recastnavigation/recastnavigation (also the EQEmu fork https://github.com/EQEmu/recastnavigation) | zlib | Navmesh build and query. EQEmu's `.nav` files are Detour data. | Low to medium (auto-run and follow features). | — |
| eqmap (kevr), Cartographer, ZlizEQMap, EqTool | https://github.com/kevrgithub/eqmap (none, 2013), https://github.com/Yoshiyuka/Cartographer (MPL-2.0), https://github.com/almx/ZlizEQMap (GPL-3.0), https://github.com/smasherprog/EqTool (MIT, C#, active) | mixed | External map viewers and overlays. | Low. | — |

---

## 8. Communities, wikis and other notable items

| Name | URL | Notes |
|---|---|---|
| EQEmu docs | https://docs.eqemu.dev/ (source https://github.com/EQEmu/eqemu-docs-v2, Go/MkDocs, no licence) | The single most useful doc site. `docs/client/` holds the WLD spec, fog, sky, sounds, model loading, race files, zon versions, the EQGZI shader list and WCEmu. `docs.eqemu.io` is the older hostname and didn't respond during this check. |
| EQEmulator forums | https://www.eqemulator.org/forums/ | Format threads ("WLD file format" t=11066, Eff→Emt t=39747, "Anyone interested in client development?" t=33432, RoF2 tracking threads). |
| EQ Resource Center | https://www.eqresource.com/ | Player-facing data (spells, zones, races). Cross-checking only. |
| RedGuides | https://www.redguides.com/ | MacroQuest community, and hosts Brewall's maps. Many RoF2/EQEmu hook projects. |
| MacroQuest / eqlib | https://github.com/macroquest/macroquest (GPL-2.0), https://github.com/macroquest/eqlib (no licence) | Useful as a structure and offset reference for the RoF2 client. |
| Rof2 client hook projects | https://github.com/CerveloFellow/rof2client-dinput8, https://github.com/knervous/eqlib (RoF2 hooks) | Small RoF2 client patches. They record known client bugs worth knowing about. Low. |
| eqaainfo (kauffman12) | https://github.com/kauffman12/eqaainfo | Python, no licence. Decodes AA data from network captures. Low (M9). |
| EQLogParser (kauffman12) | https://github.com/kauffman12/EQLogParser | Apache-2.0, C#. Large catalogue of log-line formats. Low, but useful if we want our chat log to stay compatible with parsers people already use. |

---

## Top 10 to use first, by milestone

Ranked by expected payoff. ✅ means we may port the code (permissive). 📖 means reading only (GPL or no licence).

| # | Resource | Milestone | Why first |
|---|---|---|---|
| 1 | **cjab/libeq** (✅ MIT) together with **LanternExtractor** (✅ MIT) and its `animationsources.txt` | **M1**, M4 | Port PFS and the 49 WLD fragment layouts from libeq. Take what the fragments mean (BSP regions, vertex colours, lights, sound and music instances, skeletal tracks) from Lantern. Together they cover M1 entirely, and most of M4's file side. |
| 2 | **EQEmu docs, client section**: WLD by Windcatcher, fog and clip plane, sky system (📖 facts) | **M1** | The spec plus measured rendering behaviour (21-step clip table, fog blend, sky layer speeds). Cross-check the two before coding. |
| 3 | **eq-network** transport and login crates (✅ MIT) | **M2** | The only permissive, maintained client-side session stack: framing, keyed CRC, fragments, combined packets, compression, retransmit, DES-zero login, server list. Port these and swap in RoF2 opcodes and structs for the game layer. Check behaviour against EQEmu `reliable_stream_connection` (📖 GPL). |
| 4 | **eqoxide docs** (📖 no licence): `protocol-notes.md`, `rof2-migration.md`, `character-models.md`, `equipment-textures-findings.md`, `collision-system.md` | **M2–M4**, M7 | The only other RoF2 client. Its notes cover how EQEmu recognises a RoF2 stream (`OP_SendLoginInfo` 0x7a09 at 464 bytes, `OP_ZoneEntry` 0x5089 at 76 bytes) and the traps in `rof2_structs.h`. Read it; don't copy. |
| 5 | **Shendare EQRaceInventory RoF2 tables**, EQEmu race-files and model-loading docs, and EQEmu `races` and `textures` tables (📖) | **M4** | The race-to-model-to-file table generated from RoF2 files, plus the `_chr.txt` load-list format. Regenerate it from a RoF2 asset set to validate our loader. |
| 6 | **eq-client `sidl.rs`** (✅ MIT) | **M5** | A permissive runtime SIDL reader for templates, `Ui2DAnimation` and draw templates, to compare with our own parser. eq-client's `eq-client-axes` module is also worth copying for EQ's mirrored axes (M1). |
| 7 | **xackery/quail** (✅ MIT, archived) | **M6** (and M4 for MDS/ANI) | Readers for MOD, MDS, TER, ZON v1–4, ANI, LIT, LOD, PRT, PTS, EDD, ECO, TOG and LAY, all in one MIT codebase. Pair it with the **EQGZI shader list** (72 `.fx` names and their properties) and the **zon-versions** table. Check v4 zones against zone-utilities `eqg_v4_loader` (📖 GPL). |
| 8 | **EQEmu client-file exporter, spells docs, DBStr types, `say_link.cpp`** (📖 facts) | **M7** | Exact `spells_us.txt`, `dbstr_us.txt`, `SkillCaps.txt` and `BaseData.txt` layouts, SPA semantics, and the RoF2 item-link body. rumstil/eqspellparser (✅ Apache) for spell-description text. |
| 9 | **Corsix xmi2mid** (✅ MIT) + **TinySoundFont** (✅ MIT) + **miniaudio** (✅ PD/MIT-0) + **GeneralUser GS** soundfont | **M8** | A complete permissive music path: split each EQ XMI into its sequences (as knervous/xmi_to_midi does), synthesise with TSF, and play and spatialise with miniaudio. WillEQ ships this same stack. |
| 10 | **Shendare Eff2Emt** (✅ CC0) + EQGZI `.emt` field list + WillEQ `eff_sound_format.md` (📖) | **M8** | Classic `_sounds.eff`/`_sndbnk.eff` decoding (CC0 code) and the `.emt` columns: everything we need for ambient emitters. |

Runner-up: **WillEQ** (📖 GPL, C++) is the best whole-client reference for how all these pieces fit in one C++ codebase (sky, weather, culling, audio, navmesh). Read its docs and architecture, but don't copy code.
