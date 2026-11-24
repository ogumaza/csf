# CSF: Cafe Sound Format

Version 3. CSF stores sequenced music from Nintendo Wii U games so that it can be played back as the game plays it. A
rip is in one of two modes:

- **Game mode**, like 2SF for the DS: the rip contains the game's executable and its sound files. The player runs the
  sound library linked into the game (NintendoWare for Cafe's `nw::snd`, or JAudio) on an emulated CPU. This mode
  requires a driver profile for the game (see "Ripping").
- **Archive mode**, like NCSF: the rip contains a sound archive. The player plays the archive with a model of the SDK's
  sound library (`nw::snd`, and the CPU side of the system's AX library). This mode works for any sound archive. See
  "Archive mode".

Wii U games play sounds from NintendoWare sound archives (`.bfsar`, FSAR): sequences, wave sounds and streams. CSF plays
sequences. Streams are recorded audio, and other tools already play them. Some of Nintendo's games use JAudio instead, a
sound library with archives of its own. CSF plays their sequences in game mode.

CSF rips contain no DSP program. The Wii U's DSP runs a mixer program that the system's sound library uploads, and no
game file contains that program. In both modes, every CSF player reproduces the DSP's mixing in code instead. In each
frame, the player converts the sample rate of the voices that AX passes to the DSP, filters them and mixes them. CSF's
player does the same arithmetic as the DSP program.

## Files

| Extension | Contents |
|---|---|
| `.csf` | A self-contained rip (rarely used). |
| `.csflib` | Shared data for one sound archive, or for a JAudio game. Game mode: the game's executable and the sound archive, or the sound files of a JAudio game. Archive mode: the sound archive and any group files that its sequences require. |
| `.minicsf` | One sound: a small chunk that selects the sound, plus tags. It references a `.csflib` with `_lib`. |

All three use the PSF container.

## PSF container

```
0x00  3 bytes  "PSF"
0x03  u8       version: 0x28 for CSF (see "Version byte" below)
0x04  u32      R: size of the reserved area
0x08  u32      N: size of the compressed program
0x0C  u32      CRC-32 of the compressed program
0x10  R bytes  reserved area (uncompressed)
      N bytes  program, zlib-compressed
      "[TAG]" followed by tags (optional)
```

All integers in the container, the chunk headers and the descriptors are little-endian. The CRC-32 is the one that zlib
uses. A rip stores the game's files as the game has them, in the Wii U's big-endian byte order.

### Version byte

CSF uses **0x28**. Known PSF version bytes are 0x01 (PSF1), 0x02 (PSF2), 0x11 (SSF), 0x12 (DSF), 0x13 (Sega Genesis),
0x21 (USF), 0x22 (GSF), 0x23 (SNSF), 0x24 (2SF), 0x25 (NCSF), 0x26 (3SF), 0x27 (WSF) and 0x41 (QSF). The high nibble 2
marks Nintendo systems. 0x28 is the next free value after the Wii's 0x27. `src/csf/format.h` is the one place that
defines the value.

## Program: chunks

The decompressed program is a sequence of chunks. Each chunk is an 8-byte header followed by its payload. Zeros pad the
payload to a multiple of 4 bytes:

```
u32  type (four ASCII characters, stored as bytes in order)
u32  size of the payload in bytes (excluding padding)
...  payload
```

| Type | Payload | Meaning |
|---|---|---|
| `FILE` | `u16 path length`, the UTF-8 path (no leading `/`, `/` separators), zeros to a multiple of 4, then the file data | A file of the game's title, by its path in the title (for example `content/sound/common/sound.bfsar` or `code/game.rpx`), a file that a descriptor names, the list of wave groups (`waves`, see "Wave groups"), the muted children (`muted`, see "Muted children"), the children at other volumes (`volumes`, see "Child volumes") or the statuses by section (`sections`, see "Statuses by section"). |
| `SND ` | `u32 sound id` | The sound to play: a `nw::snd` item id (`0x01000000 \| index` in the archive's sound table), or a sound ID of a JAudio driver. The last one in loading order takes effect. |

Unknown chunk types are skipped. Chunks are applied in order. For the same path, later data replaces earlier data.

### Loading order

As in other PSF formats, `_lib` is loaded recursively before the file that references it. Its path is relative to that
file. After the referencing file, `_lib2`, `_lib3` and so on are loaded in numeric order. Loading stops at the first
missing number. An empty `_lib` or `_libN` names no file. Chunks are applied in this loading order. A `.minicsf` usually
contains one `SND ` chunk.

### Wave groups

A sound of a JAudio driver may require wave groups. In the game, a scene loads them before it plays the sound. The
`FILE` chunk with the path `waves` lists them: a little-endian `u32` for each group, the wave bank in the high 16 bits
and the group in the low 16 bits. The driver loads them in that order before it starts the sound. A file whose size
isn't a multiple of 4 is an error. The `nw::snd` driver loads the waves of a sound by itself and ignores the file.

### Muted children

As Twilight Princess HD starts some pieces of music, it sets some children of the piece's track 0 to a volume of 0. The
`FILE` chunk with the path `muted` lists them: one little-endian `u32`, bit n for child n. The JAudio2 driver mutes
them after it starts the sound (see "JAudio2 driver"). Without the file, the driver mutes none. A file of another size
is an error. The other drivers ignore the file.

### Child volumes

The scenes and battles of Twilight Princess HD set some children of a piece's track 0 to volumes between 0 and 1. In
its second state, Ordon Village's music plays children 0 to 7 at 0.65. The `FILE` chunk with the path `volumes` lists
such children: for each group, a little-endian `u32`, bit n for child n, then the volume, a little-endian 32-bit float.
The JAudio2 driver sets them after it has muted the children of the `muted` file (see "JAudio2 driver"). Without the
file, the driver sets none. A file whose size isn't a multiple of 8 is an error. The other drivers ignore the file.

### Statuses by section

In each frame, the code of Twilight Princess HD's Hyrule Field reads port 10 of the root track of the day music. The
sequence puts its section there. The code then writes a status to port 9 by the section. The `FILE` chunk with the path
`sections` lists such statuses: two bytes for each entry, the section and the status. The JAudio2 driver reads port 10
through the game's `Z2SoundStarter::getPortData` in each game frame, from just after it starts the sound. It writes the
status of each entry for that section through `setPortData`. A status for section 0 therefore reaches the first tick of
the sound. Without the file, the driver writes none. A file whose size isn't a multiple of 2 is an error. The other
drivers ignore the file.

## Reserved area: descriptors

The reserved area may contain a process descriptor (game mode) or an archive descriptor (archive mode). At least one
loaded file must have a descriptor. The last descriptor in loading order takes effect and determines the mode. In
either mode, the loaded files must select a sound with a `SND ` chunk.

### Process descriptor (game mode)

```
0x00  u32  magic "CSFP"
0x04  u32  descriptor version (1 or 2)
0x08  u32  flags: none are defined (0)
0x0C  u32  output mode: 1 stereo (players play stereo only)
0x10  u32  version 2 only: the driver (see "Driver")
      u16  length of the executable's path, then the path (UTF-8)
      u16  length of the main sound file's path, then the path (UTF-8)
      zeros to a multiple of 4
      u32  P: the number of words in the game profile
      P × u32  the game profile (see "Driver")
```

The paths identify the `FILE` chunks that contain the game's executable (an RPX file) and the main sound file of the
driver: the sound archive for the `nw::snd` driver. A version 1 descriptor has no driver word and plays with the
`nw::snd` driver. A player rejects process descriptors of other versions, flags other than 0, an output mode other
than 1, a driver that the player doesn't have, or a profile with a number of words that the driver doesn't accept (see
"Driver"). The profile of a version 1 descriptor has 32 words.

### Archive descriptor (archive mode)

```
0x00  u32  magic "CSFA"
0x04  u32  descriptor version (1)
0x08  u32  flags: bit 0 for the track volumes after the effects (see "Track volumes"), or none (0)
0x0C  u32  output mode: 1 stereo (version 1 plays stereo only)
0x10  u32  nw::snd's voice count, 1 to 96: its voices and channels (96 unless the game changes it)
0x14  u32  AX settings: bit 0 = the 48 kHz renderer (0: the 32 kHz one); bit 1 = each device's output reaches
           48 kHz after the final mix stage (0: before it)
0x18  u32  the game's SDK version as major × 10,000 + minor × 100 + micro (21213 for 2.12.13), or 0 if unknown
0x1C  u16  length of the sound archive's path, then the path (UTF-8)
      u16  G: the number of group files, then for each its path's u16 length and the path (UTF-8)
      zeros to a multiple of 4
      u32  E: the number of effects (see "Effects"), then each effect:
             u8   device: 0 the TV, 1 the GamePad
             u8   bus: 0 A, 1 B, 2 C
             u8   type: 1 FxReverbHi, 2 FxReverbStd, 3 FxChorus, 4 FxDelay, 5 Nintendo Land's reverb,
                  6 Nintendo Land's delay
             u8   W: the number of words in the type's parameters (11, 10, 5, 5, 13 and 8)
             u32  S: the number of sounds that the effect lists
             u32  the parameters, W words
             u32  the sounds that the effect lists, S words, by their index in the archive's sound table
      with flag bit 0, the track volumes:
      u32  T: the number of sounds, then for each:
             u32  the sound's nw::snd item id
             16 × f32  the volumes of the sound's tracks, track 0 first
```

The paths identify the `FILE` chunks that contain the sound archive (`.bfsar`) and the group files (`.bfgrp`) that the
game keeps outside it. A group file from inside a SARC archive has the SARC's path, a `/` and the file's name in the
SARC, such as `content/sound/resources.sarc/GROUP_A.bfgrp`. A file without a name in the SARC has its index there
instead, in decimal. The archive lists the files in those groups without data. A version 1 player rejects archive
descriptors with a different version, any flag but bit 0, an output mode other than 1, a voice count outside 1 to 96,
or AX settings with bits other than 0 and 1. It also rejects track volumes that list a sound twice, or that contain a
volume that isn't a finite number of 0 or more. Output modes 0 (mono) and 2 (surround), and the GamePad's output, are
left for a later version.

A descriptor that ends after the paths has no effects. A descriptor with flag bit 0 has the effects' count E, even when
it's 0. The effects are little-endian, like the rest of the descriptor. The parameters of each effect are the words of
its class's parameter structure, in order: floats as their bits, and the modes, indices and counts as integers (see
"Effects"). A player leaves out an effect of an unknown type, and an effect with fewer words than its type has. Of a
longer list, the player uses the first words. A version 1 player rejects an effect on another device than 0 and 1, or
on another bus than 0 to 2. A player without support for effects plays without them.

Flag bit 0 came with format version 3. CSF 1.5.0 and earlier players refuse a descriptor with it: "the archive
descriptor's flags are 0x1; version 1 has none". Such a player can't set a track's volume. Without the volumes, it would
play every track at full volume. The game never plays such a piece that way. A descriptor without the flag is the same
as before, byte for byte.

### Track volumes

A game's code can set the volume of each track of a sequence through the sound's handle (`nw::snd`'s `SetTrackVolume`).
A version 1 descriptor with flag bit 0 lists such volumes. Each sound appears at most once, by its `nw::snd` item id,
with a volume for each of its 16 tracks. A track's volume multiplies into the volume of the track's channels, in the
same way as a volume set through `SetTrackVolume`. 1 leaves a track as the sequence plays it, and 0 silences it. The
player gives the tracks of the selected sound their volumes as the sound starts, before the first tick of the
sequence. The game's code does the same as it starts the sound. A sound that the list leaves out plays every track at
1. The ripper writes such a descriptor only into a `.minicsf` (see "Ripping").

## The emulated system (game mode)

A player provides:

- **The CPU.** One of Espresso's three cores runs the PowerPC code of the executable, with its floating-point and
  paired-single instructions. Code runs to completion between frames: guest time passes only a frame at a time.
- **Cafe OS.** The player loads the executable and its relocations in the same way that the system's loader loads an
  RPX. The functions that the executable imports from the system's libraries are modelled at a high level. The model
  covers the threads of `coreinit` (run in turn on the one core, by priority), mutexes, events, semaphores, message
  queues, alarms, time, heaps and the title's files. The title's files are the `FILE` chunks of the set under
  `content/`, seen at `/vol/content/`. For AX (`sndcore2` and `snd_core`), the model is the same model of AX that
  archive mode uses. The player doesn't run the callbacks of the aux buses of AX. Game mode therefore plays without the
  game's effects.
- **Output.** The TV's output, 16-bit stereo at 48,000 Hz: 144 samples a frame of 3 ms.

## Archive mode

An archive-mode player runs a model of the SDK's sound library instead of the game's code:

- **AX**: 96 voices with the priorities of AX and its frame of 3 ms. A renderer stands in for the DSP. The descriptor
  sets the rate of the renderer: 32 kHz or 48 kHz. Each device has a final mix stage. The final mix stage brings the
  output of a 32 kHz renderer to 48 kHz.
- **`nw::snd`**: the sequence player (sequence commands, tracks, variables, random numbers), bank lookup and wave
  loading, envelopes, LFOs, sweep, portamento, pitch, volume and pan curves, the low-pass and biquad filters, voice
  allocation with priorities, and the output parameters that each channel sends to the TV. The random number generator
  (x = x × 0x19660D + 0x3C6EF35F, from 0x12345678) steps once a frame. Each step comes after the updates of the
  channels. The sound starts in the second frame. That matches game mode when the game starts the sound before a frame.

CSF's implementation (ntdlib's `cafe` module) was written from a study of the code of Animal Crossing: amiibo Festival.
That game links `nw::snd` from SDK 2.12.13. Other games may use other versions of the library, or another number of
voices. Archive-mode rips of them are close but not guaranteed exact. When a game has a driver profile, game mode is
preferred.

Version 1 plays sequence sounds only. The files of a sound come from the archive, or from the group files that the
descriptor names. The ripper skips sequences whose files aren't there. The output starts with the first frame. The
sound has finished when the sequence has ended and no channel is sounding. With effects, the sound plays on until its
output has been silent for a second (see "Effects").

### Effects

Games put effects on the aux buses of AX, A, B and C, of each device: reverbs, choruses and delays. The system's library
`snduser2` contains the effects (AXFX). `nw::snd` wraps each in an effect class: `FxReverbHi`, `FxReverbStd`, `FxChorus`
and `FxDelay`. Nintendo Land defines a reverb and a delay in its code instead. Each voice sends some of its sound to the
buses. In each frame, AX passes each bus to the bus's callback. The callback runs the effects of the bus in turn. AX
mixes the result back into the device's main bus two frames after the voices sent it.

A game sets its effects up in its code. CSF plays the effects of the games that have an effect profile. A profile is
for one release of a game: the executable whose SHA-1 the profile gives. The ripper writes the effects into the archive
descriptor. Archive-mode rips of other games, and all game-mode rips, play without effects. A game with both a driver
profile and an effect profile has its effects only in archive mode (`csfrip --mode archive`). Mario Party 10's rips use
archive mode unless `--mode game` asks for game mode.

| Game | TV's aux A | TV's aux B | TV's aux C |
|---|---|---|---|
| Animal Crossing: amiibo Festival (USA) | `FxDelay`: a delay of 300 ms, a feedback of 0.4 | none | none |
| Mario Party 10 (USA) | `FxReverbStd`: a fused time of 2.2 s, a pre-delay of 10 ms | none | none |
| Wii Party U (USA, Rev 1) | `FxReverbHi`: a fused time of 2 s, a pre-delay of 10 ms | not played | none |
| Nintendo Land (USA) | each scene's reverb, for the scene's sounds; in the plaza, a delay and a reverb | none | none |

The scenes and minigames of Wii Party U put a reverb or a delay on the TV's aux B. Archive mode doesn't play them.

Nintendo Land puts a reverb on the TV's aux A as each attraction's scene starts, with the scene's settings. The plaza
puts a delay of one sample and a reverb there. Each of these effects lists the sequences of its scene that send to
aux A: 40 sound effects in all.

The parameters in the descriptor are the words that the game passes to the `SetParam` of the effect class:

- `FxReverbHi`: preDelayTime, fusedTime, coloration, damping, crossTalk, outGain, earlyMode, preDelayTimeMax, fusedMode,
  earlyGain, fusedGain.
- `FxReverbStd`: preDelayTime, fusedTime, coloration, damping, outGain, earlyMode, preDelayTimeMax, fusedMode,
  earlyGain, fusedGain.
- `FxChorus`: delayTime (ms), depth, rate (Hz), feedback, outGain.
- `FxDelay`: delay (ms), feedback, outGain, maxDelay (ms), lpf.
- Nintendo Land's reverb: the pre-delay (s), the time in which the combs decay by 60 dB (s), the coefficient of the
  all-passes, the damping, the gain, the length of the early reflection (an index from 0 to 7), the set of lengths of
  the combs and all-passes (an index from 0 to 5), the level of the early reflection, the level of the reverb, the rate
  (0 for 32 kHz, 1 for 48 kHz), the flag of the threaded path (in the first byte of the word), the frames of the ring,
  and the frames that go in before the first comes out.
- Nintendo Land's delay: the delay (ms), the feedback, the level, the damping, the rate, the flag of the threaded path,
  the frames of the ring, and the frames that go in before the first comes out.

An effect is set up for the sounds that it lists. An effect that lists no sounds is set up for every sound that no other
effect on the same bus of the device lists. A player sets up the effects of a sound before the sound starts, in the
order that the descriptor lists them. A game sets its effects up long before its music plays, and until then the
effects take in only silence. The effects of a rip start in the same state. The oscillator of a chorus is the
exception. It starts with the effects of the rip, and its phase then differs from the game's.

The `SetParam` of each class limits the game's parameters to their ranges. The class's `Initialize` then sets the
effect up with the library's `Init`. `nw::snd` adds an effect to its bus only if `Init` succeeds. The first effect on a
bus registers the callback of `nw::snd` with AX and sets the return volume of the bus to 1. Each class passes the first
three channels of the bus to the library's callback. In Dolby Pro Logic II mode, the effects of a device don't run.
Nintendo Land's classes are the exception and run in that mode too. Each processes 96 samples of the left and right
channels in each frame.

Archive mode plays CSF's models of the library's effects (in ntdlib's `cafe` module): `AXFXReverbHiExp`,
`AXFXReverbStdExp`, `AXFXChorusExp` and `AXFXDelayExp`. Each model follows the library's code operation for operation,
in single precision where the code uses it. The library fuses some multiplies and adds into one instruction with one
rounding, and the models fuse the same ones. The models give the same samples as the library. The delay works in
integers. Its feedback can keep a sample of -1 going for ever, far below anything audible. The length analysis of the
ripper plays the effects too. A sequence that ends plays on until the output has been silent for a second.

Archive mode also plays models of Nintendo Land's classes (ntdlib's `nintendo_land::Reverb` and
`nintendo_land::Delay`). Each follows the code of the class operation for operation, in single precision. Every scene
uses the threaded path of the classes. Each frame goes into a ring of 8 frames, and the thread of the class processes
it there. The bus gets the frame back 3 frames late. The game's code calls the thread right after each frame.

## Driver (game mode)

The driver word of version 2 names the driver. 0 drives NintendoWare's `nw::snd` (below), and a version 1 descriptor
has that driver. 1 drives JAudio2 and 2 drives JAudio1 (see "JAudio2 driver" and "JAudio1 driver"). A player without
a driver rejects the descriptors that name it.

A game-mode player drives the game's `nw::snd` itself, in the way that a game's code would. The game profile of the
process descriptor provides the addresses and settings. The player runs the static constructors of the executable.
Then the player sets up AX and the sound system, a sound heap, the archive (opened from `/vol/content/`), the data
manager of the archive and the archive player. Next, the player loads the data of the sound and starts the sound. From
then on, the player runs the frames of AX, the game's threads, and the update of the archive player every 60th of a
second.

The 32 words of the profile are, in order: the runner of the static constructors; the address of a word in `nw::snd`
that the game sets before it starts the sound system (0 for none), and the word's value; the AX settings (as in the
archive descriptor); the parameter constructor of the sound system, its required size and its initialiser; the
constructor and creator of the sound heap, and the heap's size; the constructor, buffer size and opener of the archive,
the two settings of the opener (priority and cache), the offset of the archive's file header, and the sizes and
loaders of the info and string blocks; the constructor of the data manager, its required size and initialiser, and the
offset of its loader within it; the constructor of the archive player, its required size, stream buffer size and
number of stream buffers, and its initialiser and update; the loader's `LoadData`; and `StartSound`. Version 3 adds
three words: the `WriteVariable`, `WriteGlobalVariable` and `WriteTrackVariable` of `SequenceSound`. A player accepts a
profile of 32 or 35 words. With 35, the player writes the variables of the `csf_var` tag through them after it starts
the sound (see "Tags"). A game writes them through the sound's handle in the same way. Each writer queues a command for
the sound thread. The sound thread runs the command before the first tick of the sequence. A track's variable reaches
only a track that the sequence has allocated. With 32 words, the player writes no variable.

The output starts with the first frame after the sound starts. The sound has finished when the game's sound handle is
free again.

### JAudio2 driver (driver 1)

The JAudio2 driver drives a game whose sound runs on JAudio2, with the game's Z2 sound layer above it. JAudio2 mixes on
the CPU, in the game's DSP thread, and passes its mix to AX through the final-mix callbacks of the devices. The player
runs the static constructors of the executable. Then it does what the game's start-up does for its sound, without its
graphics: the reset data that the audio set-up reads, the first set-up of the framework (the file system, the root
heaps and the system heap), the game's heaps and its audio memory, the threads that load the game's files, and the
audio set-up, once a game frame until it's done. Next, the player loads the wave groups of the sound (see "Wave
groups") and starts the sound. From then on, it runs the frames of AX, the game's threads, and the frame process of the
audio manager 30 times a second.

The 48 words of the profile are, in order: the machine's heap and the default heap's share of it; the runner of the
static constructors; the variable that points to the reset data, the data's size, and the offset of the byte in it that
must be 1; the first set-up of the framework, the word that it reads as the setting of the root heaps and the setting,
and the word that it reads as the size of the system heap and the size; the variables that hold the root heap, the root
heap of the second arena and the system heap; the creator of the assert heap; the creator of the audio memory, its
audio and graphics sizes, and the priorities of its threads (the stream's, the decompressor's and the pieces', a byte
each from bit 16 down); the creators and sizes of the command heap, the archive heap and the game heap; the creator of
the zelda heap, and what that heap leaves free in the system heap; the `getFreeSize`, `becomeCurrentHeap` and
`getCurrentHeap` of the heaps; the `setTransBuffer` of the audio memory stream and the size of its buffer, the creator
of the thread switch manager, the thread constructor and the creator of the disc thread; the creator of the solid heap,
the variable that holds the audio heap, and its size; the audio set-up and the byte that it sets when it's done; the
audio manager, its frame process and its `startSound`; and the scene manager, its loaders of wave groups from bank 0,
from bank 1 and from any bank, and the status of a group. Five words may follow: the variable that holds the music
manager (`Z2SeqMgr`) and its `setChildTrackVolume`, and the variable that holds the sound starter (`Z2SoundStarter`),
its `setPortData` and its `getPortData`. A player accepts 48, 50 or 53 words. A shorter profile leaves its missing words
at 0.

Where the profile names the music manager and `setChildTrackVolume`, the driver mutes the children that the `muted`
file lists (see "Muted children") once the sound has started: for each, `setChildTrackVolume` on the main handle of the
music manager, with a volume of 0, no fade, and the pan and the surround left as they are. The `bgmStart` of Twilight
Princess HD mutes the children of five pieces in the same way as it starts them. Next, the driver sets the children
that the `volumes` file lists (see "Child volumes") in the same way, each group at its volume. Where the profile names
the sound starter and `setPortData`, the driver then writes each port that the `csf_var` tag sets (see "Tags") to the
root track of the sound: `setPortData` on the starter, the handle, the port, the low 16 bits of the value and a child
of -1. The `changeBgmStatus` of Twilight Princess HD writes the status of a piece to port 9 in the same way.

The output starts with the first frame after the sound starts. The sound has finished when the game has released the
sound's handle.

### JAudio1 driver (driver 2)

The JAudio1 driver drives a game whose sound runs on JAudio1 under JSystem, on the framework of sead. JAudio1 mixes on
the CPU, in a virtual DSP thread, and passes its mix to AX through the final-mix callbacks of the devices. The player
runs the static constructors of the executable. Then it does what the game's start-up does for its sound, without its
graphics: the framework of sead, the game's root heaps, the game's sound layer (from the root heap of the sound) and
the start-up of JAudio. That start-up returns once the audio thread of JAudio runs. The thread starts after the first
frames of AX. The player then runs game frames until the sounds of the start-up and the wave systems that stay loaded
are in. Next, it loads the wave groups of the sound (see "Wave groups"; the word of a group gives the wave system in the
high 16 bits) and starts the sound. The sound starts at no position.

When a game starts a sound effect, the game passes it a value, such as the kind of ground under a footstep: the map
information of the effect. The sound library puts the low 16 bits of the value on port 6 of the effect. The player
passes the value of port 6 in the `csf_var` tag (see "Tags"), or 0 without it. A game can also set the other ports of
a sound after the start, through `JAISound::setPortData`. The `seStart` of The Wind Waker HD gives every effect port 9
that way. The player sets the other ports of an effect from the tag after the start in the same way.

A game writes a port of its music while the music plays, and the root track of a piece clears its ports in its first
commands. With a profile of 43 words, the player adds a callback to the list of the sound library through
`registerSubFrameCallback`. The callback of the root track runs the ticks of the sequence, and the sound library calls
both callbacks in the same loop. Once the root track runs and its first tick has moved its reader on from the start of
its data, the callback of the player writes the ports of the music to the root track through `writePortAppDirect`.
That point is fixed in the sequence. The sequence reads each value at its next poll of the port. With a shorter profile,
the player sets the ports of the music once the music plays (state 4 of JAISound) and one more game frame has run. With
41 words, the player writes port 9 of music through the game's writers of the status of a piece. The game's actors
write it in the same way. A wave system that JAudio has loaded keeps its group. From then on, the player runs the frames
of AX, the game's threads, and the frame process of JAudio 30 times a second.

The Zelda games start their music through their sound layer. The layer gives a piece the tracks and the tempo that the
game's state picks. `JAIZelBasic::bgmStart` starts the main music. Besides the IDs of the sound table, it accepts IDs
that start a piece in another state, such as 0x80000105. `subBgmStart` starts music that plays over the main music.
`bgmNowBattle` starts the battle music when an enemy comes near. A version 3 profile names the three. The player starts
music (an ID whose top two bits are 10) through them, on the handle of the sound layer for the main music or for the
music over it. A piece whose bit is set in the list of the profile starts through `subBgmStart`. The battle music of the
profile starts through `bgmNowBattle`. For both, the player sets the byte that the game sets once the music of a scene
has started. Otherwise `bgmNowBattle` starts nothing. The game calls `bgmNowBattle` once a game frame while a fight goes
on. The sound layer stops the battle music 30 frames after the last call. The player calls it right after
`subBgmStart` too, and then once a game frame for both. Its call sets the countdown. A piece that `subBgmStart` starts
with the countdown at 0, such as JA_BGM_SEA_ENEMY, stops in the first frame. The rest start through `bgmStart`, without
a fade. The game calls `bgmStart` the same way. `subBgmStart` and `bgmNowBattle` fade some pieces in over 60 frames.
They always do in the game. The sound layer starts with the boat's sail down, Link off the boat, his sword sheathed and
no enemy near. The starters read that state. Values 16 to 19 of the `csf_var` tag change it before the start (see
"Tags").

The 24 words of the profile are, in order: the machine's heap and the default heap's share of it; the runner of the
static constructors; the builder of the start-up argument of sead, the heap size that the argument gives, and the
start-up of sead; the game's two creators of its root heaps, and the variable that then holds the root heap of the
sound; the `setCurrentHeap` of sead; the creator of the game's sound layer; the sound system object of JAudio, its
start-up, the size that the start-up uses, and its frame process; the variable that holds the sound of the effects
sequence, and the variables that hold the list of wave systems and their load statuses; `loadGroupWave`;
`setCameraInfo`, the two position variables of the default camera and its matrix; and `startSoundVec`. Version 3 adds
two words, `JAISound::setPortData` and the variable `JAIBasic::msBasic`, then 15 words for the music: `bgmStart`,
`subBgmStart` and `bgmNowBattle`; four words that list the pieces that start through `subBgmStart` (bit n & 31 of word
n >> 5 for the piece 0x80000000 | n); the ID of the battle music; the offset in JAIZelBasic of the handle of the main
music, followed by the handle of the other music; the offset of the byte that marks the music of a scene as started;
the setters of the game's state that the starters read: `setShipSailState`, `setLinkOnBoard` and `bgmSetSwordUsing`;
and the writers of the status of a piece on port 9: `changeBgmStatus` for the main music and `changeSubBgmStatus` for
the music over it. Two words may follow: `JASystem::Kernel::registerSubFrameCallback` and
`TTrack::writePortAppDirect`. A player accepts a profile of 24, 41 or 43 words. The constructor of JAIBasic sets
`msBasic`. The game's sound layer constructs the JAIZelBasic that the game drives, after the sound system object that
the profile names. `msBasic` then points to the object of the sound layer, and JAudio reads the camera through it. With
41 or 43 words, the player drives the object that `msBasic` points to once the sound layer exists: its start-up, its
camera, its frame process, `startSoundVec` and the starters of music. With 24 words, the player drives the object that
the profile names, starts every sound with `startSoundVec`, and sets no port but 6.

The output starts with the first frame after the sound starts. The sound has finished when JAudio has cleared the
sound's handle.

## End of a sound

Like any PSF player, a CSF player plays for the tagged length plus fade. Without a length tag, the player stops at
whichever comes first: the end of the sound and the silence after a track, or a default length and fade.

The length tags count from the first sound: the first sample of the output that isn't 0 in either channel. A CSF player
skips the silence before the first sound by default. It plays 0.5 s of silence before a track and 1 s after it. A
player that counts from the start of the output plays a track later by the time to its first sound.

## Tags

The standard PSF tags apply: `title`, `artist`, `game`, `year`, `genre`, `comment`, `copyright`, `length`, `fade`,
`volume`, `_lib`, `_libN`. `csfby` credits the ripper. The ripper also writes `csf_sound`, with the label of the sound
from the sound archive (or the name of the sound in a JAudio game), and `csf_mode` (`game` or `archive`). With
`csf_mode`, players and listeners can tell the modes apart.

`length` and `fade` count time at the output's 48,000 Hz, from the first sound: a player plays `length` × 48,000 stereo
samples from the first sound before the fade.

`loop_start` and `loop_length` give a loop: the time from the first sound to the start of the loop, and the time of one
pass. Both are written as `m:ss.mmm`. `length` is `loop_start` plus two passes, or less when the loop starts before the
first sound. `loop_start` is then `0:00.000`. A player that plays N passes counts the passes in `length` as (`length` −
`loop_start`) / `loop_length`, rounded up. It ends N passes at `length` + (N − that count) × `loop_length`.

`csf_var` sets ports of a JAudio sound, or variables of an nw::snd sequence. Some effects read a value that the game's
code puts on a port or in a variable when it starts them, such as the material of the ground under a footstep. Without
the value, they play nothing, or another variant. The game's code also writes the status of a piece to a port as the
scene changes. The tag's value is one or more assignments `N=V`, separated by commas or white space. Line breaks count
as white space.

- For a JAudio sound, N is a port, 0 to 15. For a JAudio1 sound, N can also be the game's state, 16 to 19 (version 3).
  The state goes to the starters of music (see "JAudio1 driver"): 16 the boat's sail (`setShipSailState`), 17 Link on
  the boat (`setLinkOnBoard`), 18 the sword drawn (`bgmSetSwordUsing`) and 19 an enemy near (`bgmNowBattle` with a
  distance of 0). A value other than 0 sets each. With an enemy near, the call of `bgmNowBattle` after `subBgmStart`
  brings in the layers of the music of a miniboss.
- For an nw::snd sequence, N is a variable, 0 to 31. Variables 0 to 15 belong to the sequence, and 16 to 31 are the
  global ones. An nw::snd sequence also accepts the assignment `T:N=V`: variable N of track T. T is 0 to 15. N is 32 to
  47, the numbers that the commands of a track use for its variables.

V is −32,768 to 32,767. A port uses its low 16 bits. Each number is decimal, or hex with a `0x` prefix, and may have a
minus sign. A later assignment to the same variable wins. A player ignores the whole tag if any part of it is malformed
or out of range. A track's variable in the tag of a JAudio sound is out of range.

The JAudio1 driver passes port 6 to the game as the map information of the sound. It sets the other ports after the
start (see "JAudio1 driver"). Where its profile names the sound starter, the JAudio2 driver writes every port of the tag
to the root track of the sound once the sound has started (see "JAudio2 driver"). In archive mode, the player sets the
variables after it starts the sequence and before the first tick of the sequence. A game sets them through the sound's
handle at that point. A track's variable reaches only a track that the start of the sequence allocates. The nw::snd
driver of game mode writes the variables through the game's code (see "Driver"). `csfrip` writes the tag for each
variant of such an effect or such a piece, with the assignments in decimal, separated by ", ". The variables of the
sequence and the global variables come first, in increasing order of N. The variables of the tracks follow, by T and
then N: `csf_var=6=4` or `csf_var=0:47=1`. The foobar2000 component keeps the tag when it rewrites the tags of a file.

## Ripping

`csfrip` builds sets from a Wii U game or from a sound archive (`.bfsar`). A game can be a Wii U archive (`.wua`, the
ZArchive format that Exzap created) or an extracted title (a folder that contains `code/`, `content/` and `meta/`). The
game's files must be stored decrypted, and both forms store them that way. A disc image (WUD, WUX) or an installable
title (`title.tmd` and `.app` files) is refused. So are the sound archives of other consoles: those of the DS (SDAT),
the 3DS (CSAR), the Wii (RSAR) and the Switch. The Switch's sound archives are `.bfsar` files in the other byte order.
`csfrip --list` shows the sound archives of a game. A game with a driver profile is ripped in game mode, apart from
Mario Party 10, and anything else in archive mode. `--mode` overrides that choice. The rips go in a folder next to the
input, named after the input with `_csf` added. `-o` names another folder. Inside that folder, each sound archive of the
game gets a folder of its own, with the archive's name. A JAudio game has no sound archives, and its rip goes straight
into the folder.

`--only` selects the sequences whose labels match a regular expression. `--bgm` selects the music: the sequences whose
labels contain BGM (in any case), and those that play on a music player. The name of a music player has BGM, MUSIC,
SONG or FANFARE among the words that underscores separate (allowing digits after the word), and SE isn't among them.
In a JAudio game, `--bgm` selects the sounds whose names start with the game's prefix for music, such as `Z2BGM_`.

The ripper lists and skips the sequences that make no sound. Some have a volume of 0 in the archive. The sound library
multiplies that volume into everything that they play, and the game can't make them heard. The length analysis (see
"Game mode") finds the others that never sound. The skipped sequences are placeholders, sequences that only set
variables for the game, muted copies of other sounds, and sequences that play only on paths that depend on what the
game sets or on random numbers. A rip of such a sequence would always take the same path. `--no-length` leaves out the
analysis. Then only the sequences at a volume of 0 are skipped.

