# Phase 2f — EDRAM resolve probe: patch + build DONE, run BLOCKED on licence screen

**Date:** 2026-11-17 (evening CST; PC clock ~1 h behind CST)
**Run:** D11 resolve-probe run 1 (PID 9712, Phase 2f build, probe from swap 3000, budget 12)
**Verdict:** NOT answered yet. The probe never fired because the game never entered free roam — it is stuck on the GTX 570 licence screen, the same pattern as Phase 2d and 2e. This is a run-state failure, NOT a probe failure. The patch and build are verified. The probe gate (`presented_output_frames_ >= 3000`) does NOT expire — if Seth presses Start and the game enters free roam, the resolve probe fires on this same running game (PID 9712), no relaunch needed.

---

## 0. What Phase 2e proved (the question this probe answers)

The EDRAM **does** get coloured after the EDRAM triangles draw — it does **not** stay flat grey (report `~/workspace/dx11-edram3d-2e-results-2026-11-17.md`). Decisive probe #7 (band 2, pass 0): before perfectly uniform 66,66,66; after changed 28.9%, bottom pixels (85,124,146) — real sky-blue. All 12 probes on EDRAM 0/32/4x/c3, all textured, all large world triangles. The loss is **downstream of the EDRAM write** (resolve or composite). Phase 2f asks: does the coloured EDRAM content make it into the resolved texture (the guest-memory mirror), or where does it disappear?

---

## 1. The EDRAM resolve path (read from source)

### 1.1 The resolve is a compute-shader dispatch, not a hardware resolve

File: `E:\PinyonShift\work\.local\rexglue\src\graphics\d3d11\fh1_native_resolve.cpp` (467 lines).

- `D3D11Fh1NativeResolve::ResolveToMemory` — dispatches an SM 5.0 compute shader (`fh1_native_resolve_memory_color_cs` / `..._ms_cs` for MSAA, plus depth/uint variants) that reads the EDRAM surface via one-use SRVs and writes the resolved (linear) pixels into a destination UAV.
- `PlanResolveMemory` packs the constants: `constants[2]` = destination layout (resolve_key), `constants[3]` = source layout (owner = the EDRAM surface key), `constants[4]` = sample_select, `constants[5]` = dest_info | scale | unscaled flag, `constants[6]` = dest_base, `constants[7]` = dest_pitch. Dispatch is `(width+7)/8 x (height+7)/8` groups.
- `RecordResolveMemory` is the same plan appended to the command tape as a dispatch packet for replay.

### 1.2 Where the resolve is triggered (IssueCopy)

File: `E:\PinyonShift\work\.local\rexglue\src\graphics\d3d11\command_processor.cpp` (7,334 lines after the Phase 2f patch), function `IssueCopy()` (line 4542).

