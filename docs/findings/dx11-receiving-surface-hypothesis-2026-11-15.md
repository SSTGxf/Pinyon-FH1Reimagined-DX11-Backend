# Receiving-surface hypothesis verification — Pinyon FH1 D3D11

Date: 2026-11-15. Read-only verification; no code, config, or rendering behavior was modified. No game launch, no rebuild.

## Symptom (grounded)

The defect is the **black bottom-half of the composed/final frame**: the bottom half of the final image is black while the top half renders, and the guest data is bright. Primary reference frame **2911**; frame **2851** is the earlier example of the same composed-vs-guest discrepancy. This is the observed symptom from Seth's screenshots/video — not just ChatGPT's framing.

The "receiving surface" is the texture that receives the resolved pixels and that the compositor/presenter samples for the final frame. In this backend the final color resolve writes to guest memory (front-buffer base `0x1CE2D000`, key `720/16/1x/c0`), and the presenter samples the guest front buffer via a mailbox texture (`guest_output_images_`, `ConsumeGuestOutput` → `CopyGuestOutputToPresent` → paint).

## Sources used

- **Canonical PC source** (`E:\PinyonShift\work\.local\rexglue\`, freshest): `src/graphics/d3d11/fh1_native_surfaces.{h,cpp}`, `fh1_native_resolve.cpp`, `command_processor.cpp`, `src/ui/d3d11/d3d11_presenter.cpp`, `include/rex/graphics/fh1_edram_surfaces.h`, `include/rex/graphics/render_forensics.h`. Pulled to `/tmp/forensics-fresh/`.
- **Forensics capture JSONL** (`E:\PinyonShift\work\.local\audit\d3d11-forensics-1794806523.jsonl`, capture window frames 2600–3000, 1,824,289 lines, 331 MB). Frames 2851 (5,793 lines) and 2911 (5,923 lines) extracted and aggregated on the PC. Op codes from `render_forensics.h`: 0 = resource create, 1 = resolve entry, 2 = plan fail, 4 = skip.
- **Runtime log** `~/workspace/runtime-20261115-2138.log` (5,447 lines, swaps 3429–3511, ~20:45:46–20:46:06 PM PC 2026-11-15). **Only the last ~20 s** (runtime.log rotates at ~5 MB); it does NOT cover frames 2851/2911. Used for texture dimensions, the resolve-source band probe, swap `fb_ptr` alternation, and copy-down lines.
- GitHub repo was not needed (PC source was available and fresher).

### Caveats on the evidence

- **Forensics frame attribution has a known off-by-one** (Freebuff: the frame clock and the window check disagree by one swap). Frame 2851's summary says 4,763 events but the parsed records for that frame number total 4,669; frame 2911's summary says 4,729 but the parsed records total 4,799. Per-frame counts are therefore approximate. However, frames 2851 and 2911 have **identical resolve structure** (same 42 resolve entries, same destination bases and rects, same 6 RF004 skips), so the pattern is robust across the attribution error.
- **`ConsumeGuestOutput`'s definition was not located** in the files available (it is called in `d3d11_presenter.cpp:896,1214` but defined in a base class/header not pulled). The exact guest base the presenter/mailbox samples for frame 2911 could therefore **not** be confirmed from source.
- **Frame 2911's swap `fb_ptr` is not determined.** The forensics capture has no present/swap hook (H-PRES is a later stage), and the runtime log covers a different window (swaps 3429–3511). Do not assert which front-buffer pointer frame 2911 used.

---

## H1 — Allocation defect: the actual texture is half-height

**Verdict: REFUTED.**

A half-height receiving texture would produce a black bottom-half if the compositor sampled a 720-row region from a 360/512-row texture. The receiving surface is not half-height.

### Evidence

**Code — the front-buffer surface is allocated at the full EDRAM period (2048 rows), not 720 and not half-height.**

- `Fh1D3D11SurfaceImageHeight` (`include/rex/graphics/d3d11/fh1_native_surfaces.h:89-95`) returns `min(max(period_height, untile_height), max_height)`.
- For the front-buffer surface (key `720/16/1x/c0`, `pitch_tiles=16`, MSAA 1x), the period is `Fh1SurfaceHeight` (`include/rex/graphics/fh1_edram_surfaces.h:76-84`): `tile_rows = (2048 + 16 - 1) / 16 = 128`, `128 × 16 = 2048`. So `period_height = 2048`.
- `untile_height` is 720 (`kFh1D3D11UntileHeight`, `fh1_native_surfaces.h:61`), set in `command_processor.cpp:680-689` when the native executor is on and the draw scale is 1x1. `image_height = max(2048, 720) = 2048`.
- The texture is created with `desc.Height = surface->image_height * draw_resolution_scale_y` (`fh1_native_surfaces.cpp:141,147`), i.e. **2048 rows at 1x**.
- The untile expansion is active in the actual run (untile copy-down lines are present in the log and the band fix works), so the executor-on + 1x-scale condition holds.

**Runtime log — the texture is 1280×2048:**

> `FH1 resolve source surface: 1280x2048 texture=0x20d3d0e05a0 nonzero[texels sampled]=8221 first=00FFFF00 in_rect=8221 below_rect=0 rect_bottom=720` (`runtime-20261115-2138.log:37`)

A 2048-row texture for a 720-row frame is the full EDRAM period. It is not 360 or 512 rows. The resolve reads rows 0–720 of it (`rect_bottom=720`) with zero nonzero texels below the rect (`below_rect=0`), which is correct for a 720-row frame.

**Mapping to A–E:** A (incorrect surface allocation) is **refuted** for the front-buffer/receiving surface.

---

## H2 — Resource identity defect: the resolve writes to a different surface than the compositor samples

**Verdict: INCONCLUSIVE** (simple whole-frame mismatch refuted; a real identity anomaly remains open).

A clean bottom-half-only blackout points more at H1 or H3 than H2, unless the identity mismatch is band-specific. The evidence bears that out: the resolve destination and the swap's resolved base match, but there is a genuine three-destination split and one unwritten swap buffer.

### Evidence against a simple identity mismatch

- The final color resolve (key `720`, rect `(0,0,1280,720)`) writes to `dest_base=0x1CE2D000`, **three times per frame** in both frame 2851 and frame 2911 (forensics JSONL, op 1). The command-processor comment identifies this as the front buffer: "1CE2D000 is the base_tiles != 0 family (720/16/1x/c0 in free roam)" and "the front buffer the swap resolves" (`command_processor.cpp:2679`).
- The swap trace shows the last resolve call's range **matches** the swap fetch base, e.g. swap 3429: `fb_ptr=1BDB1000 ... last resolve call=97579 range=1BDB1000+3768320`; swap 3430: `fb_ptr=1C149000 ... last resolve call=97619 range=1C149000+3768320` (`runtime-20261115-2138.log:2,66`). A resolve does write to the fetched base.

### The anomaly that keeps H2 open

There are **three different 1x full-frame (1280×720) resolve destinations per frame** (identical in frames 2851 and 2911, forensics JSONL op 1, `dest_pitch=0x500`):

| key | rect | dest_base | count/frame |
|-----|------|-----------|-------------|
| 720 | (0,0,1280,720) | `0x1CE2D000` | x3 |
| 0 | (0,0,1280,720) | `0x1C149000` | x1 |
| 0 | (0,0,1280,720) | `0x1DAC5000` | x1 |

The swap fetch alternates its front-buffer pointer between `0x1BDB1000` and `0x1C149000` (runtime log swaps 3429–3431). In frames 2851 and 2911 a resolve writes to `0x1C149000` (x1) but **no resolve writes to `0x1BDB1000`**. So one of the two swap buffers receives no resolve in these frames.

But this predicts **alternating whole-frame staleness**, not a bottom-half-only blackout — and frame 2911's actual `fb_ptr` is not established (see caveats). So the unwritten-buffer anomaly does not by itself explain the symptom, and the resolve→front-buffer identity (`1CE2D000`) is confirmed matching.

### What would settle it

Determine the exact guest base the presenter/mailbox samples for frame 2911 and compare it to the resolve `dest_base` values (`1CE2D000`, `1C149000`, `1DAC5000`). Concretely: run with `fh1_swap_fetch_trace` plus the frontbuffer readback covering frame 2911 and read the `fb_ptr` and the mailbox source base at that swap. If the sampled base is `0x1BDB1000` (no resolve in frame 2911), that confirms an identity defect. If it is `0x1CE2D000` or `0x1C149000` (full resolves present), the defect is downstream of the resolve.

**Mapping to A–E:** H2 maps to C (incorrect resolve destination selection) and E (incorrect composition resource selection). Both are **inconclusive / open** — C is partly refuted (the c0 resolve dest matches the front-buffer base) but the three-destination split is unexplained; E is untested because the presenter's sampled base for frame 2911 was not determined.

---

## H3 — Resolve coverage defect: the receiving texture is full-height, but only one half receives valid resolved pixels

**Verdict: REFUTED** (as a rect-coverage and source-content defect).

### Evidence

**Rect coverage is full, not half.** In both frame 2851 and frame 2911 the forensics capture records (op 1):

- 5 full-frame 1280×720 resolves: key 0 → `0x1DAC5000` (x1) and `0x1C149000` (x1); key 720 → `0x1CE2D000` (x3).
- 3 four-times-MSAA band resolves (key 0, samples 2, `sample_select=0x00000006`): rect `(0,0,1280,256)` → `0x1C4E1000`, rect `(0,0,1280,256)` → `0x1C621000`, rect `(0,0,1280,208)` → `0x1C761000`. The bands cover 256 + 256 + 208 = 720 rows.
- Overall color-rect y span is 0..720. The resolve is asked to cover the full frame.

**Source-pixel coverage is full in the bottom half at the resolve instant.** The per-60-row band probe (`command_processor.cpp:3955-3996`) copies the resolve source texture to staging, reads it back, and counts RGB-nonzero texels per 60-row band (band 0 = rows 0–59, …, band 11 = rows 660–719). It deliberately counts colour (RGB ≠ 0), not whole words, because an 8_8_8_8 composite's unused half is `0xC0000000` (RGB 0, alpha 192) and whole-word counting would report a black half as full.

For the front-buffer resolve (key `720/16/1x/c0`):

> `FH1 resolve source bands: swap=3430 key=720/16/1x/c0 rect_bottom=720 nz=[1.000 0.999 1.000 0.999 1.000 0.999 0.998 0.996 0.985 1.000 1.000 0.998]` (`runtime-20261115-2138.log:38`)

Bands 6–11 (rows 360–719, the bottom half) are 0.985–1.000 — essentially full. The companion resolve (key `0/16/1x/c2`) shows bottom-half bands 1.000, 1.000, 1.000, 1.000, 0.996, 0.998. `in_rect=8221`, `below_rect=0`, `rect_bottom=720`. So the source surface holds valid colour pixels in the bottom half at the resolve instant, and the resolve rect covers them.

Therefore the resolve covers the full rect **and** its source has bottom-half content. H3 as a coverage defect is refuted.

**Caveat:** this is the resolve-instant probe. The code comment notes the presented-pair dump re-reads the same surface at the swap, and halves drawn after their own resolve would look empty here but full there. Here the bottom half is **full** at the resolve instant, so that timing theory does not explain a black bottom half for these resolves. The black bottom half must arise downstream of the resolve, or in a different resolve than the ones probed.

**Mapping to A–E:** H3 maps to D (incorrect resolve coverage). D is **refuted** at the rect and source-content level. B (incorrect resolve source selection) is also not supported — the probed sources contain the bottom-half content.

---

## Overall conclusion

| Hypothesis | Verdict | Strongest evidence |
|---|---|---|
| H1 — half-height allocation (A) | **REFUTED** | Front-buffer texture is 1280×2048 (full EDRAM period; `image_height = max(2048, 720) = 2048`), runtime log `runtime-20261115-2138.log:37`. |
| H2 — wrong surface identity (C / E) | **INCONCLUSIVE** | c0 resolve dest `0x1CE2D000` matches the swap's resolved base (refutes simple mismatch), but three full-frame destinations exist and `0x1BDB1000` receives no resolve in frames 2851/2911. |
| H3 — half coverage (D / B) | **REFUTED** | Full-frame 1280×720 rects (x5/frame) + 4x bands covering 720 rows; bottom-half source nz 0.985–1.000 at the resolve instant. |

**None of the three hypotheses as literally stated explains a clean black bottom-half.** Allocation (H1/A) and rect/source coverage (H3/D/B) are ruled out by direct measurements. The remaining open area is the identity/composition path (H2 → C/E), but the simple form ("resolve writes to a different surface") is refuted because the c0 resolve does write to the front-buffer base `0x1CE2D000`. The most likely remaining explanations are: (a) the presenter/mailbox samples a different guest base than the one the complete resolve wrote to; (b) the bottom-half pixels are lost in the guest-memory → mailbox transfer (presenter blit/readback); or (c) a different resolve than the probed ones feeds the bottom half and it is the one that is wrong.

**Next single check:** determine the exact guest base the presenter/mailbox samples for frame 2911 and compare it to the resolve `dest_base` values (`0x1CE2D000`, `0x1C149000`, `0x1DAC5000`). Run with `fh1_swap_fetch_trace` plus the frontbuffer readback covering frame 2911 and read the `fb_ptr` and the mailbox source base at that swap. If the sampled base is `0x1BDB1000` (no resolve in frame 2911), that confirms an identity defect (E/C). If it is `0x1CE2D000` or `0x1C149000` (full resolves present), the defect is downstream in the mailbox blit/readback, and the next check is the presenter's staging-texture dimensions and blit rect.