### Game mode

The `.csflib` contains the game's executable and the sound archive as `FILE` chunks, and a process descriptor with the
game's profile.

The sound archive in the `.csflib` has the size and the layout of the game's archive. Its header, `STRG` block and
`INFO` block keep their bytes. In the `FILE` block, the files that the ripped sequences play keep theirs: the file of
each sequence, the files of its banks, and the wave archives that those banks name. The kept bytes of each file are
rounded out to multiples of 32 bytes from the start of the archive. The game's reads of a file end on such a multiple,
or at the end of the archive. The rest of the `FILE` block is zeros. The game's code reads only the first 256 bytes of
the archive, its `INFO` and `STRG` blocks, and the files that a sequence plays. It reads each file at the place that the
file table gives, and therefore reads the same bytes as from the whole archive. The ripper reads each kept file back
from the new archive and checks that it's the same. A rip whose archive can't be read, or whose check fails, contains
the whole archive. Rips made with earlier versions contain the whole archive.

Each `.minicsf` selects one sequence with a `SND ` chunk. The length of a sequence comes from playing it on CSF's model
of the sound library (see "Archive mode"). A looping sequence gets two loops plus a 7-second fade, and `loop_start` and
`loop_length` for one pass (see "Tags"). It gets no loop tags when the fade waited for other tracks or when its first
two passes differ in length. A sequence that ends gets its length up to its last sound, and a 1-second fade. Every
length counts from the first sound. The analysis plays music for up to 3,600 seconds and other sounds for up to 600
seconds. A sound that reaches the limit with no loop or end found gets 600 seconds and the fade. Sound means a sample
louder than 8 of 32,767 (-72 dB).

