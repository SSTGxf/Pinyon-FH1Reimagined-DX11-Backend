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

#pragma once

#include <rex/cvar.h>

// GPU Core
REXCVAR_DECLARE(bool, vsync);
REXCVAR_DECLARE(bool, clear_memory_page_state);
REXCVAR_DECLARE(bool, half_pixel_offset);
REXCVAR_DECLARE(bool, async_shader_compilation);

// GPU Resolution / Readback / Queries
REXCVAR_DECLARE(int32_t, resolution_scale);
REXCVAR_DECLARE(int32_t, draw_resolution_scale_x);
REXCVAR_DECLARE(int32_t, draw_resolution_scale_y);
REXCVAR_DECLARE(bool, resolve_resolution_scale_fill_half_pixel_offset);
REXCVAR_DECLARE(std::string, readback_resolve);
REXCVAR_DECLARE(bool, occlusion_query_enable);
REXCVAR_DECLARE(int32_t, query_occlusion_fake_sample_count);

// GPU Depth / Render Target Behavior
REXCVAR_DECLARE(bool, depth_float24_round);
REXCVAR_DECLARE(bool, depth_float24_convert_in_pixel_shader);
REXCVAR_DECLARE(bool, gamma_render_target_as_unorm16);
REXCVAR_DECLARE(bool, native_2x_msaa);
REXCVAR_DECLARE(bool, snorm16_render_target_full_range);

// GPU Textures
REXCVAR_DECLARE(bool, gpu_allow_invalid_fetch_constants);
REXCVAR_DECLARE(bool, gpu_3d_to_2d_texture);
REXCVAR_DECLARE(int32_t, anisotropic_override);
REXCVAR_DECLARE(bool, force_trilinear_filtering);
REXCVAR_DECLARE(double, texture_mip_lod_bias);
REXCVAR_DECLARE(bool, fh1_native_gpu_profile);
REXCVAR_DECLARE(int32_t, texture_cache_memory_limit_render_to_texture);
REXCVAR_DECLARE(int32_t, texture_cache_memory_limit_soft);
REXCVAR_DECLARE(int32_t, texture_cache_memory_limit_hard);
REXCVAR_DECLARE(int32_t, texture_cache_memory_limit_soft_lifetime);

// GPU Primitive Processing
REXCVAR_DECLARE(bool, execute_unclipped_draw_vs_on_cpu);
REXCVAR_DECLARE(bool, execute_unclipped_draw_vs_on_cpu_with_scissor);
REXCVAR_DECLARE(bool, force_convert_line_loops_to_strips);
REXCVAR_DECLARE(bool, force_convert_quad_lists_to_triangle_lists);
REXCVAR_DECLARE(bool, force_convert_triangle_fans_to_lists);
REXCVAR_DECLARE(int32_t, primitive_processor_cache_min_indices);

// GPU Debug
REXCVAR_DECLARE(bool, gpu_debug_markers);
bool IsGpuDebugMarkersEnabled();

