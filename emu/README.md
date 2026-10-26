# The firmware in Hatari (EmuMD)

[EmuMD](https://github.com/neilrackett/emumd) builds this firmware's own C
code for the host as a `.mdfw` and runs it in its fork of Hatari, which has
a Multi-device on the cartridge port: the ST boots the cartridge image, the
firmware's `main()` runs on a host thread as core 0 and reads the ST's ROM3
accesses as it does on the device. It is the fast loop for logic, drawing,
input and the ST side. It times nothing on the RP (the emulated Multi-device
is infinitely fast): every timing is measured on the device with
`tools/dev/`.

## Setup

```sh
git submodule update --init emu/emumd
emu/emumd/tools/mdfw hatari          # EmuMD's Hatari, into ~/.cache/emumd/hatari-fork
mkdir -p emu/tos
ln -s "/path/to/TOS 1.62 - English-UK - 50Hz" emu/tos/tos162uk.img
```

`emu/tos/` and `emu/sd/` (the emulated microSD card) are gitignored. Stock
Hatari has no Multi-device: only the fork runs a `.mdfw`.

On a Mac, two things in the environment can break the fork's build; both
settings stay in its CMake cache, so later `mdfw hatari` runs keep them:

- An SDL2 framework in `/Library/Frameworks` is found before Homebrew's and
  linked without an rpath, so Hatari cannot start. Use Homebrew's:
  `cmake -S ~/.cache/emumd/hatari-fork -B ~/.cache/emumd/hatari-fork/build -DSDL2_DIR=$(brew --prefix sdl2)/lib/cmake/SDL2`
- XQuartz's `/opt/X11/include` carries an old libpng's headers, which then
  shadow Homebrew's and break screenshots and recordings ("failed to write
  png frame"). X11 is not needed: add `-DCMAKE_DISABLE_FIND_PACKAGE_X11=ON`
  to the same command.

Then `emu/emumd/tools/mdfw hatari` again.

## Build and run

```sh
emu/emumd/tools/mdfw build           # build/md-chorensa.mdfw
emu/emumd/tools/mdfw run             # in a window
emu/emumd/tools/mdfw run --headless --frames 400 --screenshot build/shot.png
```

`mdfw.ini` (at the repository's root) lists the sources: everything in
`rp/src` but the hardware layer EmuMD stands in for (the bus, SELECT, the SD
card driver), and this folder. The machine is an STE with TOS 1.62 and the
borders shown (`--borders on`); `mdfw run --machine st`, `-- --machine
megaste` and other Hatari options after `--` change it.

## What this folder adds

- `fb_workers.c`: the framebuffer's Thumb assembly workers
  (`fb_chunked_asm.S`, `fb_blit_asm.S`) as C, which the host cannot
  assemble; the host tests use the same file.
- `shim/hardware/interp.h`, `interp.c`: the interpolator, as far as the
  firmware uses it.
- `template_extra.c`: `commemul_latest()` and `commemul_getOverruns()` on
  EmuMD's ROM3 ring, and `select_poll()`.

In the firmware itself, `fb_chunked.c` passes core 1's jobs through a slot
under `#ifdef EMUMD` (a host's pointers do not fit the 32-bit FIFO).