A sequence loops when its main track jumps back to a command that the track has already played, outside a subroutine.
Inside a subroutine, such a jump counts only when the jump is unconditional and one of two things is true: the track
holds a note that sounds until the game stops the note, or the track has run no conditional command since the command
that the jump goes to. A sequence also loops when its main track reaches the end of a loop that repeats for ever. Only
a pass that changes the sound counts. A pass changes the sound when it plays a note, changes a setting of its track or
of the sequence, or opens a track. A tie or a monophonic note that continues a note at the same key and velocity changes
nothing. A pass that only waits, polls a variable or counts is idle.

The main track is the first track of the sequence. Once that track has ended, or while it makes idle passes, the first
track still playing becomes the main track if this track played a note on the way round. Each track before the new main
track must have ended, be idle or wait for ever, or come after the first track and play no notes in its passes. A later
track's pass that plays no note also counts while the track holds a note that sounds until the game stops the note, on
a looping wave at its sustain level. Every other track still playing must then be idle or wait for ever. Such a pass
changes that note, such as its volume or pitch. A track waits for ever when it waits a negative number of ticks, waits
out a held note, or waits for a note that never ends: one with no length on a looping wave. When the loop moves to
another track, or to another loop of the same track, the count starts again. Loops that go round before the first sound
don't count: a track that waits or sets values over and over while a note fades in, or before another track starts,
isn't the loop of the sound. A loop that went round without a sound ends the sequence at its last sound instead.