- `Fh1PlanResolve(*register_file_, *memory_, flags, plan)` (line 4575) — plans the resolve from the guest's registers. The plan carries `plan.resolve_key` (the EDRAM surface key, e.g. 0/32/4x/c3), `plan.dest_base`, `plan.dest_pitch`, `plan.dest_info`, the rect (`plan.x0/y0/x1/y1`), and the destination extent (`plan.info.copy_dest_extent_start/length`).
- `untile_band` (line 4556) is the band index (0/1/2), derived from the window Y offset. `source_row_offset = untile_band` — the resolve reads from the band's rows in the expanded surface.
- `resolve_sources` (line 4792) is the EDRAM surface split by tile owner. `source.surface` (line 4790) is the resolve source (the EDRAM surface, or an alternate owner's surface).
- The resolve executes per owner region: `native_resolve_->ResolveToMemory(...)` (immediate, line 5292) or `RecordResolveMemory` via `RecordResolveBatch` (tape, line 4097), then `SubmitFrameTape("resolve dispatch")`.
- The destination `memory_target` (lines 4721–4749) is selected in this order: probe UAV (`fh1_debug_resolve_small_uav`), mirror-window UAV, direct-mirror UAV (`fh1_debug_resolve_direct_mirror`), else the **staging buffer** (`EnsureResolveStaging`) with `use_staging_copy = true`. If staging fails, it falls back to the whole-mirror UAV.

### 1.3 The production path: staging buffer + copy into the mirror

The comment at lines 4725–4735 (in the source) is the key finding:

> "Production path: the destination stores go through the resolve staging buffer, not the mirror. Measured on the GTX 570 / driver 391.35: a store at offset 0 of the 512 MB mirror's raw UAV lands, a store at the front buffer's guest address (0x1BDB1000, ~470 MB) never does, and a raw UAV over a mirror window cannot even be created (E_INVALIDARG for any FirstElement != 0). So the dispatch writes the extent into a small staging buffer whose stores are known to land, and the extent is then copied into the mirror at its guest address with CopySubresourceRegion — a copy-engine operation, with no shader-address limit."

So there are **two places the content can be lost**:
1. **Dispatch → staging**: the compute shader fails to write the staging buffer (e.g. the SRV bind is silently dropped — see §1.4).
2. **Staging → mirror**: the `CopyResolveStagingToHost` / tape copy fails or lands at the wrong place.

### 1.4 Two known silent-drop defects, both already worked around in code

- **SRV/UAV conflict** (`fh1_native_resolve.cpp`, `ResolveToMemory`): D3D11 silently drops an SRV bind when the same resource is still bound as a render target / depth-stencil view in the output merger, and the compute shader then samples an unbound slot — zeros. The comment records this was the blanking defect: "srv0 read back NULL on the session's first resolve and on most later ones, while the same resolve measured content in the source (source_in_rect 1800) and nothing in the destination (0 of 942,080 words)." The fix (unbind the output merger with `OMSetRenderTargets` null RTVs before binding the SRV) is in the code.
- **Mirror SRV/UAV conflict** (`command_processor.cpp`, lines ~4780): the draw path leaves the shared-memory mirror bound as VS/PS t0; binding the mirror's UAV then makes the runtime drop it silently and every resolve store vanishes. Fixed by unbinding VS/PS t0 before the resolve.

Both are addressed in code. Whether the resolve is NOW working or still failing is what the probe measures.

### 1.5 Existing resolve diagnostics (already in the tree, now enabled)

- **"FH1 resolve staging"** (line ~5826, gated on `fh1_swap_fetch_trace`): reads the staging buffer after each resolve (`ReadbackResolveStaging`) and logs nonzero word counts + the first word — the direct measurement of "did the dispatch write the resolved pixels", independent of the copy into the mirror. Also logs `source_in_rect` (what the source surface held at dispatch time).
- **"FH1 resolve store probe"** (line ~6130, gated on `fh1_swap_fetch_trace` + extent ≥ 512 KB + swap ≥ `fh1_debug_resolve_store_probe_from`, budget 160): mirror before/after hash + nonzero for the destination extent. If the extent is untouched, it fingerprints the whole 512 MB mirror in 4 MB chunks to find where the content DID land ("FH1 resolve store sweep").
- **"FH1 resolve source probe"** (Q2, line ~6200): reads the actual MSAA source texture as the resolve sees it and compares its pixel profile with the mirror bytes.
- **`mirror_hash`** (line 5047): hashes a mirror range via `ReadbackFromHostBuffer` (with `WaitForGpu()` first — a Map only waits for its own copy, not the compute work queued before it).

These are hash/word-count measurements. The new Phase 2f probe adds **pixel values** (byte-lane means + sample words) for the EDRAM band-2 resolve — the same clarity as edram3d.

---

## 2. The resolve probe (new code)

### 2.1 Design

A gated, budgeted probe in `IssueCopy`, in the same pattern as the edram3d probe (before block reads the mirror, after block reads the mirror + staging and logs one line). It fires only on EDRAM band-2 resolves:

- **Gate**: `presented_output_frames_ >= fh1_debug_resolve_probe_from` (default 3000 — does NOT expire) AND `!plan.copying_depth` AND `plan.resolve_key` is the EDRAM family (`!is_depth && base_tiles == 0 && pitch_tiles == 32 && msaa == k4X`, the same family check as Phase 2e) AND `untile_band == 2` (the last band — the resolve after Band 2) AND the destination extent is valid (nonzero, within the 512 MB mirror). Budget: `fh1_debug_resolve_probe_draws` (default 12).
- **Before block** (after the store probe's `mirror_before_ok`, ~line 5085): `WaitForGpu()`, then `ReadbackFromHostBuffer` the destination extent into `resolve_readback_scratch_`. Computes the four byte-lane means (mean of byte 0/1/2/3 across the extent — channel-order-independent), the nonzero 32-bit word count, and the first word. Sets `resolve_probe_pending` + the probe index + the extent/dest keys.
- **After block** (after the store probe's after-log, ~line 6274): `WaitForGpu()`, then `ReadbackFromHostBuffer` the extent again (mirror after) + `ReadbackResolveStaging` (the dispatch's own output, independent of the copy into the mirror). Computes the same stats for both. Logs one line:
  `FH1 resolve probe: swap={} probe#{} call={} src={} band={} dest_base={:08X} dest_pitch={} extent={:08X}+{} bpp_log2={} staging_ok={} staging_words_nz={} staging_first={:08X} mirror_before_ok={} before_mean={:.1f},{:.1f},{:.1f},{:.1f} before_words_nz={} before_first={:08X} mirror_after_ok={} after_mean={:.1f},{:.1f},{:.1f},{:.1f} after_words_nz={} after_first={:08X}`
- **Interpretation**: the EDRAM content is sky-blue (R≈85, G≈124, B≈146). If the resolve works, the mirror-after byte-lane means show values like (85, 124, 146, 255) in some order (channel order depends on pack/endian/swap). If the mirror stayed grey, the means are ~(66,66,66,255). If zeros, ~0. The staging words_nz separates "the dispatch wrote nothing" (staging empty) from "the copy into the mirror failed" (staging full, mirror empty).

### 2.2 Files changed (diff `~/workspace/dx11-resolve-probe-diff-2026-11-17.txt`, 183 lines)

- `E:\PinyonShift\work\.local\rexglue\src\graphics\d3d11\command_processor.cpp` — before block (~line 5085) + after block (~line 6274). Backup: `command_processor.cpp.musebak-resolve-20261117`.
- `E:\PinyonShift\work\.local\rexglue\src\graphics\flags.cpp` — two new cvars (`fh1_debug_resolve_probe_from`, default 3000; `fh1_debug_resolve_probe_draws`, default 12). Backup: `flags.cpp.musebak-resolve-20261117`.
- `E:\PinyonShift\work\.local\rexglue\include\rex\graphics\flags.h` — two new `REXCVAR_DECLARE` lines (required: `REXCVAR_GET` expands to `FLAGS_<name>_storage_`, and the first build failed with "use of undeclared identifier" until these were added). Backup: `flags.h.musebak-resolve-20261117`.
- `E:\PinyonShift\work\.local\run_game.cmd` — added `--fh1_debug_resolve_probe_from=3000 --fh1_debug_resolve_probe_draws=12 --fh1_swap_fetch_trace=true --fh1_debug_resolve_store_probe=true --fh1_debug_resolve_store_probe_from=3000` (keeps the Phase 2e edram3d flags). Backup: `run_game.cmd.pre-resolve-20261117`.

The edram3d probe is untouched and still active (same run config). The WaitForGpu fix was not touched. No refactor.

### 2.3 Build

- First build **FAILED** (BUILD_EXIT=1): the two new cvars were defined in flags.cpp but not declared in flags.h, so `REXCVAR_GET` in command_processor.cpp hit "use of undeclared identifier 'FLAGS_fh1_debug_resolve_probe_from_storage_'". Fixed by adding the two `REXCVAR_DECLARE` lines to flags.h.
- Second build **PASS**: `BUILD_EXIT=0` (log `E:\PinyonShift\work\.local\muse_build_game.log`, `[55/59] Linking CXX executable pinyon_shift.exe`). Note: the build SSH session was interrupted by a runtime restart mid-build, but the build itself completed on the PC — verified after recovery.
- Binary verified: DLL `rexgpu-fh1.dll` 11,511,808 B @ 4:42:56 PM PC (was 11,500,032 B Phase 2e; +11,776 B), EXE 61,851,136 B @ 4:43:02 PM PC. String `FH1 resolve probe` present in the DLL. The new cvars (`fh1_debug_resolve_probe_from`, `fh1_debug_resolve_probe_draws`) are in the DLL.

---

## 3. Run state — stuck on the licence screen (same as Phase 2d/2e)

- The old Phase 2e game (PID 11284) was **already closed** by Seth before the relaunch (tasklist showed no pinyon_shift.exe; taskkill reported "process not found"). Nothing was lost — the Phase 2e probe data was already collected.
- New game launched via the scheduled task "PinyonShift Test Run": **PID 9712**, session 1, session `20261117T234635Z-p9712`.
- **The game never entered free roam.** Checks:
  - ~4 min after launch: memory 183,844 K, music.in_game 0, save.backup 0, 0 resolve probe lines, 0 edram3d probe lines.
  - ~10 min after launch: memory **107,012 K** (dropped, flat in the stuck range — vs 842 MB when Phase 2e entered), music.in_game 0, save.backup 0, step_gate lines present (sim held), **0 `FH1 resolve probe` lines, 0 `FH1 edram3d probe` lines**.
- This is the GTX 570 licence screen waiting for input — the third consecutive run stuck this way (Phase 2d, 2e, 2f). Remote SendKeys does not reach the game (it reads raw input). Seth is in bed / it is night.
- **Do NOT relaunch in a loop.** The game (PID 9712, Phase 2f build) is left running.

## 4. The probe gate does NOT expire

The gate is `presented_output_frames_ >= 3000`. Swap 3000 already passed during loading, but the probe fires on the first 12 EDRAM band-2 resolves whenever they happen. **If Seth presses Start and the game enters free roam, the resolve probe fires on this same running game (PID 9712, Phase 2f build) — no relaunch needed.** The edram3d probe is also still active on this run, so both probes will fire.

## 5. Verdict: NOT answered yet

- **Probe lines: 0.** No `FH1 resolve probe` line exists in any rotated log because the game never reached free roam.
- The flat-grey question's next step (does the coloured EDRAM content make it into the resolved texture?) is **still open**. This is a run-state failure, NOT a probe failure — the patch, build, and binary are verified.
- The existing resolve diagnostics (`fh1_swap_fetch_trace`, `fh1_debug_resolve_store_probe`) are also enabled on this run, so when the game enters, the staging log + store probe will corroborate the new probe.

---

## 6. Next

1. Seth presses Start on the PC (or asks Freebuff to get past the licence screen). The probe fires on the running game (PID 9712).
2. Collect the `FH1 resolve probe` lines from the rotated logs + the `FH1 resolve staging` / `FH1 resolve store probe` lines, and deliver the verdict: staging words_nz (did the dispatch write?) vs mirror-after means (did the copy land? coloured / grey / zero?).
3. If the mirror-after is coloured, the resolve works and the loss is in the composite (Draw #23's wipe). If the staging is full but the mirror is grey/zero, the staging→mirror copy is the loss. If the staging is empty, the dispatch itself is the loss.
