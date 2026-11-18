/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/ui/renderdoc_api.h>

REXCVAR_DEFINE_BOOL(gpu_allow_invalid_fetch_constants, true, "GPU",
                    "Allow invalid fetch constants");
REXCVAR_DEFINE_BOOL(native_2x_msaa, true, "GPU", "Enable native 2x MSAA");
REXCVAR_DEFINE_BOOL(fh1_debug_disable_msaa, false, "GPU",
                    "Diagnostic D3D11 override: force RB_SURFACE_INFO MSAA to 1x for FH1, to "
                    "test whether multisampled render targets cause a rendering defect")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_tile_trace_swap, 338, "GPU",
                     "Log D3D11 FH1 draw tile window/viewport/scissor state and 4x source row "
                     "hashes before resolves on this presented frame; 0 disables");
REXCVAR_DEFINE_INT32(fh1_debug_callog, 0, "GPU",
                     "Log FH1 guest-call / D3D11-call comparison lines (FH1 callog) for this "
                     "presented frame; 0 disables");
REXCVAR_DEFINE_INT32(fh1_debug_callog_budget, 8192, "GPU",
                     "Maximum FH1 callog lines per process; 0 disables the budget cap");
REXCVAR_DEFINE_INT32(fh1_debug_trace_span, 1, "GPU",
                     "Number of consecutive presented frames to log FH1 callog / "
                     "routing lines for, starting at fh1_debug_callog / "
                     "fh1_debug_tile_trace_swap; 1 = single swap (unchanged behavior)");
REXCVAR_DEFINE_INT32(fh1_debug_band2_trace_swap, 0, "GPU",
                     "Trace FH1 Band 2 packets and draw processing for this PM4 XE_SWAP frame; "
                     "0 disables");
REXCVAR_DEFINE_INT32(fh1_debug_band_suppress, 0, "GPU",
                     "Diagnostic draw suppression mask: bit 0 suppresses pass 0 (bin select 3), "
                     "bit 1 pass 1 (0xC), bit 2 pass 2 (0x30); copy/resolve/clear packets are "
                     "not suppressed; 0 disables");
REXCVAR_DEFINE_BOOL(fh1_debug_hide_hud, false, "GPU",
                    "Diagnostic: skip HUD/2D overlay draws (small-viewport color draws) "
                    "while keeping 3D world draws; false disables")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(fh1_debug_minimap_capture_swap, 338, "GPU",
                     "Capture one 320x192 FH1 minimap candidate at source, resolved guest bytes, "
                     "and sampled texture stages on this presented frame; 0 disables");
REXCVAR_DEFINE_BOOL(depth_float24_round, false, "GPU", "Round float24 depth values");
REXCVAR_DEFINE_BOOL(depth_float24_convert_in_pixel_shader, false, "GPU",
                    "Convert float24 depth in pixel shader");
REXCVAR_DEFINE_BOOL(snorm16_render_target_full_range, true, "GPU",
                    "Use full range for SNORM16 render targets");