The fade of a looping sequence also waits for the other tracks. It starts once every other track still playing is idle,
waits for ever or has made two passes that changed the sound. Each time a track opens another track again, the opened
track starts its part again. That start counts as a pass that changed the sound. The fade therefore doesn't wait for the
tracks that a looping track opens again in each of its loops.

A sequence can reach a state where each track waits for its notes to end, waits out a held note (one of 9,600 ticks or
more), waits for ever, or only makes still passes with no note whose length will run out. A still pass is idle and draws
no random number. It also leaves the variables that tracks read with the values that the pass before left. A variable
that no track reads, such as a beat that a track marks for the game, doesn't count. Notes on looping waves never end.
Such a sequence is steady once no note's envelope is in its attack, hold, decay or release, and nothing is moving. A
steady sequence loops when every running wave has looped twice since the sound last changed. If the sequence has made no
sound since then, it ended at its last sound instead. A sequence whose tracks all wait for ever or make still passes
that play no notes has ended when its notes have. A sequence that stays busy without a sound for 180 seconds ended where
it fell silent. The ripper skips a sequence that never sounds.

A game whose sound runs on JAudio is ripped in game mode as a whole. One `.csflib` contains the game's executable and
every file of its sound folder but the streams, with a version 2 process descriptor that names the driver. Each
`.minicsf` selects a sound of the sound table of the start-up archive by its JAudio ID with a `SND ` chunk, and lists
the wave groups that the sound requires (see "Wave groups"). The ripper plays each sound twice on the game's code. The
first play has only the game's common wave groups loaded, and it shows the waves that the notes of the sound look up.
The second play has the groups that contain those waves loaded, and it gives the length. A sound loops when one of its
tracks jumps back to a command that the track has already played, twice to the same command. A sound holds when a track
idles, every track has settled, and nothing has moved for 10 seconds. It loops there if it still sounds. Any other sound
runs until the game releases it or until it falls silent for good. Loops, ends and the limit then give lengths as above.

