# Tools for the original and for the firmware in emulators

Two emulators, two jobs:

- **FS-UAE runs the original.** Cho Ren Sha 68k is an X68000 game; its
  Amiga port (<https://github.com/apolkosnik/chorensha>) runs the original
  68000 code unchanged on a 68020/68030 and, given a frame count, plays the
  attract demo and records the game's state every 60 frames and its screen
  every 500. That is the reference every build of ours is compared with.
- **Hatari runs the firmware**, through EmuMD (`emu/README.md`): the
  firmware's own C code built for the host, with an emulated Multi-device
  on the cartridge port.

Nothing of the game is in this folder or anywhere in the repository: the
user's copy, the run folders and everything made from them stay outside it.
Standard-library Python 3 throughout.

## Setup

- FS-UAE (3.2.35 used here), an Amiga Kickstart 3.1 for the A1200
  (40.068) and the Workbench 3.1 disk 2 ("Workbench"), and amitools
  (`pip install amitools`, for `xdftool`) to read that disk.
- EmuMD's Hatari and a TOS 1.62 image (`emu/README.md`); TOS 1.04 for the
  ST and Mega ST if wanted, and TOS 2.06 for the Mega STE (which TOS 1.62
  does not boot).
- The user's copy of the game: a checkout of the repository above, outside
  this one. Its X68000 program exists there only as a hex dump; rebuild it
  with `xxd -r binaries/x68000/sz2.hex SZ2.X`.
- `cp re.cfg.example re.cfg` and fill in the paths (`re.cfg` is gitignored),
  then `./amiga_system.py` once to build the Amiga system drive.

## Tools

| Tool | What it does |
| --- | --- |
| `amiga_system.py` | Builds the Amiga system drive from the Workbench disk: the disk unpacked, and a startup-sequence that runs the test a run leaves on the second drive |
| `amiga_run.py` | Runs the Amiga port headless in FS-UAE (`--frames N`, `--config a1200-030` or `a1200-8mb`), one folder per run: `command.txt`, `fs-uae.log`, `output.txt`, `WORK/` (the program and data it ran with), `checkpoints.bin`, `shots/` (screen dumps and PNGs), `result.txt` (PASS when the checkpoints match the port's reference, a shorter run against its start) |
| `oracle.py` | The port's two formats: checkpoints (`text`, `compare`) and screen dumps (`png`); our builds write the same ones |
| `hatari_run.py` | Runs the firmware in EmuMD's Hatari, headless and scripted, one folder per run: `command.txt`, `hatari.out` (Hatari's and the firmware's log), `shots/`, `sd/` (the emulated card), `result.txt` |
| `recfg.py` | Reads `re.cfg` for the others |

FS-UAE on macOS has no headless video driver (SDL's offscreen one gets no
OpenGL context), so `amiga_run.py` opens its window hidden; it takes no
screenshot from outside either: the port dumps its own screens.

## Scripts (`scripts/`)

Expectation files for `hatari_run.py --expect`, one step a line: `expect
<name> <regex>` (a line of output, the firmware's log included), `quiet
<name> <s>`, `key <key>` (pressed and released in one event: in
fast-forward a host-timed hold lasts seconds of the ST's time), `hold <key>
<s>`, `fire [s]`, `mouse <dx> <dy>`, `click [s]`, `debug <command>`, `stage
<file>`, `shot <name>`, `sleep <s>`, `record <name>`. The script's header
documents each.

| Script | What it shows |
| --- | --- |
| `bench.exp` | The bench's screen after the ST's hello, then ESC back to GEM |
| `overscan.exp` | The 256-line test card and the borders' counts; with `--trace video_sync` Hatari logs where each 50/60 Hz switch lands |
| `overscan-mouse.exp` | The same with the mouse moved flat out (F2: mouse mode) |
| `overscan-long.exp` | The same left running, with screenshots along the way |
| `overscan-silent.exp` | The same with no sound from the ST's next boot (F5) |
| `overscan-copy.exp` | The frame in both copiers' layouts: auto, the CPU, the blitter (F7) |
| `overscan-piece.exp` | The blitter in pieces of 16, 32, 4 and 8 lines (F8) |
| `overscan-lines.exp` | The copy of the frame's top 240 lines (F9), then all 256 again |
| `overscan-ym.exp` | The ST family's path on an STE: the YM preferred (F10), a warm reset, the tone, 240 lines |
| `dma-tone.exp` | A 440 Hz tone (F6) recorded from Hatari (`--sound`): the DMA chip on an STE, the YM on a plain ST (`--machine st`); the recording's frequency and its longest hold say whether a sample was skipped or held |

## Examples

```sh
./amiga_run.py --frames 6000 --name oracle        # the whole attract demo, against the reference
./oracle.py png ~/chorensa/runs/<run>/shots/*.bin # screen dumps to PNG
./hatari_run.py --build --expect scripts/bench.exp --name bench
./hatari_run.py --machine st --expect scripts/bench.exp
```
