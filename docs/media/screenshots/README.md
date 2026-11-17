# README screenshots

The README shows four screenshots side by side: `shot-1.jpg` to `shot-4.jpg` in this folder. They are captured by the
maintainer in the port, from their own copy of the game. Never use screenshots from the Switch, other ports or the
internet.

## Settings for every shot

- Display resolution **3840 x 2160**, Internal resolution **100%** (native 4K).
- **Frame rate 240** (Performance > Frame rate), and check in the performance overlay that it really holds near 240
  at that spot before capturing. If it doesn't, change the README caption to the frame rate it really shows.
- Anti-aliasing: DLAA or SMAA. Shadow quality and texture detail at their highest.
- Performance overlay **off** for shots 1-3, **on** for shot 4 (it shows the resolution and FPS).
- The game's HUD may stay; the recomp's settings menu closed.

## The four shots

| File | Where | Why |
|---|---|---|
| `shot-1.jpg` | Metro Kingdom, New Donk City by day, a wide street view | The busiest scene: crowds, buildings, draw distance |
| `shot-2.jpg` | Seaside or Lake Kingdom, looking over water | Water, reflections, colour |
| `shot-3.jpg` | Cap Kingdom, Bonneton, with its fog | The game's look and atmosphere |
| `shot-4.jpg` | Any busy kingdom, **performance overlay on** | Proof: 3840 x 2160 at 240 FPS |

## How to capture

Either:

- Windows: **Win + Print Screen** (saved to Pictures\Screenshots), or the NVIDIA app (Alt + F1); or
- The port's own capture: start the game with the environment variable `ODYSSEY_WINDOW_SHOT=<frame>` set, and it
  saves the window image at that frame as `build/window.png` (from the folder it was started in). Frames count from
  the start; the stats line in the log shows the current frame.

Then save as JPEG (quality about 85, under about 1 MB each) with the names above, here in `docs/media/screenshots/`.
