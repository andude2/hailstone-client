# Audio: files and rules

What the client's sound (`src/audio/`, `src/game/sound_rules.*`, `src/app/sound_events.*`) reads and the rules it follows.

## Files

| File | Layout |
|---|---|
| `<zone>.emt` | text, one emitter per line, comma-separated; lines of 10 characters or fewer are skipped |
| `<zone>_sndbnk.eff` | text; CR, LF and spaces are removed from each line; `EMIT` numbers the following names from 1, `LOOP` from 162; a name without `.` gets `.wav` |
| `<zone>_sounds.eff` | 84-byte records, no header |
| `<zone>.xmi` | the zone's music: Miles XMIDI (`FORM XDIR`, `CAT XMID`, one `FORM XMID` per sequence) |
| `soundassets.txt` | `id^name`; the global wave names |
| `mp3index.txt` | one name per line (CR, LF, spaces removed); music selection n is line n (one-based) |
| `racedata.txt` | `race ^ 18 male ids ^ 18 female ^ 26 male ^ 26 female ^ heights ...`; the 44 ids are sound kinds 0..43 |
| `spellsnew.eff` | 268-byte records: name[64], then cast, projectile and impact stages of 68 bytes, each starting with a wave id |
| `synthusr.sf2` | the General MIDI SoundFont, beside `synthusr.dls` that Miles loads |
| `snd1.pfs` .. `snd17.pfs` | the wave files; 2,128 names |