Each sound file in the `.csflib` keeps its size. A file that the game read in none of the ripper's plays of the ripped
sounds contains zeros. The game reads its wave files (`.aw`) only while it starts and while it loads the wave groups of
a sound, both before the sound starts. In a player, the game therefore reads only files that keep their bytes. A rip
without the length analysis (`--no-length`) contains every file whole: its plays don't load the wave groups of the
sounds. So do rips made with earlier versions.

In a JAudio1 game, the ripper offers every sound of the sound table but the streams and the first sequence, the effects
sequence. The sequences come first, then the sound effects, each in the order of the sound table. The table names no
sound. The game's name file (`JaiInfo.asn`) gives the name of each sound and the ID that the game starts the sound with.
The ripper looks up the name of an effect by its ID without bits 10 and 11. A `.minicsf` of an effect selects it by its
ID with bit 11 set. With that bit, the sound library plays the effect to its end. A game starts most of its effects
with the bit, and keeps each of the others going by starting it again.

The sound library plays the effects sequence all along. When an effect starts, the sound library gives it one of the
tracks of the effects sequence. The effect plays in that track and in the tracks that it opens below it. The analysis
follows only the tracks of the sound. Some sound effects play nothing while the value that the game passes at the start
is 0, or while a port that the game sets is 0 (see "JAudio1 driver"). The ripper skips them with the other sounds that
make no sound. In The Wind Waker HD, 29 effects read port 6 first. For a step, a jump, a landing or a climb, the game
passes the material of the ground in its collision data: 1 to 27. For a hit, it passes the material of the surface, or
a code from 32 up for the kind of thing hit. A rip contains the variants of those effects instead. Values that play the
same samples share a variant, and its `csf_var` tag contains the lowest of them. The `title` and `csf_sound` tags of a
variant contain the name of the effect, a space and the value of the tag, such as `6=4`. Its file shares the number of
the effect. A user can play another value by changing the tag. The profile lists the values. Five more effects read
port 9. The game gives every effect that port: the reverb of the room that the effect plays in. One of them, the step of
a Stalfos (`JA_SE_ST_WALK`), plays nothing while the port contains 0. A rip contains six variants of it, one for each
reverb of the rooms that place a Stalfos in the GameCube release: 40, 80, 100, 110, 120 and 127. The other four play
nothing with any value, and the ripper skips them.

