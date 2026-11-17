# Status

How far the port is from the goal: **Super Mario Odyssey on PC, 1:1 with the Switch, with no crashes, no stutter and
steady memory, on Windows, Linux and the Steam Deck.**

The bars in the README come from the table below. After changing a number, run `python3 tools/progress_svg.py`: it
redraws `docs/media/progress/*.svg`, and the overall bar is the weighted average.

<!-- progress:begin -->
| Area | Weight | Done |
|---|---|---|
| Core game | 30% | 80% |
| Stability | 20% | 65% |
| Performance | 20% | 60% |
| Graphics options | 10% | 80% |
| Platforms and input | 12% | 65% |
| Player features | 8% | 60% |
<!-- progress:end -->

The numbers are the maintainer's judgement, not a measurement. First estimates, 2026-11-17, from the release notes
and the open issues.

## What each area means, and what is left

### Core game (30%)
The whole game from start to end, on game versions **1.0.0** and **1.4.1**, looking and behaving like the Switch:
rendering, effects, shadows, physics, timing, audio.
- Done: every kingdom playable on both versions; game speed stays real time below 60 FPS; the game's own dynamic
  resolution can be turned off.
- Left: Moon Kingdom warp flags (#37); audio crackling and cutscene distortion reported on 0.4.0 (#83, #84);
  side-by-side checks with the Switch for every kingdom; 1.4.1 is less tested than 1.0.0.

### Stability (20%)
No crashes and no GPU hangs, in any kingdom, at any frame rate and with any setting.
- Done in 0.4.0: Lake Kingdom above 60 FPS on 1.0.0 (#72), crash on quit with DLSS frame generation (#70, #80), key
  binding crash (#40), 120 FPS interpolation removed.
- Left: entering paintings above 60 FPS on 1.0.0 (#72 follow-up, #85); GPU hang on Linux with RX 7800 XT (#21);
  switching the upscaler with FSR frame generation on (#77); the wardrobe (#69); a nightly scripted tour of every
  kingdom to catch new ones.

### Performance (20%)
A steady frame rate with no stutter, steady memory over long sessions, and low-end hardware.
- Done: shaders compiled ahead of play and again after driver updates; Steam Deck 1280x800 at 60 FPS at 10 W in
  Gaming Mode; early frame hand-off and multithreaded rendering; frame rates from 60 to 240.
- Left: the game is CPU-bound at high frame rates (see [improving-cpu-performance.md](improving-cpu-performance.md));
  memory: the committed figure players see is far above what is really in use, and it should be lower too; drops in
  Lake and Wooded Kingdoms (#42, #75); a defined low-end floor (4 cores, integrated GPU, 8 GB) at 30 FPS.

### Graphics options (10%)
Resolution, upscaling, anti-aliasing, screen shapes and the HUD.
- Done: up to 4x internal resolution; DLSS / DLAA, FSR 2 / 3.1 / 4, XeSS, FSR 1, SMAA, FXAA; the HUD drawn at full
  resolution; 16:9, 16:10, 21:9, 32:9, 4:3 and Fill; shadow quality and texture detail; frame generation (AMD FSR,
  NVIDIA DLSS beta).
- Left: upscalers in cutscenes where the game lowers its own resolution (#66); black picture with DLSS beta (#77).

### Platforms and input (12%)
Windows, Linux, the Steam Deck and handhelds, keyboard and mouse, every controller.
- Done: Windows; Linux and Steam Deck AppImage (experimental); keyboard and controller rebinding; mouse camera; motion
  controls as on the Switch; Joy-Con pairs; mouse in the game's menus.
- Left: the Steam Deck's own gyro (#68); Add to Steam on the Deck (#81); Linux on RDNA3 GPUs (#21); shake and spin
  throw on any key or button (#62, #73).

### Player features (8%)
Everything around the game.
- Done: installer, one-click updates, RomFS mods, the Brazilian Portuguese fan translation, Discord activity (off by
  default), performance overlay, crash reports.
- Left: amiibo (#48), menu layout for mouse and keyboard (#82), launcher shortcuts and controller access to the
  settings (#81).
