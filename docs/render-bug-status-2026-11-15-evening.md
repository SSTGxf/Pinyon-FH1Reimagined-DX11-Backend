# Pinyon D3D11 — Status, evening 2026-11-15

## The band bug is fixed

The month-long render bug (free-roam world not rendering, flat blue, thin band) is **fixed and verified visually**. Cause: all three FH1 bands drew into the same EDRAM rows (0–256) and overwrote each other — D3D11 had no untile system. Fix (Copilot, in the build): expanded scene surface, viewport/scissor shifted by `-window_y_offset`, `untile_band` passed as `(untile_band << 16)`, `EndUntiledTiling` copy-down. Baseline (all bands on) now shows sky + world + cockpit simultaneously. Seth: "game actually works better then Mac at rendering."

Build: green via `muse_build_game.bat` with `CMAKE_BUILD_PARALLEL_LEVEL=4` (16 GB RAM; full parallelism OOMs clang). Current exe 2026-11-15 20:15 PM PC, includes Copilot's trace instrumentation. Launch only via scheduled task "PinyonShift Test Run" (session 1); SSH launches run in session 0 (Seth hears it, does not see it).

## What remains (separate issues)

1. **Blue tint.** Fades gradually during gameplay as surfaces populate — suggests stale/wrong surface data correcting frame by frame, not only a static fetch-swizzle error. D3D11 still drops the guest fetch red/blue swizzle (Rank 3); Claude also raised a gamma hypothesis (presenter `guest_output.hlsl` applying gamma to an already-linear surface). Unresolved which dominates.
2. **Black rectangle on menu exit.** Claude's mechanism: untile bands are keyed by surface `packed_key`; on menu→gameplay the scene surface is reallocated/rebound with different tiling state, and if `EndUntiledTiling` didn't run at the end of the last frame (or no clear is issued through the resolve path), stale band data stays and shows as the black block.
3. **"Invalid transfer rectangle" errors** in the log (2026-11-15 20:45 run): transfer rect y ranges exceed source height (e.g. rect `0,2048-80,2064` with `source_height=2048`). Real out-of-bounds transfers; cause not yet traced.
4. **"D3D11 resolve planning failed" errors.** Used to appear twice in every run. None in the latest 20-second log segment (swaps 3429–3511) — possibly fixed by Copilot's Fix 6, unconfirmed. Note: Freebuff's forensics capture (frames 2000–2400) recorded 2,400 plan failures = 6 per frame, not "two per run" — either the RF hook also records non-failing skips, or there are far more planning skips than estimated. Answerable from the capture (RF id + skip text are in the events).
5. **Error-handling defects** (ChatGPT audit + Claude, confirmed in source): `SubmitFrameTape("draw")` return ignored; `WaitForGpu()` returns true on a failed HRESULT; `SubmitFrameTape("resolved range refresh")` return ignored.
6. **Band 0 scissor not extended** to the full untile height (Vulkan does this), so the game does three full draw passes instead of one — triple the draw load on the GTX 570, and band 0's scissor may clip objects belonging to lower bands.

## Copilot trace

Copilot's viewport/scissor band trace is in the 20:15 build, but it produced **no output** in the 20:38 run — the launch did not pass `--d3d11_band_trace=true`, so the trace never ran. The untile copy-down IS running (162 lines in the tail log, all band 512, rows 0..208, samples 4).

## Rendering forensics system

Design doc: `docs/dx11-rendering-forensics-design-2026-11-15.md` (927 lines). Extends the existing `render_audit` JSONL system. 35 hook locations, 128-byte event record, JSONL capture, 9 visual debug modes, honest 3-layer pixel-history alternative (exact per-pixel history is impossible on D3D11 FL 11_0 without replay), ~512–768 KB per captured frame, recorder ~1–2 ms/frame. 8 stages.

Freebuff is implementing it. **Stage 0/1 done + verified 2026-11-15 ~22:21 CST:** `render_forensics.h` (128-byte event, bounded ring, JSONL writer), RF001–RF008 table frozen, cvars added, Gap G11 fixed (transition.log rotates at 1 MB), hooks H-RES-1/H-RES-2/H-RSC-1 placed. Rebuilt binary BUILD_EXIT=0, 0 errors. Wide-window capture (frames 2000–2400): 55,300 events — 42,800 resource creates, 10,000 resolve entries, 2,400 plan failures. Off state verified: 0 files, 0 log lines when disabled. Freebuff flagged two gaps plainly: single-frame capture is broken (frame-clock/window off-by-one, [2200,2200] captures zero events) and the 6-per-frame plan-failure count above. Stage 1 is not complete until the off-by-one is fixed and per-frame overhead is measured against the 5 ms cap.

## Team

Seth (thinker/reasoner) · Muse (search + verify against source/traces) · ChatGPT (graphics research partner; dedicated "DX11 Backend Graphics Help" conversation) · Copilot (implementation) · Freebuff (forensics implementation) · Claude (audits).
