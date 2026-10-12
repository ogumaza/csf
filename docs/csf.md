# CSF: Cafe Sound Format

Version 2. CSF stores sequenced music from Nintendo Wii U games so that it can be played back as the game plays it. A
rip is in one of two modes:

- **Game mode**, like 2SF for the DS: the rip contains the game's executable and its sound files. The player runs the
  sound library linked into the game (NintendoWare for Cafe's `nw::snd`, or JAudio) on an emulated CPU. This mode
  requires a driver profile for the game (see "Ripping").
- **Archive mode**, like NCSF: the rip contains a sound archive. The player plays the archive with a model of the SDK's
  sound library (`nw::snd`, and the CPU side of the system's AX library). This mode works for any sound archive. See
  "Archive mode".

Wii U games play sounds from NintendoWare sound archives (`.bfsar`, FSAR): sequences, wave sounds and streams. CSF plays
sequences. Streams are recorded audio. Other tools already play them. Some of Nintendo's games use JAudio instead, a
sound library with archives of its own. CSF plays their sequences in game mode.

CSF rips contain no DSP program. The Wii U's DSP runs a mixer program that the system's sound library uploads. No game
file holds that program. In both modes, every CSF player reproduces the DSP's mixing in code instead. The player
converts the sample rate of the voices that AX passes to the DSP each frame, filters them and mixes them. CSF's player
does the same arithmetic as the DSP's program.

## Files

| Extension | Contents |
|---|---|
| `.csf` | A self-contained rip (rarely used). |
| `.csflib` | Shared data for one sound archive, or for a JAudio game. Game mode: the game's executable and the sound archive, or a JAudio game's sound files. Archive mode: the sound archive and any group files that its sequences require. |
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

All integers in the container, the chunk headers and the descriptors are little-endian. The CRC-32 is the one used by
zlib. The files that a rip contains are stored as the game has them. They're in the Wii U's big-endian byte order.

### Version byte

CSF uses **0x28**. Known PSF version bytes are 0x01 (PSF1), 0x02 (PSF2), 0x11 (SSF), 0x12 (DSF), 0x13 (Sega Genesis),
0x21 (USF), 0x22 (GSF), 0x23 (SNSF), 0x24 (2SF), 0x25 (NCSF), 0x26 (3SF), 0x27 (WSF) and 0x41 (QSF). The high nibble 2
marks Nintendo systems. 0x28 is the next free value after the Wii's 0x27. The value is defined in one place:
`src/csf/format.h`.

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
| `FILE` | `u16 path length`, the UTF-8 path (no leading `/`, `/` separators), zeros to a multiple of 4, then the file data | A file of the game's title, by its path in the title (for example `content/sound/common/sound.bfsar` or `code/game.rpx`), a file a descriptor names, or the wave groups' list (`waves`, see "Wave groups"). |
| `SND ` | `u32 sound id` | The sound to play: a `nw::snd` item id (`0x01000000 \| index` in the archive's sound table), or a JAudio driver's sound ID. The last one in loading order takes effect. |

Unknown chunk types are skipped. Chunks are applied in order. For the same path, later data replaces earlier data.

### Loading order

As in other PSF formats, `_lib` is loaded recursively before the file that references it. Its path is relative to that
file. After the referencing file, `_lib2`, `_lib3` and so on are loaded in numeric order. Loading stops at the first
missing number. An empty `_lib` or `_libN` names no file. Chunks are applied in this loading order. A `.minicsf` usually
contains one `SND ` chunk.

### Wave groups

A sound of a JAudio driver may require wave groups that the game loads before it plays the sound, as a scene of the game
would. The `FILE` chunk with the path `waves` lists them: a little-endian `u32` for each group, the wave bank in the
high 16 bits and the group in the low 16 bits. The driver loads them in that order before it starts the sound. A file
whose size isn't a multiple of 4 is an error. The `nw::snd` driver loads a sound's waves itself, and ignores the file.

## Reserved area: descriptors

The reserved area may contain a process descriptor (game mode) or an archive descriptor (archive mode). At least one
loaded file must have a descriptor. The last descriptor in loading order takes effect and determines the mode. The
loaded files must select a sound with a `SND ` chunk in either mode.

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

The paths identify `FILE` chunks containing the game's executable (an RPX file) and the driver's main sound file: the
sound archive for the `nw::snd` driver. A version 1 descriptor has no driver word, and plays with the `nw::snd` driver.
A player rejects process descriptors of other versions, flags other than 0, an output mode other than 1, a driver that
it doesn't have, or a profile with another number of words than the driver takes (32 for the `nw::snd` driver).

### Archive descriptor (archive mode)

```
0x00  u32  magic "CSFA"
0x04  u32  descriptor version (1)
0x08  u32  flags: version 1 has none (0)
0x0C  u32  output mode: 1 stereo (version 1 plays stereo only)
0x10  u32  nw::snd's voice count, 1 to 96: its voices and channels (96 unless the game changes it)
0x14  u32  AX's settings: bit 0 = the 48 kHz renderer (0: the 32 kHz one); bit 1 = each device's output reaches
           48 kHz after the final mix stage (0: before it)
0x18  u32  the game's SDK version as major × 10,000 + minor × 100 + micro (21213 for 2.12.13), or 0 if unknown
0x1C  u16  length of the sound archive's path, then the path (UTF-8)
      u16  G: the number of group files, then for each its path's u16 length and the path (UTF-8)
      zeros to a multiple of 4
```

The paths identify `FILE` chunks containing the sound archive (`.bfsar`) and the group files (`.bfgrp`) that the game
keeps outside it. A group file from inside a SARC archive has the SARC's path, a `/` and the file's name in the SARC,
such as `content/sound/resources.sarc/GROUP_A.bfgrp`. A file without a name in the SARC has its index there instead,
in decimal. The archive lists the files in those groups without data. A version 1 player rejects archive
descriptors with a different version, flags other than 0, an output mode other than 1, a voice count outside 1 to 96, or
AX settings with bits other than 0 and 1. Output modes 0 (mono) and 2 (surround), and the GamePad's output, are left for
a later version.

## The emulated system (game mode)

A player provides:

- **The CPU.** One of Espresso's three cores runs the executable's PowerPC code with its floating-point and
  paired-single instructions. Code runs to completion between frames: guest time passes only a frame at a time.
- **Cafe OS.** The player loads the executable and its relocations in the same way that the system's loader loads an
  RPX. The functions that the executable imports from the system's libraries are modelled at a high level. The model
  covers `coreinit`'s threads (run in turn on the one core, by priority), mutexes, events, semaphores, message queues,
  alarms, time, heaps and the title's files. The title's files are the set's `FILE` chunks under `content/`, seen at
  `/vol/content/`. For AX (`sndcore2` and `snd_core`), the model is the same model of AX that archive mode uses.
- **Output.** The TV's output, 16-bit stereo at 48,000 Hz: 144 samples a frame of 3 ms.

## Archive mode

An archive-mode player runs a model of the SDK's sound library instead of the game's code:

- **AX**: 96 voices with AX's priorities and AX's frame of 3 ms. A renderer stands in for the DSP. The descriptor sets
  the renderer's rate: 32 kHz or 48 kHz. Each device has a final mix stage. The final mix stage brings a 32 kHz
  renderer's output to 48 kHz.
- **`nw::snd`**: the sequence player (sequence commands, tracks, variables, random numbers), bank lookup and wave
  loading, envelopes, LFOs, sweep, portamento, pitch, volume and pan curves, the low-pass and biquad filters, voice
  allocation with priorities, and the output parameters that each channel sends to the TV. The random number generator
  (x = x × 0x19660D + 0x3C6EF35F, from 0x12345678) steps once a frame. Each step comes after the channels' updates. The
  sound starts in the second frame. That matches game mode when the game starts the sound before a frame.

CSF's implementation (`src/ax`, `src/nwsnd`) was written from a study of the code of Animal Crossing: amiibo Festival.
That game links `nw::snd` from SDK 2.12.13. Other games may use other versions of the library, or another number of
voices. Archive-mode rips of them are close but not guaranteed exact. When a game has a driver profile, game mode is
preferred.

Version 1 plays sequence sounds only. A sound's files come from the archive, or from the group files that the descriptor
names. The ripper skips sequences whose files aren't there. The output starts with the first frame. The sound has
finished when the sequence has ended and no channel is sounding.

## Driver (game mode)

Version 2's driver word names the driver: 0 drives NintendoWare's `nw::snd` (below), and a version 1 descriptor has that
driver. 1 drives JAudio2 and 2 drives JAudio1 (see "JAudio2 driver" and "JAudio1 driver"). A player without a driver
rejects the descriptors that name it.

A game-mode player drives the game's `nw::snd` itself, in the way that a game's code would. The process descriptor's
game profile provides the addresses and settings. The player runs the executable's static constructors. Then the player
sets up AX and the sound system, a sound heap, the archive (opened from `/vol/content/`), the archive's data manager and
the archive player. Next, the player loads the sound's data and starts the sound. From then on, the player runs AX's
frames, the game's threads, and the archive player's update every 60th of a second.

The profile's 32 words are, in order: the static constructors' runner; the address of a word in `nw::snd` that the game
sets before it starts the sound system (0 for none), and the word's value; AX's settings (as in the archive descriptor);
the sound system's parameter constructor, required size and initialiser; the sound heap's constructor and creator, and
the heap's size; the archive's constructor, buffer size and opener, the opener's two settings (priority and cache), the
offset of the archive's file header, and the info and string blocks' sizes and loaders; the data manager's constructor,
required size and initialiser, and the offset of the data manager's loader within it; the archive player's constructor,
required size, stream buffer size and number of stream buffers, and its initialiser and update; the loader's `LoadData`;
and `StartSound`.

The output starts with the first frame after the sound starts. The sound has finished when the game's sound handle is
free again.

### JAudio2 driver (driver 1)

The JAudio2 driver drives a game whose sound runs on JAudio2, with the game's Z2 sound layer above it. JAudio2 mixes on
the CPU, in the game's DSP thread, and hands its mix to AX through the devices' final-mix callbacks. The player runs the
executable's static constructors. Then it does what the game's start-up does for its sound, without its graphics: the
reset data that the audio set-up reads, the framework's first set-up (the file system, the root heaps and the system
heap), the game's heaps and its audio memory, the threads that load the game's files, and the audio set-up, once a game
frame until it's done. Next, the player loads the sound's wave groups (see "Wave groups") and starts the sound. From
then on, it runs AX's frames, the game's threads, and the audio manager's frame process 30 times a second.

The profile's 48 words are, in order: the machine's heap and the default heap's share of it; the static constructors'
runner; the variable that points to the reset data, the data's size, and the offset of the byte in it that must be 1;
the framework's first set-up, the word that it reads as the root heaps' setting and the setting, and the word that it
reads as the system heap's size and the size; the variables that hold the root heap, the second arena's root heap and
the system heap; the assert heap's creator; the audio memory's creator, its audio and graphics sizes, and its threads'
priorities (the stream's, the decompressor's and the pieces', a byte each from bit 16 down); the creators and sizes of
the command heap, the archive heap and the game heap; the zelda heap's creator, and what that heap leaves free in the
system heap; the heaps' `getFreeSize`, `becomeCurrentHeap` and `getCurrentHeap`; the audio memory stream's
`setTransBuffer` and its buffer's size, the thread switch manager's creator, the thread constructor and the disc
thread's creator; the solid heap's creator, the variable that holds the audio heap, and its size; the audio set-up and
the byte that it sets when it's done; the audio manager, its frame process and its `startSound`; and the scene manager,
its loaders of wave groups from bank 0, from bank 1 and from any bank, and a group's status.

The output starts with the first frame after the sound starts. The sound has finished when the game has released the
sound's handle.

### JAudio1 driver (driver 2)

The JAudio1 driver drives a game whose sound runs on JAudio1 under JSystem, on sead's framework. JAudio1 mixes on the
CPU, in a virtual DSP thread, and hands its mix to AX through the devices' final-mix callbacks. The player runs the
executable's static constructors. Then it does what the game's start-up does for its sound, without its graphics:
sead's framework, the game's root heaps, the game's sound layer (from the sound's root heap) and JAudio's start-up.
That start-up returns once JAudio's audio thread runs. The thread starts after AX's first frames. The player then runs
game frames until the start-up's sounds and the wave systems that stay loaded are in. Next, it loads the sound's wave
groups (see "Wave groups"; a group's word gives the wave system in the high 16 bits) and starts the sound. A wave
system that JAudio has loaded keeps its group. From then on, it runs AX's frames, the game's threads, and JAudio's
frame process 30 times a second.

The profile's 24 words are, in order: the machine's heap and the default heap's share of it; the static constructors'
runner; the builder of sead's start-up argument, the heap size that the argument gives, and sead's start-up; the
game's two creators of its root heaps, and the variable that then holds the sound's root heap; sead's
`setCurrentHeap`; the creator of the game's sound layer; JAudio's sound system object, its start-up, the size that
the start-up takes, and its frame process; the variable that holds the effects sequence's sound, and the variables
that hold the wave systems' list and their load statuses; `loadGroupWave`; `setCameraInfo`, the default camera's two
position variables and its matrix; and `startSoundVec`.

The output starts with the first frame after the sound starts. The sound has finished when JAudio has cleared the
sound's handle.

## End of a sound

Like any PSF player, a CSF player plays for the tagged length plus fade. Without a length tag, the player stops at
whichever comes first: the end of the sound plus half a second, or a default length and fade.

## Tags

The standard PSF tags apply: `title`, `artist`, `game`, `year`, `genre`, `comment`, `copyright`, `length`, `fade`,
`volume`, `_lib`, `_libN`. `csfby` credits the ripper. The ripper also writes `csf_sound` with the sound's label from
the sound archive (or a JAudio game's name for the sound), and `csf_mode` (`game` or `archive`). `csf_mode` lets players
and listeners tell the modes apart.

`length` and `fade` count time at the output's 48,000 Hz: a player plays `length` × 48,000 stereo samples before the
fade.

## Ripping

`csfrip` builds sets from a Wii U game or from a sound archive (`.bfsar`). A game can be a Wii U archive (`.wua`) or an
extracted title (a folder holding `code/`, `content/` and `meta/`). The game's files must be stored decrypted. Both
forms store them that way. A disc image (WUD, WUX) or an installable title (`title.tmd` and `.app` files) is refused. So
are other consoles' sound archives: those of the 3DS (CSAR), the Wii (RSAR) and the Switch. The Switch's sound archives
are `.bfsar` files in the other byte order. `csfrip --list` shows a game's sound archives. A game with a driver profile
is ripped in game mode, and anything else in archive mode. `--mode` overrides that choice. The rips go in a folder next
to the input, with the input's name and `_csf` added. `-o` names another folder. Inside that folder, each of the game's
sound archives gets a folder of its own. Each of those folders has the archive's name. A JAudio game has no sound
archives, and its rip goes straight into the folder.

`--only` takes the sequences whose labels match a regular expression. `--bgm` takes the music: the sequences whose
labels have BGM in them (in any case), and those that play on a music player. A music player's name has BGM, MUSIC, SONG
or FANFARE among the words that underscores separate (allowing digits after the word), and SE isn't among them.

The ripper lists and skips the sequences that make no sound. Some have a volume of 0 in the archive. The sound library
multiplies that volume into everything they play. The game can't make them heard. The length analysis (see "Game mode")
finds the others that never sound. The skipped sequences are placeholders, sequences that only set variables for the
game, muted copies of other sounds, and sequences that play only on paths that depend on what the game sets or on random
numbers. A rip of such a sequence would always take the same path. `--no-length` leaves out the analysis. Then only the
sequences at a volume of 0 are skipped.

### Game mode

The `.csflib` contains the game's executable and the sound archive as `FILE` chunks, and a process descriptor with the
game's profile. Each `.minicsf` selects one sequence with a `SND ` chunk. A sequence's length comes from playing it on
CSF's model of the sound library (see "Archive mode"). A looping sequence gets two loops plus a 10-second fade. A
sequence that ends gets its length up to its last sound, plus half a second. Either way, the length is at most 600
seconds. Sound means a sample louder than 8 of 32,767 (-72 dB).

A sequence loops when its main track jumps back to a command that the track has already played, outside a subroutine.
Inside a subroutine, such a jump counts only when the jump is unconditional and one of two things holds: the track holds
a note that sounds until the game stops the note, or the track has run no conditional command since the command that the
jump goes to. A sequence also loops when its main track reaches the end of a loop that repeats for ever. Only a pass
that changes the sound counts. A pass changes the sound when it plays a note, changes a setting of its track or of the
sequence, or opens a track. A tie or a monophonic note that continues a note at the same key and velocity changes
nothing. A pass that only waits, polls a variable or counts is idle. The main track is the sequence's first track. Once
that track has ended, or while it makes idle passes, the first track still playing becomes the main track if this track
played a note on the way round. Each track before the new main track must have ended, be idle or wait for ever, or come
after the first track and play no notes in its passes. A later track's pass that plays no note also counts while the
track holds a note that sounds until the game stops the note, on a looping wave at its sustain level. Every other track
still playing must then be idle or wait for ever. Such a pass changes that note, such as its volume or pitch. A track
waits for ever when it waits a negative number of ticks, waits out a held note, or waits for a note that never ends: one
with no length on a looping wave. When the loop moves to another track, or to another loop of the same track, the count
starts again. Loops that go round before the first sound don't count: a track that waits or sets values over and over
while a note fades in, or before another track starts, isn't the sound's loop. A loop that went round without a sound
ends the sequence at its last sound instead.

A sequence can reach a state where each track waits for its notes to end, waits out a held note (one of 9,600 ticks or
more), waits for ever, or only makes still passes with no note whose length will run out. A still pass is idle and draws
no random number. It also leaves the variables that tracks read with the values that the pass before left. A variable
that no track reads, such as a beat that a track marks for the game, doesn't count. Notes on looping waves never end.
Such a sequence is steady once no note's envelope is in its attack, hold, decay or release, and nothing is moving. A
steady sequence loops when every running wave has looped twice since the sound last changed. If the sequence has made no
sound since then, it ended at its last sound instead. A sequence whose tracks all wait for ever or make still passes
that play no notes has ended when its notes have. A sequence that stays busy without a sound for 180 seconds ended where
it fell silent. The ripper skips a sequence that never sounds.

A game whose sound runs on JAudio is ripped in game mode as a whole. One `.csflib` holds the game's executable and every
file of its sound folder but the streams, with a version 2 process descriptor that names the driver. Each `.minicsf`
selects a sound of the start-up archive's sound table by its JAudio ID with a `SND ` chunk, and lists the wave groups
that the sound requires (see "Wave groups"). The ripper plays each sound twice on the game's code: first with only the
game's common wave groups loaded, which shows the waves that its notes look up, then with the groups that hold those
waves. The second play gives the length. A sound loops when one of its tracks jumps back to a command that the track
has already played, twice to the same command. A sound holds when a track idles, every track has settled, and nothing
has moved for 10 seconds. It loops there if it still sounds. Any other sound runs until the game releases it or until it
falls silent for good. Loops, ends and the limit then give lengths as above. In a JAudio1 game, the ripper offers the
sequences of the sound table but the first, the effects sequence. That sequence plays all along, and the sound effects
play in its tracks. The analysis follows only the sound's tracks.

Game profiles are specific to each game and version. A profile is matched by the title's ID and the SHA-1 of the title's
executable. Version 1 has a profile for Animal Crossing: amiibo Festival (USA). Version 2 adds a JAudio2 profile for The
Legend of Zelda: Twilight Princess HD (USA) and a JAudio1 profile for The Legend of Zelda: The Wind Waker HD (USA).

### Archive mode

In `FILE` chunks, the `.csflib` holds the sound archive and any group files that its sequences require. The `.csflib`
also holds an archive descriptor. Each `.minicsf` selects one sequence with a `SND ` chunk. Lengths come from the same
analysis as in game mode. For a game, the descriptor gets the game's SDK version. For a game with a profile, the
descriptor also gets the voice count and AX settings from the profile. For a loose `.bfsar`, the descriptor gets 96
voices and the 32 kHz renderer. Each device's output then reaches 48 kHz after the final mix stage. The ripper prints a
notice that archive mode uses CSF's model of the SDK's sound library and that a game with a profile gives exact rips.
Sequences whose data isn't all there are listed and skipped. Wave sounds aren't ripped.

Tags written by `csfrip`: `title` and `csf_sound` (the sound's label), `game`, `length` and `fade`, `csf_mode`, `csfby`,
`utf8=1`, and optionally `artist`, `year` and `copyright`.

## Copyright

A `.csflib` contains copyrighted game code or data. Rips are for personal use.