Game profiles are specific to each game and version. A profile is matched by the title's ID and the SHA-1 of the
title's executable. Version 1 has a profile for Animal Crossing: amiibo Festival (USA). Version 2 adds a JAudio2 profile
for The Legend of Zelda: Twilight Princess HD (USA) and a JAudio1 profile for The Legend of Zelda: The Wind Waker HD
(USA). Version 3 adds three words to the profile of amiibo Festival, 19 to The Wind Waker HD's and five to Twilight
Princess HD's (see "Driver"). A player made for version 2 refuses the libraries that a ripper of version 3 makes for
those three games. CSF 1.7.0 added the last two words of the profile of The Wind Waker HD. CSF 1.6.1 and
earlier players accept only 24 or 41 words, and they refuse the libraries with 43. The Japanese release of The Wind
Waker HD has a profile of the same form, at the addresses of that release. Version 3 also adds a profile of 35 words for
Mario Party 10 (USA).

### Archive mode

The `.csflib` contains the sound archive and any group files that its sequences require, as `FILE` chunks, and an
archive descriptor. Each `.minicsf` selects one sequence with a `SND ` chunk. Lengths come from the same analysis as in
game mode. For a game, the descriptor gets the game's SDK version. For a game with a profile, the descriptor also gets
the voice count and the AX settings from the profile. For a game with an effect profile, the descriptor also gets the
game's effects (see "Effects"). For a loose `.bfsar`, the descriptor gets 96 voices and the 32 kHz renderer. Each
device's output then reaches 48 kHz after the final mix stage. The ripper prints a notice that archive mode uses CSF's
model of the SDK's sound library, and that a game with a profile gives exact rips. Sequences whose data isn't all there
are listed and skipped. Wave sounds aren't ripped.

