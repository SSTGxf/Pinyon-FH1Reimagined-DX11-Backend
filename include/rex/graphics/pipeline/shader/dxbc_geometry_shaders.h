#pragma once
// DXBC geometry shaders for guest primitive types without native host
// equivalents (point sprites, rectangle lists, quad lists): generated with the
// DXBC assembler so their signatures match the translated vertex / pixel
// shader linkage exactly (the interpolator and clip / cull distance layout
// depends on the shaders of the draw). Shared by the DXBC-based backends
// (Direct3D 11 and 12); originally written for the Direct3D 12 pipeline
// cache.
//
// The key mirrors the Direct3D 12 pipeline's persisted
// PipelineGeometryShader values (the `type` field), so stored shader pack /
// pipeline entries remain comparable.

#include <cstdint>
#include <functional>
#include <vector>

#include <rex/assert.h>

namespace rex::graphics {

// Matches d3d12::PipelineCache::PipelineGeometryShader values.
enum class GeometryShaderType : uint32_t {
  kNone,
  kPointList,
  kRectangleList,
  kQuadList,
};

union GeometryShaderKey {
  uint32_t key;
  struct {
    GeometryShaderType type : 2;
    uint32_t interpolator_count : 5;
    uint32_t user_clip_plane_count : 3;
    uint32_t user_clip_plane_cull : 1;
    uint32_t has_vertex_kill_and : 1;
    uint32_t has_point_size : 1;
    uint32_t has_point_coordinates : 1;
    // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives.
    uint32_t point_ps_ucp_mode : 2;
  };

  GeometryShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

  struct Hasher {
    size_t operator()(const GeometryShaderKey& key) const {
      return std::hash<uint32_t>{}(key.key);
    }
  };
  bool operator==(const GeometryShaderKey& other_key) const { return key == other_key.key; }
  bool operator!=(const GeometryShaderKey& other_key) const { return key != other_key.key; }
};

// Builds the DXBC container of the geometry shader for the key. target_sm_50
// emits shader model 5_0 (Direct3D 11 feature level 11_0) instead of 5_1.
void CreateDxbcGeometryShader(GeometryShaderKey key, std::vector<uint32_t>& shader_out,
                              bool target_sm_50 = false);

}  // namespace rex::graphics
