# 1.0.0 poison collision readback preview

Issue [#3](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp/issues/3)
reports damage from invisible poison during the Wooded Kingdom Spewart fight
on game 1.0.0 / GTX 1060. The supplied save and log were used locally with a
freshly generated, compatible 1.0.0 game-code library. No report or save was
uploaded, and no production install or saves were changed.

## Evidence and candidate

`PaintObj::checkPaintAlpha` calls
`al::getColorFromLinearTexture(TextureData,x,y)` and tests the returned alpha's
high bit. The reader packs RGBA8 bytes into a color; its CPU memory pool was not
receiving the corresponding GPU render-target contents.

The private Spewart run observed a native 128x128 RGBA8 (`0x25`) linear render
target, pitch 512. At (64,64), CPU memory contained `0x91890005` while the GPU
image contained `0x00000000`. Rendering into this exact texture was also logged;
this was not merely a comparison against an unrelated allocated image. Across
later queries CPU and GPU values continued to differ at clear mask samples.

The opt-in candidate stages the requested GPU texel on the render thread, then
copies only its four validated bytes back on the game thread, before the
original reader continues. The existing bounded per-present cache avoids a GPU
readback for every CPU query. Only native, single-layer, single-sample RGBA8
images owned by the GPU qualify. CPU-authored textures, unknown builds, scaled
images and unsupported formats retain their existing behavior. Blanket
pass-end writeback remains off.

The hook is confined to main build ID `3ca12dfaaf9c82da`, MemoryPoolMap return
PC `0x7000a2b7cc`. Disassembly verifies w19=y, w20=x, x21=TextureData and
w22=pool offset; pitch is loaded from TextureData+0x4c after Map. The hook
matches the reader's 32-bit row arithmetic and validates the CPU pool span.
The internal texel request supports a packed RGBA8 element while existing F16
requests retain their two-byte interpretation.

`ODYSSEY_PAINT_CPU_READBACK=1` enables this preview. It remains **off by default**
pending manual interaction testing. `ODYSSEY_LOG_PAINT_READS=1` enables sampled
CPU/GPU comparison logs without itself changing CPU bytes.

## Checks and limits

- Windows build passed; bounded baseline, comparison and candidate fights exited
  normally on RTX 4090. Original stale CPU bytes became the sampled GPU values
  with the candidate enabled. A later painted sample returned `0xff0000ff`,
  including nonzero alpha: the candidate does not unconditionally erase poison.
- The 1.4.1 Cap Kingdom regression also exited normally with valid nonzero F16
  samples. Its existing fog hook remains enabled and its copy size remains two
  bytes. The approved fog-preview executable was preserved.
- These runs establish the memory mismatch and the candidate's data path. They
  do not by themselves establish that every lost heart was invisible-poison
  damage; Spewart contact and visible poison can also hurt Mario. Two attempted
  edge controls landed outside the arena and respawned; discard those as health
  comparisons.
- Manual checks must include crossing ground after clearing it with Cappy,
  taking damage from actual visible poison, and completing/retrying the fight.
  Do not close the public issue before that confirmation. GTX 1060 and other
  hardware, long sessions, experimental 120 FPS, other paint effects and Linux
  remain unverified. The Metal renderer does not implement this readback path.
- This hooks the named color reader; other raw CPU paint-processing loops have
  not been established as coherent. It is not a general NVN memory-coherence
  implementation or proof that all poison-related rendering is fixed.

## Private reproduction

Artifacts are under `build/poison-review/` and must not be committed: reporter
save/log, game-derived placement JSON, settings, per-case copied saves, logs and
captures. The test helper `ODYSSEY_TEST_PLAYER_POS=2400:-4350:6800:-20400` moves
Mario near Spewart after frame 2400 on 1.0.0. It calls the game's
PlayerPuppet::resetPosition rather than changing simulation timing or leaving
stale custom collision state. It is inactive without the explicit variable.
`boss002` is the later flower-field entrance, not Spewart's arena.

The manual launcher enables the candidate, uses a copied reporter save and
private settings/cache, and allows normal controls. Resume, wait for the test
placement if necessary, advance/skip the Broodal introduction, and test the
fight. Restart the launcher to repeat the one-time placement after a death.