The archive in the `.csflib` contains only the files that the ripped sequences play: the file of each sequence, the
files of its banks, and the wave archives that those banks name. The file of a group, in the archive or beside it, stays
only when the model reads a copy of one of those files from it. The header, `STRG` block and `INFO` block of the
archive keep their bytes, apart from the sizes and the places of the files. The `FILE` block contains the kept files in
the order of the archive. Each starts at a multiple of 32 bytes from the start of the archive. NintendoWare's tools lay
out a `FILE` block in the same way. A dropped file has no place in the file table. The ripper reads each kept file back
from the new archive and checks that it's the same. When the cut fails, the ripper prints a warning, and the `.csflib`
contains the whole archive. In the game mode of nw::snd, the `.csflib` contains the archive with zeros in place of the
files that the ripped sequences don't play (see "Game mode"). Rips made with earlier versions contain the whole
archive.

Some sound effects read a variable of the sequence, or a global one, before they set it. The game sets the variable
when it starts the sound, such as the speed of a lift or the surface under a footstep. The variables start at -1.
Without the game's value, such an effect plays nothing or plays another variant. For a sound archive that ntdlib's lists
of sequence starts name, archive mode rips a variant of each such effect for each set of values in the lists. The
length analysis plays each variant with its values set before the first tick. The players set them at the same point.
A variant whose analysis finds no sound is skipped. The `title` and `csf_sound` tags of a variant contain the label of
the effect, a space and the values, such as `SQ_SE_MG0206_JUMP 15=1`. Its `csf_var` tag contains the values (see
"Tags"). Its file shares the number of the effect. Values that play the same samples share a variant. The variant uses
the fewest variables, then the lowest values. The lists give the values. They come from the comparisons in the
sequences, and from the ranges that a sequence computes from the value. Nobody has read which values
most games set. The values of Wii Party U come from a reading of the game's code. A user can play another value by
changing the tag. One piece of music reads such a variable too. Wii Party U's `SEQ_BGM_NPG006_THEMA` fades in a track
at levels 1 and 2 of the player's score and plays its ending at 3. A rip contains a variant for -1 and for each level.
Game mode rips the same variants, and its nw::snd driver writes the variables through the game's code (see "Driver").