REXCVAR_DEFINE_BOOL(gamma_render_target_as_unorm16, true, "GPU",
                    "Use R16G16B16A16_UNORM for gamma render targets (more accurate than sRGB)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "Path to dump shaders to");
REXCVAR_DEFINE_BOOL(fh1_d3d11_shader_pack, false, "GPU",
                    "Use the FH1 precompiled Direct3D 11 shader pack in the shareable shader "
                    "storage instead of translating guest shaders at runtime. Enable only with a "
                    "pack produced by the current translator: a pack from an older translator "
                    "carries bytecode this device may refuse or remove the device on")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_swap_fetch_trace, false, "GPU",
                    "Per-swap trace of the swap fetch descriptor and the last native resolve "
                    "destination (guest addresses, pitch, format) for diagnosing a presented "
                    "frame that does not follow the guest's front buffer")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_d3d11_resolve_readback, true, "GPU",
                    "Direct3D 11: at each swap, pull the front buffer's extent - the guest "
                    "range texture fetch constant 0 describes - from the GPU mirror back into "
                    "guest memory, so guest memory holds the frame being presented and the "
                    "swap's untiling upload shows it instead of the last frame the CPU wrote "
                    "there. Off by default: it costs a GPU-mirror readback per swap, and the "
                    "pipeline it reveals still has open items (the C2 fill cost, scaled "
                    "targets). See DX11_FINISH_NOTES.md, PHASE 4 - the frontbuffer readback")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_d3d11_command_lists, false, "GPU",
                    "Direct3D 11: record the frame and swap passes into deferred command lists "
                    "and replay them on the immediate context (Phase 3 two-list recorder). Off by "
                    "default: the recording path is still being wired, and the immediate path is "
                    "the reference")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_d3d11_command_list_validate, false, "GPU",
                    "Diagnostic: with fh1_d3d11_command_lists, validate every recorded resource "
                    "transition and packet binding and log the first mismatch. Has no effect while "
                    "the command lists are off")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_d3d11_command_list_ab, false, "GPU",
                    "Diagnostic: with fh1_d3d11_command_lists, run every recorded resolve a second "
                    "time through the immediate path on the same source and compare the two "
                    "dispatches' staging output bit-for-bit, then land each path's extent in the "
                    "GPU mirror and compare the two destination fingerprints. Doubles the "
                    "resolve work; for the Phase 3 M2 equivalence measurement only")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pinyon_draw_trace, false, "GPU",
                     "Log the world draws' viewport and scissor as they change");
REXCVAR_DEFINE_INT32(pinyon_audit_passes_from, 0, "GPU",
                    "First frame recorded in the per-pass trace (0 with to=0 records nothing)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(pinyon_audit_passes_to, 0, "GPU",
                    "Last frame recorded in the per-pass trace")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pinyon_audit_passes, false, "GPU",
                     "Write a per-pass trace: each EDRAM tile claim with its owner before/after, "
                     "and each resolve with its rectangle, source occupancy and destination write "
                     "coverage per 16-row band")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_forensics_capture, false, "GPU",
                    "Record a bounded forensics event stream for the frame window below")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(fh1_forensics_from, 0, "GPU",
                     "First presented frame recorded by the forensics capture")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_forensics_to, 0, "GPU",
                     "Last presented frame recorded (0 with from=0 captures nothing)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_forensics_event_mask, -1, "GPU",
                     "Forensics event bitmask (-1 records every enabled kind)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_forensics_ring_events, 65536, "GPU",
                     "Bounded forensics ring size; overflow is counted and reported")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_forensics_include_binds, false, "GPU",
                    "Include texture and constant-buffer bindings in the stream")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_forensics_include_uploads, false, "GPU",
                    "Include guest-to-host uploads in the stream")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pinyon_msaa_probe, false, "GPU",
                     "Record MSAA source provenance for the 4x resolves of the captured "
                     "frame: format support bits, the three-resource readback chain and its validity")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pinyon_transition_log, false, "GPU",
                     "Log one line per presented frame: its id and how many draws used the "
                     "8192-wide world viewport, to show whether the world pass disappears")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pinyon_audit, false, "GPU",
                     "Write one JSONL render-audit record per 300 frames: bound RT sample counts, resolve sample counts, draw tally and error tally")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pinyon_resolve_trace, false, "GPU",
                     "Capture the next three large D3D11 FH1 resolves: owner rectangles, source "
                     "row hashes, resolve completion and the guest-memory mirror");
REXCVAR_DEFINE_BOOL(fh1_resolve_swap_rb, false, "GPU",
                     "Resolve colour surfaces with red and blue exchanged: the guest's swap bit (dest_info 6) inverted for 3D content only, never for presentation");
