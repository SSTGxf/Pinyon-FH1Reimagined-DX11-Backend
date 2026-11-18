# Baseline run evidence — Band 2 isolation experiment (2026-11-15 ~16:32–16:37 CST)

## Run
- Build: GREEN (BUILD_EXIT=0, exe 15:04:10 PM PC, dll 15:03:54 PM PC).
  All four new strings verified in the DLL.
- Config: `fh1_debug_band_suppress = 0` (Baseline, nothing suppressed),
  `fh1_debug_band2_trace_swap = 2200`, resolve + swap-fetch + tile-trace
  (2200) + bins (2200) + transition log all on.
- Game: pid 14436, session 1, launched via "PinyonShift Test Run".
  Ran 15:32:16–15:37:51 PM PC (~5.5 min), reached swap 2606.
- Logs: runtime.log (2.6 MB, 11,162 lines, swaps 2200–2606) +
  runtime.1.log (5.24 MB, the rotated file covering swap 2200) +
  runtime.2.log / runtime.3.log. The runtime.log ROTATES at ~5 MB, so the
  swap-2200 band2 packet lines live in runtime.1.log, not runtime.log.

## HEADLINE: Band 2 DOES draw — the earlier "zero Band 2 draws" does not hold

At swap 2200 (all in runtime.1.log):

| Stage | Band 2 result |
|---|---|
| Guest packets (pass=1, select 0xC) | **1,138** |
| Predicate accepted | **1,132** |
| Predicate rejected | 6 (reason `empty_bin_intersection`, mask=0x20) |
| D3D11 draws issued (pass=1, window_y=-256) | **900** |

Tile-draw counts at swap 2200 (runtime.1.log, 2,082 total):
- Band 2 (window_y=-256): **900 draws**, all viewport_h=464,
  scissor_h=256, claim 0..256.
- Band 3 (window_y=-512): 523 draws, all viewport_h=208.
- Band 1 (window_y=0, viewport_h=256): 55 draws.
- Full-frame (window_y=0, viewport_h=720): 659 draws.

The 8 "FH1 band2 ERROR" lines: 6x predicate rejected
(`empty_bin_intersection`, mask=0x20) + 2x "D3D11 resolve planning failed".

## What this means

Band 2 is NOT missing at any stage: packets arrive, the predicate accepts
them (1,132/1,138), shared processing passes them through, and D3D11 issues
Band 2 draws (900). The earlier tile-trace observation (zero Band 2 draws
in one frame of the previous run) was a sampling artifact — the tile trace
only captures a slice of the frame (in this run it caught just 23
full-frame draws at the end of swap 2200), not the full band activity.

The problem is therefore DOWNSTREAM of the draw stage. The draws all claim
the SAME EDRAM rows (Band 1: 0..256, Band 2: 0..256, Band 3: 0..208), so
they overwrite each other — this is the missing-untile / row-separation
defect (Rank 1). Band 2's content is drawn but then overwritten by the
other bands writing to the same rows, and its resolve reads rows that no
longer hold Band 2's content.

## Consequence for the experiment

Test A (suppress Bands 1 and 3, isolate Band 2) is now the decisive test:
if Band 2's content becomes visible when the other bands stop overwriting
its rows, that confirms the untile/row-overwrite mechanism. Test B
(suppress Band 2) validates the classification. The bin-predicate path is
exonerated as the cause of the missing middle band.

## Open items
- The resolve trace still shows `source_row_offset=0` for all three
  resolves (same as before) — the untile separation is still absent.
- The 2 "D3D11 resolve planning failed" errors are unexplained (minor).
- The 6 predicate rejections (mask=0x20, empty_bin_intersection) are a
  small fraction (0.5%) — not the cause.