Kirby and the Rainbow Curse's stage select plays one piece, `BGM_SEQ_MENU_INTERACTIVE`, with one to three of its tracks.
The game's code picks the tracks by what the stage select shows: the area view, where the player picks a world, the
list of stages of a world, or the boss stage of a world. It sets the volumes of the tracks through the sound's handle,
and fades them to the new volumes when the view changes. ntdlib's lists of sequence starts contain the states of the
piece. `csfrip` finds the game's sound archive (`seven.bfsar`) by the archive's SHA-1. The `.minicsf` of the piece
plays the area view. Each other state gets a `.minicsf` too. Its label is the label of the piece, a space and the name
of the state, such as `BGM_SEQ_MENU_INTERACTIVE world 3 boss`. The file name has underscores for the spaces. `--only`
selects a state by that label. The tracks of a state play at the state's volumes from the start. Each `.minicsf` of the
piece contains an archive descriptor in its reserved area: the descriptor of the library, with flag bit 0 and the track
volumes of the state (see "Track volumes"). The set loads that descriptor last, and it therefore takes effect. The
length analysis plays each state with its volumes. The states, with the tracks that play in each (the piece has no
track 9):

| State | Tracks | State | Tracks |
|---|---|---|---|
| the area view (no name) | 7 | | |
| `world 1` | 0 and 10 | `world 1 boss` | 0, 8 and 10 |
| `world 2` | 1 | `world 2 boss` | 1 and 11 |
| `world 3` | 2 and 12 | `world 3 boss` | 2, 8 and 12 |
| `world 4` | 3 | `world 4 boss` | 3 and 8 |
| `world 5` | 4 | `world 5 boss` | 4 and 11 |
| `world 6` | 5 | `world 6 boss` | 5 and 11 |
| `world 7` | 6 | `world 7 boss` | 6 and 8 |

Tags written by `csfrip`: `title` and `csf_sound` (the label of the sound), `game`, `length` and `fade`, `loop_start`
and `loop_length` for a loop, `csf_mode`, `csfby`, `utf8=1`, `csf_var` for a variant of an effect that reads the
game's variables, and optionally `artist`, `year` and `copyright`.

`game` is the text of `--game`, or else the game's English name from its `meta/meta.xml`. A game without that name, or
whose folder contains the XML files of another title, gets the name of the input. So do the rips of a loose sound
archive. `artist`, `year` and `copyright` come only from the options. No tag value comes from a table in the ripper's
code. Whoever makes a set adds the other tags by hand.

## Copyright

A `.csflib` contains copyrighted game code or data. Rips are for personal use.
