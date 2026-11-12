# Super Odyssey PC Recomp on Linux and Steam Deck

The Linux version is an AppImage: one file that runs on SteamOS and on desktop distributions (glibc 2.35 or newer:
Ubuntu 22.04, Debian 12, Fedora 36, Arch, SteamOS, ...). It's built with `make appimage` (`tools/linux/appimage.sh`,
in Ubuntu 22.04 containers).

You need a Vulkan 1.3 driver (Mesa RADV / ANV / NVK, or NVIDIA's), a CPU with AVX2 (2013 or newer), about 12 GB of free
space, and your own copy of Super Mario Odyssey (1.0.0 as NSP, XCI or an extracted folder, optionally the 1.4.1
update) with the prod.keys from your own console.

## Installing

    chmod +x Super.Odyssey.PC.Recomp-<version>-linux-x86_64.AppImage
    ./Super.Odyssey.PC.Recomp-<version>-linux-x86_64.AppImage

The installer asks for your game file, the optional update, prod.keys (found by itself next to the game file or in
`~/.switch`) and where to install. Then it reads your game, builds the game code for your PC (a few minutes) and
compiles every shader, once.

If the AppImage won't start with a FUSE error, install your distribution's `fuse3` package, or run it with
`APPIMAGE_EXTRACT_AND_RUN=1`.

## Steam Deck

Do the install once in **Desktop Mode** (the file pickers need it), then play from **Gaming Mode**.

1. Put the AppImage on the SD card (for example `/run/media/deck/<card>/Super Odyssey/`) and make it executable
   (Properties > Permissions > *Is executable*, or `chmod +x` in Konsole).
2. Start it. At **Install the game to**, choose a folder on the SD card: the game data, the compiled shaders and your
   saves go there, nothing big stays on the internal drive. The choice is remembered in
   `~/.config/super-odyssey-pc-recomp/data-folder.txt`.
3. Add the AppImage to Steam (Steam > *Add a Non-Steam Game*), then play it from Gaming Mode.

On a Steam Deck the game starts with the **battery saver** settings: 75% internal resolution (960 x 600) upscaled with
AMD FSR 1, no SMAA, no 120 FPS or interpolation, 4x texture filtering. The menu's Performance tab has a *Battery
saver* button to apply them again after changing things. For the biggest savings, also lower the power limit in the
Deck's Quick Access menu (Performance > TDP Limit, e.g. 8-10 W). Don't use the Deck's frame limit below 60: Odyssey's
gameplay runs one step per frame, so a 30 or 40 FPS limit slows the game itself down.

## Where things are

The install folder you chose (default `~/.local/share/super-odyssey-pc-recomp`, or `$XDG_DATA_HOME/...`):

- `extracted/` - your game data and the game code built from it
- `save/` - your saves, `cache/` - compiled shaders, `powermoon.ini` - settings

The log, `SuperOdysseyRecomp.log`, is always in `~/.local/share/super-odyssey-pc-recomp` (on the internal drive,
wherever the game is installed); started from a terminal, it prints there instead.

`ODYSSEY_DATA_ROOT=<dir>` uses another folder for one start.

## Updates

The menu's Updates tab (F1 > Updates) downloads the new AppImage from this project's releases and replaces the one you
started, then restarts. It uses the system's `curl`, and the AppImage's folder must be writable (an SD card is fine).

## Reporting a problem

Open an issue with your distribution or SteamOS version, GPU and driver version, what happened, and the log file.
Never attach game files or keys.