// GPU Shader Translation
REXCVAR_DECLARE(std::string, dump_shaders);
REXCVAR_DECLARE(bool, fh1_d3d11_shader_pack);
REXCVAR_DECLARE(bool, fh1_swap_fetch_trace);
REXCVAR_DECLARE(int32_t, fh1_debug_tile_trace_swap);
REXCVAR_DECLARE(int32_t, fh1_debug_callog);
REXCVAR_DECLARE(int32_t, fh1_debug_callog_budget);
REXCVAR_DECLARE(int32_t, fh1_debug_trace_span);
REXCVAR_DECLARE(int32_t, fh1_debug_band2_trace_swap);
REXCVAR_DECLARE(int32_t, fh1_debug_band_suppress);
REXCVAR_DECLARE(int32_t, fh1_debug_minimap_capture_swap);
REXCVAR_DECLARE(bool, fh1_d3d11_resolve_readback);
REXCVAR_DECLARE(bool, fh1_d3d11_command_lists);
REXCVAR_DECLARE(bool, fh1_d3d11_command_list_validate);
REXCVAR_DECLARE(bool, fh1_d3d11_command_list_ab);
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_force_pack);
REXCVAR_DECLARE(bool, fh1_resolve_swap_rb);
REXCVAR_DECLARE(bool, pinyon_transition_log);
REXCVAR_DECLARE(bool, fh1_forensics_capture);
REXCVAR_DECLARE(int32_t, fh1_forensics_from);
REXCVAR_DECLARE(int32_t, fh1_forensics_to);
REXCVAR_DECLARE(int32_t, fh1_forensics_event_mask);
REXCVAR_DECLARE(int32_t, fh1_forensics_ring_events);
REXCVAR_DECLARE(bool, fh1_forensics_include_binds);
REXCVAR_DECLARE(bool, fh1_forensics_include_uploads);
REXCVAR_DECLARE(bool, pinyon_msaa_probe);
REXCVAR_DECLARE(bool, pinyon_audit);
REXCVAR_DECLARE(bool, pinyon_audit_passes);
REXCVAR_DECLARE(int32_t, pinyon_audit_passes_from);
REXCVAR_DECLARE(int32_t, pinyon_audit_passes_to);
REXCVAR_DECLARE(bool, pinyon_resolve_trace);
REXCVAR_DECLARE(bool, pinyon_draw_trace);
REXCVAR_DECLARE(bool, fh1_debug_resolve_store_probe);
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_store_probe_from);
REXCVAR_DECLARE(std::string, fh1_debug_resolve_alias_base);
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_alias_bytes);
REXCVAR_DECLARE(bool, fh1_debug_resolve_small_uav);
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_window_mb);
REXCVAR_DECLARE(bool, fh1_debug_resolve_direct_mirror);
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_force_endian);
REXCVAR_DECLARE(bool, fh1_debug_resolve_cb_capture);
REXCVAR_DECLARE(bool, fh1_debug_resolve_cb_update);
REXCVAR_DECLARE(bool, fh1_debug_transfer_off);
REXCVAR_DECLARE(bool, fh1_debug_transfer_unbatched);
REXCVAR_DECLARE(bool, fh1_debug_transfer_direct);
REXCVAR_DECLARE(bool, fh1_debug_resolve_bind_check);
REXCVAR_DECLARE(std::string, fh1_debug_surface_dump);
REXCVAR_DECLARE(int32_t, fh1_debug_surface_dump_swap);
REXCVAR_DECLARE(std::string, fh1_debug_presented_pair);
REXCVAR_DECLARE(int32_t, fh1_debug_presented_pair_swap);
REXCVAR_DECLARE(int32_t, fh1_debug_presented_pair_count);
REXCVAR_DECLARE(std::string, fh1_debug_dump_guest);
REXCVAR_DECLARE(int32_t, fh1_debug_composite_draws);
REXCVAR_DECLARE(std::string, fh1_debug_composite_dump);
REXCVAR_DECLARE(std::string, fh1_debug_composite_dump_world);
REXCVAR_DECLARE(bool, fh1_debug_composite_world_only);
REXCVAR_DECLARE(int32_t, fh1_debug_clear_draw);
REXCVAR_DECLARE(std::string, fh1_debug_c2_probe);
REXCVAR_DECLARE(int32_t, fh1_debug_c2_probe_from);
REXCVAR_DECLARE(int32_t, fh1_debug_c2_probe_draws);
REXCVAR_DECLARE(std::string, fh1_debug_edram3d_probe);
REXCVAR_DECLARE(int32_t, fh1_debug_edram3d_probe_from);
REXCVAR_DECLARE(int32_t, fh1_debug_edram3d_probe_draws);
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_probe_from);
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_probe_draws);
REXCVAR_DECLARE(int32_t, fh1_debug_srv_bind_all_from);
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_source_probe_from);
REXCVAR_DECLARE(int32_t, fh1_debug_post_clip);
REXCVAR_DECLARE(int32_t, fh1_debug_vertex_dump);
REXCVAR_DECLARE(std::string, fh1_debug_texture_dump);
REXCVAR_DECLARE(std::string, fh1_debug_guest_probe);
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_trace);
REXCVAR_DECLARE(bool, fh1_resolve_guest_refresh);
REXCVAR_DECLARE(int32_t, fh1_native_executor);
REXCVAR_DECLARE(std::string, swap_post_effect);
REXCVAR_DECLARE(bool, swap_post_effect_scene_only);
REXCVAR_DECLARE(std::string, scene_lut_path);
REXCVAR_DECLARE(int32_t, scene_lut_strength);

// Guest shader translation cvars (dxbc_switch, dxbc_source_map,
// use_fuzzy_alpha_epsilon, draw_resolution_scaled_texture_offsets) are
// defined and read only inside the translator, which the D3D12 runtime does
// not compile; the offline shader producer owns them.

#if REX_HAS_VULKAN
// Shared readback controls only the Vulkan backend reads.
REXCVAR_DECLARE(bool, readback_resolve_half_pixel_offset);
REXCVAR_DECLARE(bool, readback_memexport);
REXCVAR_DECLARE(bool, readback_memexport_fast);
// EDRAM render target cache (compiled only with the Vulkan backend).
REXCVAR_DECLARE(bool, depth_transfer_not_equal_test);
REXCVAR_DECLARE(bool, mrt_edram_used_range_clamp_to_min);
REXCVAR_DECLARE(bool, execute_unclipped_draw_vs_on_cpu_for_psi_render_backend);
REXCVAR_DECLARE(bool, non_seamless_cube_map);
REXCVAR_DECLARE(bool, vulkan_sparse_shared_memory);
REXCVAR_DECLARE(bool, vulkan_submit_on_primary_buffer_end);
REXCVAR_DECLARE(bool, vulkan_dynamic_rendering);
REXCVAR_DECLARE(bool, vulkan_async_skip_incomplete_frames);
REXCVAR_DECLARE(int32_t, vulkan_pipeline_creation_threads);
REXCVAR_DECLARE(bool, vulkan_tessellation_wireframe);
REXCVAR_DECLARE(bool, vulkan_force_expand_point_sprites_in_vs);
REXCVAR_DECLARE(bool, vulkan_force_expand_rectangle_lists_in_vs);
REXCVAR_DECLARE(bool, vulkan_force_convert_quad_lists_to_triangle_lists);
REXCVAR_DECLARE(std::string, render_target_path_vulkan);
// Legacy backend compatibility aliases for shared readback controls.
REXCVAR_DECLARE(bool, vulkan_readback_resolve);
REXCVAR_DECLARE(bool, vulkan_readback_memexport);
#endif  // REX_HAS_VULKAN

#if REX_HAS_D3D12
REXCVAR_DECLARE(bool, d3d12_bindless);
REXCVAR_DECLARE(bool, d3d12_submit_on_primary_buffer_end);
REXCVAR_DECLARE(bool, d3d12_dxbc_disasm);
REXCVAR_DECLARE(bool, d3d12_dxbc_disasm_dxilconv);
REXCVAR_DECLARE(int32_t, d3d12_pipeline_creation_threads);
REXCVAR_DECLARE(bool, d3d12_tessellation_wireframe);
REXCVAR_DECLARE(bool, d3d12_tiled_shared_memory);
#endif  // REX_HAS_D3D12

#define XE_GPU_FINE_GRAINED_DRAW_SCOPES 1
