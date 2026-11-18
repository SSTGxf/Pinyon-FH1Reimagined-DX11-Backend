# Test B evidence — Band 2 suppressed (2026-11-15 ~16:57–17:01 CST)

## Run
- Config: `fh1_debug_band_suppress = 2` (Band 2 suppressed, Bands 1 and 3
  active), band2_trace_swap=2200, resolve + swap-fetch + tile-trace
  (2200) + bins (2200) + transition log all on. Same build as Baseline/Test A.
- Game: pid 24852, session 1, launched via "PinyonShift Test Run".
  Ran 15:57:36–16:01:56 PM PC (~4.3 min), reached swap 2237.
- Logs: runtime.log (4.8 MB) + runtime.1.log + runtime.2.log +
  resolve-trace.log.

## VISUAL RESULT: middle region blank, Bands 1 and 3 render normally

Seth's photos (17:01 CST):
- Photo 1: top band solid blue (Band 1 sky, with a small terrain patch on
  the left edge); middle band pale white/blank (Band 2 suppressed); bottom
  band shows the cockpit/dashboard (Band 3), heavily blue-tinted. Black
  rectangle artifact visible on the left.
- Photo 2: top dark with scattered yellow dots (Band 1 night sky / distant
  lights); middle white/blank (Band 2 suppressed); bottom band shows the
  cockpit, speedometer (black rectangle over it), "1.7 MI" waypoint, and
  the road on the left (Band 3).

The middle region has NO scene content when Band 2 is suppressed, while
Bands 1 and 3 render normally. This is the exact complement of Test A
(Band 2 isolated -> middle shows the world).

## Log numbers (strong — the trace caught the world state this time)

- **Band 2 packets at swap 2200: 998 draw packets + 292 IB packets**
  (vs 910 + 228 in Baseline). The game WAS in the world at the trace.
- **983 "FH1 band suppression" lines, ALL pass=1** (Band 2),
  select=0xC, suppress_mask=02. The suppression fired on 983 Band 2 draws.
  Sample: `pass=1 select=000000000000000C mask=000000008000003F
  suppress_mask=02 packet=C0032201`. 983 matching "FH1 band2:
  diagnostic suppression" lines.
- **Tile draws at swap 2200 (846 total): Band 3 (wy=-512, vh=208) = 515
  draws; Band 1 (wy=0, vh=256) = 45 draws; full-frame (wy=0, vh=720) =
  259 draws; 8192-wide world pass = 6 draws. ZERO wy=-256 (Band 2) tile
  draws.** Band 2 produced no tile claims at all.
- **15 ERROR lines:** 13x predicate rejected (12x op=22 + 1x op=3F,
  all empty_bin_intersection, mask=0x20) + 2x "D3D11 resolve planning
  failed" (same 2 as Baseline and Test A). The 15 errors + 983
  suppressions = 998 draw packets, accounting for every Band 2 packet.

## Frontbuffer

- guest_words_nz at swap 2200: 28,500 (same as Baseline 28,486 and
  Test A 28,480). Note: in Test B the middle is pale WHITE (nonzero
  0xFFFFFFFF), not black, so the word count stays high even though Band 2
  drew nothing. The white is the clear color showing through.

## Conclusion

Test B VALIDATES the band classification and completes the isolation
experiment:
- Band 1 = top (rows 0–256): sky.
- Band 2 = middle (rows 256–512): the world/scene (trees, road, car).
- Band 3 = bottom (rows 512–720): cockpit/dashboard.
- Suppressing Band 2 removes the middle scene cleanly while Bands 1 and
  3 render normally. Isolating Band 2 (Test A) shows the middle scene.
- Together, Baseline + Test A + Test B prove the bands compete for the
  same EDRAM rows and overwrite each other, and that each band's content
  is real and correctly attributed.

The next step is the fix: derive the correct D3D11 source-row mapping
from D3D12/Vulkan and implement band separation (the untile equivalent)
so each band's content survives to its own resolve. The 2 "D3D11 resolve
planning failed" errors (present in all three runs) deserve a separate
investigation.