Sound files are looked up as `sounds\<name>`, then `voice\default\<name>`, then the PFS archives (the game directory and the archives). Loose files under `sounds\` take precedence over the archives. An install may carry loose files of under 100 bytes (for example `wind_lp2.wav`, an empty float WAV) that make those sounds silent.

### `_sounds.eff` record

| Offset | Field |
|---|---|
| 16, 20, 24 | x, y, z (zone file coordinates, the client's world coordinates) |
| 28 | radius; negative = the emitter only plays in listener environment `-radius`, and the radius becomes 10,000 |
| 32 / 36 | cooldown, day / night side (ms) |
| 40 | random extra delay, both sides |
| 48 / 52 | sound, day / night: a wave id; music: negative = `mp3index.txt` line `-id`, else a sequence of `<zone>.xmi` |
| 56 / 57 | kind of the first / second side |
| 60 / 64 | level (effects), loop count (music) |
| 68 / 72 | fade-out (ms) |
| 76 / 80 | music: fade-in (ms); kind 2: full-volume distance; kind 3: spread |

The two sides become one always-on emitter when kind, level, cooldown and sound all agree, or when the first kind is 2. Otherwise the first side plays by day and the second by night. Both sides take the kind at byte 56; byte 57 is not used for the second side.

### Emitter kinds

| Kind | Emitter |
|---|---|
| 0 | 2D: no distance attenuation; plays while the listener is inside the radius |
| 1 | music (below) |
| 2 | 3D: Miles distances max(param, 3) .. 100 |
| 3 | 3D at a random spot `param + 150` around the listener, distances 8 .. 100 |

- Gain: a level of 0 or less gives 0.2 for kind 0 and 1.0 for kinds 2/3; levels 1..10,000 give 10^(−level/2000); larger levels are silent.
- Repeat: a cooldown of 0 or less loops the sound for as long as it is in range (the voice is kept); otherwise it plays once every `[cooldown, cooldown]`, or `[cooldown + 500, cooldown + 500 + random)` with a positive random.
- The fade-out is how long a kept voice takes to fade when it goes out of range or is disabled.

### EMT fields

0 version (below 2: fields 19..21 absent), 1 file, 2 environment, 3 period (0 always, 1 day, 2 night), 4 gain, 5 fade-in ms, 6 fade-out ms, 7 loop count (0 = continuous), 8..10 position, 11 full-volume distance, 12 max distance, 13 random-spot distance, 14 activation range, 15/16 delay min/max, 17 XMI sequence, 18 echo, 19 user-disableable, 20/21 music flags. A `.mp3` or `.xmi` file is a music emitter.

## Runtime

Each frame, for each enabled emitter:

1. The range is the radius or EMT range, 10,000,000 when not positive, and no more than the max distance when that is set. Outside it, a kept voice fades out over the fade-out time, and a music emitter's entry stops.
2. Inside it, the sound is due when the delay is 0, or strictly more ms than the delay have passed since the timer was last reset (delay = min + `rand() % (max − min)`); music is always due.
3. A due effect resets the timer, then: with a delay and a loop count it plays once and is not kept; otherwise it plays looping and is kept (and not restarted while it plays).

The timer starts on the first update, in range or not. Emitters of another period (day = stored hour 5..18, checked every 500 ms), another listener environment, and user-disableable ones when the player turns environment sounds off are disabled.

### Music

- Each music emitter owns an entry: file, sequence, volume, priority `int(range + 10000)` (lower wins, so smaller regions win), fade-in, a 1,500 ms cross-fade and the fade-out to silence (capped at 20,000 ms for EFF).
- In range the entry becomes active; out of range it stops. The current entry is kept while it is active, else the active entry with the lowest priority (signed) is picked; ties keep the current one, then the first on the list. The old one fades over the cross-fade (or its fade-out when nothing replaces it). The new one starts (fade-in) only when its file is not still playing, so moving between two regions with the same music restarts it after the fade.
- XMI sequences are selected by zero-based index; an index past the file's sequences plays nothing.
- The zone's entries share the music manager with the global ones (`audio/music_manager.h`). A replaced entry fades out with its stop fade when nothing replaces it, else with its replace fade. With music on, the entry playing fades in to its volume when its start was requested and the volume is above 0; with music off every instance of its asset fades to 0. `Play(id)` selects an entry and requests its start (nothing for an unknown id); `Stop(id)` deselects it, then both select again.
- The mixer renders a stream (an MP3's decoder, an XMI's synthesizer) with its lock released, so the game thread's per-frame voice calls do not wait on a decode; a finished voice is destroyed after the lock is released.
- The files of the global entries and of the zone's entries are read ahead on the sound library's worker (`SoundLibrary::set_resident_files`, at start-up and at each zone change; the last zone's go), so a track starting does not read a multi-MB MP3, or open a whole `snd*.pfs`, on the game thread. The worker reads them after the waves asked for now and ahead of the zone's wave preloads, so the zone-entry track is not held up by a bulk preload. One asked for before its read is done is read then, as before (or, if the worker is reading it, that read is waited for), and not again on the worker. One that cannot be read is not read again (a music emitter in range asks for it every frame).
- Decoded waves stay in the sound library's cache until a zone change finds one not asked for (played or preloaded) since the zone change before: it is freed, unless it is one of the global waves or a voice still plays it (`SoundLibrary::drop_idle_waves`).

### Global music

Fifteen entries, set at start-up: in this client once the sound archives are indexed (`AudioSystem::set_up_global_music`, from the first update after the worker has opened every `snd*.pfs`, or at the first zone, which waits for it), since choosing a file checks whether it exists and the constructor waited on the index for that. A selection made before then (the login theme) is kept and applied when the entries are set; the combat theme's rand() is still drawn at start-up.

| Entry | Asset | Priority | Fades in / replaced / stopped (ms) | Played |
|---|---|---|---|---|
| 1 | eqtheme.mp3, else opener4.xmi sequence 0 | 10 | 0 / 1500 / 5000 | login and server select |
| 4 | eqtheme.mp3, else opener4.xmi sequence 1 (priority 9) | 10 | 0 / 1500 / 5000 | character select |
| 2 | combattheme1.mp3 for an odd rand(), combattheme2.mp3 for an even one (after srand of the time), else gl.xmi 0 (fade-in 1500) | 3 | 1000 / 3000 / 4000 | on with auto attack and the CombatMusic option, off otherwise (only when the attack state changes) |
| 8 | deaththeme.mp3, else gl.xmi 3 | 2 | 1500 / 1500 / 2500 | the local player's death, until the repop or MSG_READY |
| 3 | gl.xmi 21 | 5 | 1500 / 2500 / 3500 | the bank window (and the guild bank) while open |
| 6 | gl.xmi 21 | 5 | 1500 / 2500 / 3500 | merchant windows (and the point merchants) while open |
| 7 | gl.xmi 23 | 5 | 1500 / 2500 / 3500 | the trainer window while open |
| 15, 9, 10 | gl.xmi 19, 20, 7 | 5, 5, 6 | 1500 / 2500 / 3500 | never |
| 11..14 | gl.xmi 8..11 | 5 | 1500 / 1500 / 2500 | never |
| 5 | none | 0 | 1600 / 1400 / 10000 | the loot window (silent) |

mp3 entries use the mp3 pool (pool 1). A zone's music has priority int(range + 10000), so any global entry wins over it. Setting an entry clears the id's old one (were it playing, nothing is) and puts the new one at the head of the list, so of two new entries with the same priority the later one wins. Each entry carries a flag (EMT field 21 for a zone's music, 0 for the global entries) that goes to the instance fade-in; when such an mp3 instance has ended (faded out or played through), every entry of that asset is marked finished, and a finished entry is never chosen again. Each entry into character select turns entry 1 off and 4 on.

## Sounds and their distances

| Played by | Position | Miles min / max | Volume |
|---|---|---|---|
| a spawn's sounds | the spawn | height/2, height/2 × 24 | 1.0; 0.8 for the local player (pool 3) |
| doors, switches | the switch | 5, 80 | 1.0 |
| a spawn's loop sound | the spawn | max(height/2, 2), × 24 | 1.0 |
| a 2D event sound without a control (level up) | — | — | 1.0 |

A 3D wave play negates z (Miles space is (y, x, −z)), multiplies the volume by the effects level and the reverb level by the Realism option. A 3D sound beyond its max distance does not start.

### The Miles 3D curve

The client reproduces the "Miles Fast 2D Positional Audio" provider's sample update (`audio/sound_control.h` `miles_fast2d_levels`). Each 3D sample gets its distances (max, min), its effects level, its position, a zero velocity and its volume, which the instance ramps over the fade and sends every update; no rolloff, distance factor, Doppler or sample preference is set. For distance d (game coordinates and Miles' agree: the listener, wave play and the emitter loaders all negate z):

- past the max distance (strictly) the sample is muted and plays on ("Mute at maximum", on by default): volume 0, pan 0.5;
- gain = volume^(5/3) × min / d once d is past min + 0.0001 (min at least 0.0001; the rolloff stays 1), × 0.75 when the sound is behind the listener (dot of the facing and the direction below 0);
- pan p = acos(dot(right, direction)) / π, right = up × face = (sin a, −cos a, 0); 0.5 within 0.0001 of the listener;
- the levels are left = p × gain, right = (1 − p) × gain, kept as linear amplitudes, so a sound straight ahead plays at half the gain on each side; a 2D sound plays its volume on both;
- the sample's reverb levels are dry 1, wet = its effects level (obstruction and exclusion stay 0).

### Voice pools

A play takes the first free instance of the control's pool and plays nothing when there is none; music borrows (a new instance when the pool is empty). The pools (16 3D and 6 2D voices in total):

| Pool | 3D | 2D | Used by |
|---|---|---|---|
| 0 | 16 | 6 | everything else: other spawns' sounds, doors, 2D event sounds, mobile emitters, rain and wind |
| 1 | 0 | 1 | the mp3 music (borrowed) |
| 2 | 0 | 1 | — |
| 3 | 2 | 1 | the local player's sounds |
| 4 | 4 | 2 | the zone's wave emitters |

An instance goes back to its pool when its asset's sweep finds it finished; the client counts a voice as busy while the mixer plays it.

### Rain and wind

Two global 2D emitters are created disabled: rain (wave 159 `rainloop.wav`) and wind (wave 158 `wind_lp1.wav`), each gain 0.5, looping, no delay, no range limit, fading in over 2000 ms and out over 3000 ms, pool 0. Rain is enabled while raining (weather kind 0) and wind while windy (kind 1, snow), neither indoors. An install may ship `sounds\rainloop.wav` and `wind_lp1.wav` as 80-byte files without samples, in which case nothing is heard.

## Which sound for which event

- **Level up**: `LevelUp.WAV` (0x8b) when OP_LevelUpdate raises the level.
- **Doors**: by switch type and the new state (`game/sound_rules.cpp` `door_sounds`); e.g. type 0 plays `DoorWd_O` (0x87) on opening and `DoorWd_C` (0x88) on closing. A door that closes by itself (repop) is silent.
- **Melee hits**: on the defender, for players and NPCs only; for a player-race hit voice (ids 32..89) first `FlshHit1`/`FlshHit2` (0x93/0x94, the latter when hp / damage < 10 with hp = the local player's HP or the defender's level × 30), then one of the defender's Hit1..4 at random. A block (−1) plays StaffBlk, SwordBlk when both hold blades, or ShldBlk half the time with a shield; a dodge (−4) `fall_hit`, a rune (−6) StaffBlk. Hits between others more than 75 units away are not reported.
- **Animations** (by the classic animation number OP_Animation uses): attacks (kick `Kick1`, piercing `Stab`, 2H `Swing`/0x79, one-handed: the swing `Swing` when holding something plus the race's Attk1 unless that is 0x82, bare hands one of five swings), jump (the race's Jump), crouch, climbing (alternating ladder steps), swim/tread water (0x65/0x66, every 600/550 ms), drown, death, sit. Held item kinds come from `game::held_type`.
- **Footsteps**: the local player walking or running plays one of `StepWlkR/L/2/3` or `StepRunR/L/2/3` at random every 500 ms (not while sneaking or levitating); other spawns' walk or run sound becomes their loop sound (`Walk_Lp`, `runlp` for the player races) through their mobile emitter. The local player has no loop sound: `PlayerClient::SetCurrentLoopSound` @0x58b860 sets 0 for it unless the byte 0xddf654 is set, and nothing writes that byte but zeros (the offline switch the cast completion also reads), so its steps are only the timed plays above.
- **Landing**: `jumpland` when a landing is at 2 × SpeedZ ≤ −2.5 out of water, then `fall_hit` after fall damage (else `jumpland`).
- **Spells**: the spell's effect pair is {spells.eff record `spaindex` (0..255, 0xae8 bytes each), SpellsNew.eff record `SpellAnim` (1..count, with a name)}; a stage's sound is its first word, new record +0x40 + stage × 0x44, else old record +0x224 + stage × 0x3a0, −1 none. Stage 0 (OP_BeginCast) plays at the caster, stage 2 (every OP_Action 231, both the flag-0 and the flag-4 copy) at the struck spawn, stage 1 (missiles) never, so spell projectiles are silent in flight (a missile's hit plays the impact through the struck spawn); nothing without the caster spawn (OP_Action word +2). `Teleport` (0x89) plays at the player for SPA 83 / 145 with a zone (player spawns only) and for a valid SPA 379.
- **Swim and tread sounds**: for player races: the spawn's race up to 12 (signed, so 0 and negatives count), or 128, 130, 330, 522.

Race sound ids: 13 races map to others (44, 55, 67, 71 → 1; 77 → 6; 78 → 3; 81 → 11; 90 → 2; 92 → 9; 93 → 10; 94 → 8; 106 → 5; 112 → 4) and each kind gets a default when racedata.txt has 0 (`game::spawn_sounds`).

## The listener

- **Place**: the local player's position (the feet plus AvatarHeight) with the camera's heading goes to Miles as (y, x, −z) and heading × 2π × 1/512 (double) rounded once to a float; the listener faces along (cos a, sin a, 0) with up (0, 0, −1). At character select the camera's own position is used.
- **Environment**: every frame unless a special room is set, h = ftol(|ceiling − floor|): low (3) up to 20, high (2) up to 500 when indoors (the player actor's scaled ambient below 0.48), else outside (1). An environment change re-enables the emitters.
- **Room** (Miles' `AIL_set_3D_room_type`): Low / High / Outside use rooms 12 (hallway) / 2 (room) / 19 (plain), then the per-zone table when it names the zone. That table is not in Hailstone: it is read at run time from the player's own `eqgame.exe` in the asset folder (never a server's overlay), read only and only the PE headers and the table (`audio/environment_presets.h`). In the RoF2 build it is at virtual address 0xac7b80: 127 records of 0x2c bytes, the zone's short name (32 bytes, NUL-terminated), then u32 room types for outside (+0x20), high (+0x24) and low (+0x28). Every record must have a lower-case short name and room types 0..25; with no `eqgame.exe`, another build or anything that does not check out, every zone takes the defaults, and that is logged once. Room 0x16 (underwater) is set when the head goes under water and 0x17 (drugged) when the player is stunned, with the special flag; leaving the water and a repop run the update again, which clears it. The room goes to Miles' digital master reverb; the parameters per room are {level, decay s, damping, predelay s} (room 0 has level 0). The client runs a comb / all-pass reverb with those parameters over the 3D voices' reverb levels. A NaN or infinite volume, level, send or room parameter is taken as 0 and a non-finite sample as silence, so none reaches the reverb's feedback lines.

## XMI playback

The sequencer follows the Miles timer callback: 120 ticks a second; each tick first counts down the 32 note slots and releases expired notes in slot order, then plays the due events; a note-on stores its duration in the first free slot (a zero duration lasts one tick; with all 32 busy the sequence stops); end of track releases the held notes. Controllers 116/117 are loops (four slots; count 0 = forever; a 117 below 64 closes the loop). Music loops forever (the entry's loop count is 0). The synthesiser is TinySoundFont with `synthusr.sf2`; it will not sound identical to Miles' DLS synth with `synthusr.dls`.

## Settings

`hailstone.ini`: `audio.enabled` (open a device), `audio.master`, `audio.music`, `audio.effects` (0..100), `audio.npc_hits` (0..100, default 100; Hailstone, not in the original: the hit, block and dodge sounds of an NPC other than the player, scaled linearly on top of `audio.effects` by `audio::option_volume_factor`), `audio.mute`, `audio.environment` (the zone's user-disableable emitters), `audio.soundfont` (an SF2 path; default the game's `synthusr.sf2`), `audio.realism` (0..100, the reverb level of the event sounds; default 5), `audio.reverb` (the room reverb at all), `audio.combat_music` (the CombatMusic option, /combatmusic), `audio.pregame_music` (the login, server select and character select themes: the login screen's Music box; default on), `audio.reverse_sound` (/reversesound: the client swaps the left and right channels of its whole output; RoF2 only keeps ReverseSound, 0xde0d48: Cmd_reversesound and the ini [Defaults] load and save are its only readers, so in the original it has no effect), `audio.mix_ahead` (/mixahead, SoundMixAhead 1..100, default 8: the output's buffer, `audio::mix_ahead_ms` = clamp(n, 4, 96) x 8 ms; changing it opens the device again). `--no-audio` never opens a device. Without a device (none present, `SDL_AUDIODRIVER=dummy`) the client runs silently; `SDL_AUDIODRIVER=disk` writes the output to a file (`SDL_AUDIO_DISK_OUTPUT_FILE`), which is how the sound can be checked without speakers. `tools/audiodump` renders an XMI sequence, a sound or a zone's sounds at a position into a WAV offline.