REXCVAR_DEFINE_INT32(fh1_debug_resolve_force_pack, -1, "GPU",
                    "Diagnostic: rewrite the native resolve's destination pack (0-7) before it is "
                    "sent to the shader, to test whether a destination format class stores at all. "
                    "-1 leaves the guest's value alone")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_resolve_store_probe, false, "GPU",
                    "Diagnostic: run the probe build of the native resolve shader, which stores "
                    "0xDEADBEEF at offset 0 of its bound UAV before touching guest data. With the "
                    "small-UAV diagnostic it separates \"UAV writes never land\" from \"the resolve "
                    "writes the wrong thing\"")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_resolve_store_probe_from, 0, "GPU",
                     "Diagnostic: with fh1_swap_fetch_trace, start the per-resolve mirror "
                     "before/after hash probe at this presented-frame index instead of at the "
                     "session's first resolves (the front buffer's own copy is call ~12,000, "
                     "far past them), up to 160 probes - the record that says whether a resolve's "
                     "bytes reached the mirror the texture upload reads")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_debug_resolve_alias_base, "", "GPU",
                    "Diagnostic: a guest base (hex, e.g. 1CA95000) that a 640x360 8_8_8_8 "
                    "texture fetch samples. While set, a resolve whose destination starts "
                    "at the base but does not reproduce that fetch's own writer (pack 0, "
                    "640x360, pitch 640) is skipped and logged, so the base has one writer "
                    "per swap - the separation check for a pool address the guest reuses "
                    "for a depth copy and a 320x192 colour target (the mips allocated inside "
                    "the extent are left alone). Measured on the GTX 570: enabling it stalls "
                    "the title before free roam, because the pre-world frames need the c3 "
                    "copy - a dissection probe, not a fix. Empty = off")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_resolve_alias_bytes, 921600, "GPU",
                     "Diagnostic: the protected length for fh1_debug_resolve_alias_base "
                     "(default 921,600 = 640x360 x 4 bytes)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_resolve_alias_mode, 0, "GPU",
                     "Diagnostic: which base writers fh1_debug_resolve_alias_base guards "
                     "- 0 every non-texture writer, 1 the depth writer only (pack 4), 2 the "
                     "colour writers only. The split showed the c3 class is needed by the "
                     "pre-world frames and the depth class is not this texture's content, so "
                     "it is a measurement, not a fix")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_resolve_alias_from_swap, 0, "GPU",
                     "Diagnostic: start fh1_debug_resolve_alias_base's guard at this "
                     "presented-frame index instead of the first resolve (0 = no gate). "
                     "Lets the title boot on the unguarded path, then reduces a live world "
                     "swap to its 640x360 8_8_8_8 writer - the one-writer census of PHASE 13")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_frontbuffer_draws, 0, "GPU",
                     "Diagnostic: log up to this many draws that bind the front-buffer family "
                     "(base_tiles != 0, pitch_tiles 16, 1x) with key, viewport, scissor, primitive, "
                     "vertex counts and the textures they sample - the pass that writes the "
                     "surface the swap resolves at 1CE2D000 (the composite log only names the "
                     "base_tiles==0 c2 family)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_frontbuffer_draws_from, 0, "GPU",
                     "Diagnostic: start fh1_debug_frontbuffer_draws at this presented-frame index "
                     "instead of at the session's first draws, so the budget covers the world "
                     "frame rather than the menus")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_resolve_owner_census, 0, "GPU",
                     "Diagnostic: for up to this many front-buffer resolves (base_tiles != 0, "
                     "pitch_tiles 16, 1x, colour), log the EDRAM tile owners under the copy's "
                     "rectangle - unowned/mixed counts and the owner histogram - beside the "
                     "by-owner result, which is what decides whether the source fallback reads "
                     "another surface or the copy's own")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_resolve_small_uav, false, "GPU",
                    "Diagnostic: bind the resolve's destination to a dedicated 4 MB raw UAV instead "
                    "of the 512 MB shared-memory mirror, to test the mirror view's size against the "
                    "driver")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_resolve_force_endian, -1, "GPU",
                    "Diagnostic: rewrite the resolve destination's endianness bits (3:5) before "
                    "the dispatch, to isolate which endianness classes land on this driver. "
                    "-1 keeps the guest's value")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_resolve_cb_capture, false, "GPU",
                    "Diagnostic: snapshot the resolve constant buffer as the GPU saw it after the "
                    "first two resolve dispatches, and log it beside the CPU-packed values")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_resolve_cb_update, false, "GPU",
                    "Diagnostic: upload the resolve constants with UpdateSubresource on a DEFAULT "
                    "buffer instead of Map/WRITE_DISCARD")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_transfer_off, false, "GPU",
                    "Diagnostic: the FH1 native executor still claims EDRAM tiles and counts "
                    "what it would transfer, but performs no transfer, to bisect an ownership "
                    "defect from a transfer defect")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_transfer_unbatched, false, "GPU",
                    "Diagnostic: the FH1 native executor transfers one rectangle per dispatch "
                    "instead of one dispatch per destination/source pair (the C3 behaviour), to "
                    "measure the batching: the rectangles, the words moved and the stencil reset "
                    "are the same, only the number of compute dispatches and transfer calls "
                    "differs")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_transfer_direct, false, "GPU",
                    "Diagnostic: depth-destination transfers read the previous owner's surface "
                    "in the depth and stencil passes themselves, with no words buffer and no "
                    "compute dispatch, instead of the batched EDRAM words pass. Measured no "
                    "faster than the words pass one rectangle per transfer (the per-transfer "
                    "overhead, not the dispatch, dominates and the source read then repeats per "
                    "pass), so it stays a bisect")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_resolve_bind_check, false, "GPU",
                    "Diagnostic: after a resolve's binds and before its dispatch, read the bound "
                    "shader resources, UAV and constant buffer back from the context and log "
                    "whether each request survived (Direct3D 11 drops a bind silently when the "
                    "view conflicts with another bound view of the same resource)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(fh1_debug_surface_dump, "", "GPU",
                    "Diagnostic: at the swap named by fh1_debug_surface_dump_swap, write every "
                    "native EDRAM surface the executor holds into this directory as a PPM plus a "
                    "stats line (size, samples, nonzero share, distinct colours, mean), and the "
                    "EDRAM tile ownership histogram, once - which surface holds the frame, by key")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_surface_dump_swap, 300, "GPU",
                     "Diagnostic: the presented-frame index at which fh1_debug_surface_dump "
                     "fires (the free-roam world starts around frame 500 on the executor path)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_debug_dump_guest, "", "GPU",
                    "Diagnostic: with fh1_debug_surface_dump, also write these guest ranges "
                    "(comma-separated addr+length in hex, e.g. 1C4E1000+3768320) from guest memory "
                    "and from the GPU mirror as .bin plus a linear 32-bit PPM, with word statistics "
                    "- what a resolved texture actually holds where the guest will sample it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_composite_draws, 0, "GPU",
                     "Diagnostic: from this presented-frame index on, log every draw that writes the "
                     "1280-wide 1x surface family the front buffer is composed into (viewport, "
                     "scissor, and each texture it samples with its guest base, size and format), "
                     "up to 400 lines - what the composite reads while it builds the frame")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_debug_composite_dump, "", "GPU",
                    "Diagnostic: with fh1_debug_composite_draws, write the front-buffer-family render "
                    "target as a PPM before each of the first 12 draws that write it at the traced "
                    "swap - which pass builds which part of the presented image")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_vertex_dump, 0, "GPU",
                     "Diagnostic: for this many traced draws that sample a frame-sized texture,"
                     " log each vertex the shader actually fetches - the fetch constant's base plus"
                     " the draw's index times the binding stride, plus the attribute offset - with"
                     " the positions (the attribute fetched into r0) mapped through the draw's"
                     " viewport, in pixels: the real corner of every composite pass")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_debug_texture_dump, "", "GPU",
                      "Diagnostic: with fh1_debug_composite_draws, name an existing directory;"
                      " at each world rectangle draw the largest frame-sized texture it samples"                      "is read back from the host texture the cache holds (its SRV format) and"
                      " written there as a PPM - the scene as the composite sees it, before any"
                      " draw composes it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_debug_guest_probe, "", "GPU",
                      "Diagnostic: name an existing directory; at each world-half composite draw"
                      " (a 1280x360 rectangle that samples the scene target) write the guest bytes"
                      " of every sampled texture 320x192 or larger, from guest memory and from the"
                      " GPU mirror, with the fetch's base, size, format, tiled and endian bits -"
                      " what the texture cache untiles, before it decodes it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_debug_presented_pair, "", "GPU",
                    "Diagnostic: once, at fh1_debug_presented_pair_swap, write two PPMs from "
                    "one call at that swap - the composite the presenting resolve sampled "
                    "and the untiled swap texture the presenter paints - each with a per-60-"
                    "row nonzero/mean band profile, so the two images are provably the same "
                    "moment (the same swap counter on both lines)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_presented_pair_swap, 0, "GPU",
                     "Diagnostic: the presented-frame index at which "
                     "fh1_debug_presented_pair fires (0 = the first swap after the flag is "
                     "set); the free-roam world starts around swap 300 on the executor path")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_presented_pair_count, 1, "GPU",
                     "Diagnostic: how many consecutive swaps fh1_debug_presented_pair dumps "
                     "(default 1); more than one is what tells a stable frame from an "
                     "alternating one")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_resolve_trace, 0, "GPU",
                     "Diagnostic: from this presented-frame index on, log every resolve's"
                     " destination (surface key, rectangle, destination base, pitch and info word,"
                     " the memory extent it writes, and the source surface), up to 4000 lines -"
                     " the frame's own producer record for a guest range a texture is sampled from")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_debug_c2_probe, "", "GPU",
                      "Diagnostic: name an existing directory; for each of the first "
                      "fh1_debug_c2_probe_draws draws that write a frame-sized (1280+ wide) "
                      "surface at or after fh1_debug_c2_probe_from, read that surface twice "
                      "before the draw (a readback stability control that must report 0), then "
                      "once after it, and log the pixels the draw itself changed - the share, "
                      "the bounding box, and the change per 64-pixel column and 60-pixel row - "
                      "next to the draw's viewport, scissor and surface descriptor. The first "
                      "two probed draws are also written as before/control/after PPMs")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_c2_probe_from, 0, "GPU",
                     "Diagnostic: the presented-frame index at which fh1_debug_c2_probe starts "
                     "probing draws (0 = from the first frame)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_c2_probe_draws, 24, "GPU",
                     "Diagnostic: how many draws fh1_debug_c2_probe reads back (two reads "
                     "before, one after), each of them a full surface readback")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_debug_edram3d_probe, "", "GPU",
                      "Diagnostic: name an existing directory; for each of the first "
                      "fh1_debug_edram3d_probe_draws draws that write the EDRAM colour "
                      "surface (0/32/4x/c3) at or after fh1_debug_edram3d_probe_from, read "
                      "that surface twice before the draw (a readback stability control "
                      "that must report 0), then once after it, and log the pixels the draw "
                      "itself changed - the share, the bounding box, and the change per "
                      "64-pixel column and 60-pixel row - next to the draw's viewport, "
                      "scissor and surface descriptor. The first three probed draws are "
                      "also written as before/control/after PPMs")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_edram3d_probe_from, 1500, "GPU",
                     "Diagnostic: the presented-frame index at which fh1_debug_edram3d_probe "
                     "starts probing draws (0 = from the first frame)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_edram3d_probe_draws, 3, "GPU",
                     "Diagnostic: how many draws fh1_debug_edram3d_probe reads back (two reads "
                     "before, one after), each of them a full surface readback")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_resolve_probe_from, 3000, "GPU",
                     "Diagnostic: the presented-frame index at which fh1_debug_resolve_probe "
                     "starts probing EDRAM band-2 resolves (0 = from the first frame)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_resolve_probe_draws, 12, "GPU",
                     "Diagnostic: how many EDRAM band-2 resolves fh1_debug_resolve_probe reads "
                     "back (mirror before/after + staging), each a full extent readback")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_srv_bind_all_from, 0, "GPU",
                     "Diagnostic: at this presented-frame index, log the bind result of "
                     "every texture fetch (any size) in GetActiveTextureBindfulSRV, "
                     "deduplicated per distinct fetch/binding outcome, up to 400 lines - "
                     "the 3D draws' sub-frame fetches, which the frame-sized srv-bind log "
                     "never sees (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_resolve_source_probe_from, 0, "GPU",
                     "Diagnostic: at this presented-frame index, for each resolve whose "
                     "destination extent starts at the FH1 scene target (1C4E1000), read "
                     "the MSAA resolve-source texture as the resolve sees it "
                     "(ResolveSubresource to 1x + staging readback) and compare its pixel "
                     "profile with the mirror bytes the resolve just wrote for the same "
                     "rect, up to 8 probes (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_post_clip, 0, "GPU",
                     "Diagnostic: with fh1_debug_vertex_dump, log the post pass's own vertices "
                     "(up to 48 for its triangle list) with the pixel position each maps to "
                     "through the draw's ndc transform, and their extent - the geometry behind "
                     "the region the pass actually rasterizes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_clear_draw, 0, "GPU",
                     "Diagnostic: with fh1_debug_composite_dumps, clear the front-buffer target to "
                     "magenta before every traced draw from this one on, so each dump shows the "
                     "footprint of the single draw before it (0 = off; a probe only, the frame is "
                     "destroyed)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_composite_world_only, false, "GPU",
                    "Diagnostic: with fh1_debug_composite_draws, log and dump only the draws that "
                    "sample a frame-sized (640x360 or larger) texture - the 3D scene's own draws "
                    "and the post passes after them, instead of the hundreds of fills that precede "
                    "them in the same frame")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_debug_composite_dump_world, "", "GPU",
                    "Diagnostic: with fh1_debug_composite_draws, name a directory; the first time a "
                    "draw into the front-buffer surface samples a texture sized 640x360 or larger "
                    "(the 3D scene), the target is written before each of the next 12 draws that "
                    "write it - which pass paints which band of the presented frame, starting at "
                    "the world draw instead of at the frame's first draw")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_resolve_guest_refresh, true, "GPU",
                    "EXPERIMENTAL, off by default: before a texture upload reads a guest range the "
                    "resolve path wrote into the GPU mirror of shared memory, land that range in "
                    "guest memory (the D3D11 texture cache untiles from guest memory, so without "
                    "this every texture the guest resolved and then sampled - the 3D scene first - "
                    "fails its load and samples nothing). Measured on the GTX 570: correct data is "
                    "landed (920/920 GPU-written pages become CPU-authoritative), but the per-load "
                    "GPU wait and readback cost too much for the free-roam gate (4.75 Hz distinct "
                    "presentation vs the 5 Hz floor, median 181 ms vs 150 ms) - it needs the flush "
                    "batched or the upload moved off the CPU before it can default on")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_native_executor, 1, "GPU",
                     "Direct3D 11: run the FH1 native executor for draw targets (0 = off, 1 = "
                     "on, 2 reserved). On, each draw derives its EDRAM surfaces from the guest "
                     "registers, claims their tiles in the shared Fh1EdramTiles and binds them "
                     "through the executor - the same surface registry the native resolve "
                     "reads. Off keeps the previous render target path exactly")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_debug_resolve_direct_mirror, false, "GPU",
                    "Diagnostic: dispatch resolves straight into the 512 MB mirror UAV (the pre-fix "
                    "behaviour) instead of the staging buffer, to measure the staging path's cost")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_debug_resolve_window_mb, 0, "GPU",
                    "Diagnostic: bind the resolve's destination as a raw UAV window of this many "
                    "MEBIBYTES at a fixed mirror offset (0x08000000) instead of the whole 512 MB "
                    "view, to bisect the largest view the driver honours. 0 = window over the "
                    "resolve's own range (the production path)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_native_gpu_profile, false, "GPU",
                    "Measure the FH1 native executor's GPU time per phase (transfers, resolves, "
                    "clears, and on Vulkan the whole frame) with timestamp queries, and its CPU "
                    "time per phase (target preparation and binding, transfers, resolves), and "
                    "report them with the periodic stats")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gpu_debug_markers, false, "GPU",
                    "Insert debug markers into GPU command streams for tools "
                    "like PIX and RenderDoc. Automatically enabled when "
                    "RenderDoc is detected.");

bool IsGpuDebugMarkersEnabled() {
  static bool cached = false;
  static bool result = false;
  if (!cached) {
    cached = true;
    if (REXCVAR_GET(gpu_debug_markers)) {
      result = true;
      REXLOG_INFO("GPU debug markers enabled via CVar");
    } else {
      auto renderdoc_api = rex::ui::RenderDocAPI::CreateIfConnected();
      if (renderdoc_api) {
        result = true;
        REXLOG_INFO("GPU debug markers auto-enabled (RenderDoc detected)");
      }
    }
  }
  return result;
}
