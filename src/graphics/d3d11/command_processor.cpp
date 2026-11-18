/**
 * @file        graphics/d3d11/command_processor.cpp
 * @brief       D3D11 backend command processor (Phase 3: real draws)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/command_processor.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <string>

// Trace-only: the colour render target bound for the most recent draw.
static std::string g_trace_rt_key;

// FH1 footprint-vs probe once-flags (file scope: the probe blocks live in
// two different draw functions, and the function holding Block 2 runs
// before the one holding Blocks 1 and 3, so a shared flag set by the
// first block to run would block the others). Each block fires on the
// first A39E draw it sees, then never again.
static bool g_draw23_vs_vdump_done = false;    // Block 3: vertex data
static bool g_draw23_vs_vsdump_done = false;   // Block 1: VS ucode/DXBC/meta/fetch
static bool g_draw23_vs_applied_done = false;  // Block 2: applied exp-bias
#include <rex/graphics/d3d11/command_list.h>
#include <rex/graphics/d3d11/fh1_native_resolve.h>
// Defined once in the D3D12 command processor (same flag, both backends read
// it via REXCVAR_GET); a second REXCVAR_DEFINE here is a duplicate symbol.
REXCVAR_DECLARE(double, fh1_hud_squeeze);
REXCVAR_DECLARE(int32_t, fh1_debug_frontbuffer_draws);
REXCVAR_DECLARE(int32_t, fh1_debug_frontbuffer_draws_from);
// Defined in graphics/flags.cpp; declared here only, so the alias guard's mode
// does not force every translation unit that includes flags.h to rebuild.
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_alias_mode);
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_alias_from_swap);
REXCVAR_DECLARE(bool, fh1_debug_disable_msaa);
REXCVAR_DECLARE(bool, fh1_debug_hide_hud);
REXCVAR_DECLARE(int32_t, fh1_debug_tile_trace_swap);
REXCVAR_DECLARE(int32_t, fh1_debug_callog);
REXCVAR_DECLARE(int32_t, fh1_debug_callog_budget);
REXCVAR_DECLARE(int32_t, fh1_debug_minimap_capture_swap);
REXCVAR_DECLARE(bool, d3d11_skip_texture_refresh);
#include <rex/graphics/d3d11/fh1_native_surfaces.h>
#include <rex/graphics/d3d11/graphics_system.h>
#include <rex/graphics/d3d11/host_render_config.h>
#include <rex/graphics/d3d11/resource_state_tracker.h>
#include <rex/graphics/d3d11/shared_memory_upload_device.h>
#include <rex/graphics/d3d11/submission_queue.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/fh1_edram_resolve.h>
#include <rex/graphics/registers.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/d3d11/d3d11_presenter.h>
#include <rex/ui/d3d11/d3d11_provider.h>
#include <rex/ui/presenter.h>
#include <rex/graphics/render_audit.h>
#include <rex/graphics/render_forensics.h>

REXCVAR_DEFINE_BOOL(d3d11_band_trace, false, "GPU",
                    "Log D3D11 FH1 untile resolve and viewport/scissor band diagnostics");

namespace rex::graphics::d3d11 {

namespace {

// FH1 call logger (Phase 2): gate + budget for the "FH1 callog" lines.
// Returns true once per line actually emitted; false when the logger is
// off, the swap does not match, or the line budget is exhausted.
uint32_t g_callog_lines_used = 0;
bool g_callog_budget_exhausted_logged = false;
// Phase 2c: multi-swap trace window (same contract as the shared command
// processor's copy). The callog gates fire for swaps in
// [start, start + span), where span is fh1_debug_trace_span (default 1 =
// single swap, unchanged behavior).
int32_t Fh1TraceSpan() {
  const int32_t span = REXCVAR_GET(fh1_debug_trace_span);
  return span < 1 ? 1 : span;
}
bool Fh1SwapInTraceWindow(uint64_t swap, int32_t start) {
  if (start <= 0) return false;
  return swap >= uint64_t(start) &&
         swap < uint64_t(start) + uint64_t(Fh1TraceSpan());
}
bool Fh1CallogEnabled(uint64_t swap) {
  const int32_t callog_swap = REXCVAR_GET(fh1_debug_callog);
  if (!Fh1SwapInTraceWindow(swap, callog_swap)) {
    return false;
  }
  const int32_t budget = REXCVAR_GET(fh1_debug_callog_budget);
  if (budget > 0 && g_callog_lines_used >= uint32_t(budget)) {
    if (!g_callog_budget_exhausted_logged) {
      g_callog_budget_exhausted_logged = true;
      REXGPU_INFO("FH1 callog budget exhausted swap={} budget={}", swap, budget);
    }
    return false;
  }
  ++g_callog_lines_used;
  return true;
}

// IEEE-754 binary16 -> float, for reading back a 16_16_16_16_FLOAT surface (the
// storage format of the FH1 scene targets) without depending on F16C.
float HalfToFloat(uint16_t value) {
  const uint32_t sign = uint32_t(value & 0x8000u) << 16;
  uint32_t exponent = uint32_t(value >> 10) & 0x1Fu;
  uint32_t mantissa = uint32_t(value) & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (!mantissa) {
      bits = sign;
    } else {
      int shift = 0;
      while (!(mantissa & 0x400u)) {
        mantissa <<= 1;
        ++shift;
      }
      mantissa &= 0x3FFu;
      bits = sign | (uint32_t(127 - 15 - shift) << 23) | (mantissa << 13);
    }
  } else if (exponent == 0x1F) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  float result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

// One texel of a surface as RGB in 0..1 for the dump, by the typed format the
// readback texture was created with.
void DecodeDumpedTexel(DXGI_FORMAT format, const uint8_t* texel, float rgb[3]) {
  switch (format) {
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
      uint32_t packed;
      std::memcpy(&packed, texel, sizeof(packed));
      rgb[0] = float(packed & 0x3FFu) / 1023.0f;
      rgb[1] = float((packed >> 10) & 0x3FFu) / 1023.0f;
      rgb[2] = float((packed >> 20) & 0x3FFu) / 1023.0f;
      return;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: {
      uint16_t channels[4];
      std::memcpy(channels, texel, sizeof(channels));
      for (uint32_t i = 0; i < 3; ++i) {
        rgb[i] = format == DXGI_FORMAT_R16G16B16A16_FLOAT
                     ? HalfToFloat(channels[i])
                     : float(channels[i]) / 65535.0f;
      }
      return;
    }
    // Every narrower 32-bit format the colour draw path can produce is an
    // 8-bit RGBA resource (R8G8B8A8_UNORM), which is also the safe assumption
    // for a format this diagnostic does not know: read it as bytes.
    default:
      rgb[0] = float(texel[0]) / 255.0f;
      rgb[1] = float(texel[1]) / 255.0f;
      rgb[2] = float(texel[2]) / 255.0f;
      return;
  }
}

struct SurfaceDumpStats {
  uint64_t nonzero = 0;
  uint64_t total = 0;
  uint32_t distinct = 0;
  double mean[3] = {};
};

uint64_t HashRows(const uint8_t* bytes, uint32_t row_pitch, uint32_t rows,
                  uint32_t row_bytes, std::vector<uint64_t>* row_hashes) {
  uint64_t hash = UINT64_C(1469598103934665603);
  if (row_hashes) {
    row_hashes->clear();
    row_hashes->reserve(rows);
  }
  for (uint32_t y = 0; y < rows; ++y) {
    const uint8_t* row = bytes + size_t(row_pitch) * y;
    uint64_t row_hash = UINT64_C(1469598103934665603);
    for (uint32_t x = 0; x < row_bytes; ++x) {
      row_hash = (row_hash ^ row[x]) * UINT64_C(1099511628211);
      hash = (hash ^ row[x]) * UINT64_C(1099511628211);
    }
    if (row_hashes) {
      row_hashes->push_back(row_hash);
    }
  }
  return hash;
}

// Stages `source` and decodes its pixels into `rgb_out` (three bytes per pixel,
// row-major, top-down), filling in what the pixels contain. The shared readback
// of the surface dump, the sampled-texture dump and the draw footprint probe:
// `read_format` is the typed format the data is to be read as (a surface's view
// format, or a texture's SRV format - what the shader samples).
bool ReadTexture2DRgb(ID3D11Device* device, ID3D11DeviceContext* context,
                      ID3D11Texture2D* source, DXGI_FORMAT read_format,
                      std::vector<uint8_t>& rgb_out, uint32_t& width_out, uint32_t& height_out,
                      SurfaceDumpStats& stats, std::string& error) {
  if (!source) {
    error = "no texture";
    return false;
  }
  // Only the formats DecodeDumpedTexel knows are read back, and each with the
  // size of one decoded texel: reading a block-compressed resource's rows at 4
  // bytes per pixel walks off the mapped image (an access violation, not a bad
  // picture), so those are refused by name instead.
  uint32_t texel_size = 0;
  switch (read_format) {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      texel_size = 4;
      break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
      texel_size = 8;
      break;
    default:
      error = fmt::format("unsupported readback format {}", uint32_t(read_format));
      return false;
  }
  D3D11_TEXTURE2D_DESC staging_desc;
  source->GetDesc(&staging_desc);
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;
  staging_desc.Format = read_format;
  staging_desc.SampleDesc.Count = 1;
  staging_desc.SampleDesc.Quality = 0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
    error = "staging creation failed";
    return false;
  }
  context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source, 0, nullptr);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    error = "map failed";
    return false;
  }
  const uint32_t width = staging_desc.Width;
  const uint32_t height = std::min<uint32_t>(staging_desc.Height, 2048u);
  std::vector<uint8_t> rgb(size_t(width) * height * 3u);
  std::set<uint32_t> colours;
  // Never trust the row pitch to hold a whole row of the requested format -
  // read only the columns the mapped row actually covers (the rest stays
  // black) rather than walking past the mapping.
  const uint32_t readable_width =
      std::min<uint32_t>(width, uint32_t(size_t(mapped.RowPitch) / texel_size));
  double sums[3] = {};
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row =
        static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * y;
    for (uint32_t x = 0; x < readable_width; ++x) {
      float texel[3];
      DecodeDumpedTexel(read_format, row + size_t(x) * texel_size, texel);
      uint8_t* out = rgb.data() + (size_t(y) * width + x) * 3u;
      for (uint32_t c = 0; c < 3; ++c) {
        const float clamped = std::isfinite(texel[c]) ? std::clamp(texel[c], 0.0f, 1.0f) : 1.0f;
        out[c] = uint8_t(clamped * 255.0f + 0.5f);
        sums[c] += clamped;
      }
      ++stats.total;
      if (out[0] || out[1] || out[2]) ++stats.nonzero;
      if ((!(x & 3u)) && (!(y & 3u)) && colours.size() < 400000u) {
        colours.insert((uint32_t(out[0]) << 16) | (uint32_t(out[1]) << 8) | out[2]);
      }
    }
  }
  context->Unmap(staging.Get(), 0);
  stats.distinct = uint32_t(colours.size());
  for (uint32_t c = 0; c < 3; ++c) {
    stats.mean[c] = stats.total ? sums[c] / double(stats.total) : 0.0;
  }
  rgb_out = std::move(rgb);
  width_out = width;
  height_out = height;
  return true;
}
// Writes `rgb` (three bytes per pixel) as a binary PPM.
bool WriteRgbPpm(const std::string& path, const std::vector<uint8_t>& rgb, uint32_t width,
                 uint32_t height) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  file << "P6\n" << width << " " << height << "\n255\n";
  file.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
  return file.good();
}

// A per-60-row view of an RGB image: for each band, the share of texels that
// are not black and the mean luma. A frame missing one of its halves is a band
// profile that collapses to zero above a row, which a single mean hides - so
// every image in a presented-vs-composite comparison is reported this way.
struct BandProfile {
  std::string nonzero;
  std::string mean;
};

BandProfile ProfileBands(const std::vector<uint8_t>& rgb, uint32_t width, uint32_t height,
                         uint32_t max_rows = 720) {
  BandProfile profile;
  const uint32_t rows = std::min(height, max_rows);
  for (uint32_t band = 0; band < rows; band += 60) {
    const uint32_t end = std::min(band + 60u, rows);
    uint64_t total = 0, nonzero = 0, luma = 0;
    for (uint32_t y = band; y < end; ++y) {
      const uint8_t* row = rgb.data() + size_t(y) * width * 3u;
      for (uint32_t x = 0; x < width; ++x) {
        const uint32_t value = uint32_t(row[x * 3]) + uint32_t(row[x * 3 + 1]) + row[x * 3 + 2];
        ++total;
        nonzero += value > 12u;
        luma += value;
      }
    }
    if (!profile.nonzero.empty()) {
      profile.nonzero += ' ';
      profile.mean += ' ';
    }
    profile.nonzero += fmt::format("{:.3f}", total ? double(nonzero) / double(total) : 0.0);
    profile.mean += fmt::format("{:.1f}", total ? double(luma) / double(total * 3u) : 0.0);
  }
  return profile;
}

// The typed format a readback can use for a resource's stored format: the
// cache keeps swap textures under their DXGI resource format, which may be
// typeless, and a staging copy needs the typed sibling.
DXGI_FORMAT TypedReadbackFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return DXGI_FORMAT_R16G16B16A16_UNORM;
    default:
      return format;
  }
}

// Stages `source` and writes `path` as a binary PPM.
bool DumpTexture2DPpm(ID3D11Device* device, ID3D11DeviceContext* context,
                      ID3D11Texture2D* source, DXGI_FORMAT read_format, const std::string& path,
                      SurfaceDumpStats& stats, std::string& error) {
  std::vector<uint8_t> rgb;
  uint32_t width = 0;
  uint32_t height = 0;
  if (!ReadTexture2DRgb(device, context, source, read_format, rgb, width, height, stats, error)) {
    return false;
  }
  if (!WriteRgbPpm(path, rgb, width, height)) {
    error = "open failed";
    return false;
  }
  return true;
}

// Writes `path` as a binary PPM of the surface and fills in what the pixels
// contain. MSAA surfaces are resolved to 1x first; a surface this fails on is
// reported with the HRESULT instead of silently skipped.
bool DumpSurfacePpm(ID3D11Device* device, ID3D11DeviceContext* context,
                    D3D11Fh1NativeSurface* surface, const std::string& path,
                    SurfaceDumpStats& stats, std::string& error) {
  if (!surface || !surface->texture) {
    error = "no texture";
    return false;
  }
  D3D11_TEXTURE2D_DESC desc;
  surface->texture->GetDesc(&desc);
  const DXGI_FORMAT read_format =
      surface->key.is_depth ? DXGI_FORMAT_UNKNOWN : surface->view_format;
  if (read_format == DXGI_FORMAT_UNKNOWN) {
    error = "no typed readback format";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D11Texture2D> resolved;
  ID3D11Texture2D* source = surface->texture;
  if (desc.SampleDesc.Count > 1) {
    D3D11_TEXTURE2D_DESC resolve_desc = desc;
    resolve_desc.SampleDesc.Count = 1;
    resolve_desc.SampleDesc.Quality = 0;
    resolve_desc.Format = read_format;
    resolve_desc.BindFlags = 0;
    resolve_desc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&resolve_desc, nullptr, &resolved))) {
      error = "resolve target creation failed";
      return false;
    }
    context->ResolveSubresource(resolved.Get(), 0, surface->texture, 0, read_format);
    source = resolved.Get();
  }
  return DumpTexture2DPpm(device, context, source, read_format, path, stats, error);
}

std::string SanitizedKeyName(const Fh1SurfaceKey& key) {
  std::string name = key.Describe();
  for (char& c : name) {
    if (c == '/') c = '_';
  }
  return name;
}

// The same readback as DumpSurfacePpm without a file: the surface's pixels in
// host memory, for a probe that compares two reads of one surface.
// Per-60-row RGB nonzero bands of a read-back image. The footprint a draw probe
// reports says which region a pass touched; these say what it left there, which
// is the difference between "the half was never drawn" and "it was drawn and
// then blacked out by a later pass".
std::string RgbBandProfile(const std::vector<uint8_t>& rgb, uint32_t width, uint32_t height) {
  std::string out;
  if (width == 0 || height == 0 || rgb.size() < size_t(width) * height * 3) {
    return out;
  }
  for (uint32_t y0 = 0; y0 < height; y0 += 60) {
    const uint32_t y1 = std::min(height, y0 + 60);
    uint64_t nonzero = 0, total = 0;
    for (uint32_t y = y0; y < y1; ++y) {
      const uint8_t* row = rgb.data() + size_t(y) * width * 3;
      for (uint32_t x = 0; x < width; ++x) {
        ++total;
        if (row[x * 3] || row[x * 3 + 1] || row[x * 3 + 2]) {
          ++nonzero;
        }
      }
    }
    out += fmt::format("{}{:.2f}", out.empty() ? "" : " ",
                       total ? double(nonzero) / double(total) : 0.0);
  }
  return out;
}

bool ReadSurfaceRgb(ID3D11Device* device, ID3D11DeviceContext* context,
                    D3D11Fh1NativeSurface* surface, std::vector<uint8_t>& rgb_out,
                    uint32_t& width_out, uint32_t& height_out, SurfaceDumpStats& stats,
                    std::string& error) {
  if (!surface || !surface->texture) {
    error = "no texture";
    return false;
  }
  D3D11_TEXTURE2D_DESC desc;
  surface->texture->GetDesc(&desc);
  const DXGI_FORMAT read_format =
      surface->key.is_depth ? DXGI_FORMAT_UNKNOWN : surface->view_format;
  if (read_format == DXGI_FORMAT_UNKNOWN) {
    error = "no typed readback format";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D11Texture2D> resolved;
  ID3D11Texture2D* source = surface->texture;
  if (desc.SampleDesc.Count > 1) {
    D3D11_TEXTURE2D_DESC resolve_desc = desc;
    resolve_desc.SampleDesc.Count = 1;
    resolve_desc.SampleDesc.Quality = 0;
    resolve_desc.Format = read_format;
    resolve_desc.BindFlags = 0;
    resolve_desc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&resolve_desc, nullptr, &resolved))) {
      error = "resolve target creation failed";
      return false;
    }
    context->ResolveSubresource(resolved.Get(), 0, surface->texture, 0, read_format);
    source = resolved.Get();
  }
  return ReadTexture2DRgb(device, context, source, read_format, rgb_out, width_out, height_out,
                          stats, error);
}

// A depth surface's memory, as the depth resolve would read it. The colour dump
// refuses depth outright, so the depth leg of the chain - geometry into a d0
// surface, then the depth resolve into guest memory, then the front-buffer
// draws that sample the result - had no instrument at all: every surface dump
// reported "depth surface (not dumped)". Direct3D 11 does allow CopyResource on
// a depth-stencil resource into a same-format staging texture, which is what
// this does, so a dump can now say whether a d0 surface holds a scene.
//
// The packing of 24-bit depth in a 32-bit word is implementation-defined:
// R24_UNORM_X8_TYPELESS puts the depth in the low 24 bits, and drivers that
// store it in the high 24 bits exist too, so both decodings are measured and
// reported instead of one being assumed. The written PPM shades the low-24-bit
// decoding (for an R32_FLOAT surface, the float itself).
struct DepthDumpStats {
  uint64_t total = 0;
  uint64_t zero_words = 0;
  uint64_t filled_words = 0;
  uint32_t first = 0;
  double mean_lo = 0.0;
  double mean_hi = 0.0;
  double min_lo = 1.0;
  double max_lo = 0.0;
};

bool DumpDepthSurfacePpm(ID3D11Device* device, ID3D11DeviceContext* context,
                         D3D11Fh1NativeSurface* surface, const std::string& path,
                         DepthDumpStats& stats, std::string& error) {
  if (!surface || !surface->texture) {
    error = "no texture";
    return false;
  }
  if (!surface->key.is_depth) {
    error = "not a depth surface";
    return false;
  }
  D3D11_TEXTURE2D_DESC desc;
  surface->texture->GetDesc(&desc);
  if (desc.SampleDesc.Count > 1) {
    error = fmt::format("multisampled depth ({}x) is not readable", desc.SampleDesc.Count);
    return false;
  }
  const bool float_depth = desc.Format == DXGI_FORMAT_R32_TYPELESS ||
                           desc.Format == DXGI_FORMAT_D32_FLOAT ||
                           desc.Format == DXGI_FORMAT_R32_FLOAT;
  D3D11_TEXTURE2D_DESC staging_desc = desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
    error = "staging creation failed";
    return false;
  }
  context->CopyResource(staging.Get(), surface->texture);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    error = "map failed";
    return false;
  }
  // Never walk past the mapped row pitch: a 1280-wide surface can be mapped
  // with fewer readable words than its width.
  const uint32_t width =
      std::min<uint32_t>(desc.Width, uint32_t(size_t(mapped.RowPitch) / sizeof(uint32_t)));
  const uint32_t height = std::min<uint32_t>(desc.Height, 2048u);
  std::vector<uint8_t> rgb(size_t(desc.Width) * height * 3u, 0u);
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t* row = reinterpret_cast<const uint32_t*>(
        static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * y);
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t word = row[x];
      double lo;
      if (float_depth) {
        float value;
        std::memcpy(&value, &word, sizeof(value));
        lo = std::isfinite(value) ? double(std::clamp(value, 0.0f, 1.0f)) : 0.0;
      } else {
        lo = double(word & 0xFFFFFFu) / 16777215.0;
      }
      const double hi = double(word >> 8) / 16777215.0;
      if (!stats.total) stats.first = word;
      stats.mean_lo += lo;
      stats.mean_hi += hi;
      stats.min_lo = std::min(stats.min_lo, lo);
      stats.max_lo = std::max(stats.max_lo, lo);
      if (!word) ++stats.zero_words;
      if (word == 0xFFFFFFFFu) ++stats.filled_words;
      ++stats.total;
      const uint8_t shade = uint8_t(std::clamp(lo, 0.0, 1.0) * 255.0 + 0.5);
      uint8_t* out = rgb.data() + (size_t(y) * desc.Width + x) * 3u;
      out[0] = shade;
      out[1] = shade;
      out[2] = shade;
    }
  }
  context->Unmap(staging.Get(), 0);
  if (stats.total) {
    stats.mean_lo /= double(stats.total);
    stats.mean_hi /= double(stats.total);
  }
  if (!WriteRgbPpm(path, rgb, desc.Width, height)) {
    error = "open failed";
    return false;
  }
  return true;
}

// The footprint of a draw, from two reads of the surface it writes: the share
// of pixels that changed (channel difference above 8/255), the bounding box of
// those changes, and the changes per 64-pixel column and per 60-pixel row band
// as 0-9 deciles - enough to see which band of the frame a pass writes without
// a PPM per draw. The same function measures a stability control (two reads
// with no draw between them), which must report 0.
struct DrawFootprint {
  double changed = 0.0;  // Over rows 0..min(height, 720).
  int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = -1, y1 = -1;
  std::string columns;  // 0-9 per 64-pixel column, rows 0..min(height, 720).
  std::string rows;     // 0-9 per 60-pixel row, all columns.
};

DrawFootprint MeasureFootprint(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after,
                               uint32_t width, uint32_t height) {
  DrawFootprint footprint;
  const uint32_t rows_considered = std::min<uint32_t>(height, 720u);
  if (before.size() != after.size() || before.empty()) {
    footprint.columns = "n/a";
    footprint.rows = "n/a";
    return footprint;
  }
  const uint32_t column_band = 64;
  const uint32_t column_bands = (width + column_band - 1) / column_band;
  const uint32_t row_band = 60;
  const uint32_t row_bands = (height + row_band - 1) / row_band;
  std::vector<uint64_t> column_changed(column_bands, 0), column_total(column_bands, 0);
  std::vector<uint64_t> row_changed(row_bands, 0), row_total(row_bands, 0);
  uint64_t changed = 0;
  for (uint32_t y = 0; y < height; ++y) {
    const size_t row = size_t(y) * width * 3u;
    for (uint32_t x = 0; x < width; ++x) {
      const size_t at = row + size_t(x) * 3u;
      const uint32_t d = std::max({uint32_t(std::abs(int(before[at]) - int(after[at]))),
                                   uint32_t(std::abs(int(before[at + 1]) - int(after[at + 1]))),
                                   uint32_t(std::abs(int(before[at + 2]) - int(after[at + 2])))});
      if (y < rows_considered) {
        ++column_total[x / column_band];
      }
      ++row_total[y / row_band];
      if (d <= 8) {
        continue;
      }
      if (y < rows_considered) {
        ++column_changed[x / column_band];
        ++changed;
      }
      ++row_changed[y / row_band];
      footprint.x0 = std::min<int32_t>(footprint.x0, int32_t(x));
      footprint.x1 = std::max<int32_t>(footprint.x1, int32_t(x));
      footprint.y0 = std::min<int32_t>(footprint.y0, int32_t(y));
      footprint.y1 = std::max<int32_t>(footprint.y1, int32_t(y));
    }
  }
  const uint64_t considered = uint64_t(width) * rows_considered;
  footprint.changed = considered ? double(changed) / double(considered) : 0.0;
  for (uint32_t i = 0; i < column_bands; ++i) {
    footprint.columns += char('0' + (column_total[i]
                                         ? std::min<uint32_t>(9, uint32_t(column_changed[i] * 10 /
                                                                         column_total[i]))
                                         : 0));
  }
  for (uint32_t i = 0; i < row_bands; ++i) {
    footprint.rows += char('0' + (row_total[i] ? std::min<uint32_t>(9, uint32_t(row_changed[i] * 10 /
                                                                                row_total[i]))
                                               : 0));
  }
  return footprint;
}

}  // namespace

D3D11CommandProcessor::D3D11CommandProcessor(D3D11GraphicsSystem* graphics_system,
                                             system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state), d3d11_graphics_system_(graphics_system) {}

D3D11CommandProcessor::~D3D11CommandProcessor() = default;

bool D3D11CommandProcessor::SetupContext() {
  // Phase 3: guest packets run, draws issue for real on the immediate
  // context (pipeline cache + primitive processor + texture bindings), the
  // FH1 native resolve path handles EDRAM copies (Phase 4 groundwork).
  ui::d3d11::D3D11Provider& provider = GetD3D11Provider();
  shared_memory_ = std::make_unique<D3D11SharedMemory>(
      *memory_, std::make_unique<D3D11SharedMemoryDeviceUploadSink>(
                    provider.GetDevice(), provider.GetImmediateContext()));
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("D3D11: shared memory initialization failed");
    return false;
  }
  primitive_processor_ =
      std::make_unique<D3D11PrimitiveProcessor>(*register_file_, *memory_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("D3D11: primitive processor initialization failed");
    return false;
  }
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  host_render_config_ = std::make_unique<D3D11HostRenderConfig>(draw_resolution_scale_x,
                                                                draw_resolution_scale_y);
  host_render_config_->Initialize(provider.GetDevice());
  pipeline_cache_ =
      std::make_unique<D3D11PipelineCache>(*this, *register_file_, *host_render_config_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("D3D11: pipeline cache initialization failed");
    return false;
  }
  texture_cache_ = std::make_unique<D3D11TextureCache>(
      *register_file_, *shared_memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this);
  if (!texture_cache_->Initialize()) {
    REXGPU_ERROR("D3D11: texture cache initialization failed");
    return false;
  }
  render_target_cache_ = std::make_unique<D3D11RenderTargetCache>(
      *register_file_, *memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this);
  if (!render_target_cache_->Initialize()) {
    REXGPU_ERROR("D3D11: render target cache initialization failed");
    return false;
  }
  const int32_t executor_mode = REXCVAR_GET(fh1_native_executor);
  const uint32_t untile_height =
      executor_mode != 0 && executor_mode != 2 && draw_resolution_scale_x == 1 &&
              draw_resolution_scale_y == 1
          ? kFh1D3D11UntileHeight
          : 0u;
  // Phase 4: the FH1 native EDRAM surface layer and its SM5.0 resolve path
  // into the guest-memory mirror (the FH1 route owns its render targets -
  // there is no generic render target cache on it, mirroring D3D12).
  native_surfaces_ = std::make_unique<D3D11Fh1NativeSurfaces>(
      provider.GetDevice(), *host_render_config_, untile_height);
  uint32_t resolve_scale = draw_resolution_scale_x;
  if (draw_resolution_scale_y != resolve_scale || resolve_scale > 4) {
    REXGPU_WARN(
        "D3D11 FH1 native resolve: non-uniform or over-4x draw resolution scale ({}x{}), "
        "resolving at 1x",
        draw_resolution_scale_x, draw_resolution_scale_y);
    resolve_scale = 1;
  }
  native_resolve_ = std::make_unique<D3D11Fh1NativeResolve>(
      provider.GetDevice(), provider.GetImmediateContext(), *host_render_config_, resolve_scale);
  // Phase 2 C1: the native executor, over the same surface registry the resolve
  // reads. Off by default and fully gated: with fh1_native_executor=0 this block
  // is skipped and no other code path changes.
  {
    if (executor_mode == 2) {
      REXGPU_WARN("D3D11 FH1 native executor: mode 2 is reserved; running with the executor off");
    } else if (executor_mode != 0) {
      D3D11Fh1NativeExecutorConfig executor_config;
      executor_config.gamma_as_unorm16 = host_render_config_->gamma_render_target_as_unorm16();
      executor_config.scale = draw_resolution_scale_x;
      executor_config.untile_height = native_surfaces_->untile_height();
      executor_config.surfaces = native_surfaces_.get();
      native_executor_ = std::make_unique<D3D11Fh1NativeExecutor>(*this, *register_file_);
      if (!native_executor_->Initialize(executor_config)) {
        native_executor_.reset();
      }
    }
  }
  // The readback presents what the resolve path actually produced. Without the
  // executor the draws never reach the EDRAM surfaces the resolve reads, so the
  // readback faithfully presents an empty frame while the readback-off path
  // faithfully preserves the CPU's last frame - say which one is running.
  if (REXCVAR_GET(fh1_d3d11_resolve_readback) && !native_executor_) {
    REXGPU_WARN(
        "D3D11 FH1 native resolve: fh1_d3d11_resolve_readback is on but the native executor is "
        "off - the resolve reads surfaces the draws never fill, so the presented frame will be "
        "empty (set fh1_native_executor=1 to present drawn frames)");
  }
  // Phase 3 M2: the command tape, arena, replay sink and resource-state map
  // exist behind a default-off flag; the resolve path and its mirror transfers
  // record into the tape and replay at the resolve boundary. Draws and the
  // swap path remain immediate, so the flag-off path is unchanged.
  command_lists_enabled_ = REXCVAR_GET(fh1_d3d11_command_lists);
  if (command_lists_enabled_) {
    command_list_ = std::make_unique<D3D11CommandList>();
    resource_state_tracker_ = std::make_unique<D3D11ResourceStateTracker>();
    submission_queue_ = std::make_unique<D3D11SubmissionQueue>(provider.GetImmediateContext());
    REXGPU_INFO(
        "D3D11 command lists: fh1_d3d11_command_lists is on - the resolve path records into "
        "the tape and replays at the resolve boundary (Phase 3 M2); draws and the swap path "
        "remain immediate");
  }
  REXGPU_INFO("D3D11 GPU backend: Phase 3 - draws issue through the pipeline cache on the "
              "immediate context");
  return true;
}

void D3D11CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                   uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  if (!pipeline_cache_) {
    return;
  }
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id);
}

void D3D11CommandProcessor::LogSwapWait() {
  const double swap_wait_ms = double(swap_wait_ticks_) * 1000.0 /
                              double(rex::chrono::Clock::QueryHostTickFrequency());
  REXGPU_INFO(
      "D3D11 presentation: wait-for-GPU on {} swaps, {:.1f} ms total, {:.3f} ms mean per "
      "swap",
      swap_wait_count_, swap_wait_ms, swap_wait_ms / double(swap_wait_count_));
  // The title hard-exits on termination, so the shutdown summary never runs on
  // a normal session: report the pack tallies with the presentation line.
  if (pipeline_cache_) {
    pipeline_cache_->LogPrecompiledShaderStats();
  }
  if (native_resolve_) {
    REXGPU_INFO(
        "D3D11 FH1 native resolve: {} resolve calls, {} frontbuffer readbacks, {} MB to guest "
        "memory",
        resolve_call_count_, resolve_readback_count_, resolve_readback_bytes_ / (1024 * 1024));
  }
}

void D3D11CommandProcessor::LogTapeCounters() {
  if (!command_lists_enabled_) {
    return;
  }
  REXGPU_INFO(
      "D3D11 command lists: {} submissions, {} packets, {} arena bytes; {} resolve packets, "
      "{} draw packets, {} texture uploads; draws immediate[fallback={} unrecordable={} "
      "empty_viewport={}]",
      tape_submit_count_, tape_packets_submitted_, tape_arena_bytes_submitted_,
      tape_resolve_packets_, tape_draw_packets_, tape_texture_upload_packets_,
      tape_draw_fallbacks_, tape_draw_unrecordable_, tape_draw_empty_viewport_);
}

void D3D11CommandProcessor::ShutdownContext() {
  // The command-list services hold references into the resources torn down
  // below; drop them first (the tape's packets hold COM references to the
  // surfaces and buffers they name, so it must go before those resources).
  command_sink_.reset();
  submission_queue_.reset();
  resource_state_tracker_.reset();
  LogTapeCounters();
  command_list_.reset();
  command_lists_enabled_ = false;
  if (swap_wait_count_) {
    LogSwapWait();
  }
  if (native_resolve_) {
    REXGPU_INFO("D3D11 FH1 native resolve: {} dispatches, {} skipped (last: {}), {} surfaces",
                native_resolve_->dispatch_count(), native_resolve_->skipped_count(),
                native_resolve_->last_skip() ? native_resolve_->last_skip() : "none",
                native_surfaces_ ? native_surfaces_->surface_count() : 0);
    native_resolve_->Shutdown();
    native_resolve_.reset();
  }
  if (native_executor_) {
    native_executor_->LogStats(presented_output_frames_);
    native_executor_->Shutdown();
    native_executor_.reset();
  }
  native_surfaces_.reset();
  if (pipeline_cache_) {
    pipeline_cache_->Shutdown();
    pipeline_cache_.reset();
  }
  host_render_config_.reset();
  if (render_target_cache_) {
    render_target_cache_->Shutdown();
    render_target_cache_.reset();
  }
  if (texture_cache_) {
    texture_cache_->Shutdown();
    texture_cache_.reset();
  }
  if (primitive_processor_) {
    primitive_processor_->Shutdown();
    primitive_processor_.reset();
  }
  if (shared_memory_) {
    shared_memory_->Shutdown();
    shared_memory_.reset();
  }
}

Shader* D3D11CommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

//
// Register write invalidation (mirrors the Direct3D 12 command processor).
//

void D3D11CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  if (index == XE_GPU_REG_RB_SURFACE_INFO && REXCVAR_GET(fh1_debug_disable_msaa)) {
    reg::RB_SURFACE_INFO surface_info;
    surface_info.value = value;
    if (surface_info.msaa_samples != xenos::MsaaSamples::k1X) {
      surface_info.msaa_samples = xenos::MsaaSamples::k1X;
      value = surface_info.value;
      static bool logged_override = false;
      if (!logged_override) {
        logged_override = true;
        REXGPU_WARN("D3D11 FH1 diagnostic: forcing RB_SURFACE_INFO to 1x MSAA");
      }
    }
  }
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    if (float_constant_index >= 256) {
      float_constant_index -= 256;
      if (current_float_constant_map_pixel_[float_constant_index >> 6] &
          (1ull << (float_constant_index & 63))) {
        cbuffer_binding_float_pixel_.up_to_date = false;
      }
    } else {
      if (current_float_constant_map_vertex_[float_constant_index >> 6] &
          (1ull << (float_constant_index & 63))) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_binding_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    cbuffer_binding_fetch_.up_to_date = false;
    if (texture_cache_ != nullptr) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
    // FH1 call logger (Phase 2): texture-fetch constant write (single).
    if (Fh1CallogEnabled(presented_output_frames_)) {
      const uint32_t fetch_index =
          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6;
      REXGPU_INFO("FH1 callog side=G swap={} kind=texfetch first={} last={}",
                  presented_output_frames_, fetch_index, fetch_index);
    }
    }
    const uint32_t vfetch_index = (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 2;
    vertex_buffers_in_sync_[vfetch_index >> 6] &= ~(uint64_t(1) << (vfetch_index & 63));
  }
}

void D3D11CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  const uint32_t surface_info_index = XE_GPU_REG_RB_SURFACE_INFO;
  const uint64_t end_index_wide = uint64_t(start_index) + num_registers;
  if (REXCVAR_GET(fh1_debug_disable_msaa) && start_index <= surface_info_index &&
      surface_info_index < end_index_wide) {
    // Most guest register packets take this bulk path, which writes directly
    // into RegisterFile and never reaches the per-register override above.
    std::vector<uint32_t> adjusted(base, base + num_registers);
    const uint32_t offset = surface_info_index - start_index;
    reg::RB_SURFACE_INFO surface_info;
    surface_info.value = rex::byte_swap(adjusted[offset]);
    if (surface_info.msaa_samples != xenos::MsaaSamples::k1X) {
      surface_info.msaa_samples = xenos::MsaaSamples::k1X;
      adjusted[offset] = rex::byte_swap(surface_info.value);
      base = adjusted.data();
      static bool logged_override = false;
      if (!logged_override) {
        logged_override = true;
        REXGPU_WARN("D3D11 FH1 diagnostic: forcing bulk RB_SURFACE_INFO writes to 1x MSAA");
      }
    }
  }
  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const uint64_t* usage_map, uint32_t first_constant,
                                         uint32_t last_constant) {
    if (first_constant > last_constant) {
      return false;
    }
    uint32_t first_word = first_constant >> 6;
    uint32_t last_word = last_constant >> 6;
    uint32_t first_bit = first_constant & 63;
    uint32_t last_bit = last_constant & 63;
    if (first_word == last_word) {
      uint32_t bit_count = last_bit - first_bit + 1;
      uint64_t mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
      return (usage_map[first_word] & mask) != 0;
    }
    if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
      return true;
    }
    for (uint32_t word = first_word + 1; word < last_word; ++word) {
      if (usage_map[word]) {
        return true;
      }
    }
    uint32_t last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
    return (usage_map[last_word] & last_mask) != 0;
  };

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    if (first_float_constant < 256) {
      uint32_t last_vertex_constant = std::min(last_float_constant, 255u);
      if (range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                       last_vertex_constant)) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
    if (last_float_constant >= 256) {
      uint32_t first_pixel_constant =
          first_float_constant >= 256 ? first_float_constant - 256 : 0;
      uint32_t last_pixel_constant = last_float_constant - 256;
      if (range_has_any_constant_usage(current_float_constant_map_pixel_, first_pixel_constant,
                                       last_pixel_constant)) {
        cbuffer_binding_float_pixel_.up_to_date = false;
      }
    }
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_bool_loop_.up_to_date = false;
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_fetch_.up_to_date = false;
    uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    // FH1 call logger (Phase 2): texture-fetch constant write (range).
    if (Fh1CallogEnabled(presented_output_frames_)) {
      REXGPU_INFO("FH1 callog side=G swap={} kind=texfetch first={} last={}",
                  presented_output_frames_, first_fetch_dword / 6, last_fetch_dword / 6);
    }
    }
    const uint32_t first_vfetch = first_fetch_dword / 2;
    const uint32_t last_vfetch = last_fetch_dword / 2;
    for (uint32_t vfetch = first_vfetch; vfetch <= last_vfetch; ++vfetch) {
      vertex_buffers_in_sync_[vfetch >> 6] &= ~(uint64_t(1) << (vfetch & 63));
    }
    return;
  }

  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

//
// Draw target binding (FH1 native EDRAM surfaces).
//

bool D3D11CommandProcessor::BindDrawRenderTargets(
    bool rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, int32_t claim_x0, int32_t claim_y0, int32_t claim_x1,
    int32_t claim_y1, uint32_t& bound_depth_and_color_render_target_bits_out,
    uint32_t* bound_depth_and_color_render_target_formats_out) {
  bound_depth_and_color_render_target_bits_out = 0;
  if (!rasterization_done) {
    return true;
  }
  if (native_executor_) {
    // The executor derives the targets, claims the EDRAM tiles of the draw's
    // rectangle and binds the surfaces of the registry the resolve reads. The
    // claim is what decides whether a transfer is due: taking over tiles the
    // previous owner wrote moves their content first.
    Fh1DrawInfo draw;
    draw.rasterization_done = true;
    draw.normalized_depth_control = normalized_depth_control;
    draw.normalized_color_mask = normalized_color_mask;
    native_executor_->PrepareTargets(draw, claim_x0, claim_y0, claim_x1, claim_y1);
    const bool bound = native_executor_->BindTargets(
        bound_depth_and_color_render_target_bits_out,
        bound_depth_and_color_render_target_formats_out);
    // The same key record the render target path keeps, so the resolve trace's
    // rt_bound and the key log mean the same thing with the executor on.
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (!(native_executor_->pending_used_bits() & (1u << i))) {
        continue;
      }
      const Fh1SurfaceKey& key = native_executor_->pending_key(i);
      if (ever_bound_render_target_keys_.insert(key.Pack()).second &&
          REXCVAR_GET(fh1_swap_fetch_trace) && render_target_key_logs_ < 24) {
        ++render_target_key_logs_;
        REXGPU_INFO("FH1 draw RT key: {} (executor)", key.Describe());
      }
    }
    return bound;
  }
  const RegisterFile& regs = *register_file_;
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t msaa = uint32_t(surface_info.msaa_samples);
  const uint32_t pitch_tiles = Fh1PitchTiles(surface_info.surface_pitch, msaa);
  if (!pitch_tiles) {
    return true;
  }

  Fh1SurfaceKey keys[1 + xenos::kMaxColorRenderTargets] = {};
  uint32_t used_bits = 0;
  if (normalized_depth_control.z_enable || normalized_depth_control.stencil_enable) {
    const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    keys[0] = Fh1SurfaceKey::Depth(depth_info.depth_base, pitch_tiles, msaa,
                                   depth_info.depth_format);
    used_bits |= 1;
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(normalized_color_mask & (0xFu << (4 * i)))) {
      continue;
    }
    const auto color_info =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    keys[1 + i] = Fh1SurfaceKey::Color(color_info.color_base, pitch_tiles, msaa,
                                       color_info.color_format,
                                       host_render_config_->gamma_render_target_as_unorm16());
    used_bits |= 1u << (1 + i);
  }
  // The lower slot wins a shared base (mirrors the D3D12 executor).
  for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(used_bits & (1u << i))) {
      continue;
    }
    for (uint32_t j = 1; j < 1 + xenos::kMaxColorRenderTargets; ++j) {
      if (j != i && j > i && (used_bits & (1u << j)) && keys[j].base_tiles == keys[i].base_tiles) {
        used_bits &= ~(1u << j);
      }
    }
  }

  ID3D11RenderTargetView* rtvs[xenos::kMaxColorRenderTargets] = {};
  uint32_t rtv_count = 0;
  ID3D11DepthStencilView* dsv = nullptr;
  for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(used_bits & (1u << i))) {
      continue;
    }
    D3D11Fh1NativeSurface* surface = native_surfaces_->GetOrCreateSurface(keys[i]);
    if (!surface || !surface->rtv) {
      continue;
    }
    if (ever_bound_render_target_keys_.insert(keys[i].Pack()).second &&
        REXCVAR_GET(fh1_swap_fetch_trace) && render_target_key_logs_ < 24) {
      ++render_target_key_logs_;
      REXGPU_INFO("FH1 draw RT key: {} samples={} rtv=1", keys[i].Describe(), surface->samples);
    }
    while (rtv_count < i - 1) {
      rtvs[rtv_count++] = nullptr;
    }
    rtvs[rtv_count++] = surface->rtv;
    bound_depth_and_color_render_target_formats_out[i] = keys[i].format;
    bound_depth_and_color_render_target_bits_out |= 1u << i;
  }
  if (used_bits & 1) {
    D3D11Fh1NativeSurface* surface = native_surfaces_->GetOrCreateSurface(keys[0]);
    if (surface && surface->dsv) {
      dsv = surface->dsv;
      bound_depth_and_color_render_target_formats_out[0] = keys[0].format;
      bound_depth_and_color_render_target_bits_out |= 1;
    }
  }
  if (REXCVAR_GET(pinyon_draw_trace) || REXCVAR_GET(pinyon_audit)) {
    for (uint32_t trace_slot = 1; trace_slot < 4; ++trace_slot) {
      if (used_bits & (1u << trace_slot)) {
        if (REXCVAR_GET(pinyon_draw_trace)) {
          g_trace_rt_key = keys[trace_slot].Describe();
        }
        if (REXCVAR_GET(pinyon_audit)) {
          rex::graphics::render_audit::NoteDraw(keys[trace_slot].Describe().c_str(),
                                               uint32_t(keys[trace_slot].msaa),
                                               keys[trace_slot].format, 0, 0, 0);
        }
        break;
      }
    }
  }
  GetD3D11Provider().GetImmediateContext()->OMSetRenderTargets(
      rtv_count, rtv_count ? rtvs : nullptr, dsv);
  return true;
}

//
// Fixed-function state and bindings.
//

void D3D11CommandProcessor::UpdateFixedFunctionState(
    const draw_util::ViewportInfo& viewport_info, const draw_util::Scissor& scissor,
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control) {
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();

  if (REXCVAR_GET(pinyon_audit_passes)) {
    const auto& pass_window_offset = register_file_->Get<reg::PA_SC_WINDOW_OFFSET>();
    rex::graphics::render_audit::SetDrawGeo(
        int32_t(pass_window_offset.window_x_offset),
        int32_t(pass_window_offset.window_y_offset), float(viewport_info.xy_extent[0]),
        float(viewport_info.xy_extent[1]), float(scissor.offset[0]),
        float(scissor.offset[1]), float(scissor.extent[0]), float(scissor.extent[1]));
  }
  D3D11_VIEWPORT viewport;
  viewport.TopLeftX = float(viewport_info.xy_offset[0]);
  viewport.TopLeftY = float(viewport_info.xy_offset[1]);
  viewport.Width = float(viewport_info.xy_extent[0]);
  viewport.Height = float(viewport_info.xy_extent[1]);
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  const bool untile_tiling = native_executor_ && native_executor_->untile_height();
  if (untile_tiling) {
    // D3D11 keeps each pass's guest bin selection; unlike Vulkan's optional
    // select-all mode, the first pass does not extend its scissor over every
    // band.
    const auto& window_offset = register_file_->Get<reg::PA_SC_WINDOW_OFFSET>();
    const int32_t untile_rows = -int32_t(window_offset.window_y_offset);
    viewport.TopLeftY += float(untile_rows *
                               int32_t(texture_cache_->draw_resolution_scale_y()));
  }
  if (ff_viewport_update_needed_ ||
      std::memcmp(&viewport, &ff_viewport_, sizeof(D3D11_VIEWPORT)) != 0) {
    ff_viewport_ = viewport;
    ff_viewport_update_needed_ = false;
    context->RSSetViewports(1, &viewport);
  }

  if (REXCVAR_GET(pinyon_draw_trace)) {
    static uint32_t draw_trace_logs = 0;
    static float last_w = -1.0f, last_h = -1.0f;
    const float trace_w = float(viewport_info.xy_extent[0]);
    const float trace_h = float(viewport_info.xy_extent[1]);
    if (draw_trace_logs < 80 && (trace_w != last_w || trace_h != last_h)) {
      last_w = trace_w;
      last_h = trace_h;
      ++draw_trace_logs;
      std::FILE* trace_file = std::fopen("E:/PinyonShift/work/.local/draw-trace.log", "a");
      if (trace_file) {
        std::fprintf(trace_file,
                     "DRAW viewport=%.1fx%.1f+%.1f+%.1f z=%.3f..%.3f scissor=%ld,%ld %ldx%ld\n",
                     trace_w, trace_h, double(viewport_info.xy_offset[0]),
                     double(viewport_info.xy_offset[1]), double(viewport_info.z_min),
                     double(viewport_info.z_max), LONG(scissor.offset[0]), LONG(scissor.offset[1]),
                     LONG(scissor.extent[0]), LONG(scissor.extent[1]));
        const bool trace_clip_disabled =
            register_file_->Get<reg::PA_CL_CLIP_CNTL>().clip_disable != 0;
        std::fprintf(trace_file,
                     "NDC clip_disabled=%d scale=%.6f,%.6f,%.6f offset=%.6f,%.6f,%.6f"
                     " uploaded=%.6f,%.6f\n",
                     int(trace_clip_disabled), double(viewport_info.ndc_scale[0]),
                     double(viewport_info.ndc_scale[1]), double(viewport_info.ndc_scale[2]),
                     double(viewport_info.ndc_offset[0]), double(viewport_info.ndc_offset[1]),
                     double(viewport_info.ndc_offset[2]),
                     double(system_constants_.ndc_scale[0]),
                     double(system_constants_.ndc_scale[1]));
        std::fprintf(trace_file, "RT key=%s\n", g_trace_rt_key.c_str());
        std::fclose(trace_file);
      }
    }
  }

  D3D11_RECT scissor_rect;
  int32_t scissor_x = int32_t(scissor.offset[0]);
  int32_t scissor_y = int32_t(scissor.offset[1]);
  if (untile_tiling) {
    const auto& window_offset = register_file_->Get<reg::PA_SC_WINDOW_OFFSET>();
    scissor_y += -int32_t(window_offset.window_y_offset) *
                 int32_t(texture_cache_->draw_resolution_scale_y());
  }
  scissor_rect.left = LONG(scissor_x);
  scissor_rect.top = LONG(scissor_y);
  scissor_rect.right = LONG(scissor_x + int32_t(scissor.extent[0]));
  scissor_rect.bottom = LONG(scissor_y + int32_t(scissor.extent[1]));
  if (untile_tiling && REXCVAR_GET(d3d11_band_trace)) {
    const uint32_t current_untile_band = Fh1D3D11UntileBand(int32_t(
        register_file_->Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset));
    REXGPU_INFO("D3D11 band trace viewport: frame={} band={} viewport_y={} viewport_height={} "
                "scissor_top={} scissor_bottom={}",
                presented_output_frames_, current_untile_band, viewport.TopLeftY, viewport.Height,
                scissor_rect.top, scissor_rect.bottom);
  }
  if (ff_scissor_update_needed_ ||
      std::memcmp(&scissor_rect, &ff_scissor_, sizeof(D3D11_RECT)) != 0) {
    ff_scissor_ = scissor_rect;
    ff_scissor_update_needed_ = false;
    context->RSSetScissorRects(1, &scissor_rect);
  }

  const RegisterFile& regs = *register_file_;
  float blend_factor[] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
  };
  ff_blend_factor_update_needed_ |=
      std::memcmp(ff_blend_factor_, blend_factor, sizeof(float) * 4) != 0;
  if (ff_blend_factor_update_needed_) {
    std::memcpy(ff_blend_factor_, blend_factor, sizeof(float) * 4);
    // Null state keeps the currently bound blend state object.
    context->OMSetBlendState(nullptr, ff_blend_factor_, UINT32_MAX);
    ff_blend_factor_update_needed_ = false;
  }

  Register stencil_ref_mask_reg;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal && normalized_depth_control.backface_enable &&
      pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
    stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
  } else {
    stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
  }
  uint32_t stencil_ref = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg).stencilref;
  ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
  if (ff_stencil_ref_update_needed_) {
    ff_stencil_ref_ = stencil_ref;
    context->OMSetDepthStencilState(nullptr, ff_stencil_ref_);
    ff_stencil_ref_update_needed_ = false;
  }
}

void D3D11CommandProcessor::UpdateSystemConstantValues(
    bool shared_memory_is_uav, bool primitive_polygonal, uint32_t line_loop_closing_index,
    xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  (void)normalized_depth_control;
  (void)normalized_color_mask;
  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  uint32_t vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;

  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  reg::RB_COLOR_INFO color_infos[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  if (shared_memory_is_uav) {
    flags |= DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
  }
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  if (!host_render_config_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  float tessellation_factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  dirty |= system_constants_.line_loop_closing_index != line_loop_closing_index;
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  dirty |= system_constants_.vertex_index_offset != vgt_indx_offset;
  system_constants_.vertex_index_offset = vgt_indx_offset;

  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x = float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y = float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    float point_screen_diameter_to_ndc_radius_x =
        (float(draw_resolution_scale_x)) / std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (float(draw_resolution_scale_y)) / std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  uint32_t textures_resolution_scaled = 0;
  uint32_t textures_normalized_fixed_point = 0;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
    uint32_t texture_signs_shift = (texture_index & 3) * 8;
    uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
    uint32_t texture_signs_shifted = uint32_t(texture_signs) << texture_signs_shift;
    uint32_t texture_signs_mask = uint32_t(0b11111111) << texture_signs_shift;
    dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
    texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
    textures_resolution_scaled |=
        uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
    textures_normalized_fixed_point |=
        uint32_t(texture_cache_->IsActiveTextureNormalizedFixedPoint(texture_index))
        << texture_index;
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;
  dirty |= system_constants_.textures_normalized_fixed_point !=
           textures_normalized_fixed_point;
  system_constants_.textures_normalized_fixed_point = textures_normalized_fixed_point;

  uint32_t sample_count_log2_x = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  dirty |= system_constants_.sample_count_log2[0] != sample_count_log2_x;
  dirty |= system_constants_.sample_count_log2[1] != sample_count_log2_y;
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (1 << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  for (uint32_t i = 0; i < 4; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      if (!host_render_config_->IsFixed16TruncatedToMinus1To1()) {
        color_exp_bias -= 5;
      }
    }
    auto color_exp_bias_scale =
        rex::memory::Reinterpret<float>(int32_t(0x3F800000 + (color_exp_bias << 23)));
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
  }

  cbuffer_binding_system_.up_to_date &= !dirty;
}

//
// Constant buffers, textures, samplers.
//

bool D3D11CommandProcessor::UpdateConstantBuffer(ConstantBufferBinding& binding,
                                                 const void* data, uint32_t size_bytes) {
  const uint32_t aligned_size = (size_bytes + 15u) & ~15u;
  if (!binding.buffer || binding.size < aligned_size) {
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = aligned_size;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    if (FAILED(GetD3D11Provider().GetDevice()->CreateBuffer(&desc, nullptr, &buffer))) {
      REXGPU_ERROR("D3D11: failed to create a constant buffer of {} bytes", aligned_size);
      return false;
    }
    binding.buffer = std::move(buffer);
    binding.size = aligned_size;
    binding.up_to_date = false;
  }
  if (!binding.up_to_date) {
    // Phase 3 M3: keep the exact bytes of this update. The packet carries them
    // so replay can write the same contents before the draw; on the record path
    // the write itself is deferred, and if the draw falls back to the immediate
    // path FlushDeferredConstantBuffers writes them then.
    binding.cpu_bytes.assign(static_cast<const uint8_t*>(data),
                             static_cast<const uint8_t*>(data) + size_bytes);
    if (!recording_draw_) {
      GetD3D11Provider().GetImmediateContext()->UpdateSubresource(binding.buffer.Get(), 0,
                                                                  nullptr, data, 0, 0);
    }
    binding.up_to_date = true;
  }
  return true;
}

void D3D11CommandProcessor::FlushDeferredConstantBuffers() {
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  if (!context) {
    return;
  }
  ConstantBufferBinding* bindings[5] = {
      &cbuffer_binding_system_, &cbuffer_binding_float_vertex_, &cbuffer_binding_float_pixel_,
      &cbuffer_binding_bool_loop_, &cbuffer_binding_fetch_};
  for (ConstantBufferBinding* binding : bindings) {
    if (!binding->buffer || binding->cpu_bytes.empty()) {
      continue;
    }
    D3D11_BOX box = {0, 0, 0, UINT(binding->cpu_bytes.size()), 1, 1};
    context->UpdateSubresource(binding->buffer.Get(), 0, &box, binding->cpu_bytes.data(), 0, 0);
  }
}

namespace {
// Copies the float constants a shader uses, in register order, to `out`: each
// contiguous run of used registers is one copy (same gather as D3D12).
uint8_t* GatherFloatConstants(uint8_t* out, const uint64_t (&bitmap)[4], const uint32_t* first) {
  for (uint32_t word = 0; word < 4; ++word) {
    uint64_t remaining = bitmap[word];
    while (remaining) {
      const uint32_t start = uint32_t(std::countr_zero(remaining));
      const uint32_t length = uint32_t(std::countr_one(remaining >> start));
      const size_t bytes = size_t(length) * 4 * sizeof(float);
      std::memcpy(out, first + (word << 8) + (start << 2), bytes);
      out += bytes;
      remaining &= length == 64 ? 0 : ~(((uint64_t(1) << length) - 1) << start);
    }
  }
  return out;
}
}  // namespace

bool D3D11CommandProcessor::EnsureVertexBufferResidency(
    const Shader::ConstantRegisterMap& constant_map_vertex) {
  const RegisterFile& regs = *register_file_;
  for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      uint64_t vfetch_bit = uint64_t(1) << (vfetch_index & 63);
      if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
        continue;
      }
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          REXGPU_WARN(
              "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
              "(--gpu_allow_invalid_fetch_constants=true bypasses this)",
              vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
        default:
          REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
      }
      VertexBufferState& state = vertex_buffer_states_[vfetch_index];
      if (state.address == vfetch_constant.address && state.size == vfetch_constant.size) {
        vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
        continue;
      }
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        REXGPU_ERROR("Failed to request vertex buffer at 0x{:08X} (size {}) in the shared memory",
                     vfetch_constant.address << 2, vfetch_constant.size << 2);
        return false;
      }
      state.address = vfetch_constant.address;
      state.size = vfetch_constant.size;
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
    }
  }
  return true;
}

Microsoft::WRL::ComPtr<ID3D11SamplerState> D3D11CommandProcessor::GetSamplerState(
    const D3D11_SAMPLER_DESC& desc, uint64_t desc_hash) {
  for (const auto& entry : sampler_cache_) {
    if (entry.first == desc_hash) {
      return entry.second;
    }
  }
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
  if (FAILED(GetD3D11Provider().GetDevice()->CreateSamplerState(&desc, &sampler))) {
    REXGPU_ERROR("D3D11: failed to create a sampler state");
    return nullptr;
  }
  sampler_cache_.emplace_back(desc_hash, sampler);
  return sampler;
}

bool D3D11CommandProcessor::UpdateBindings(D3D11Shader* vertex_shader, D3D11Shader* pixel_shader) {
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  const RegisterFile& regs = *register_file_;

  // System constants (b0).
  if (!UpdateConstantBuffer(cbuffer_binding_system_, &system_constants_,
                            sizeof(system_constants_))) {
    return false;
  }
  // Float constants (b1): VS reads 0-255, PS reads 256-511 - separate
  // buffers bound per stage.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  const Shader::ConstantRegisterMap& float_constant_map_pixel =
      pixel_shader ? pixel_shader->constant_register_map() : Shader::ConstantRegisterMap();
  std::memcpy(current_float_constant_map_vertex_, float_constant_map_vertex.float_bitmap,
              sizeof(current_float_constant_map_vertex_));
  if (pixel_shader) {
    std::memcpy(current_float_constant_map_pixel_, float_constant_map_pixel.float_bitmap,
                sizeof(current_float_constant_map_pixel_));
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }
  uint8_t float_constants_vertex[sizeof(float) * 4 * 256];
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  if (!cbuffer_binding_float_vertex_.up_to_date ||
      cbuffer_binding_float_vertex_.size < float_constant_count_vertex * 16) {
    GatherFloatConstants(float_constants_vertex, float_constant_map_vertex.float_bitmap,
                         &regs[XE_GPU_REG_SHADER_CONSTANT_000_X]);
    if (!UpdateConstantBuffer(cbuffer_binding_float_vertex_, float_constants_vertex,
                              std::max(float_constant_count_vertex, uint32_t(1)) * 16)) {
      return false;
    }
  }
  uint8_t float_constants_pixel[sizeof(float) * 4 * 256];
  if (pixel_shader) {
    uint32_t float_constant_count_pixel = float_constant_map_pixel.float_count;
    if (!cbuffer_binding_float_pixel_.up_to_date ||
        cbuffer_binding_float_pixel_.size < float_constant_count_pixel * 16) {
      GatherFloatConstants(float_constants_pixel, float_constant_map_pixel.float_bitmap,
                           &regs[XE_GPU_REG_SHADER_CONSTANT_256_X]);
      if (!UpdateConstantBuffer(cbuffer_binding_float_pixel_, float_constants_pixel,
                                std::max(float_constant_count_pixel, uint32_t(1)) * 16)) {
        return false;
      }
    }
  } else if (!cbuffer_binding_float_pixel_.buffer) {
    // Always keep a valid binding for the stage.
    uint32_t zero = 0;
    if (!UpdateConstantBuffer(cbuffer_binding_float_pixel_, &zero, 16)) {
      return false;
    }
  }
  // Bool/loop (b2) and fetch (b3) constants - global, both stages.
  constexpr uint32_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
  if (!cbuffer_binding_bool_loop_.up_to_date) {
    if (!UpdateConstantBuffer(cbuffer_binding_bool_loop_,
                              &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                              kBoolLoopConstantsSize)) {
      return false;
    }
  }
  constexpr uint32_t kFetchConstantsSize = 32 * 6 * sizeof(uint32_t);
  if (!cbuffer_binding_fetch_.up_to_date) {
    if (!UpdateConstantBuffer(cbuffer_binding_fetch_,
                              &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                              kFetchConstantsSize)) {
      return false;
    }
  }

  ID3D11Buffer* vertex_cbs[4] = {
      cbuffer_binding_system_.buffer.Get(), cbuffer_binding_float_vertex_.buffer.Get(),
      cbuffer_binding_bool_loop_.buffer.Get(), cbuffer_binding_fetch_.buffer.Get()};
  ID3D11Buffer* pixel_cbs[4] = {
      cbuffer_binding_system_.buffer.Get(), cbuffer_binding_float_pixel_.buffer.Get(),
      cbuffer_binding_bool_loop_.buffer.Get(), cbuffer_binding_fetch_.buffer.Get()};
  context->VSSetConstantBuffers(0, 4, vertex_cbs);
  context->PSSetConstantBuffers(0, 4, pixel_cbs);

  // Shared memory mirror (t0) - in-shader vertex fetch + guest texture reads.
  auto* upload_sink =
      static_cast<D3D11SharedMemoryDeviceUploadSink*>(shared_memory_->upload_sink());
  ID3D11ShaderResourceView* shared_memory_srv = upload_sink->GetHostBufferSRV();
  context->VSSetShaderResources(0, 1, &shared_memory_srv);
  context->PSSetShaderResources(0, 1, &shared_memory_srv);

  // Textures (t1+) and samplers (s#) - bindful, per stage, straight from the
  // Phase 2 texture cache.
  auto bind_shader_inputs = [&](D3D11Shader* shader, bool vertex_stage) {
    if (!shader) {
      return;
    }
    const auto& texture_bindings = shader->GetTextureBindingsAfterTranslation();
    if (!texture_bindings.empty()) {
      std::vector<ID3D11ShaderResourceView*> srvs(texture_bindings.size());
      for (size_t i = 0; i < texture_bindings.size(); ++i) {
        srvs[i] = texture_cache_->GetActiveTextureBindfulSRV(texture_bindings[i]);
      }
      if (vertex_stage) {
        context->VSSetShaderResources(1, uint32_t(srvs.size()), srvs.data());
      } else {
        context->PSSetShaderResources(1, uint32_t(srvs.size()), srvs.data());
      }
    }
    const auto& sampler_bindings = shader->GetSamplerBindingsAfterTranslation();
    if (!sampler_bindings.empty()) {
      std::vector<ID3D11SamplerState*> samplers(sampler_bindings.size());
      for (size_t i = 0; i < sampler_bindings.size(); ++i) {
        const D3D11_SAMPLER_DESC desc = texture_cache_->GetSamplerDesc(sampler_bindings[i]);
        const uint64_t desc_hash = XXH3_64bits(&desc, sizeof(desc));
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler = GetSamplerState(desc, desc_hash);
        samplers[i] = sampler.Get();
      }
      if (vertex_stage) {
        context->VSSetSamplers(0, uint32_t(samplers.size()), samplers.data());
      } else {
        context->PSSetSamplers(0, uint32_t(samplers.size()), samplers.data());
      }
    }
  };
  bind_shader_inputs(vertex_shader, true);
  bind_shader_inputs(pixel_shader, false);
  return true;
}

//
// IssueDraw - the Phase 3 draw path.
//

bool D3D11CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  (void)primitive_type;
  (void)major_mode_explicit;
  // Phase 3 M3: a draw consumes the GPU mirror through in-shader fetches, and
  // its target preparation below (BindDrawRenderTargets -> the executor's
  // ownership transfers, clears and copies) is still immediate - those passes
  // are not on the tape yet. Both must observe every recorded packet before
  // they run, so the open tape is replayed here, before any of this draw's
  // reads or transfers. This is also the submission point for the previous
  // draw's packet: draws leave the tape in reverse order otherwise.
  if (command_lists_enabled_) {
    SubmitFrameTape("draw");
  }
  const RegisterFile& regs = *register_file_;
  const uint32_t current_untile_band =
      native_executor_ && native_executor_->untile_height()
          ? Fh1D3D11UntileBand(int32_t(regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset))
          : 0u;
  if (current_untile_band &&
      current_untile_band >= native_executor_->untile_height()) {
    REXGPU_ERROR("D3D11 FH1 draw: band {} exceeds untile height {}", current_untile_band,
                 native_executor_->untile_height());
    ++draw_counters_.dropped_other;
    return false;
  }
  const int32_t tile_trace_swap = REXCVAR_GET(fh1_debug_tile_trace_swap);
  const bool trace_tile = tile_trace_swap > 0 &&
                          presented_output_frames_ == uint64_t(tile_trace_swap);
  const bool trace_band2 = current_draw_band_trace_ && current_draw_tiling_pass_ == 1;
  auto trace_tile_path = [&](const char* stage) {
    if (!trace_tile && !trace_band2) return;
    const auto& window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    REXGPU_INFO(
        "{} swap={} stage={} pass={} issued={} window_y={} mode={} "
        "select={:016X} mask={:016X}",
        trace_band2 ? "FH1 band2 D3D11" : "FH1 tile path",
        presented_output_frames_, stage, current_draw_tiling_pass_, draw_counters_.issued,
        int32_t(window_offset.window_y_offset), regs[XE_GPU_REG_RB_MODECONTROL] & 7,
        current_draw_bin_select_, current_draw_bin_mask_);
  };
  trace_tile_path("issue_enter");
  // fh1_debug_texture_dump (PHASE 10): set when this draw is the post pass, so
  // the frame's surface is dumped again once the draw has run - the difference
  // between the two dumps is exactly what the pass wrote, which no pre-draw
  // dump of a target shared with the composite can show.
  bool dump_c2_after_draw = false;
  uint64_t dump_c2_after_key = 0;
  char dump_c2_after_tag[64] = {};

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  // FH1 routing probe (stage C: IssueDraw entry). One line per draw reaching
  // the backend's IssueDraw at the tile-trace swap, with the guest primitive
  // type the tile trace does not record.
  if (trace_tile) {
    const auto routing_color0 =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
    auto routing_vs = static_cast<D3D11Shader*>(active_vertex_shader());
    auto routing_ps = static_cast<D3D11Shader*>(active_pixel_shader());
    REXGPU_INFO(
        "FH1 routing issue: swap={} prim={} idx={} mode={} pass={} winy={} c0={:08X}:f{} "
        "surf={:08X} vs={:016X} ps={:016X}",
        presented_output_frames_, uint32_t(primitive_type), index_count,
        uint32_t(edram_mode), current_draw_tiling_pass_,
        int32_t(regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset),
        uint32_t(routing_color0.color_base), uint32_t(routing_color0.color_format),
        regs[XE_GPU_REG_RB_SURFACE_INFO],
        routing_vs ? routing_vs->ucode_data_hash() : 0,
        routing_ps ? routing_ps->ucode_data_hash() : 0);
  }
  if (edram_mode == xenos::EdramMode::kCopy) {
    trace_tile_path("copy_mode");
    return IssueCopy();
  }

  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  auto vertex_shader = static_cast<D3D11Shader*>(active_vertex_shader());
  if (!vertex_shader) {
    trace_tile_path("no_vertex_shader");
    ++draw_counters_.dropped_other;
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    trace_tile_path("zero_surface_pitch");
    return true;
  }
  D3D11Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<D3D11Shader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    if (!memexport_used_vertex) {
      trace_tile_path("no_rasterization_no_memexport");
      return true;
    }
  }
  bool memexport_used_pixel = pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  if (!memexport_used && !normalized_depth_control.z_enable &&
      !normalized_depth_control.stencil_enable && !normalized_color_mask) {
    // No guest-visible output.
    trace_tile_path("no_guest_visible_output");
    return true;
  }
  if (memexport_used) {
    // Memexport needs a UAV-bound shared memory; feature level 11_0 has no
    // vertex-shader UAVs and the FH1 executor route (Step C) owns transfers.
    trace_tile_path("memexport_not_supported");
    ++draw_counters_.dropped_memexport;
    return true;
  }

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_->Process(primitive_processing_result)) {
    trace_tile_path("primitive_processor_failed");
    ++draw_counters_.dropped_other;
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    trace_tile_path("zero_host_vertex_count");
    return true;
  }
  if (primitive_processing_result.IsTessellated()) {
    // Hull/domain translations are not part of Phase 3.
    trace_tile_path("tessellation_not_supported");
    ++draw_counters_.dropped_tessellation;
    return true;
  }

  // Shader modifications.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;
  DxbcShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask);
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader
          ? pipeline_cache_->GetCurrentPixelShaderModification(
                *pixel_shader, interpolator_mask, ps_param_gen_pos, normalized_depth_control)
          : DxbcShaderTranslator::Modification(0);

  // Viewport and scissor are derived before the targets are bound: the
  // executor's claim, and the ownership transfers it triggers, follow the
  // rectangle the draw can actually write (viewport intersected with scissor)
  // rather than the whole scissor, which the guest usually keeps at the full
  // render target size.
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();
  bool convert_z_to_float24 = host_render_config_->depth_float24_convert_in_pixel_shader();
  bool ps_writes_depth = pixel_shader && pixel_shader->writes_depth();

  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(regs, draw_resolution_scale_x, draw_resolution_scale_y, true,
                                 32767, 32767, false, normalized_depth_control,
                                 convert_z_to_float24,
                                 /* full_float24_in_0_to_1 */ true, ps_writes_depth,
                                 viewport_info);

  // fh1_debug_hide_hud (diagnostic only): skip HUD/2D overlay draws while
  // keeping 3D world draws. The HUD renders to its own 1280-wide surfaces
  // with small viewports (320x192, 160x96, 80x48); the world halves use a
  // 1280-wide viewport and the post pass 1280x720. A color draw whose
  // viewport is narrower than the full frame is a HUD draw. Depth-only
  // passes (shadow maps) are not gated (normalized_color_mask == 0).
  if (REXCVAR_GET(fh1_debug_hide_hud) && normalized_color_mask != 0 &&
      viewport_info.xy_extent[0] < 1280u * draw_resolution_scale_x) {
    if (trace_tile) {
      REXGPU_INFO("FH1 routing drop: swap={} stage=hide_hud prim={} mode={} pass={}",
                  presented_output_frames_, uint32_t(primitive_type),
                  uint32_t(edram_mode), current_draw_tiling_pass_);
    }
    ++draw_counters_.dropped_other;
    return true;
  }

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;

  // What the draw can write, in host (here guest, scale 1) pixels.
  int32_t claim_x0 = std::max(int32_t(viewport_info.xy_offset[0]), int32_t(scissor.offset[0]));
  int32_t claim_y0 = std::max(int32_t(viewport_info.xy_offset[1]), int32_t(scissor.offset[1]));
  int32_t claim_x1 = std::min(int32_t(viewport_info.xy_offset[0] + viewport_info.xy_extent[0]),
                              int32_t(scissor.offset[0] + scissor.extent[0]));
  int32_t claim_y1 = std::min(int32_t(viewport_info.xy_offset[1] + viewport_info.xy_extent[1]),
                              int32_t(scissor.offset[1] + scissor.extent[1]));

  // Bounded FH1 tile provenance: one configured presented frame only.
  if (trace_tile) {
    const auto& window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    const auto& mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    const auto& scissor_tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
    REXGPU_INFO(
        "FH1 tile draw: swap={} resolve_count={} draw={} window_y={} "
        "vtx_window_offset_enable={} window_offset_disable={} "
        "viewport_y={} viewport_h={} scissor_y={} scissor_h={} claim_y={}..{}",
        presented_output_frames_, resolve_call_count_, draw_counters_.issued,
        int32_t(window_offset.window_y_offset),
        mode_cntl.vtx_window_offset_enable, scissor_tl.window_offset_disable,
        viewport_info.xy_offset[1], viewport_info.xy_extent[1], scissor.offset[1],
        scissor.extent[1], claim_y0, claim_y1);
  }

  // Bind the native render targets this draw writes.
  uint32_t bound_depth_and_color_render_target_bits = 0;
  uint32_t bound_depth_and_color_render_target_formats[1 + xenos::kMaxColorRenderTargets] = {};
  const bool render_targets_bound = BindDrawRenderTargets(
      is_rasterization_done, normalized_depth_control, normalized_color_mask, claim_x0, claim_y0,
      claim_x1, claim_y1, bound_depth_and_color_render_target_bits,
      bound_depth_and_color_render_target_formats);
  if (!render_targets_bound) {
    if (trace_tile) {
      REXGPU_INFO("FH1 routing drop: swap={} stage=dropped_targets prim={} mode={} pass={}",
                  presented_output_frames_, uint32_t(primitive_type),
                  uint32_t(edram_mode), current_draw_tiling_pass_);
    }
    ++draw_counters_.dropped_targets;
    return false;
  }
  // FH1 routing probe (stage E: render-target selection). Log the bound
  // targets for draws at the tile-trace swap, so the report can say which
  // surface each triangle draw actually targets.
  if (trace_tile) {
    Fh1SurfaceKey routing_c0;
    Fh1SurfaceKey routing_d;
    if (native_executor_ && (native_executor_->pending_used_bits() & (1u << 1))) {
      routing_c0 = native_executor_->pending_key(1);
    } else {
      const auto routing_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
      const uint32_t routing_msaa = uint32_t(routing_surface_info.msaa_samples);
      const auto routing_color_info =
          regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
      routing_c0 = Fh1SurfaceKey::Color(
          routing_color_info.color_base,
          Fh1PitchTiles(routing_surface_info.surface_pitch, routing_msaa), routing_msaa,
          routing_color_info.color_format,
          host_render_config_->gamma_render_target_as_unorm16());
    }
    if (native_executor_ && (native_executor_->pending_used_bits() & (1u << 0))) {
      routing_d = native_executor_->pending_key(0);
    } else if (normalized_depth_control.z_enable ||
               normalized_depth_control.stencil_enable) {
      const auto routing_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
      const uint32_t routing_msaa = uint32_t(routing_surface_info.msaa_samples);
      const auto routing_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
      routing_d = Fh1SurfaceKey::Depth(
          routing_depth_info.depth_base,
          Fh1PitchTiles(routing_surface_info.surface_pitch, routing_msaa), routing_msaa,
          routing_depth_info.depth_format);
    }
    REXGPU_INFO(
        "FH1 routing bound: swap={} prim={} mode={} pass={} bits={:02X} c0={} d={}",
        presented_output_frames_, uint32_t(primitive_type), uint32_t(edram_mode),
        current_draw_tiling_pass_, bound_depth_and_color_render_target_bits,
        routing_c0.Describe(), routing_d.Describe());
  }

  // Obtain shader metadata and configure/bind the pipeline state.
  D3D11Shader::D3D11Translation* vertex_shader_translation =
      static_cast<D3D11Shader::D3D11Translation*>(
          vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
  D3D11Shader::D3D11Translation* pixel_shader_translation =
      pixel_shader ? static_cast<D3D11Shader::D3D11Translation*>(
                         pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                   : nullptr;
  D3D11PipelineCache::D3D11Pipeline* pipeline = pipeline_cache_->ConfigurePipeline(
      vertex_shader_translation, pixel_shader_translation, primitive_processing_result,
      normalized_depth_control, normalized_color_mask, bound_depth_and_color_render_target_bits,
      bound_depth_and_color_render_target_formats);
  if (!pipeline) {
    if (trace_tile) {
      REXGPU_INFO("FH1 routing drop: swap={} stage={} prim={} mode={} pass={}",
                  presented_output_frames_,
                  !vertex_shader_translation->is_valid() ? "dropped_shader"
                                                         : "dropped_pipeline_state",
                  uint32_t(primitive_type), uint32_t(edram_mode),
                  current_draw_tiling_pass_);
    }
    if (!vertex_shader_translation->is_valid()) {
      ++draw_counters_.dropped_shader;
    } else {
      ++draw_counters_.dropped_pipeline_state;
    }
    return false;
  }

  // FH1 call logger (Phase 2): shader translation outcome (joins to the
  // guest shader load by vs/ps hash).
  if (Fh1CallogEnabled(presented_output_frames_)) {
    REXGPU_INFO(
        "FH1 callog side=A swap={} kind=shader vs={:016X} ps={:016X} vvalid={} "
        "pvalid={} pipeline={:016X}",
        presented_output_frames_,
        vertex_shader ? vertex_shader->ucode_data_hash() : 0,
        pixel_shader ? pixel_shader->ucode_data_hash() : 0,
        vertex_shader_translation->is_valid() ? 1 : 0,
        pixel_shader_translation
            ? (pixel_shader_translation->is_valid() ? 1 : 0)
            : -1,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pipeline)));
  }

  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  // Phase 3 M3: the per-stage slots this draw binds. t0 is the guest-memory
  // mirror (in-shader fetch) and is bound for both stages; the fetch bindings
  // follow it. A draw that needs more than the packet holds stays immediate.
  const uint32_t vertex_srv_count =
      1 + uint32_t(vertex_shader->GetTextureBindingsAfterTranslation().size());
  const uint32_t pixel_srv_count =
      1 + (pixel_shader ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size())
                        : 0u);
  const uint32_t vertex_sampler_count =
      uint32_t(vertex_shader->GetSamplerBindingsAfterTranslation().size());
  const uint32_t pixel_sampler_count =
      pixel_shader ? uint32_t(pixel_shader->GetSamplerBindingsAfterTranslation().size()) : 0u;
  // A draw whose bindings do not fit the packet is not recordable at all; it
  // must keep its texture uploads immediate too, or an immediate draw would
  // sample textures whose uploads are still packets behind it.
  const bool draw_recordable =
      command_lists_enabled_ && vertex_srv_count <= kMaxDrawTextureSrvs &&
      pixel_srv_count <= kMaxDrawTextureSrvs && vertex_sampler_count <= kMaxDrawSamplers &&
      pixel_sampler_count <= kMaxDrawSamplers;
  // The uploads this request triggers become UploadTexture packets on the tape
  // (the cache keeps its CPU untiling at record time).
  recording_texture_uploads_ = draw_recordable;
  texture_cache_->RequestTextures(used_texture_mask);
  recording_texture_uploads_ = false;

  // fh1_debug_composite_draws: which draws write the surface family the front
  // buffer is composed into, and which guest textures they sample - the
  // composite's own record, next to the surfaces the 3D pass left behind.
  // (After RequestTextures, so a texture's load - and any resolve refresh it
  // needed - has already happened for this draw.)
  {
    const int64_t composite_trace_from = REXCVAR_GET(fh1_debug_composite_draws);
    if (composite_trace_from > 0 &&
        presented_output_frames_ >= uint64_t(composite_trace_from)) {
      static uint32_t composite_draw_logs = 0;
      Fh1SurfaceKey color0_key;
      if (native_executor_ && (native_executor_->pending_used_bits() & (1u << 1))) {
        color0_key = native_executor_->pending_key(1);
      } else {
        const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
        const uint32_t msaa = uint32_t(surface_info.msaa_samples);
        const auto color_info =
            regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
        color0_key = Fh1SurfaceKey::Color(color_info.color_base,
                                          Fh1PitchTiles(surface_info.surface_pitch, msaa), msaa,
                                          color_info.color_format,
                                          host_render_config_->gamma_render_target_as_unorm16());
      }
      bool same_family = color0_key.base_tiles == 0 && color0_key.pitch_tiles == 16 &&
                         color0_key.msaa == uint32_t(xenos::MsaaSamples::k1X);
      // fh1_debug_composite_world_only: only the draws that sample a frame-sized
      // texture - the 3D scene's own draws and the post passes after them - so a
      // 400-line budget covers the world frame instead of the fills before it.
      bool samples_frame_texture = false;
      {
        uint32_t scan_remaining = used_texture_mask;
        uint32_t scan_index;
        while (rex::bit_scan_forward(scan_remaining, &scan_index)) {
          scan_remaining &= ~(uint32_t(1) << scan_index);
          const xenos::xe_gpu_texture_fetch_t scan_fetch = regs.GetTextureFetch(scan_index);
          if (uint32_t(scan_fetch.size_2d.width) + 1 >= 640 &&
              uint32_t(scan_fetch.size_2d.height) + 1 >= 360) {
            samples_frame_texture = true;
            break;
          }
        }
      }
      if (REXCVAR_GET(fh1_debug_composite_world_only) && !samples_frame_texture) {
        same_family = false;
      }
      // fh1_debug_composite_dump: the target as the frame is being built - the
      // state left by the previous draws, before this one runs.
      const std::string composite_dump_dir = REXCVAR_GET(fh1_debug_composite_dump);
      // Only the surface the front buffer is resolved from - the other family
      // members are separate alias textures whose content is not presented.
      if (same_family && !composite_dump_dir.empty() &&
          color0_key.Pack() == last_resolve_source_key_.Pack()) {
        static uint32_t composite_dumps = 0;
        if (composite_dumps < 16) {
          D3D11Fh1NativeSurface* dump_surface = native_surfaces_->FindSurface(color0_key.Pack());
          if (dump_surface) {
            SurfaceDumpStats dump_stats;
            std::string dump_error;
            char dump_name[96];
            std::snprintf(dump_name, sizeof(dump_name), "%s/c2-before-%02u.ppm",
                          composite_dump_dir.c_str(), composite_dumps);
            const bool dump_written =
                DumpSurfacePpm(GetD3D11Provider().GetDevice(),
                               GetD3D11Provider().GetImmediateContext(), dump_surface, dump_name,
                               dump_stats, dump_error);
            ++composite_dumps;
            REXGPU_INFO(
                "FH1 composite dump: swap={} before draw#{} key={} {} [{}] nonzero={} "
                "colours={}",
                presented_output_frames_, composite_draw_logs + 1, color0_key.Describe(),
                dump_name, dump_written ? "written" : dump_error.c_str(),
                dump_stats.total ? double(dump_stats.nonzero) / double(dump_stats.total) : 0.0,
                dump_stats.distinct);
          }
        }
      }
      // fh1_debug_clear_draw: with the world dump, clear the front-buffer target
      // to magenta before every other dumped draw, so each odd dump shows only
      // the pixels the single draw before it wrote - a draw's real footprint.
      const bool clear_before_dump = REXCVAR_GET(fh1_debug_clear_draw) != 0;
      // fh1_debug_composite_dump_world: the same dump, armed by the first draw
      // that samples a world-sized texture instead of by draw number, so the
      // sequence starts at the 3D scene's draw and shows which pass paints
      // which band of the presented frame.
      const std::string world_dump_dir = REXCVAR_GET(fh1_debug_composite_dump_world);
      if (same_family && !world_dump_dir.empty() &&
          color0_key.Pack() == last_resolve_source_key_.Pack()) {
        static bool world_dump_armed = false;
        static uint32_t world_dumps = 0;
        if (!world_dump_armed) {
          uint32_t scan_remaining = used_texture_mask;
          uint32_t scan_index;
          while (rex::bit_scan_forward(scan_remaining, &scan_index)) {
            scan_remaining &= ~(uint32_t(1) << scan_index);
            const xenos::xe_gpu_texture_fetch_t scan_fetch = regs.GetTextureFetch(scan_index);
            if (uint32_t(scan_fetch.size_2d.width) + 1 >= 640 &&
                uint32_t(scan_fetch.size_2d.height) + 1 >= 360) {
              world_dump_armed = true;
              break;
            }
          }
        }
        if (world_dump_armed && world_dumps < 12) {
          D3D11Fh1NativeSurface* dump_surface = native_surfaces_->FindSurface(color0_key.Pack());
          if (dump_surface && clear_before_dump && (world_dumps % 2) == 0 &&
              dump_surface->rtv) {
            ID3D11DeviceContext* clear_context = GetD3D11Provider().GetImmediateContext();
            if (clear_context) {
              const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
              clear_context->ClearRenderTargetView(dump_surface->rtv, magenta);
            }
          }
          if (dump_surface) {
            SurfaceDumpStats dump_stats;
            std::string dump_error;
            char dump_name[96];
            std::snprintf(dump_name, sizeof(dump_name), "%s/world-before-%02u.ppm",
                          world_dump_dir.c_str(), world_dumps);
            const bool dump_written =
                DumpSurfacePpm(GetD3D11Provider().GetDevice(),
                               GetD3D11Provider().GetImmediateContext(), dump_surface, dump_name,
                               dump_stats, dump_error);
            ++world_dumps;
            REXGPU_INFO(
                "FH1 composite world dump: swap={} before draw#{} vp={},{} {}x{} [{}] "
                "nonzero={} colours={}",
                presented_output_frames_, composite_draw_logs + 1, viewport_info.xy_offset[0],
                viewport_info.xy_offset[1], viewport_info.xy_extent[0], viewport_info.xy_extent[1],
                dump_written ? "written" : dump_error.c_str(),
                dump_stats.total ? double(dump_stats.nonzero) / double(dump_stats.total) : 0.0,
                dump_stats.distinct);
          }
        }
      }
      // fh1_debug_texture_dump (PHASE 10): the composite leg's three points in
      // one swap - the sampled scene at the world draw, the composed surface
      // before the post pass, and what the post pass itself samples. Gated on
      // the sampled textures being in the cache, which is what separates the
      // world phase from the menus without naming a frame number.
      const std::string texture_dump_dir = REXCVAR_GET(fh1_debug_texture_dump);
      // Both halves, and every one of them: a swap has more than one prim-8
      // 1280x360 draw, and the first is not the scene's (it samples one
      // 640x360 texture, the later one the 1280x720 scene and its layers). So
      // the world point is only a draw that samples the full-resolution scene
      // target - otherwise the loading screen's halves spend the dump budget.
      bool samples_scene_target = false;
      {
        uint32_t scene_scan_remaining = used_texture_mask;
        uint32_t scene_scan_index;
        while (rex::bit_scan_forward(scene_scan_remaining, &scene_scan_index)) {
          scene_scan_remaining &= ~(uint32_t(1) << scene_scan_index);
          const xenos::xe_gpu_texture_fetch_t scene_fetch = regs.GetTextureFetch(scene_scan_index);
          if (uint32_t(scene_fetch.size_2d.width) + 1 >= 1280 &&
              uint32_t(scene_fetch.size_2d.height) + 1 >= 720) {
            samples_scene_target = true;
            break;
          }
        }
      }
      const bool dump_world_half =
          samples_scene_target &&
          primitive_processing_result.guest_primitive_type ==
              xenos::PrimitiveType::kRectangleList &&
          viewport_info.xy_offset[0] == 0 && viewport_info.xy_extent[0] == 1280 &&
          viewport_info.xy_extent[1] == 360;
      const bool dump_post_pass =
          samples_scene_target &&
          primitive_processing_result.guest_primitive_type == xenos::PrimitiveType::kTriangleList &&
          viewport_info.xy_offset[0] == 0 && viewport_info.xy_offset[1] == 0 &&
          viewport_info.xy_extent[0] == 1280 && viewport_info.xy_extent[1] == 720;
      if (!texture_dump_dir.empty() && samples_frame_texture &&
          (dump_world_half || dump_post_pass)) {
        // Phase 3 M3: the draw's texture uploads are packets on the open tape
        // (RequestTextures ran above); replay them before the dump reads the
        // host textures, or the dump would show their previous contents.
        DrainTapeForDiagnostic("texture dump");
        // One budget per point: the world draws would otherwise spend the
        // whole of it before the post pass of any swap is reached.
        static uint32_t world_texture_dumps = 0;
        static uint32_t post_texture_dumps = 0;
        static uint64_t world_dump_swap = UINT64_MAX;
        static uint64_t post_dump_swap = UINT64_MAX;
        // The two world halves of a swap sample the same textures (one dump);
        // the post pass is dumped only in a swap whose world half was dumped,
        // so every point comes from the same frame.
        const bool dump_this_draw =
            dump_world_half
                ? true
                : world_dump_swap == presented_output_frames_ &&
                      post_dump_swap != presented_output_frames_;
        // Which of this draw's fetches the cache actually holds: the menus'
        // frame-sized fetches have no host texture, the world's do. For the
        // post pass a format-mismatched hit is kept and marked - a fetch the
        // cache holds only under another format is a different texture from
        // the one the shader names, and the frame's black areas would follow.
        uint32_t dump_indices[8];
        bool dump_any_format[8] = {};
        uint32_t dump_count = 0;
        if (dump_this_draw && (dump_post_pass ? post_texture_dumps : world_texture_dumps) < 300) {
          uint32_t scan_remaining = used_texture_mask;
          uint32_t scan_index;
          while (rex::bit_scan_forward(scan_remaining, &scan_index) && dump_count < 8) {
            scan_remaining &= ~(uint32_t(1) << scan_index);
            const xenos::xe_gpu_texture_fetch_t scan_fetch = regs.GetTextureFetch(scan_index);
            if (uint32_t(scan_fetch.size_2d.width) + 1 < 320 ||
                uint32_t(scan_fetch.size_2d.height) + 1 < 192) {
              continue;
            }
            const uint32_t scan_base = uint32_t(scan_fetch.base_address) << 12;
            DXGI_FORMAT scan_format = DXGI_FORMAT_UNKNOWN;
            uint32_t scan_width = 0;
            uint32_t scan_height = 0;
            if (texture_cache_->FindTexture2DForGuestBase(scan_base, scan_fetch.format, scan_format,
                                                          scan_width, scan_height)) {
              dump_indices[dump_count++] = scan_index;
              continue;
            }
            if (!dump_post_pass) {
              continue;
            }
            uint32_t host_format = 0;
            if (texture_cache_->FindAnyTexture2DForGuestBase(scan_base, scan_format, scan_width,
                                                             scan_height, host_format)) {
              REXGPU_INFO(
                  "FH1 texture dump: swap={} t{} base={:08X} fetch_fmt={} - the cache holds this "
                  "base only as fmt={} ({}x{}), not under the fetch's format",
                  presented_output_frames_, scan_index, scan_base, uint32_t(scan_fetch.format),
                  host_format, scan_width, scan_height);
              dump_indices[dump_count] = scan_index;
              dump_any_format[dump_count] = true;
              ++dump_count;
            } else {
              REXGPU_INFO(
                  "FH1 texture dump: swap={} t{} base={:08X} fetch_fmt={} - base not held at all",
                  presented_output_frames_, scan_index, scan_base,
                  uint32_t(scan_fetch.format));
            }
          }
        }
        // The post pass of a swap is dumped only if that swap's world half was
        // reached, so all three points come from one frame; the world half is
        // marked even once its dump budget is spent, so the post pass keeps
        // being measurable after it.
        if (dump_world_half) {
          world_dump_swap = presented_output_frames_;
          // The composite's own footprint: the same surface before and after
          // one world draw (a half of the frame), so tiling and aliasing cancel
          // out of the difference.
          if (world_texture_dumps < 300) {
            D3D11Fh1NativeSurface* before_surface = native_surfaces_->FindSurface(color0_key.Pack());
            if (before_surface) {
              std::snprintf(dump_c2_after_tag, sizeof(dump_c2_after_tag), "c2-world-%llu-draw%u",
                            static_cast<unsigned long long>(presented_output_frames_),
                            composite_draw_logs + 1);
              dump_c2_after_draw = true;
              dump_c2_after_key = color0_key.Pack();
              const std::string before_name =
                  texture_dump_dir + "/before-" + dump_c2_after_tag + ".ppm";
              SurfaceDumpStats before_stats;
              std::string before_error;
              DumpSurfacePpm(GetD3D11Provider().GetDevice(),
                             GetD3D11Provider().GetImmediateContext(), before_surface, before_name,
                             before_stats, before_error);
            }
          }
        }
        if (dump_post_pass && dump_this_draw) {
          post_dump_swap = presented_output_frames_;
        }
        for (uint32_t dump_i = 0; dump_i < dump_count; ++dump_i) {
          const uint32_t texture_index = dump_indices[dump_i];
          const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(texture_index);
          const uint32_t base_address = uint32_t(fetch.base_address) << 12;
          DXGI_FORMAT read_format = DXGI_FORMAT_UNKNOWN;
          uint32_t texture_width = 0;
          uint32_t texture_height = 0;
          uint32_t any_host_format = 0;
          ID3D11Texture2D* sampled_texture =
              dump_any_format[dump_i]
                  ? texture_cache_->FindAnyTexture2DForGuestBase(
                        base_address, read_format, texture_width, texture_height, any_host_format)
                  : texture_cache_->FindTexture2DForGuestBase(
                        base_address, fetch.format, read_format, texture_width, texture_height);
          if (!sampled_texture) {
            continue;
          }
          SurfaceDumpStats dump_stats;
          std::string dump_error;
          char dump_name[192];
          std::snprintf(dump_name, sizeof(dump_name), "%s/%s-%llu-t%u.ppm",
                        texture_dump_dir.c_str(), dump_post_pass ? "post" : "world",
                        static_cast<unsigned long long>(presented_output_frames_), texture_index);
          const bool dump_written = DumpTexture2DPpm(
              GetD3D11Provider().GetDevice(), GetD3D11Provider().GetImmediateContext(),
              sampled_texture, read_format, dump_name, dump_stats, dump_error);
          ++(dump_post_pass ? post_texture_dumps : world_texture_dumps);
          REXGPU_INFO(
              "FH1 texture dump: swap={} draw#{} stage={} prim={} vp={},{} {}x{} t{} "
              "base={:08X} guest={}x{} fmt={} tiled={} host={}x{} srv_fmt={} held_fmt={} [{}] {} "
              "nonzero={} mean={:.4f},{:.4f},{:.4f} colours={}",
              presented_output_frames_, composite_draw_logs + 1,
              dump_post_pass ? "post" : "world",
              uint32_t(primitive_processing_result.guest_primitive_type),
              viewport_info.xy_offset[0], viewport_info.xy_offset[1], viewport_info.xy_extent[0],
              viewport_info.xy_extent[1], texture_index, base_address,
              uint32_t(fetch.size_2d.width) + 1, uint32_t(fetch.size_2d.height) + 1,
              uint32_t(fetch.format), uint32_t(fetch.tiled), texture_width, texture_height,
              uint32_t(read_format),
              dump_any_format[dump_i] ? any_host_format : uint32_t(fetch.format), dump_name,
              dump_written ? "written" : dump_error.c_str(),
              dump_stats.total ? double(dump_stats.nonzero) / double(dump_stats.total) : 0.0,
              dump_stats.mean[0], dump_stats.mean[1], dump_stats.mean[2], dump_stats.distinct);
        }
        if (dump_post_pass && dump_this_draw) {
          // Point (i): the front-buffer surface the post pass is about to read,
          // after both world halves have composed into it.
          D3D11Fh1NativeSurface* composed_surface = native_surfaces_->FindSurface(color0_key.Pack());
          if (composed_surface) {
            SurfaceDumpStats dump_stats;
            std::string dump_error;
            char dump_name[192];
            std::snprintf(dump_name, sizeof(dump_name), "%s/c2-after-world-%llu.ppm",
                          texture_dump_dir.c_str(),
                          static_cast<unsigned long long>(presented_output_frames_));
            const bool dump_written = DumpSurfacePpm(
                GetD3D11Provider().GetDevice(), GetD3D11Provider().GetImmediateContext(),
                composed_surface, dump_name, dump_stats, dump_error);
            REXGPU_INFO(
                "FH1 surface dump: swap={} draw#{} stage=c2-after-world [{}] {} nonzero={} "
                "mean={:.4f},{:.4f},{:.4f} colours={}",
                presented_output_frames_, composite_draw_logs + 1, dump_name,
                dump_written ? "written" : dump_error.c_str(),
                dump_stats.total ? double(dump_stats.nonzero) / double(dump_stats.total) : 0.0,
                dump_stats.mean[0], dump_stats.mean[1], dump_stats.mean[2], dump_stats.distinct);
            std::snprintf(dump_c2_after_tag, sizeof(dump_c2_after_tag), "c2-after-post-%llu",
                          static_cast<unsigned long long>(presented_output_frames_));
            dump_c2_after_draw = true;
            dump_c2_after_key = color0_key.Pack();
          }
        }
      }
      // fh1_debug_guest_probe (PHASE 12): the bytes the texture cache untiles,
      // at the draw that samples them. Guest memory and the GPU mirror are
      // written side by side for every sampled texture 320x192 or larger, with
      // the fetch's own base, size, format, tiling and endian bits - so a wrong
      // image can be traced to the bytes themselves (a producer that packed
      // them differently) instead of to the SRV the cache built from them.
      // RequestTextures above has already refreshed these ranges, so guest
      // memory here is exactly what this draw's uploads read.
      const std::string guest_probe_dir = REXCVAR_GET(fh1_debug_guest_probe);
      if (!guest_probe_dir.empty() && same_family && dump_world_half) {
        static uint32_t guest_probe_swaps = 0;
        static uint64_t guest_probe_last_swap = UINT64_MAX;
        if (guest_probe_swaps < 4 && guest_probe_last_swap != presented_output_frames_) {
          guest_probe_last_swap = presented_output_frames_;
          ++guest_probe_swaps;
          WaitForGpu();
          auto* upload_sink =
              static_cast<D3D11SharedMemoryDeviceUploadSink*>(shared_memory_->upload_sink());
          uint32_t probe_scan = used_texture_mask;
          uint32_t probe_index;
          while (rex::bit_scan_forward(probe_scan, &probe_index)) {
            probe_scan &= ~(uint32_t(1) << probe_index);
            const xenos::xe_gpu_texture_fetch_t probe_fetch = regs.GetTextureFetch(probe_index);
            const uint32_t probe_width = uint32_t(probe_fetch.size_2d.width) + 1;
            const uint32_t probe_height = uint32_t(probe_fetch.size_2d.height) + 1;
            if (probe_width < 320 || probe_height < 192) {
              continue;
            }
            const uint32_t probe_base = uint32_t(probe_fetch.base_address) << 12;
            const FormatInfo* probe_info = FormatInfo::Get(probe_fetch.format);
            const uint32_t probe_bpp = probe_info ? std::max(probe_info->bits_per_pixel / 8, 1u) : 4u;
            const uint32_t probe_bytes = std::min<uint32_t>(
                uint32_t(uint64_t(probe_width) * probe_height * probe_bpp), 4u << 20);
            if (probe_base >= SharedMemory::kBufferSize || !probe_bytes) {
              continue;
            }
            std::vector<uint8_t> probe_guest(probe_bytes, 0);
            const uint8_t* probe_mapped =
                reinterpret_cast<const uint8_t*>(memory_->TranslatePhysical(probe_base));
            if (probe_mapped) {
              std::memcpy(probe_guest.data(), probe_mapped, probe_bytes);
            }
            std::vector<uint8_t> probe_mirror(probe_bytes, 0);
            const bool probe_have_mirror =
                upload_sink &&
                upload_sink->ReadbackFromHostBuffer(probe_base, probe_mirror.data(), probe_bytes);
            char probe_name[64];
            std::snprintf(probe_name, sizeof(probe_name), "%08X-%llu", probe_base,
                          static_cast<unsigned long long>(presented_output_frames_));
            {
              std::ofstream bin(guest_probe_dir + "/guest-" + probe_name + ".bin",
                                std::ios::binary | std::ios::trunc);
              bin.write(reinterpret_cast<const char*>(probe_guest.data()),
                        std::streamsize(probe_bytes));
            }
            if (probe_have_mirror) {
              std::ofstream bin(guest_probe_dir + "/mirror-" + probe_name + ".bin",
                                std::ios::binary | std::ios::trunc);
              bin.write(reinterpret_cast<const char*>(probe_mirror.data()),
                        std::streamsize(probe_bytes));
            }
            auto probe_word_stats = [](const std::vector<uint8_t>& bytes) {
              uint64_t nonzero = 0;
              std::set<uint32_t> sampled;
              for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
                const uint32_t word = *reinterpret_cast<const uint32_t*>(bytes.data() + i);
                nonzero += word != 0;
                if (!(i & 255)) {
                  sampled.insert(word);
                }
              }
              return std::make_pair(nonzero, sampled.size());
            };
            const auto probe_guest_stats = probe_word_stats(probe_guest);
            const auto probe_mirror_stats = probe_word_stats(probe_mirror);
            const bool probe_same =
                probe_have_mirror && std::memcmp(probe_guest.data(), probe_mirror.data(),
                                                 probe_bytes) == 0;
            REXGPU_INFO(
                "FH1 guest probe: swap={} t{} base={:08X} fetch={}x{} fmt={} tiled={} endian={} "
                "swizzle={:08X} mip_max={} dim={} bytes={} guest_nz={}/{} distinct={} "
                "mirror={} same={} first={:08X}",
                presented_output_frames_, probe_index, probe_base, probe_width, probe_height,
                uint32_t(probe_fetch.format), uint32_t(probe_fetch.tiled),
                uint32_t(probe_fetch.endianness), uint32_t(probe_fetch.swizzle),
                uint32_t(probe_fetch.mip_max_level), uint32_t(probe_fetch.dimension), probe_bytes,
                probe_guest_stats.first, probe_bytes / 4, probe_guest_stats.second,
                probe_have_mirror ? 1 : 0, probe_same ? 1 : 0,
                *reinterpret_cast<const uint32_t*>(probe_guest.data()));
          }
        }
        // The EDRAM surfaces behind those samples: every front-buffer-family
        // surface (base tiles 0, pitch 16) at the same draw, so the resolve's
        // source can be compared with the bytes it wrote into memory. The
        // resolve reads the same host texture the rasteriser wrote and
        // re-encodes it, so a difference is the resolve's, not the cache's.
        if (native_surfaces_) {
          uint32_t surface_index = 0;
          for (const auto& surface_entry : native_surfaces_->surfaces()) {
            D3D11Fh1NativeSurface* surface = surface_entry.second.get();
            if (!surface || !surface->texture || surface->key.is_depth) {
              continue;
            }
            if (surface->key.base_tiles != 0 || surface->key.pitch_tiles != 16 ||
                surface->width < 640) {
              continue;
            }
            if (surface_index >= 12) {
              break;
            }
            char surface_name[192];
            std::snprintf(surface_name, sizeof(surface_name), "%s/%s-%llu.ppm",
                          guest_probe_dir.c_str(), SanitizedKeyName(surface->key).c_str(),
                          static_cast<unsigned long long>(presented_output_frames_));
            SurfaceDumpStats surface_stats;
            std::string surface_error;
            const bool surface_written =
                DumpSurfacePpm(GetD3D11Provider().GetDevice(),
                               GetD3D11Provider().GetImmediateContext(), surface, surface_name,
                               surface_stats, surface_error);
            ++surface_index;
            REXGPU_INFO(
                "FH1 surface probe: swap={} key={} {}x{} fmt={} [{}] {} nonzero={} "
                "mean={:.4f},{:.4f},{:.4f} colours={}",
                presented_output_frames_, surface->key.Describe(), surface->width,
                surface->height, uint32_t(surface->srv_format),
                surface_written ? surface_name : surface_error.c_str(),
                surface_written ? "written" : "error",
                surface_stats.total
                    ? double(surface_stats.nonzero) / double(surface_stats.total)
                    : 0.0,
                surface_stats.mean[0], surface_stats.mean[1], surface_stats.mean[2],
                surface_stats.distinct);
          }
        }
      }
      // fh1_debug_vertex_dump: the guest vertices of the first N world draws.
      // A fetch constant names only a buffer - the vertex the shader reads is at
      // base + stride * index + attribute offset, with the index being the
      // draw's own (auto-generated or DMA) plus VGT_INDX_OFFSET. Reading the
      // constant's base alone (the earlier attempt) logs vertex 0 of the buffer
      // rather than of the draw, which is why it showed nothing.
      // fh1_debug_post_clip: the post pass's positions, all of them (48 for its
      // triangle list), mapped through the draw's own ndc transform - the
      // geometry behind the region the pass actually rasterizes.
      const bool post_clip_probe = REXCVAR_GET(fh1_debug_post_clip) > 0;
      if ((REXCVAR_GET(fh1_debug_vertex_dump) > 0 || post_clip_probe) && same_family &&
          samples_frame_texture && (!post_clip_probe || dump_post_pass)) {
        static uint32_t vertex_dumps = 0;
        const uint32_t vertex_dump_budget =
            post_clip_probe ? uint32_t(REXCVAR_GET(fh1_debug_post_clip))
                            : uint32_t(REXCVAR_GET(fh1_debug_vertex_dump));
        // The post-clip probe shares the c2 probe's window, so both describe the
        // same frames instead of the menus' first post pass.
        const uint64_t clip_probe_from =
            post_clip_probe
                ? uint64_t(std::max<int32_t>(REXCVAR_GET(fh1_debug_c2_probe_from), 0))
                : 0u;
        if (vertex_dumps < vertex_dump_budget && presented_output_frames_ >= clip_probe_from) {
          ++vertex_dumps;
          const auto draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
          const uint32_t indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
          const uint32_t max_vertices = post_clip_probe ? 48u : 3u;
          const uint32_t guest_vertex_count =
              std::min(primitive_processing_result.guest_draw_vertex_count, max_vertices);
          // The index the vertex shader receives is (host index + base vertex
          // index) masked to 24 bits - the same for auto and DMA draws.
          uint32_t guest_indices[48] = {};
          for (uint32_t i = 0; i < guest_vertex_count; ++i) {
            guest_indices[i] = (i + indx_offset) & xenos::kVertexIndexMask;
          }
          const bool dma_indices =
              draw_initiator.source_select == xenos::SourceSelect::kDMA && index_buffer_info &&
              index_buffer_info->guest_base != 0;
          if (dma_indices) {
            const bool index_32bit = index_buffer_info->format == xenos::IndexFormat::kInt32;
            const size_t index_size = index_32bit ? sizeof(uint32_t) : sizeof(uint16_t);
            for (uint32_t i = 0; i < guest_vertex_count; ++i) {
              uint8_t index_bytes[4] = {};
              if (!shared_memory_->CopyCpuRange(
                      index_buffer_info->guest_base + uint32_t(i * index_size),
                      std::span<uint8_t>(index_bytes, index_size))) {
                continue;
              }
              // The DMA buffer is read with the draw's own index endianness (the
              // translated VS applies the same swap) - without it a 16-bit
              // k8in16 buffer reads as 0x0400 instead of the index 4 it holds.
              uint32_t host_index;
              if (index_32bit) {
                std::memcpy(&host_index, index_bytes, sizeof(host_index));
                host_index = xenos::GpuSwap(host_index, index_buffer_info->endianness);
              } else {
                uint16_t host_index_16;
                std::memcpy(&host_index_16, index_bytes, sizeof(host_index_16));
                host_index = xenos::GpuSwap(host_index_16, index_buffer_info->endianness);
              }
              guest_indices[i] = (host_index + indx_offset) & xenos::kVertexIndexMask;
            }
          }
          REXGPU_INFO(
              "FH1 vertex dump: swap={} draw#{} prim={} vertices={} source={} index_offset={} "
              "indices={},{},{}",
              presented_output_frames_, composite_draw_logs + 1,
              uint32_t(primitive_processing_result.guest_primitive_type), guest_vertex_count,
              dma_indices ? "dma" : "auto", indx_offset, guest_indices[0], guest_indices[1],
              guest_indices[2]);
              float position_px_min[2] = {1e30f, 1e30f};
          float position_px_max[2] = {-1e30f, -1e30f};
          uint32_t position_count = 0;
          for (const Shader::VertexBinding& binding : vertex_shader->vertex_bindings()) {
            const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(binding.fetch_constant);
            if (vf.type != xenos::FetchConstantType::kVertex) {
              continue;
            }
            const uint32_t base_address = uint32_t(vf.address) << 2;
            for (const Shader::VertexBinding::Attribute& attribute : binding.attributes) {
              const ParsedVertexFetchInstruction& fetch_instr = attribute.fetch_instr;
              const xenos::VertexFormat data_format = fetch_instr.attributes.data_format;
              if (data_format == xenos::VertexFormat::kUndefined) {
                continue;
              }
              const int component_count = xenos::GetVertexFormatComponentCount(data_format);
              const bool is_float = data_format == xenos::VertexFormat::k_32_FLOAT ||
                                    data_format == xenos::VertexFormat::k_32_32_FLOAT ||
                                    data_format == xenos::VertexFormat::k_32_32_32_FLOAT ||
                                    data_format == xenos::VertexFormat::k_32_32_32_32_FLOAT;
              // The attribute the fetch writes to r0.xyzw is the position the
              // rest of the shader transforms - report it in the draw's own
              // viewport pixels, next to its raw values.
              const bool is_position =
                  fetch_instr.result.storage_target == InstructionStorageTarget::kRegister &&
                  fetch_instr.result.storage_index == 0;
              if (post_clip_probe && !is_position) {
                continue;
              }
              for (uint32_t v = 0; v < guest_vertex_count; ++v) {
                const int64_t address_dwords = int64_t(vf.address) +
                                               int64_t(binding.stride_words) * guest_indices[v] +
                                               int64_t(fetch_instr.attributes.offset);
                uint32_t data[4] = {};
                const bool read = address_dwords >= 0 &&
                                  shared_memory_->CopyCpuRange(
                                      uint32_t(address_dwords) * 4,
                                      std::span<uint8_t>(reinterpret_cast<uint8_t*>(data),
                                                         size_t(component_count) * 4));
                std::string values;
                if (!read) {
                  values = "unavailable";
                } else {
                  for (int c = 0; c < 4; ++c) {
                    data[c] = c < component_count ? xenos::GpuSwap(data[c], vf.endian) : 0;
                  }
                  if (is_float) {
                    float floats[4] = {};
                    for (int c = 0; c < 4; ++c) {
                      std::memcpy(&floats[c], &data[c], sizeof(float));
                    }
                    values = fmt::format("({:.6g},{:.6g},{:.6g},{:.6g})", floats[0], floats[1],
                                         floats[2], floats[3]);
                    if (is_position) {
                      const float w = component_count >= 4 && floats[3] != 0.0f ? floats[3] : 1.0f;
                      const float ndc_x = floats[0] / w;
                      const float ndc_y = floats[1] / w;
                      const float px = (ndc_x * viewport_info.ndc_scale[0] +
                                        viewport_info.ndc_offset[0] + 1.0f) *
                                           0.5f * float(viewport_info.xy_extent[0]) +
                                       float(viewport_info.xy_offset[0]);
                      const float py = (ndc_y * viewport_info.ndc_scale[1] +
                                        viewport_info.ndc_offset[1] + 1.0f) *
                                           0.5f * float(viewport_info.xy_extent[1]) +
                                       float(viewport_info.xy_offset[1]);
                      values += fmt::format(" ndc=({:.4f},{:.4f}) px=({:.1f},{:.1f})", ndc_x, ndc_y,
                                            px, py);
                      position_px_min[0] = std::min(position_px_min[0], px);
                      position_px_min[1] = std::min(position_px_min[1], py);
                      position_px_max[0] = std::max(position_px_max[0], px);
                      position_px_max[1] = std::max(position_px_max[1], py);
                      ++position_count;
                    }
                  } else {
                    for (int c = 0; c < component_count; ++c) {
                      values += fmt::format("{:08X} ", data[c]);
                    }
                  }
                }
                REXGPU_INFO(
                    "FH1 vertex dump:   slot={} base={:08X} size={} stride={} fmt={} off={} "
                    "pos={} v{} index={} addr={:08X} {}",
                    binding.fetch_constant, base_address, uint32_t(vf.size) << 2,
                    binding.stride_words * 4, uint32_t(data_format),
                    fetch_instr.attributes.offset, is_position ? 1 : 0, v, guest_indices[v],
                    uint32_t(address_dwords) * 4, values);
              }
              if (post_clip_probe && is_position && position_count) {
                REXGPU_INFO(
                    "FH1 post clip: swap={} draw#{} stage={} positions={} px_x=[{:.1f},{:.1f}] "
                    "px_y=[{:.1f},{:.1f}] vp={},{} {}x{} ndc_scale={},{},{} ndc_offset={},{},{}",
                    presented_output_frames_, composite_draw_logs + 1,
                    dump_post_pass ? "post" : "other", position_count, position_px_min[0],
                    position_px_max[0], position_px_min[1], position_px_max[1],
                    viewport_info.xy_offset[0], viewport_info.xy_offset[1],
                    viewport_info.xy_extent[0], viewport_info.xy_extent[1],
                    viewport_info.ndc_scale[0], viewport_info.ndc_scale[1],
                    viewport_info.ndc_scale[2], viewport_info.ndc_offset[0],
                    viewport_info.ndc_offset[1], viewport_info.ndc_offset[2]);
              }
            }
          }
        }
      }

      // FH1 footprint-vs probe (draw #23 vertex data): the guest vertex
      // bytes for this textureless draw. The existing vertex dump is gated
      // on samples_frame_texture, which is false for #23, so this block has
      // its own predicate (PS hash at swap 1500) and reads all 48 vertices.
      // Purely diagnostic: guest-memory reads + log lines only.
      if (!g_draw23_vs_vdump_done && pixel_shader &&
          pixel_shader->ucode_data_hash() == 0xA39E23E2FD1F8A8BULL) {
        g_draw23_vs_vdump_done = true;
        const auto d23_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
        const uint32_t d23_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
        const uint32_t d23_guest_vertex_count =
            std::min(primitive_processing_result.guest_draw_vertex_count, 48u);
        uint32_t d23_guest_indices[48] = {};
        for (uint32_t i = 0; i < d23_guest_vertex_count; ++i) {
          d23_guest_indices[i] = (i + d23_indx_offset) & xenos::kVertexIndexMask;
        }
        const bool d23_dma_indices =
            d23_draw_initiator.source_select == xenos::SourceSelect::kDMA && index_buffer_info &&
            index_buffer_info->guest_base != 0;
        if (d23_dma_indices) {
          const bool d23_index_32bit = index_buffer_info->format == xenos::IndexFormat::kInt32;
          const size_t d23_index_size = d23_index_32bit ? sizeof(uint32_t) : sizeof(uint16_t);
          for (uint32_t i = 0; i < d23_guest_vertex_count; ++i) {
            uint8_t d23_index_bytes[4] = {};
            if (!shared_memory_->CopyCpuRange(
                    index_buffer_info->guest_base + uint32_t(i * d23_index_size),
                    std::span<uint8_t>(d23_index_bytes, d23_index_size))) {
              continue;
            }
            uint32_t d23_host_index;
            if (d23_index_32bit) {
              std::memcpy(&d23_host_index, d23_index_bytes, sizeof(d23_host_index));
              d23_host_index = xenos::GpuSwap(d23_host_index, index_buffer_info->endianness);
            } else {
              uint16_t d23_host_index_16;
              std::memcpy(&d23_host_index_16, d23_index_bytes, sizeof(d23_host_index_16));
              d23_host_index = xenos::GpuSwap(d23_host_index_16, index_buffer_info->endianness);
            }
            d23_guest_indices[i] = (d23_host_index + d23_indx_offset) & xenos::kVertexIndexMask;
          }
        }
        REXGPU_INFO(
            "FH1 draw23 vertex dump: swap={} draw#{} prim={} vertices={} source={} "
            "index_offset={} indices={},{},{}",
            presented_output_frames_, composite_draw_logs + 1,
            uint32_t(primitive_processing_result.guest_primitive_type),
            d23_guest_vertex_count, d23_dma_indices ? "dma" : "auto", d23_indx_offset,
            d23_guest_indices[0], d23_guest_indices[1], d23_guest_indices[2]);
        float d23_position_px_min[2] = {1e30f, 1e30f};
        float d23_position_px_max[2] = {-1e30f, -1e30f};
        uint32_t d23_position_count = 0;
        for (const Shader::VertexBinding& d23_binding : vertex_shader->vertex_bindings()) {
          const xenos::xe_gpu_vertex_fetch_t d23_vf =
              regs.GetVertexFetch(d23_binding.fetch_constant);
          if (d23_vf.type != xenos::FetchConstantType::kVertex) {
            continue;
          }
          const uint32_t d23_base_address = uint32_t(d23_vf.address) << 2;
          for (const Shader::VertexBinding::Attribute& d23_attribute :
               d23_binding.attributes) {
            const ParsedVertexFetchInstruction& d23_fetch_instr = d23_attribute.fetch_instr;
            const xenos::VertexFormat d23_data_format = d23_fetch_instr.attributes.data_format;
            if (d23_data_format == xenos::VertexFormat::kUndefined) {
              continue;
            }
            const int d23_component_count =
                xenos::GetVertexFormatComponentCount(d23_data_format);
            const bool d23_is_float = d23_data_format == xenos::VertexFormat::k_32_FLOAT ||
                                      d23_data_format == xenos::VertexFormat::k_32_32_FLOAT ||
                                      d23_data_format == xenos::VertexFormat::k_32_32_32_FLOAT ||
                                      d23_data_format == xenos::VertexFormat::k_32_32_32_32_FLOAT;
            const bool d23_is_position =
                d23_fetch_instr.result.storage_target == InstructionStorageTarget::kRegister &&
                d23_fetch_instr.result.storage_index == 0;
            for (uint32_t v = 0; v < d23_guest_vertex_count; ++v) {
              const int64_t d23_address_dwords =
                  int64_t(d23_vf.address) +
                  int64_t(d23_binding.stride_words) * d23_guest_indices[v] +
                  int64_t(d23_fetch_instr.attributes.offset);
              uint32_t d23_data[4] = {};
              const bool d23_read =
                  d23_address_dwords >= 0 &&
                  shared_memory_->CopyCpuRange(
                      uint32_t(d23_address_dwords) * 4,
                      std::span<uint8_t>(reinterpret_cast<uint8_t*>(d23_data),
                                         size_t(d23_component_count) * 4));
              std::string d23_values;
              if (!d23_read) {
                d23_values = "unavailable";
              } else {
                for (int c = 0; c < 4; ++c) {
                  d23_data[c] =
                      c < d23_component_count ? xenos::GpuSwap(d23_data[c], d23_vf.endian) : 0;
                }
                if (d23_is_float) {
                  float d23_floats[4] = {};
                  for (int c = 0; c < 4; ++c) {
                    std::memcpy(&d23_floats[c], &d23_data[c], sizeof(float));
                  }
                  d23_values = fmt::format("({:.6g},{:.6g},{:.6g},{:.6g})", d23_floats[0],
                                           d23_floats[1], d23_floats[2], d23_floats[3]);
                  if (d23_is_position) {
                    const float d23_w =
                        d23_component_count >= 4 && d23_floats[3] != 0.0f ? d23_floats[3] : 1.0f;
                    const float d23_ndc_x = d23_floats[0] / d23_w;
                    const float d23_ndc_y = d23_floats[1] / d23_w;
                    const float d23_px = (d23_ndc_x * viewport_info.ndc_scale[0] +
                                          viewport_info.ndc_offset[0] + 1.0f) *
                                             0.5f * float(viewport_info.xy_extent[0]) +
                                         float(viewport_info.xy_offset[0]);
                    const float d23_py = (d23_ndc_y * viewport_info.ndc_scale[1] +
                                          viewport_info.ndc_offset[1] + 1.0f) *
                                             0.5f * float(viewport_info.xy_extent[1]) +
                                         float(viewport_info.xy_offset[1]);
                    d23_values += fmt::format(" ndc=({:.4f},{:.4f}) px=({:.1f},{:.1f})",
                                              d23_ndc_x, d23_ndc_y, d23_px, d23_py);
                    d23_position_px_min[0] = std::min(d23_position_px_min[0], d23_px);
                    d23_position_px_min[1] = std::min(d23_position_px_min[1], d23_py);
                    d23_position_px_max[0] = std::max(d23_position_px_max[0], d23_px);
                    d23_position_px_max[1] = std::max(d23_position_px_max[1], d23_py);
                    ++d23_position_count;
                  }
                } else {
                  for (int c = 0; c < d23_component_count; ++c) {
                    d23_values += fmt::format("{:08X} ", d23_data[c]);
                  }
                }
              }
              REXGPU_INFO(
                  "FH1 draw23 vertex:   slot={} base={:08X} size={} stride={} fmt={} off={} "
                  "pos={} v{} index={} addr={:08X} {}",
                  d23_binding.fetch_constant, d23_base_address, uint32_t(d23_vf.size) << 2,
                  d23_binding.stride_words * 4, uint32_t(d23_data_format),
                  d23_fetch_instr.attributes.offset, d23_is_position ? 1 : 0, v,
                  d23_guest_indices[v], uint32_t(d23_address_dwords) * 4, d23_values);
            }
          }
        }
        if (d23_position_count) {
          REXGPU_INFO(
              "FH1 draw23 vertex positions: swap={} draw#{} positions={} "
              "px_x=[{:.1f},{:.1f}] px_y=[{:.1f},{:.1f}] vp={},{} {}x{}",
              presented_output_frames_, composite_draw_logs + 1, d23_position_count,
              d23_position_px_min[0], d23_position_px_max[0], d23_position_px_min[1],
              d23_position_px_max[1], viewport_info.xy_offset[0], viewport_info.xy_offset[1],
              viewport_info.xy_extent[0], viewport_info.xy_extent[1]);
        }
      }
      // The log has to survive the menus to reach the world phase (400 draws
      // were spent before it in one run, leaving the world draws unnamed).
      if ((composite_draw_logs < 4000 && same_family) ||
          (!g_draw23_vs_vsdump_done && pixel_shader &&
           pixel_shader->ucode_data_hash() == 0xA39E23E2FD1F8A8BULL)) {
        ++composite_draw_logs;
        std::string textures;
        uint32_t texture_remaining = used_texture_mask;
        uint32_t texture_index;
        while (rex::bit_scan_forward(texture_remaining, &texture_index)) {
          texture_remaining &= ~(uint32_t(1) << texture_index);
          const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(texture_index);
          // ... and the fetch's own endian and component swizzle: the guest's
          // sampler applies both, so a layer that looks channel-shifted is
          // decided here, not in the arithmetic the draw does with it.
          textures += fmt::format(
              " t{}={:08X}:{}x{}:f{}:t{}:e{}:s{:03X}", texture_index,
              uint32_t(fetch.base_address) << 12, uint32_t(fetch.size_2d.width) + 1,
              uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.format), uint32_t(fetch.tiled),
              uint32_t(fetch.endianness), uint32_t(fetch.swizzle));
        }
        REXGPU_INFO(
            "FH1 composite draw: swap={} draw#{} key={} vp={},{} {}x{} scissor={},{} {}x{} "
            "prim={} verts={}/{} shader={} ndc=scale {},{} offset {},{}",
            presented_output_frames_, composite_draw_logs, color0_key.Describe(),
            viewport_info.xy_offset[0], viewport_info.xy_offset[1], viewport_info.xy_extent[0],
            viewport_info.xy_extent[1], scissor.offset[0], scissor.offset[1], scissor.extent[0],
            scissor.extent[1], uint32_t(primitive_processing_result.guest_primitive_type),
            primitive_processing_result.guest_draw_vertex_count,
            primitive_processing_result.host_draw_vertex_count,
            int(primitive_processing_result.host_vertex_shader_type),
            viewport_info.ndc_scale[0], viewport_info.ndc_scale[1], viewport_info.ndc_offset[0],
            viewport_info.ndc_offset[1]);
        REXGPU_INFO("FH1 composite draw:   textures={} {}", used_texture_mask == 0 ? "none" : "",
                    textures);
        // FH1 follow-up probe (Q1): which guest state does the backend
        // misinterpret for draw #23? Log, for each same-family draw at swap
        // 1500, the pixel shader's identity (ucode hash), the pixel shader's
        // float constants as the guest set them, the blend state (RB_BLENDCONTROL
        // for target 0, blend-factor constants, RB_COLORCONTROL, alpha ref),
        // the raw colour mask, the normalised colour mask, and every bound
        // colour target. Purely diagnostic: register reads + log lines only.
        {
          static uint32_t composite_state_logs = 0;
          if ((presented_output_frames_ == 1500 && composite_state_logs < 40) ||
              (!g_draw23_vs_vsdump_done && pixel_shader &&
               pixel_shader->ucode_data_hash() == 0xA39E23E2FD1F8A8BULL)) {
            ++composite_state_logs;
            const uint64_t ps_hash =
                pixel_shader ? pixel_shader->ucode_data_hash() : 0;
            const uint64_t vs_hash =
                vertex_shader ? vertex_shader->ucode_data_hash() : 0;
            std::string ps_consts;
            uint32_t ps_float_count = 0;
            uint32_t ps_consts_logged = 0;
            if (pixel_shader) {
              const Shader::ConstantRegisterMap& ps_map =
                  pixel_shader->constant_register_map();
              ps_float_count = ps_map.float_count;
              const uint32_t* ps_first = &regs[XE_GPU_REG_SHADER_CONSTANT_256_X];
              for (uint32_t r = 0; r < 256 && ps_consts_logged < 16; ++r) {
                if (!(ps_map.float_bitmap[r >> 6] & (uint64_t(1) << (r & 63)))) {
                  continue;
                }
                float c[4];
                std::memcpy(c, ps_first + r * 4, sizeof(c));
                ps_consts += fmt::format(" c{}=({:.6g},{:.6g},{:.6g},{:.6g})", r, c[0],
                                         c[1], c[2], c[3]);
                ++ps_consts_logged;
              }
              if (ps_consts.empty()) {
                ps_consts = " (no float constants used)";
              }
            } else {
              ps_consts = " (no pixel shader)";
            }
            const auto blend0 =
                regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
            const auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
            const uint32_t raw_color_mask = regs.Get<reg::RB_COLOR_MASK>().value;
            std::string rt_slots;
            if (native_executor_) {
              for (uint32_t slot = 1; slot < 1 + xenos::kMaxColorRenderTargets; ++slot) {
                if (!(native_executor_->pending_used_bits() & (1u << slot))) {
                  continue;
                }
                rt_slots += fmt::format(
                    " c{}={}:m{:X}", slot - 1, native_executor_->pending_key(slot).Describe(),
                    (normalized_color_mask >> (4 * (slot - 1))) & 0xF);
              }
            }
            REXGPU_INFO(
                "FH1 composite state: swap={} draw#{} ps_hash={:016X} vs_hash={:016X} "
                "ps_floats={} ps_consts={} blend0=src{} op{} dst{} srcA{} opA{} dstA{} "
                "blend_factor=({:.6g},{:.6g},{:.6g},{:.6g}) colorcontrol={:08X} "
                "alpha_test={} alpha_func={} alpha_ref={:.6g} raw_mask={:08X} "
                "norm_mask={:08X} used_tex_mask={:08X} rt={}",
                presented_output_frames_, composite_draw_logs, ps_hash, vs_hash,
                ps_float_count, ps_consts, uint32_t(blend0.color_srcblend),
                uint32_t(blend0.color_comb_fcn), uint32_t(blend0.color_destblend),
                uint32_t(blend0.alpha_srcblend), uint32_t(blend0.alpha_comb_fcn),
                uint32_t(blend0.alpha_destblend),
                regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
                regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
                regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA), colorcontrol.value,
                uint32_t(colorcontrol.alpha_test_enable), uint32_t(colorcontrol.alpha_func),
                regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF), raw_color_mask,
                normalized_color_mask, used_texture_mask, rt_slots);
          // FH1 next-evidence probe (Goal 2): draw #23's shader and the
          // actually-applied blend state. The guest blend registers and the
          // mapping table are already known; this reads back the
          // D3D11_BLEND_DESC the pipeline bound for this draw, and dumps the
          // guest ucode disassembly plus the translated DXBC (and its runtime
          // disassembly) for draws #23 and #24, so the two shaders can be
          // compared. Purely diagnostic: reads + file writes + log lines.
          if (composite_draw_logs >= 23 && composite_draw_logs <= 25) {
            if (pipeline && pipeline->blend_state) {
              D3D11_BLEND_DESC applied_blend = {};
              pipeline->blend_state->GetDesc(&applied_blend);
              const D3D11_RENDER_TARGET_BLEND_DESC& ab = applied_blend.RenderTarget[0];
              REXGPU_INFO(
                  "FH1 draw23 applied blend: swap={} draw#{} ps_hash={:016X} "
                  "blend_enable={} src={} dst={} op={} srcA={} dstA={} opA={} "
                  "write_mask={:X} indep={} a2c={}",
                  presented_output_frames_, composite_draw_logs, ps_hash,
                  int(ab.BlendEnable), int(ab.SrcBlend), int(ab.DestBlend), int(ab.BlendOp),
                  int(ab.SrcBlendAlpha), int(ab.DestBlendAlpha), int(ab.BlendOpAlpha),
                  uint32_t(ab.RenderTargetWriteMask),
                  int(applied_blend.IndependentBlendEnable),
                  int(applied_blend.AlphaToCoverageEnable));
            } else {
              REXGPU_INFO(
                  "FH1 draw23 applied blend: swap={} draw#{} ps_hash={:016X} no_blend_state",
                  presented_output_frames_, composite_draw_logs, ps_hash);
            }
          }
          if ((composite_draw_logs == 23 || composite_draw_logs == 24) && pixel_shader) {
            const std::string shader_dump_dir = REXCVAR_GET(fh1_debug_composite_dump);
            if (!shader_dump_dir.empty()) {
              const std::string& guest_dis = pixel_shader->ucode_disassembly();
              uint32_t guest_dis_lines = 0;
              for (char ch : guest_dis) {
                if (ch == '\n') {
                  ++guest_dis_lines;
                }
              }
              char guest_path[256];
              std::snprintf(guest_path, sizeof(guest_path), "%s/draw%u-ps-guest-%016llX.txt",
                            shader_dump_dir.c_str(), composite_draw_logs,
                            (unsigned long long)ps_hash);
              {
                std::ofstream gf(guest_path, std::ios::binary);
                if (gf) {
                  gf << guest_dis;
                }
              }
              const std::vector<uint8_t>* dxbc_bytes = nullptr;
              if (pixel_shader_translation) {
                dxbc_bytes = &pixel_shader_translation->translated_binary();
              }
              std::string dxbc_path;
              if (dxbc_bytes && !dxbc_bytes->empty()) {
                char dxbc_name[256];
                std::snprintf(dxbc_name, sizeof(dxbc_name), "%s/draw%u-ps-%016llX.dxbc",
                              shader_dump_dir.c_str(), composite_draw_logs,
                              (unsigned long long)ps_hash);
                {
                  std::ofstream df(dxbc_name, std::ios::binary);
                  if (df) {
                    df.write(reinterpret_cast<const char*>(dxbc_bytes->data()),
                             std::streamsize(dxbc_bytes->size()));
                  }
                }
                dxbc_path = dxbc_name;
              }
              std::string dxbc_dis_path;
              bool dxbc_dis_ok = false;
              if (dxbc_bytes && !dxbc_bytes->empty()) {
                using D3DDisassembleFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR,
                                                          ID3DBlob**);
                static HMODULE d3dcompiler = nullptr;
                static D3DDisassembleFn d3d_disassemble = nullptr;
                static bool d3dcompiler_tried = false;
                if (!d3dcompiler_tried) {
                  d3dcompiler_tried = true;
                  d3dcompiler = ::LoadLibraryA("d3dcompiler_47.dll");
                  if (d3dcompiler) {
                    d3d_disassemble = reinterpret_cast<D3DDisassembleFn>(
                        ::GetProcAddress(d3dcompiler, "D3DDisassemble"));
                  }
                }
                if (d3d_disassemble) {
                  Microsoft::WRL::ComPtr<ID3DBlob> dis_blob;
                  const HRESULT dhr = d3d_disassemble(dxbc_bytes->data(), dxbc_bytes->size(),
                                                      0, nullptr, &dis_blob);
                  if (SUCCEEDED(dhr) && dis_blob) {
                    char dis_name[256];
                    std::snprintf(dis_name, sizeof(dis_name), "%s/draw%u-ps-dxbc-%016llX.txt",
                                  shader_dump_dir.c_str(), composite_draw_logs,
                                  (unsigned long long)ps_hash);
                    {
                      std::ofstream df(dis_name, std::ios::binary);
                      if (df) {
                        df.write(static_cast<const char*>(dis_blob->GetBufferPointer()),
                                 std::streamsize(dis_blob->GetBufferSize()));
                      }
                    }
                    dxbc_dis_path = dis_name;
                    dxbc_dis_ok = true;
                  }
                }
              }
              REXGPU_INFO(
                  "FH1 draw23 shader dump: swap={} draw#{} ps_hash={:016X} "
                  "guest_dis_lines={} guest_file={} dxbc_bytes={} dxbc_file={} "
                  "dxbc_dis_ok={} dxbc_dis_file={}",
                  presented_output_frames_, composite_draw_logs, ps_hash, guest_dis_lines,
                  guest_path, dxbc_bytes ? dxbc_bytes->size() : 0,
                  dxbc_path.empty() ? "(none)" : dxbc_path.c_str(),
                  dxbc_dis_ok ? 1 : 0,
                  dxbc_dis_path.empty() ? "(none)" : dxbc_dis_path.c_str());
            }
          }

          // FH1 footprint-vs probe (draw #23 VS): the vertex shader behind
          // the wipe. Dumps the guest VS ucode + disassembly, the translated
          // VS DXBC + runtime disassembly, the interpolator writes, the VS
          // float constants it reads, the per-draw exp-bias scale, and the
          // full vertex-fetch list - so the values reaching the pixel shader
          // can be calculated. Purely diagnostic: reads + file writes + log
          // lines only. Gated on the PS hash, not the draw number.
          if (!g_draw23_vs_vsdump_done && ps_hash == 0xA39E23E2FD1F8A8BULL &&
              vertex_shader) {
            g_draw23_vs_vsdump_done = true;
            const std::string vs_dump_dir = REXCVAR_GET(fh1_debug_composite_dump);
            if (!vs_dump_dir.empty()) {
              const auto vs_ucode_paths = vertex_shader->DumpUcode(vs_dump_dir);
              const std::vector<uint8_t>* vs_dxbc_bytes = nullptr;
              if (vertex_shader_translation) {
                vs_dxbc_bytes = &vertex_shader_translation->translated_binary();
              }
              std::string vs_dxbc_path;
              if (vs_dxbc_bytes && !vs_dxbc_bytes->empty()) {
                char vs_dxbc_name[256];
                std::snprintf(vs_dxbc_name, sizeof(vs_dxbc_name),
                              "%s/draw23-vs-%016llX.dxbc", vs_dump_dir.c_str(),
                              (unsigned long long)vs_hash);
                {
                  std::ofstream vsdf(vs_dxbc_name, std::ios::binary);
                  if (vsdf) {
                    vsdf.write(reinterpret_cast<const char*>(vs_dxbc_bytes->data()),
                               std::streamsize(vs_dxbc_bytes->size()));
                  }
                }
                vs_dxbc_path = vs_dxbc_name;
              }
              std::string vs_dxbc_dis_path;
              bool vs_dxbc_dis_ok = false;
              if (vs_dxbc_bytes && !vs_dxbc_bytes->empty()) {
                using VsD3DDisassembleFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR,
                                                            ID3DBlob**);
                static HMODULE vs_d3dcompiler = nullptr;
                static VsD3DDisassembleFn vs_d3d_disassemble = nullptr;
                static bool vs_d3dcompiler_tried = false;
                if (!vs_d3dcompiler_tried) {
                  vs_d3dcompiler_tried = true;
                  vs_d3dcompiler = ::LoadLibraryA("d3dcompiler_47.dll");
                  if (vs_d3dcompiler) {
                    vs_d3d_disassemble = reinterpret_cast<VsD3DDisassembleFn>(
                        ::GetProcAddress(vs_d3dcompiler, "D3DDisassemble"));
                  }
                }
                if (vs_d3d_disassemble) {
                  Microsoft::WRL::ComPtr<ID3DBlob> vs_dis_blob;
                  const HRESULT vs_dhr = vs_d3d_disassemble(
                      vs_dxbc_bytes->data(), vs_dxbc_bytes->size(), 0, nullptr, &vs_dis_blob);
                  if (SUCCEEDED(vs_dhr) && vs_dis_blob) {
                    char vs_dis_name[256];
                    std::snprintf(vs_dis_name, sizeof(vs_dis_name),
                                  "%s/draw23-vs-dxbc-%016llX.txt", vs_dump_dir.c_str(),
                                  (unsigned long long)vs_hash);
                    {
                      std::ofstream vsdf(vs_dis_name, std::ios::binary);
                      if (vsdf) {
                        vsdf.write(static_cast<const char*>(vs_dis_blob->GetBufferPointer()),
                                   std::streamsize(vs_dis_blob->GetBufferSize()));
                      }
                    }
                    vs_dxbc_dis_path = vs_dis_name;
                    vs_dxbc_dis_ok = true;
                  }
                }
              }
              REXGPU_INFO(
                  "FH1 draw23 vs dump: swap={} draw#{} vs_hash={:016X} ucode_bin={} "
                  "ucode_dis={} dxbc_bytes={} dxbc_file={} dxbc_dis_ok={} dxbc_dis_file={}",
                  presented_output_frames_, composite_draw_logs, vs_hash,
                  vs_ucode_paths.first.string(), vs_ucode_paths.second.string(),
                  vs_dxbc_bytes ? vs_dxbc_bytes->size() : 0,
                  vs_dxbc_path.empty() ? "(none)" : vs_dxbc_path.c_str(),
                  vs_dxbc_dis_ok ? 1 : 0,
                  vs_dxbc_dis_path.empty() ? "(none)" : vs_dxbc_dis_path.c_str());
            }
            // VS metadata: which interpolators the VS writes, the float
            // constants it reads (VS base 0-255), the target-0 colour info,
            // and the exp-bias scale this draw computes (same rule as
            // UpdateSystemConstantValues).
            const Shader::ConstantRegisterMap& vs_map = vertex_shader->constant_register_map();
            std::string vs_consts;
            uint32_t vs_consts_logged = 0;
            const uint32_t* vs_first = &regs[XE_GPU_REG_SHADER_CONSTANT_000_X];
            for (uint32_t r = 0; r < 256 && vs_consts_logged < 16; ++r) {
              if (!(vs_map.float_bitmap[r >> 6] & (uint64_t(1) << (r & 63)))) {
                continue;
              }
              float c[4];
              std::memcpy(c, vs_first + r * 4, sizeof(c));
              vs_consts += fmt::format(" c{}=({:.6g},{:.6g},{:.6g},{:.6g})", r, c[0], c[1],
                                       c[2], c[3]);
              ++vs_consts_logged;
            }
            if (vs_consts.empty()) {
              vs_consts = " (no float constants used)";
            }
            const auto vs_color_info =
                regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
            int32_t vs_exp_bias = vs_color_info.color_exp_bias;
            if (vs_color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
                vs_color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
              if (!host_render_config_->IsFixed16TruncatedToMinus1To1()) {
                vs_exp_bias -= 5;
              }
            }
            const float vs_exp_bias_scale =
                rex::memory::Reinterpret<float>(int32_t(0x3F800000 + (vs_exp_bias << 23)));
            REXGPU_INFO(
                "FH1 draw23 vs meta: swap={} draw#{} vs_hash={:016X} "
                "writes_interpolators={:04X} vs_floats={} vs_consts={} "
                "color_fmt={} color_exp_bias={} exp_bias_scale={:.6g}",
                presented_output_frames_, composite_draw_logs, vs_hash,
                vertex_shader->writes_interpolators(), vs_map.float_count, vs_consts,
                uint32_t(vs_color_info.color_format),
                int32_t(vs_color_info.color_exp_bias), vs_exp_bias_scale);
            // VS fetch list: every vertex binding and attribute, so the
            // formats feeding exports 0/1 are on record.
            for (const Shader::VertexBinding& vs_binding : vertex_shader->vertex_bindings()) {
              for (const Shader::VertexBinding::Attribute& vs_attr : vs_binding.attributes) {
                const ParsedVertexFetchInstruction& vs_fetch = vs_attr.fetch_instr;
                REXGPU_INFO(
                    "FH1 draw23 vs fetch: swap={} draw#{} fetch_const={} stride_words={} "
                    "op={} mini={} pred={} fmt={} off={} stride={} exp_adj={} prefetch={} "
                    "signed_rf={} idx_round={} is_signed={} is_int={} "
                    "result_target={} result_index={} write_mask={:X}",
                    presented_output_frames_, composite_draw_logs, vs_binding.fetch_constant,
                    vs_binding.stride_words,
                    vs_fetch.opcode_name ? vs_fetch.opcode_name : "(null)",
                    vs_fetch.is_mini_fetch ? 1 : 0, vs_fetch.is_predicated ? 1 : 0,
                    uint32_t(vs_fetch.attributes.data_format),
                    vs_fetch.attributes.offset, vs_fetch.attributes.stride,
                    vs_fetch.attributes.exp_adjust, vs_fetch.attributes.prefetch_count,
                    uint32_t(vs_fetch.attributes.signed_rf_mode),
                    vs_fetch.attributes.is_index_rounded ? 1 : 0,
                    vs_fetch.attributes.is_signed ? 1 : 0,
                    vs_fetch.attributes.is_integer ? 1 : 0,
                    uint32_t(vs_fetch.result.storage_target),
                    vs_fetch.result.storage_index, vs_fetch.result.original_write_mask);
              }
            }
          }
          }
        }
      }
    }
  }

  // fh1_debug_frontbuffer_draws: the front buffer the swap resolves at
  // 1CE2D000 is the base_tiles != 0 family (720/16/1x/c0 in free roam). It can
  // be a secondary colour target of a draw, not color0 - which the composite
  // log above excludes by construction - so this names every draw that binds
  // the family in any colour slot, with all bound slots and their colour
  // masks, the viewport, the scissor and the guest textures it samples: the
  // pass that fills (or fails to fill) the surface the swap resolves.
  {
    const int32_t frontbuffer_draw_budget = REXCVAR_GET(fh1_debug_frontbuffer_draws);
    const int64_t frontbuffer_draw_from = REXCVAR_GET(fh1_debug_frontbuffer_draws_from);
    static uint32_t frontbuffer_draw_logs = 0;
    Fh1SurfaceKey frontbuffer_key;
    uint32_t frontbuffer_slot = 0;
    if (native_executor_) {
      for (uint32_t slot = 1; slot < 1 + xenos::kMaxColorRenderTargets; ++slot) {
        if (!(native_executor_->pending_used_bits() & (1u << slot))) {
          continue;
        }
        const Fh1SurfaceKey& key = native_executor_->pending_key(slot);
        if (key.base_tiles != 0 && key.pitch_tiles == 16 &&
            key.msaa == uint32_t(xenos::MsaaSamples::k1X)) {
          frontbuffer_key = key;
          frontbuffer_slot = slot;
          break;
        }
      }
    }
    if (frontbuffer_draw_budget > 0 && frontbuffer_slot != 0 &&
        presented_output_frames_ >= uint64_t(std::max<int64_t>(frontbuffer_draw_from, 0)) &&
        frontbuffer_draw_logs < uint32_t(frontbuffer_draw_budget)) {
      ++frontbuffer_draw_logs;
      std::string frontbuffer_slots;
      if (native_executor_) {
        for (uint32_t slot = 1; slot < 1 + xenos::kMaxColorRenderTargets; ++slot) {
          if (!(native_executor_->pending_used_bits() & (1u << slot))) {
            continue;
          }
          frontbuffer_slots += fmt::format(
              " c{}={}:m{:X}", slot - 1, native_executor_->pending_key(slot).Describe(),
              (normalized_color_mask >> (4 * (slot - 1))) & 0xF);
        }
      }
      std::string frontbuffer_textures;
      uint32_t frontbuffer_texture_remaining = used_texture_mask;
      uint32_t frontbuffer_texture_index;
      while (rex::bit_scan_forward(frontbuffer_texture_remaining,
                                   &frontbuffer_texture_index)) {
        frontbuffer_texture_remaining &= ~(uint32_t(1) << frontbuffer_texture_index);
        const xenos::xe_gpu_texture_fetch_t fetch =
            regs.GetTextureFetch(frontbuffer_texture_index);
        frontbuffer_textures += fmt::format(
            " t{}={:08X}:{}x{}:f{}:t{}:e{}:s{:03X}", frontbuffer_texture_index,
            uint32_t(fetch.base_address) << 12, uint32_t(fetch.size_2d.width) + 1,
            uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.format), uint32_t(fetch.tiled),
            uint32_t(fetch.endianness), uint32_t(fetch.swizzle));
      }
      REXGPU_INFO(
          "FH1 frontbuffer draw: swap={} #{} key={} slot={} vp={},{} {}x{} scissor={},{} "
          "{}x{} prim={} verts={}/{} shader={} mask={:08X} bound={} textures={} {}",
          presented_output_frames_, frontbuffer_draw_logs, frontbuffer_key.Describe(),
          frontbuffer_slot, viewport_info.xy_offset[0], viewport_info.xy_offset[1],
          viewport_info.xy_extent[0], viewport_info.xy_extent[1], scissor.offset[0],
          scissor.offset[1], scissor.extent[0], scissor.extent[1],
          uint32_t(primitive_processing_result.guest_primitive_type),
          primitive_processing_result.guest_draw_vertex_count,
          primitive_processing_result.host_draw_vertex_count,
          int(primitive_processing_result.host_vertex_shader_type), normalized_color_mask,
          frontbuffer_slots, used_texture_mask == 0 ? "none" : "", frontbuffer_textures);
    }
  }

  // Blend factor / stencil reference, and the HUD squeeze of the NDC offset.
  const double hud_squeeze = REXCVAR_GET(fh1_hud_squeeze);
  if (hud_squeeze > 1.0 && normalized_color_mask &&
      regs.Get<reg::RB_COLOR_INFO>(XE_GPU_REG_RB_COLOR_INFO).color_format ==
          xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10) {
    viewport_info.ndc_scale[0] = float(viewport_info.ndc_scale[0] / hud_squeeze);
    viewport_info.ndc_offset[0] = float(viewport_info.ndc_offset[0] / hud_squeeze);
  }

  // Bind the pipeline first (dynamic state updates reference the bound
  // objects with null state, keeping them).
  pipeline_cache_->BindPipeline(*pipeline);
  UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal,
                           normalized_depth_control);

  // System constants (no memexport: shared memory is read-only here).
  UpdateSystemConstantValues(false, primitive_polygonal,
                             primitive_processing_result.line_loop_closing_index,
                             primitive_processing_result.host_shader_index_endian, viewport_info,
                             used_texture_mask, normalized_depth_control, normalized_color_mask);

  // FH1 footprint-vs probe (draw #23 applied exp-bias): the system-constant
  // values actually bound for this draw, read after UpdateSystemConstantValues
  // so the per-draw bias is the applied one, not the previous draw's.
  if (!g_draw23_vs_applied_done && pixel_shader &&
      pixel_shader->ucode_data_hash() == 0xA39E23E2FD1F8A8BULL) {
    REXGPU_INFO(
        "FH1 draw23 vs applied: swap={} exp_bias={:.6g},{:.6g},{:.6g},{:.6g} "
        "alpha_ref={:.6g}",
        presented_output_frames_, system_constants_.color_exp_bias[0],
        system_constants_.color_exp_bias[1], system_constants_.color_exp_bias[2],
        system_constants_.color_exp_bias[3], system_constants_.alpha_test_reference);
    g_draw23_vs_applied_done = true;
  }

  // Vertex buffer residency for in-shader fetch.
  if (!EnsureVertexBufferResidency(vertex_shader->constant_register_map())) {
    if (trace_tile) {
      REXGPU_INFO("FH1 routing drop: swap={} stage=dropped_vbuf_residency prim={} mode={} pass={}",
                  presented_output_frames_, uint32_t(primitive_type),
                  uint32_t(edram_mode), current_draw_tiling_pass_);
    }
    ++draw_counters_.dropped_other;
    return false;
  }

  // Constant buffers, textures, samplers. Phase 3 M3: while this draw is
  // prepared for the tape, constant-buffer updates are held in the packet
  // instead of being written to the GPU; the guard restores the immediate
  // behaviour on every return path below (a dropped draw must not leave the
  // flag set). Texture uploads already ran above, before this flag was set.
  struct DrawRecordScope {
    D3D11CommandProcessor* processor;
    bool& flag;
    bool committed = false;
    DrawRecordScope(D3D11CommandProcessor* owner, bool& value)
        : processor(owner), flag(value) {}
    ~DrawRecordScope() {
      flag = false;
      // Every path that leaves this draw without a packet (an unsupported
      // topology, a missing index buffer, a tape fallback) must still land the
      // constant-buffer bytes the record path deferred, or the next draw would
      // read whatever the buffer held before.
      if (!committed) {
        processor->FlushDeferredConstantBuffers();
      }
    }
  } draw_record_scope(this, recording_draw_);
  recording_draw_ = draw_recordable;
  const bool bindings_updated = UpdateBindings(vertex_shader, pixel_shader);
  if (!bindings_updated) {
    if (trace_tile) {
      REXGPU_INFO("FH1 routing drop: swap={} stage=dropped_bindings prim={} mode={} pass={}",
                  presented_output_frames_, uint32_t(primitive_type),
                  uint32_t(edram_mode), current_draw_tiling_pass_);
    }
    ++draw_counters_.dropped_bindings;
    return false;
  }

  // fh1_debug_c2_probe: the readback stability control, taken before the draw -
  // two reads of the same frame-sized target with no draw between them. A
  // before/after pair without this control is not evidence (PHASE 10's pairs
  // were reported 0.50 changed in every band and could not be told apart from
  // an unstable readback).
  const std::string c2_probe_dir = REXCVAR_GET(fh1_debug_c2_probe);
  bool c2_probe_pending = false;
  bool c2_probe_write_files = false;
  uint32_t c2_probe_index = 0;
  Fh1SurfaceKey c2_probe_key;
  std::vector<uint8_t> c2_probe_before;
  uint32_t c2_probe_width = 0;
  uint32_t c2_probe_height = 0;
  if (!c2_probe_dir.empty() && native_executor_ && native_surfaces_ &&
      presented_output_frames_ >=
          uint64_t(std::max<int32_t>(REXCVAR_GET(fh1_debug_c2_probe_from), 0))) {
    static uint32_t c2_probe_draws = 0;
    const int32_t probe_budget = REXCVAR_GET(fh1_debug_c2_probe_draws);
    if (probe_budget > 0 && c2_probe_draws < uint32_t(probe_budget) &&
        (native_executor_->pending_used_bits() & (1u << 1))) {
      const Fh1SurfaceKey probe_key = native_executor_->pending_key(1);
      // Only the front-buffer family (the 1280-wide 1x surface the composite and
      // the post pass write), and only their rectangle and triangle passes: the
      // HUD's thousands of line-list glyphs would otherwise spend the budget
      // before any composite draw is reached.
      const bool probe_family = !probe_key.is_depth && probe_key.base_tiles == 0 &&
                                probe_key.pitch_tiles == 16 && probe_key.msaa == 0;
      const bool probe_shape =
          primitive_processing_result.guest_primitive_type == xenos::PrimitiveType::kRectangleList ||
          primitive_processing_result.guest_primitive_type == xenos::PrimitiveType::kTriangleList;
      // And only a draw that covers the frame's width: the two 1280x360 world
      // halves and the 1280x720 post pass. The HUD's own 1280-wide surfaces get
      // 320x192, 160x96 and 80x48 viewports.
      const bool probe_frame_viewport = viewport_info.xy_extent[0] == 1280 &&
                                        viewport_info.xy_extent[1] >= 360;
      D3D11Fh1NativeSurface* probe_surface = native_surfaces_->FindSurface(probe_key.Pack());
      if (probe_family && probe_shape && probe_frame_viewport && probe_surface &&
          probe_surface->texture && probe_surface->width >= 1280) {
        D3D11_TEXTURE2D_DESC probe_desc = {};
        probe_surface->texture->GetDesc(&probe_desc);
        std::vector<uint8_t> control;
        SurfaceDumpStats stats_before, stats_control;
        std::string error_before, error_control;
        uint32_t control_width = 0, control_height = 0;
        ID3D11Device* probe_device = GetD3D11Provider().GetDevice();
        ID3D11DeviceContext* probe_context = GetD3D11Provider().GetImmediateContext();
        const bool read_before = ReadSurfaceRgb(probe_device, probe_context, probe_surface,
                                               c2_probe_before, c2_probe_width, c2_probe_height,
                                               stats_before, error_before);
        const bool read_control = ReadSurfaceRgb(probe_device, probe_context, probe_surface, control,
                                                 control_width, control_height, stats_control,
                                                 error_control);
        if (read_before && read_control) {
          const DrawFootprint stability =
              MeasureFootprint(c2_probe_before, control, c2_probe_width, c2_probe_height);
          c2_probe_index = ++c2_probe_draws;
          c2_probe_key = probe_key;
          c2_probe_pending = true;
          c2_probe_write_files = c2_probe_index <= 6;
          REXGPU_INFO(
              "FH1 c2 probe control: swap={} probe#{} key={} host={}x{} res_fmt={} view_fmt={} "
              "samples={} distinct={}/{} control_changed={:.6f} control_bbox={},{}-{},{}",
              presented_output_frames_, c2_probe_index, probe_key.Describe(),
              probe_desc.Width, probe_desc.Height, uint32_t(probe_desc.Format),
              uint32_t(probe_surface->view_format), probe_desc.SampleDesc.Count,
              stats_before.distinct, stats_control.distinct, stability.changed, stability.x0,
              stability.y0, stability.x1, stability.y1);
          if (c2_probe_write_files) {
            char before_name[255];
            std::snprintf(before_name, sizeof(before_name), "%s/c2p-%llu-%u-before.ppm",
                          c2_probe_dir.c_str(),
                          static_cast<unsigned long long>(presented_output_frames_),
                          c2_probe_index);
            WriteRgbPpm(before_name, c2_probe_before, c2_probe_width, c2_probe_height);
            std::snprintf(before_name, sizeof(before_name), "%s/c2p-%llu-%u-control.ppm",
                          c2_probe_dir.c_str(),
                          static_cast<unsigned long long>(presented_output_frames_),
                          c2_probe_index);
            WriteRgbPpm(before_name, control, control_width, control_height);
          }
        } else {
          REXGPU_INFO("FH1 c2 probe: swap={} key={} readback failed ({} / {})",
                      presented_output_frames_, probe_key.Describe(),
                      read_before ? std::to_string(stats_before.distinct) : error_before,
                      read_control ? std::to_string(stats_control.distinct) : error_control);
        }
      }
    }
  }

  // FH1 next-evidence probe (Goal 1): capture a representative 3D draw's
  // actual pixel-shader output. For the first few draws that write the EDRAM
  // colour surface (0/32/4x/c3) at/after fh1_debug_edram3d_probe_from, read
  // that surface twice before the draw (a readback stability control) and
  // once after it. The before/after pair distinguishes "the shader outputs
  // grey" from "the draw writes nothing while the surface keeps its clear
  // value". Purely diagnostic: readbacks + log lines + PPMs only.
  const std::string edram3d_probe_dir = REXCVAR_GET(fh1_debug_edram3d_probe);
  bool edram3d_probe_pending = false;
  bool edram3d_probe_write_files = false;
  uint32_t edram3d_probe_index = 0;
  Fh1SurfaceKey edram3d_probe_key;
  std::vector<uint8_t> edram3d_probe_before;
  uint32_t edram3d_probe_width = 0;
  uint32_t edram3d_probe_height = 0;
  auto edram3d_px = [](const std::vector<uint8_t>& img, uint32_t w, uint32_t h, uint32_t x,
                       uint32_t y) {
    if (w == 0 || h == 0 || img.empty()) {
      return std::string("(?)");
    }
    x = std::min(x, w - 1);
    y = std::min(y, h - 1);
    const uint8_t* px = img.data() + (size_t(y) * w + x) * 3u;
    return fmt::format("({},{},{})", px[0], px[1], px[2]);
  };
  if (!edram3d_probe_dir.empty() && native_executor_ && native_surfaces_ &&
      presented_output_frames_ >=
          uint64_t(std::max<int32_t>(REXCVAR_GET(fh1_debug_edram3d_probe_from), 0))) {
    static uint32_t edram3d_probe_draws = 0;
    const int32_t edram3d_budget = REXCVAR_GET(fh1_debug_edram3d_probe_draws);
    // Phase 2e: EDRAM-only trigger. Keep the Phase 2d slot scan (every bound
    // render-target slot) and the dropped used_texture_mask gate, but restore
    // the EDRAM family check (pitch_tiles == 32 && 4x MSAA) so the probe only
    // fires on the EDRAM surface (0/32/4x/c3) and the budget cannot be spent
    // on composite/loading draws. The slot index is the pending_key index
    // (0 = depth, 1..N = colour RTs), so scan 0..kMaxColorRenderTargets to
    // cover all four colour RTs.
    const bool edram3d_is_tri =
        primitive_processing_result.guest_primitive_type == xenos::PrimitiveType::kTriangleList ||
        primitive_processing_result.guest_primitive_type == xenos::PrimitiveType::kTriangleStrip ||
        primitive_processing_result.guest_primitive_type == xenos::PrimitiveType::kTriangleFan;
    if (edram3d_budget > 0 && edram3d_probe_draws < uint32_t(edram3d_budget) && edram3d_is_tri) {
      const uint32_t edram3d_used_bits = native_executor_->pending_used_bits();
      Fh1SurfaceKey edram3d_key;
      D3D11Fh1NativeSurface* edram3d_surface = nullptr;
      uint32_t edram3d_rt_slot = 0;
      for (uint32_t rt_slot = 0; rt_slot < 1 + xenos::kMaxColorRenderTargets; ++rt_slot) {
        if (!(edram3d_used_bits & (1u << rt_slot))) {
          continue;
        }
        const Fh1SurfaceKey candidate_key = native_executor_->pending_key(rt_slot);
        if (candidate_key.is_depth || candidate_key.base_tiles != 0 ||
            candidate_key.pitch_tiles != 32 ||
            candidate_key.msaa != uint32_t(xenos::MsaaSamples::k4X)) {
          continue;
        }
        D3D11Fh1NativeSurface* candidate_surface =
            native_surfaces_->FindSurface(candidate_key.Pack());
        if (candidate_surface && candidate_surface->texture) {
          edram3d_key = candidate_key;
          edram3d_surface = candidate_surface;
          edram3d_rt_slot = rt_slot;
          break;
        }
      }
      if (edram3d_surface) {
        D3D11_TEXTURE2D_DESC edram3d_desc = {};
        edram3d_surface->texture->GetDesc(&edram3d_desc);
        std::vector<uint8_t> edram3d_control;
        SurfaceDumpStats edram3d_stats_before, edram3d_stats_control;
        std::string edram3d_error_before, edram3d_error_control;
        uint32_t edram3d_control_width = 0, edram3d_control_height = 0;
        ID3D11Device* edram3d_device = GetD3D11Provider().GetDevice();
        ID3D11DeviceContext* edram3d_context = GetD3D11Provider().GetImmediateContext();
        const bool edram3d_read_before =
            ReadSurfaceRgb(edram3d_device, edram3d_context, edram3d_surface,
                           edram3d_probe_before, edram3d_probe_width, edram3d_probe_height,
                           edram3d_stats_before, edram3d_error_before);
        const bool edram3d_read_control =
            ReadSurfaceRgb(edram3d_device, edram3d_context, edram3d_surface, edram3d_control,
                           edram3d_control_width, edram3d_control_height,
                           edram3d_stats_control, edram3d_error_control);
        if (edram3d_read_before && edram3d_read_control) {
          const DrawFootprint edram3d_stability = MeasureFootprint(
              edram3d_probe_before, edram3d_control, edram3d_probe_width, edram3d_probe_height);
          edram3d_probe_index = ++edram3d_probe_draws;
          edram3d_probe_key = edram3d_key;
          edram3d_probe_pending = true;
          edram3d_probe_write_files = edram3d_probe_index <= 3;
          const uint64_t edram3d_ps_hash = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
          REXGPU_INFO(
              "FH1 edram3d probe control: swap={} probe#{} key={} rt={} texmask={:08X} host={}x{} res_fmt={} "
              "view_fmt={} samples={} distinct={}/{} control_changed={:.6f} "
              "control_bbox={},{}-{},{} ps_hash={:016X} verts={}/{} vp={},{} {}x{} "
              "before_mean={:.6g},{:.6g},{:.6g} px centre={} tl={} tr={} bl={} br={}",
              presented_output_frames_, edram3d_probe_index, edram3d_key.Describe(),
              edram3d_rt_slot, used_texture_mask,
              edram3d_desc.Width, edram3d_desc.Height, uint32_t(edram3d_desc.Format),
              uint32_t(edram3d_surface->view_format), edram3d_desc.SampleDesc.Count,
              edram3d_stats_before.distinct, edram3d_stats_control.distinct,
              edram3d_stability.changed, edram3d_stability.x0, edram3d_stability.y0,
              edram3d_stability.x1, edram3d_stability.y1, edram3d_ps_hash,
              primitive_processing_result.guest_draw_vertex_count,
              primitive_processing_result.host_draw_vertex_count,
              viewport_info.xy_offset[0], viewport_info.xy_offset[1],
              viewport_info.xy_extent[0], viewport_info.xy_extent[1],
              edram3d_stats_before.mean[0], edram3d_stats_before.mean[1],
              edram3d_stats_before.mean[2],
              edram3d_px(edram3d_probe_before, edram3d_probe_width, edram3d_probe_height,
                         edram3d_probe_width / 2, edram3d_probe_height / 2),
              edram3d_px(edram3d_probe_before, edram3d_probe_width, edram3d_probe_height, 0, 0),
              edram3d_px(edram3d_probe_before, edram3d_probe_width, edram3d_probe_height,
                         edram3d_probe_width - 1, 0),
              edram3d_px(edram3d_probe_before, edram3d_probe_width, edram3d_probe_height, 0,
                         edram3d_probe_height - 1),
              edram3d_px(edram3d_probe_before, edram3d_probe_width, edram3d_probe_height,
                         edram3d_probe_width - 1, edram3d_probe_height - 1));
          if (edram3d_probe_write_files) {
            char edram3d_name[255];
            std::snprintf(edram3d_name, sizeof(edram3d_name), "%s/edram3d-%llu-%u-before.ppm",
                          edram3d_probe_dir.c_str(),
                          static_cast<unsigned long long>(presented_output_frames_),
                          edram3d_probe_index);
            WriteRgbPpm(edram3d_name, edram3d_probe_before, edram3d_probe_width,
                        edram3d_probe_height);
            std::snprintf(edram3d_name, sizeof(edram3d_name), "%s/edram3d-%llu-%u-control.ppm",
                          edram3d_probe_dir.c_str(),
                          static_cast<unsigned long long>(presented_output_frames_),
                          edram3d_probe_index);
            WriteRgbPpm(edram3d_name, edram3d_control, edram3d_control_width,
                        edram3d_control_height);
          }
        } else {
          REXGPU_INFO("FH1 edram3d probe: swap={} key={} readback failed ({} / {})",
                      presented_output_frames_, edram3d_key.Describe(),
                      edram3d_read_before ? std::to_string(edram3d_stats_before.distinct)
                                          : edram3d_error_before,
                      edram3d_read_control ? std::to_string(edram3d_stats_control.distinct)
                                           : edram3d_error_control);
        }
      }
    }
  }

  // Primitive topology.
  D3D11_PRIMITIVE_TOPOLOGY primitive_topology;
  switch (primitive_processing_result.host_primitive_type) {
    case xenos::PrimitiveType::kPointList:
      primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
      break;
    case xenos::PrimitiveType::kLineList:
      primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
      primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
      break;
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kRectangleList:
      primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      break;
    case xenos::PrimitiveType::kQuadList:
      primitive_topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
      break;
    default:
      REXGPU_ERROR("Host primitive type {} is not supported by the D3D11 command processor",
                   uint32_t(primitive_processing_result.host_primitive_type));
      if (trace_tile) {
        REXGPU_INFO("FH1 routing drop: swap={} stage=dropped_host_prim prim={} mode={} pass={}",
                    presented_output_frames_, uint32_t(primitive_type),
                    uint32_t(edram_mode), current_draw_tiling_pass_);
      }
      ++draw_counters_.dropped_other;
      return false;
  }

  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  context->IASetPrimitiveTopology(primitive_topology);

  // Phase 3 M3: the draw call is the one thing the tape defers. Everything
  // above (pipeline state, fixed function, constants, views, targets,
  // topology) has run on the immediate context; the packet captures that state
  // and replay rebinds it and issues this draw at the tape's submission point.
  bool draw_recorded = false;
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    if (recording_draw_) {
      draw_recorded = RecordDrawPacket(*pipeline, primitive_topology,
                                       primitive_processing_result.host_draw_vertex_count,
                                       /*indexed=*/false, nullptr, DXGI_FORMAT_UNKNOWN, 0,
                                       vertex_srv_count, pixel_srv_count,
                                       vertex_sampler_count, pixel_sampler_count);
    }
    if (!draw_recorded) {
      context->Draw(primitive_processing_result.host_draw_vertex_count, 0);
    if (REXCVAR_GET(pinyon_audit_passes)) {
      rex::graphics::render_audit::NoteDrawSubmission();
    }
    }
  } else {
    DXGI_FORMAT index_format =
        primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
            ? DXGI_FORMAT_R16_UINT
            : DXGI_FORMAT_R32_UINT;
    ID3D11Buffer* index_buffer = nullptr;
    uint32_t index_buffer_offset = 0;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        // Guest DMA binds straight from the shared-memory mirror (it has
        // BIND_INDEX_BUFFER); the translated VS handles endianness.
        index_buffer = static_cast<D3D11SharedMemoryDeviceUploadSink*>(
                           shared_memory_->upload_sink())
                           ->GetHostBuffer();
        index_buffer_offset = primitive_processing_result.guest_index_base;
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        index_buffer_offset = primitive_processor_->GetConvertedIndexBufferOffset(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer = primitive_processor_->GetBuiltinIndexBuffer();
        index_buffer_offset = primitive_processor_->GetBuiltinIndexBufferOffset(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        REXGPU_ERROR("Unhandled index buffer type {}",
                     uint32_t(primitive_processing_result.index_buffer_type));
        if (trace_tile) {
          REXGPU_INFO("FH1 routing drop: swap={} stage=dropped_index_buffer prim={} mode={} pass={}",
                      presented_output_frames_, uint32_t(primitive_type),
                      uint32_t(edram_mode), current_draw_tiling_pass_);
        }
        ++draw_counters_.dropped_other;
        return false;
    }
    if (!index_buffer) {
      if (trace_tile) {
        REXGPU_INFO("FH1 routing drop: swap={} stage=dropped_index_buffer prim={} mode={} pass={}",
                    presented_output_frames_, uint32_t(primitive_type),
                    uint32_t(edram_mode), current_draw_tiling_pass_);
      }
      ++draw_counters_.dropped_other;
      return false;
    }
    context->IASetIndexBuffer(index_buffer, index_format, index_buffer_offset);
    if (recording_draw_) {
      // Converted indices are ring allocations written at record time; the
      // packet holds the buffer and offset, and pages are not reused within a
      // frame, so the bytes the draw reads at replay are the ones it read here.
      draw_recorded = RecordDrawPacket(*pipeline, primitive_topology,
                                       primitive_processing_result.host_draw_vertex_count,
                                       /*indexed=*/true, index_buffer, index_format,
                                       index_buffer_offset, vertex_srv_count, pixel_srv_count,
                                       vertex_sampler_count, pixel_sampler_count);
    }
    if (!draw_recorded) {
      context->DrawIndexed(primitive_processing_result.host_draw_vertex_count, 0, 0);
    }
  }
  // The draw is issued (packet or immediate); the deferred constant buffers
  // now live on the tape, so the scope must not flush them behind its back.
  draw_record_scope.committed = true;
  if (draw_recorded && (c2_probe_pending || dump_c2_after_draw)) {
    // The probe/dump below reports this draw's own footprint, so the recorded
    // draw has to run before the readback (the immediate path reads after it).
    DrainTapeForDiagnostic("draw probe");
  }
  if (c2_probe_pending) {
    // The draw's own footprint: the same surface, read again. Its difference
    // from the pre-draw read (whose stability the control above proved) is
    // exactly the pixels this draw wrote, reported per 64-pixel column and
    // 60-pixel row so a pass that paints one band shows as one.
    D3D11Fh1NativeSurface* probe_surface = native_surfaces_->FindSurface(c2_probe_key.Pack());
    std::vector<uint8_t> after_rgb;
    SurfaceDumpStats stats_after;
    std::string error_after;
    uint32_t after_width = 0, after_height = 0;
    if (probe_surface &&
        ReadSurfaceRgb(GetD3D11Provider().GetDevice(),
                       GetD3D11Provider().GetImmediateContext(), probe_surface, after_rgb,
                       after_width, after_height, stats_after, error_after)) {
      const DrawFootprint footprint =
          MeasureFootprint(c2_probe_before, after_rgb, c2_probe_width, c2_probe_height);
      REXGPU_INFO(
          "FH1 c2 probe draw: swap={} probe#{} key={} vp={},{} {}x{} scissor={},{} {}x{} "
          "prim={} verts={}/{} changed={:.6f} bbox={},{}-{},{} cols={} rows={} after={}",
          presented_output_frames_, c2_probe_index, c2_probe_key.Describe(),
          viewport_info.xy_offset[0], viewport_info.xy_offset[1], viewport_info.xy_extent[0],
          viewport_info.xy_extent[1], scissor.offset[0], scissor.offset[1], scissor.extent[0],
          scissor.extent[1], uint32_t(primitive_processing_result.guest_primitive_type),
          primitive_processing_result.guest_draw_vertex_count,
          primitive_processing_result.host_draw_vertex_count, footprint.changed, footprint.x0,
          footprint.y0, footprint.x1, footprint.y1, footprint.columns, footprint.rows,
          RgbBandProfile(after_rgb, after_width, after_height));
      if (c2_probe_write_files) {
        char after_name[255];
        std::snprintf(after_name, sizeof(after_name), "%s/c2p-%llu-%u-after.ppm",
                      c2_probe_dir.c_str(),
                      static_cast<unsigned long long>(presented_output_frames_), c2_probe_index);
        WriteRgbPpm(after_name, after_rgb, after_width, after_height);
      }
    } else {
      REXGPU_INFO("FH1 c2 probe draw: swap={} probe#{} key={} after-readback failed ({})",
                  presented_output_frames_, c2_probe_index, c2_probe_key.Describe(),
                  error_after.c_str());
    }
  }
  if (edram3d_probe_pending) {
    if (draw_recorded) {
      DrainTapeForDiagnostic("edram3d probe");
    }
    D3D11Fh1NativeSurface* edram3d_after_surface =
        native_surfaces_->FindSurface(edram3d_probe_key.Pack());
    std::vector<uint8_t> edram3d_after_rgb;
    SurfaceDumpStats edram3d_stats_after;
    std::string edram3d_error_after;
    uint32_t edram3d_after_width = 0, edram3d_after_height = 0;
    if (edram3d_after_surface &&
        ReadSurfaceRgb(GetD3D11Provider().GetDevice(),
                       GetD3D11Provider().GetImmediateContext(), edram3d_after_surface,
                       edram3d_after_rgb, edram3d_after_width, edram3d_after_height,
                       edram3d_stats_after, edram3d_error_after)) {
      const DrawFootprint edram3d_footprint = MeasureFootprint(
          edram3d_probe_before, edram3d_after_rgb, edram3d_probe_width, edram3d_probe_height);
      REXGPU_INFO(
          "FH1 edram3d probe draw: swap={} probe#{} key={} vp={},{} {}x{} scissor={},{} "
          "{}x{} prim={} verts={}/{} changed={:.6f} bbox={},{}-{},{} cols={} rows={} "
          "after_distinct={} after_mean={:.6g},{:.6g},{:.6g} after={} px centre={} tl={} "
          "tr={} bl={} br={}",
          presented_output_frames_, edram3d_probe_index, edram3d_probe_key.Describe(),
          viewport_info.xy_offset[0], viewport_info.xy_offset[1], viewport_info.xy_extent[0],
          viewport_info.xy_extent[1], scissor.offset[0], scissor.offset[1], scissor.extent[0],
          scissor.extent[1], uint32_t(primitive_processing_result.guest_primitive_type),
          primitive_processing_result.guest_draw_vertex_count,
          primitive_processing_result.host_draw_vertex_count, edram3d_footprint.changed,
          edram3d_footprint.x0, edram3d_footprint.y0, edram3d_footprint.x1,
          edram3d_footprint.y1, edram3d_footprint.columns, edram3d_footprint.rows,
          edram3d_stats_after.distinct, edram3d_stats_after.mean[0],
          edram3d_stats_after.mean[1], edram3d_stats_after.mean[2],
          RgbBandProfile(edram3d_after_rgb, edram3d_after_width, edram3d_after_height),
          edram3d_px(edram3d_after_rgb, edram3d_after_width, edram3d_after_height,
                     edram3d_after_width / 2, edram3d_after_height / 2),
          edram3d_px(edram3d_after_rgb, edram3d_after_width, edram3d_after_height, 0, 0),
          edram3d_px(edram3d_after_rgb, edram3d_after_width, edram3d_after_height,
                     edram3d_after_width - 1, 0),
          edram3d_px(edram3d_after_rgb, edram3d_after_width, edram3d_after_height, 0,
                     edram3d_after_height - 1),
          edram3d_px(edram3d_after_rgb, edram3d_after_width, edram3d_after_height,
                     edram3d_after_width - 1, edram3d_after_height - 1));
      if (edram3d_probe_write_files) {
        char edram3d_after_name[255];
        std::snprintf(edram3d_after_name, sizeof(edram3d_after_name),
                      "%s/edram3d-%llu-%u-after.ppm", edram3d_probe_dir.c_str(),
                      static_cast<unsigned long long>(presented_output_frames_),
                      edram3d_probe_index);
        WriteRgbPpm(edram3d_after_name, edram3d_after_rgb, edram3d_after_width,
                    edram3d_after_height);
      }
    } else {
      REXGPU_INFO("FH1 edram3d probe draw: swap={} probe#{} key={} after-readback failed ({})",
                  presented_output_frames_, edram3d_probe_index, edram3d_probe_key.Describe(),
                  edram3d_error_after.c_str());
    }
  }
  if (dump_c2_after_draw && native_surfaces_) {
    D3D11Fh1NativeSurface* drawn_surface = native_surfaces_->FindSurface(dump_c2_after_key);
    if (drawn_surface) {
      const std::string dir = REXCVAR_GET(fh1_debug_texture_dump);
      SurfaceDumpStats dump_stats;
      std::string dump_error;
      char dump_name[192];
      std::snprintf(dump_name, sizeof(dump_name), "%s/after-%s.ppm", dir.c_str(),
                    dump_c2_after_tag);
      const bool dump_written =
          DumpSurfacePpm(GetD3D11Provider().GetDevice(),
                         GetD3D11Provider().GetImmediateContext(), drawn_surface, dump_name,
                         dump_stats, dump_error);
      REXGPU_INFO(
          "FH1 surface dump: swap={} stage=c2-after-post [{}] {} nonzero={} "
          "mean={:.4f},{:.4f},{:.4f} colours={}",
          presented_output_frames_, dump_name,
          dump_written ? "written" : dump_error.c_str(),
          dump_stats.total ? double(dump_stats.nonzero) / double(dump_stats.total) : 0.0,
          dump_stats.mean[0], dump_stats.mean[1], dump_stats.mean[2], dump_stats.distinct);
    }
  }
  // Report the issued draw to the performance counters (draw_calls, and the
  // vertex count the gate reads as activity). The D3D12 and D3D11 backends
  // did not do this, so every capture of this backend showed draw_calls = 0
  // even while draws were executing.
  PROFILE_DRAW_CALL();
  PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
  ++draw_counters_.issued;
  // FH1 routing probe (stage D: reached the D3D11 Draw call).
  if (trace_tile) {
    REXGPU_INFO("FH1 routing issued: swap={} prim={} mode={} pass={} verts={}/{}",
                presented_output_frames_, uint32_t(primitive_type), uint32_t(edram_mode),
                current_draw_tiling_pass_,
                primitive_processing_result.guest_draw_vertex_count,
                primitive_processing_result.host_draw_vertex_count);
  }
  // FH1 call logger (Phase 2): issued draw (joins to the guest draw by order).
  // Review additions 7 (design 3.1 / 8.2): the same line also carries
  // the per-draw state that decides the #23 split — depth/stencil
  // state (raw dword + decoded enables/funcs from
  // normalized_depth_control, registers.h:798-818), the full scissor
  // rect (offset + extent, already scaled by draw_resolution_scale_*),
  // the claim rect (viewport ∩ scissor), the index count, and the
  // guest/host vertex counts. The claim rect is the rasterisable
  // extent given the bound viewport/scissor; the TRUE rasterised
  // extent is the existing c2-probe footprint (the comparison joins
  // A3 draw N with the c2-probe bbox for draw N). A true post-VS
  // screen-space vertex Y-range is NOT computable on the CPU here
  // (the VS runs on the GPU), so it is deliberately not claimed.
  if (Fh1CallogEnabled(presented_output_frames_)) {
    REXGPU_INFO("FH1 callog side=A swap={} kind=issued prim={} mode={} pass={} topo={} "
                "verts={}/{} idx={} depth={:08X} z={} sten={} zfunc={} stenfunc={} "
                "scissor={},{},{}x{} claim={},{}-{},{}",
                presented_output_frames_, uint32_t(primitive_type), uint32_t(edram_mode),
                current_draw_tiling_pass_, uint32_t(primitive_topology),
                primitive_processing_result.guest_draw_vertex_count,
                primitive_processing_result.host_draw_vertex_count,
                index_count,
                normalized_depth_control.value,
                normalized_depth_control.z_enable ? 1 : 0,
                normalized_depth_control.stencil_enable ? 1 : 0,
                static_cast<uint32_t>(normalized_depth_control.zfunc),
                static_cast<uint32_t>(normalized_depth_control.stencilfunc),
                scissor.offset[0], scissor.offset[1],
                scissor.extent[0], scissor.extent[1],
                claim_x0, claim_y0, claim_x1, claim_y1);
  }
  if (native_executor_) {
    Fh1DrawInfo draw;
    draw.rasterization_done = true;
    native_executor_->NativeDrawIssued(draw, current_untile_band);
  }
  return true;
}

// Phase 3 M2: appends the resolve's dispatch (with its input already on the
// tape) as a batch, retrying once at a tape overflow. The graphics stages'
// slot 0 holds the shared-memory mirror's SRV after a draw; Direct3D 11 drops a
// bind of another view of that resource on the compute stages while an
// incompatible view is bound (the resolve blanking defect), so the batch starts
// with the explicit unbind the immediate path performs inline.
bool D3D11CommandProcessor::RecordResolveBatch(const Fh1ResolvePlan& plan,
                                               const D3D11Fh1NativeResolve::SourceRect& source_rect,
                                               uint32_t sample_select,
                                               ID3D11UnorderedAccessView* memory_target,
                                               bool use_staging_copy) {
  if (!command_lists_enabled_ || !command_list_ || !native_resolve_ || !source_rect.surface) {
    return false;
  }
  D3D11CommandList& tape = *command_list_;
  const D3D11Fh1NativeResolve::SourceRect& source = source_rect;
  // MSAA-safe provenance readback: ResolveSubresource copies the 4x colour
  // surface to a temporary 1x texture without touching the live render path,
  // then staging reads up to the first 256 host-texture rows.
  if (REXCVAR_GET(fh1_debug_tile_trace_swap) > 0 &&
      presented_output_frames_ == uint64_t(REXCVAR_GET(fh1_debug_tile_trace_swap)) &&
      source.surface && source.surface->texture && source.surface->samples > 1 &&
      !plan.copying_depth) {
    ID3D11Device* device = GetD3D11Provider().GetDevice();
    ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
    D3D11_TEXTURE2D_DESC source_desc;
    source.surface->texture->GetDesc(&source_desc);
    D3D11_TEXTURE2D_DESC resolved_desc = source_desc;
    resolved_desc.SampleDesc.Count = 1;
    resolved_desc.SampleDesc.Quality = 0;
    resolved_desc.Usage = D3D11_USAGE_DEFAULT;
    resolved_desc.BindFlags = 0;
    resolved_desc.CPUAccessFlags = 0;
    resolved_desc.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> resolved_temp;
    const uint32_t rows = std::min<uint32_t>(256, source_desc.Height);
    if (SUCCEEDED(device->CreateTexture2D(&resolved_desc, nullptr, &resolved_temp))) {
      context->ResolveSubresource(resolved_temp.Get(), 0, source.surface->texture, 0,
                                  source_desc.Format);
      D3D11_TEXTURE2D_DESC staging_desc = resolved_desc;
      staging_desc.Height = rows;
      staging_desc.Usage = D3D11_USAGE_STAGING;
      staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
      if (SUCCEEDED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
        D3D11_BOX box{0, 0, 0, source_desc.Width, rows, 1};
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, resolved_temp.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
          uint64_t hash = UINT64_C(1469598103934665603);
          const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
          for (uint32_t y = 0; y < rows; ++y) {
            const auto* row = bytes + size_t(mapped.RowPitch) * y;
            for (uint32_t x = 0; x < source_desc.Width * 4u; ++x) {
              hash = (hash ^ row[x]) * UINT64_C(1099511628211);
            }
          }
          REXGPU_INFO("FH1 tile source hash: swap={} resolve_count={} key={} rows=0..{} "
                      "hash={:016X} samples={} format={}",
                      presented_output_frames_, resolve_call_count_, source.surface->key.Describe(),
                      rows, hash, source.surface->samples, uint32_t(source_desc.Format));
          context->Unmap(staging.Get(), 0);
        }
      }
    }
  }
  const uint32_t resolve_dest_base =
      use_staging_copy ? plan.dest_base - plan.info.copy_dest_extent_start : plan.dest_base;
  auto append_batch = [&]() -> bool {
    const size_t mark = tape.command_count();
    if (!tape.Append<D3D11UnbindGraphicsSrvPacket>()) {
      return false;
    }
    if (resource_state_tracker_) {
      resource_state_tracker_->Transition(tape, source.surface->texture,
                                          D3D11ResourceState::kShaderResource);
    }
    Microsoft::WRL::ComPtr<ID3D11Resource> target_resource;
    if (memory_target) {
      memory_target->GetResource(&target_resource);
    }
    if (resource_state_tracker_ && target_resource) {
      resource_state_tracker_->Transition(tape, target_resource.Get(),
                                          D3D11ResourceState::kUnorderedAccess);
    }
    if (!native_resolve_->RecordResolveMemory(tape, source, plan.resolve_key, sample_select,
                                              plan.dest_info, resolve_dest_base, plan.dest_pitch,
                                              memory_target, true)) {
      // A named skip (pipeline or view creation) leaves no dispatch; drop the
      // partial batch so the tape stays a description of what actually runs.
      tape.Truncate(mark);
      return false;
    }
    if (resource_state_tracker_ && target_resource) {
      resource_state_tracker_->TransitionUav(tape, target_resource.Get());
    }
    ++tape_resolve_packets_;
    return true;
  };
  if (append_batch()) {
    return true;
  }
  if (!tape.overflowed()) {
    return false;
  }
  // The overflow safety valve of the plan: submit at the overflow point and
  // continue in a new tape; never run the remainder immediately while a tape is
  // pending. The gate must not reach this.
  static bool overflow_submit_logged = false;
  if (!overflow_submit_logged) {
    overflow_submit_logged = true;
    REXGPU_WARN(
        "D3D11 command lists: the resolve tape overflowed; submitting at the overflow point "
        "and continuing in a new tape (this must not occur in a normal run)");
  }
  if (!SubmitFrameTape("resolve batch overflow")) {
    return false;
  }
  return append_batch();
}

// Phase 3 M2: the staging-to-mirror copy as a recorded buffer copy. The
// immediate path queues the same CopySubresourceRegion right after the resolve
// dispatch; recording it keeps the pair ordered in one tape.
bool D3D11CommandProcessor::RecordResolveStagingCopy(uint32_t host_offset, uint32_t length) {
  if (!command_lists_enabled_ || !command_list_ || !shared_memory_ || !length) {
    return false;
  }
  auto* upload_sink =
      static_cast<D3D11SharedMemoryDeviceUploadSink*>(shared_memory_->upload_sink());
  ID3D11Buffer* staging = upload_sink ? upload_sink->GetResolveStagingBuffer() : nullptr;
  ID3D11Buffer* host = upload_sink ? upload_sink->GetHostBuffer() : nullptr;
  if (!staging || !host) {
    return false;
  }
  D3D11CommandList& tape = *command_list_;
  const size_t mark = tape.command_count();
  if (resource_state_tracker_) {
    resource_state_tracker_->Transition(tape, staging, D3D11ResourceState::kCopySource);
    resource_state_tracker_->Transition(tape, host, D3D11ResourceState::kCopyDest);
  }
  D3D11CopyBufferPacket* copy = tape.Append<D3D11CopyBufferPacket>();
  if (!copy) {
    tape.Truncate(mark);
    return false;
  }
  copy->source = staging;
  copy->destination = host;
  copy->source_offset = 0;
  copy->destination_offset = host_offset;
  copy->length = length;
  return true;
}

// Phase 3 M2: replays the open tape on the immediate context through the
// tape's sink, then resets it (releasing the packets' COM references; Direct3D
// 11 keeps in-flight resources alive internally). Called at each resolve
// boundary and as a safety net before a draw, the swap, and any CPU-side read
// of the GPU mirror. No GPU wait here: the readers that need completion use
// WaitForGpu, and the swap has its own wait.
bool D3D11CommandProcessor::SubmitFrameTape(const char* reason) {
  if (!command_lists_enabled_ || !command_list_ || command_list_->empty()) {
    return true;
  }
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  if (!context) {
    return false;
  }
  if (!command_sink_) {
    command_sink_ = CreateD3D11ImmediateCommandSink(context);
    if (!command_sink_) {
      return false;
    }
  }
  const size_t packets = command_list_->command_count();
  const size_t arena_bytes = command_list_->arena_bytes();
  command_list_->Replay(*command_sink_);
  ++tape_submit_count_;
  tape_packets_submitted_ += packets;
  tape_arena_bytes_submitted_ += arena_bytes;
  if (REXCVAR_GET(fh1_swap_fetch_trace)) {
    static uint32_t submit_logs = 0;
    if (submit_logs < 24) {
      ++submit_logs;
      REXGPU_INFO("D3D11 command lists: submitted {} packets, {} arena bytes ({})",
                  uint64_t(packets), uint64_t(arena_bytes), reason ? reason : "?");
    }
  }
  command_list_->Reset();
  if (resource_state_tracker_) {
    resource_state_tracker_->Reset();
  }
  return true;
}

bool D3D11CommandProcessor::DrainTapeForDiagnostic(const char* reason) {
  return SubmitFrameTape(reason);
}

// Phase 3 M3: a texture-cache load's upload as a packet. The cache keeps its
// CPU untiling at record time (the bytes are copied into the tape's arena) and
// skips its own UpdateSubresource, so replay performs the upload before the
// draw that samples it. The arena overflow valve submits the open tape and
// retries once; a second failure returns false and the cache uploads inline.
bool D3D11CommandProcessor::RecordTextureUpload(ID3D11Resource* destination,
                                                uint32_t destination_subresource,
                                                const void* data, uint32_t size,
                                                uint32_t row_pitch, uint32_t depth_pitch) {
  if (!command_lists_enabled_ || !command_list_ || !recording_texture_uploads_ ||
      !destination || !data || !size) {
    return false;
  }
  D3D11CommandList& tape = *command_list_;
  auto append_upload = [&]() -> bool {
    const size_t mark = tape.command_count();
    D3D11UploadTexturePacket* packet = tape.Append<D3D11UploadTexturePacket>();
    if (!packet) {
      tape.Truncate(mark);
      return false;
    }
    D3D11ArenaSpan span;
    if (!tape.AppendBytes(data, size, span, "D3D11UploadTexturePacket")) {
      tape.Truncate(mark);
      return false;
    }
    packet->destination = destination;
    packet->destination_subresource = destination_subresource;
    packet->data = span;
    packet->row_pitch = row_pitch;
    packet->depth_pitch = depth_pitch;
    return true;
  };
  if (append_upload()) {
    ++tape_texture_upload_packets_;
    if (REXCVAR_GET(fh1_swap_fetch_trace) && size >= (256u << 10)) {
      static uint32_t upload_logs = 0;
      if (upload_logs < 24) {
        ++upload_logs;
        REXGPU_INFO(
            "FH1 draw tape upload: dest={} sub={} size={} row={} depth={} first={:08X} "
            "packets={} arena={}",
            static_cast<const void*>(destination), destination_subresource, size, row_pitch,
            depth_pitch, size >= 4 ? *static_cast<const uint32_t*>(data) : 0u,
            uint64_t(tape.command_count()), uint64_t(tape.arena_bytes()));
      }
    }
    return true;
  }
  if (!tape.overflowed()) {
    return false;
  }
  static bool overflow_logged = false;
  if (!overflow_logged) {
    overflow_logged = true;
    REXGPU_INFO(
        "D3D11 command lists: texture upload overflowed the tape; submitting at the overflow "
        "point and continuing in a new tape");
  }
  if (!SubmitFrameTape("texture upload overflow")) {
    return false;
  }
  if (!append_upload()) {
    return false;
  }
  ++tape_texture_upload_packets_;
  return true;
}

// Phase 3 M3: capture the state the draw path has just bound and append it with
// the draw arguments. Everything is read back from the immediate context, so
// the packet is a record of what was bound rather than a second description of
// the intended state. The five constant buffers carry the bytes of their last
// record-time update (deferred, not yet written) - the packet is self-contained
// and does not depend on buffer contents between submissions.
bool D3D11CommandProcessor::RecordDrawPacket(const D3D11PipelineCache::D3D11Pipeline& pipeline,
                                             D3D11_PRIMITIVE_TOPOLOGY topology,
                                             uint32_t vertex_count, bool indexed,
                                             ID3D11Buffer* index_buffer,
                                             DXGI_FORMAT index_format,
                                             uint32_t index_buffer_offset,
                                             uint32_t vertex_srv_count, uint32_t pixel_srv_count,
                                             uint32_t vertex_sampler_count,
                                             uint32_t pixel_sampler_count) {
  if (!command_lists_enabled_ || !command_list_ || !recording_draw_ || !vertex_count) {
    return false;
  }
  if (vertex_srv_count > kMaxDrawTextureSrvs || pixel_srv_count > kMaxDrawTextureSrvs ||
      vertex_sampler_count > kMaxDrawSamplers || pixel_sampler_count > kMaxDrawSamplers) {
    ++tape_draw_unrecordable_;
    static bool binding_logged = false;
    if (!binding_logged) {
      binding_logged = true;
      REXGPU_WARN(
          "D3D11 draw tape: a draw binds {}//{} texture slots and {}//{} samplers, more than the "
          "packet holds; leaving draws immediate",
          vertex_srv_count, pixel_srv_count, vertex_sampler_count, pixel_sampler_count);
    }
    return false;
  }
  if (indexed && !index_buffer) {
    ++tape_draw_unrecordable_;
    static bool index_logged = false;
    if (!index_logged) {
      index_logged = true;
      REXGPU_WARN("D3D11 draw tape: an indexed draw has no index buffer; leaving draws immediate");
    }
    return false;
  }
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  if (!context) {
    ++tape_draw_unrecordable_;
    return false;
  }
  D3D11CommandList& tape = *command_list_;
  ConstantBufferBinding* constant_buffers[5] = {
      &cbuffer_binding_system_, &cbuffer_binding_float_vertex_, &cbuffer_binding_float_pixel_,
      &cbuffer_binding_bool_loop_, &cbuffer_binding_fetch_};
  // The tape already holds this draw's texture uploads (RequestTextures ran
  // above); no packet on it changes GPU state other than those uploads, so if
  // the arena or packet space is short, running the tape now is safe and keeps
  // the draw's bound state intact. Without this, a nearly full arena would
  // force the draw back to the immediate path with its uploads still queued.
  size_t needed_arena_bytes = 64;
  for (ConstantBufferBinding* binding : constant_buffers) {
    const size_t upload_size = binding->size ? size_t(binding->size) : binding->cpu_bytes.size();
    needed_arena_bytes += upload_size + D3D11CommandList::kArenaAlignment;
  }
  if (tape.arena_bytes() + needed_arena_bytes > D3D11CommandList::kMaxArenaBytes ||
      tape.command_count() + 2 > D3D11CommandList::kMaxPackets) {
    if (!SubmitFrameTape("draw packet space")) {
      return false;
    }
  }
  bool viewport_missing = false;
  auto append_draw = [&]() -> bool {
    const size_t mark = tape.command_count();
    std::array<D3D11ArenaSpan, 5> spans = {};
    std::vector<uint8_t> padded;    for (size_t i = 0; i < 5; ++i) {
      ConstantBufferBinding* binding = constant_buffers[i];
      if (!binding->buffer || binding->cpu_bytes.empty()) {
        continue;
      }
      // Upload the whole allocation, not the bytes this update used: the
      // immediate path's UpdateSubresource has no box and writes ByteWidth
      // bytes, and the boxed form was measured not to land on this driver for
      // a constant block whose size is not a multiple of 16 (the GPU kept
      // zeros while the packet held the right dwords).
      const size_t upload_size =
          std::max<size_t>(binding->size ? size_t(binding->size) : 0, binding->cpu_bytes.size());
      padded.assign(upload_size, 0);
      std::memcpy(padded.data(), binding->cpu_bytes.data(), binding->cpu_bytes.size());
      if (!tape.AppendBytes(padded.data(), upload_size, spans[i], "D3D11DrawPacket")) {
        tape.Truncate(mark);
        return false;
      }
    }
    D3D11DrawPacket* packet = tape.Append<D3D11DrawPacket>();
    if (!packet) {
      tape.Truncate(mark);
      return false;
    }
    for (size_t i = 0; i < 5; ++i) {
      packet->constant_buffers[i].buffer = constant_buffers[i]->buffer;
      packet->constant_buffers[i].data = spans[i];
    }
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11PixelShader* pixel_shader = nullptr;
    ID3D11GeometryShader* geometry_shader = nullptr;
    context->VSGetShader(&vertex_shader, nullptr, nullptr);
    context->PSGetShader(&pixel_shader, nullptr, nullptr);
    context->GSGetShader(&geometry_shader, nullptr, nullptr);
    packet->vertex_shader.Attach(vertex_shader);
    packet->pixel_shader.Attach(pixel_shader);
    packet->geometry_shader.Attach(geometry_shader);
    ID3D11Buffer* vs_cbs[4] = {};
    ID3D11Buffer* ps_cbs[4] = {};
    context->VSGetConstantBuffers(0, 4, vs_cbs);
    context->PSGetConstantBuffers(0, 4, ps_cbs);
    for (size_t i = 0; i < 4; ++i) {
      packet->vs_constant_buffers[i].Attach(vs_cbs[i]);
      packet->ps_constant_buffers[i].Attach(ps_cbs[i]);
    }
    ID3D11ShaderResourceView* vs_srvs[kMaxDrawTextureSrvs] = {};
    ID3D11ShaderResourceView* ps_srvs[kMaxDrawTextureSrvs] = {};
    if (vertex_srv_count) {
      context->VSGetShaderResources(0, vertex_srv_count, vs_srvs);
    }
    if (pixel_srv_count) {
      context->PSGetShaderResources(0, pixel_srv_count, ps_srvs);
    }
    for (uint32_t i = 0; i < vertex_srv_count; ++i) {
      packet->vs_srvs[i].Attach(vs_srvs[i]);
    }
    for (uint32_t i = 0; i < pixel_srv_count; ++i) {
      packet->ps_srvs[i].Attach(ps_srvs[i]);
    }
    packet->vs_srv_count = vertex_srv_count;
    packet->ps_srv_count = pixel_srv_count;
    ID3D11SamplerState* vs_samplers[kMaxDrawSamplers] = {};
    ID3D11SamplerState* ps_samplers[kMaxDrawSamplers] = {};
    if (vertex_sampler_count) {
      context->VSGetSamplers(0, vertex_sampler_count, vs_samplers);
    }
    if (pixel_sampler_count) {
      context->PSGetSamplers(0, pixel_sampler_count, ps_samplers);
    }
    for (uint32_t i = 0; i < vertex_sampler_count; ++i) {
      packet->vs_samplers[i].Attach(vs_samplers[i]);
    }
    for (uint32_t i = 0; i < pixel_sampler_count; ++i) {
      packet->ps_samplers[i].Attach(ps_samplers[i]);
    }
    packet->vs_sampler_count = vertex_sampler_count;
    packet->ps_sampler_count = pixel_sampler_count;
    ID3D11RenderTargetView* render_targets[kMaxDrawRenderTargets] = {};
    ID3D11DepthStencilView* depth_stencil_view = nullptr;
    context->OMGetRenderTargets(kMaxDrawRenderTargets, render_targets, &depth_stencil_view);
    uint32_t render_target_count = 0;
    for (uint32_t i = 0; i < kMaxDrawRenderTargets; ++i) {
      if (render_targets[i]) {
        render_target_count = i + 1;
      }
      packet->render_targets[i].Attach(render_targets[i]);
    }
    packet->render_target_count = render_target_count;
    packet->depth_stencil_view.Attach(depth_stencil_view);
    ID3D11BlendState* blend_state = nullptr;
    float blend_factor[4] = {};
    uint32_t sample_mask = UINT32_MAX;
    context->OMGetBlendState(&blend_state, blend_factor, &sample_mask);
    packet->blend_state.Attach(blend_state);
    for (size_t i = 0; i < 4; ++i) {
      packet->blend_factor[i] = blend_factor[i];
    }
    packet->sample_mask = sample_mask;
    ID3D11DepthStencilState* depth_stencil_state = nullptr;
    uint32_t stencil_ref = 0;
    context->OMGetDepthStencilState(&depth_stencil_state, &stencil_ref);
    packet->depth_stencil_state.Attach(depth_stencil_state);
    packet->stencil_ref = stencil_ref;
    ID3D11RasterizerState* rasterizer_state = nullptr;
    context->RSGetState(&rasterizer_state);
    packet->rasterizer_state.Attach(rasterizer_state);
    UINT viewport_count = 1;
    context->RSGetViewports(&viewport_count, &packet->viewport);
    UINT scissor_count = 1;
    context->RSGetScissorRects(&scissor_count, &packet->scissor);
    // A draw recorded with no viewport set would replay with a zero-sized
    // viewport (Direct3D 11 would otherwise use the default full-target one) and
    // silently lose its geometry. Measured, not assumed: count it and leave the
    // draw immediate.
    if (!packet->viewport.Width || !packet->viewport.Height) {
      tape.Truncate(mark);
      viewport_missing = true;
      return false;
    }
    packet->topology = topology;
    packet->vertex_count = vertex_count;
    packet->indexed = indexed;
    if (indexed) {
      packet->index_buffer = index_buffer;
      packet->index_format = index_format;
      packet->index_buffer_offset = index_buffer_offset;
    }
    if (REXCVAR_GET(fh1_swap_fetch_trace)) {
      static uint32_t packet_logs = 0;
      if (vertex_count >= 512 && packet_logs < 24) {
        ++packet_logs;
        std::string blobs;
        for (size_t i = 0; i < 5; ++i) {
          const D3D11DrawConstantBuffer& cb = packet->constant_buffers[i];
          const uint8_t* bytes = tape.Data(cb.data);
          blobs += fmt::format(" {}:{:08X}", cb.data.size,
                               bytes && cb.data.size >= 4
                                   ? *reinterpret_cast<const uint32_t*>(bytes)
                                   : 0u);
        }
        Microsoft::WRL::ComPtr<ID3D11Resource> target_resource;
        if (packet->render_target_count) {
          packet->render_targets[0]->GetResource(&target_resource);
        }
        REXGPU_INFO(
            "FH1 draw packet: vtx={} indexed={} topo={} cbs=[{} ] vs={} ps={} srvs={}/{} "
            "samplers={}/{} targets={} dsv={} target={} vp={},{} {}x{} scissor={},{} {}x{}",
            vertex_count, indexed ? 1 : 0, uint32_t(topology), blobs,
            packet->vertex_shader ? 1 : 0, packet->pixel_shader ? 1 : 0, packet->vs_srv_count,
            packet->ps_srv_count, packet->vs_sampler_count, packet->ps_sampler_count,
            packet->render_target_count, packet->depth_stencil_view ? 1 : 0,
            static_cast<const void*>(target_resource.Get()),
            uint32_t(packet->viewport.TopLeftX), uint32_t(packet->viewport.TopLeftY),
            uint32_t(packet->viewport.Width), uint32_t(packet->viewport.Height),
            packet->scissor.left, packet->scissor.top, packet->scissor.right,
            packet->scissor.bottom);
      }
    }
    return true;
  };
  if (append_draw()) {
    ++tape_draw_packets_;
    return true;
  }
  if (viewport_missing) {
    ++tape_draw_empty_viewport_;
    return false;
  }
  if (!tape.overflowed()) {
    ++tape_draw_unrecordable_;
    return false;
  }
  static bool overflow_logged = false;
  if (!overflow_logged) {
    overflow_logged = true;
    REXGPU_WARN(
        "D3D11 command lists: a draw overflowed the tape; submitting at the overflow point and "
        "continuing in a new tape (this must not occur in a normal run)");
  }
  if (!SubmitFrameTape("draw packet overflow")) {
    return false;
  }
  if (!append_draw()) {
    ++tape_draw_fallbacks_;
    // The constants of this draw were deferred; the immediate fallback must
    // write them itself, or the draw would read whatever the buffer held.
    FlushDeferredConstantBuffers();
    return false;
  }
  ++tape_draw_packets_;
  return true;
}

bool D3D11CommandProcessor::IssueCopy() {
  // Phase 4: the resolve path between the native EDRAM surfaces and the
  // guest-memory mirror, at the seam the Direct3D 12 command processor uses
  // (IssueCopy -> the executor's NativeResolve). Plan the guest's copy from
  // the registers and run one SM5.0 compute resolve per source surface into
  // the mirror; Phase 5's FH1 native executor takes over the run-ordered
  // passes around it (clears, ownership transfers, scaled targets, readback).
  const bool trace_band2 = current_draw_band_trace_ && current_draw_tiling_pass_ == 1;
  if (!native_surfaces_ || !native_resolve_) {
    if (trace_band2) {
      REXGPU_ERROR("FH1 band2 ERROR: D3D11 resolve unavailable (native surfaces or resolver)");
    }
    return true;
  }
  const uint32_t untile_band =
      native_executor_ && native_executor_->untile_height()
          ? Fh1D3D11UntileBand(int32_t(
                register_file_->Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset))
          : 0u;
  if (untile_band > 0xFFFFu ||
      (untile_band && untile_band >= native_executor_->untile_height())) {
    REXGPU_ERROR("D3D11 FH1 resolve: band {} exceeds the supported untile height {}",
                 untile_band, native_executor_->untile_height());
    return false;
  }
  if (native_executor_ && !untile_band && native_executor_->has_untile_bands()) {
    if (command_lists_enabled_ && !SubmitFrameTape("untile copy-down ordering")) return false;
    if (!native_executor_->EndUntiledTiling()) return false;
  }
  const Fh1ResolveFlags flags{host_render_config_->depth_float24_round(),
                              host_render_config_->gamma_render_target_as_unorm16(),
                              host_render_config_->IsFixed16TruncatedToMinus1To1()};
  Fh1ResolvePlan plan;
  const bool resolve_plan_succeeded = Fh1PlanResolve(*register_file_, *memory_, flags, plan);

  // Forensics (H-RES-1/H-RES-2): every planning failure carries a stable RF id,
  // and every native resolve records its rectangle and destination.
  if (rex::graphics::render_forensics::Active()) {
    rex::graphics::render_forensics::NoteResolvePlan(
        plan.resolve_key.base_tiles, static_cast<uint8_t>(plan.failure), plan.skip, plan.x0,
        plan.y0, plan.x1, plan.y1);
    if (resolve_plan_succeeded && plan.copy) {
      rex::graphics::render_forensics::NoteResolveEntry(
          plan.resolve_key.base_tiles, plan.x0, plan.y0, plan.x1, plan.y1, plan.msaa,
          plan.sample_select, plan.dest_base, plan.dest_pitch);
    }
  }
  if (REXCVAR_GET(d3d11_band_trace)) {
    REXGPU_INFO(
        "D3D11 band trace resolve: frame={} band={} rect={},{}-{},{} key={} "
        "clear_color={} clear_depth={}",
        presented_output_frames_, untile_band, plan.x0, plan.y0, plan.x1, plan.y1,
        plan.resolve_key.Describe(), plan.info.IsClearingColor() ? 1 : 0,
        plan.info.IsClearingDepth() ? 1 : 0);
  }
  // FH1 call logger (Phase 2): resolve/copy plan outcome. Placed here
  // (right after Fh1PlanResolve, BEFORE the plan-failure early return)
  // so EVERY copy-mode draw gets an A6 record, including the ones
  // whose plan fails (copy=0 with the skip reason) — the first version
  // sat after the early return and missed those (swap-3000: 31
  // tile-path copies but only 25 A6 lines). The `resolve=` field is
  // resolve_call_count_, which only increments for successful
  // resolves, so a failed plan's A6 line shares the previous
  // successful call's number; failed plans are distinguished by
  // copy=0 (and by having no resolve-trace line).
  if (Fh1CallogEnabled(presented_output_frames_)) {
    REXGPU_INFO(
        "FH1 callog side=A swap={} kind=copy resolve={} copy={} skip={} pkind={} "
        "copying_depth={} src={} dest={:08X} dest_extent={:08X}+{}",
        presented_output_frames_, resolve_call_count_, plan.copy ? 1 : 0,
        plan.skip ? plan.skip : "none", plan.kind, plan.copying_depth ? 1 : 0,
        plan.resolve_key.Describe(), plan.dest_base,
        plan.info.copy_dest_extent_start, plan.info.copy_dest_extent_length);
  }
  if (!resolve_plan_succeeded) {
    if (trace_band2) {
      const RegisterFile& regs = *register_file_;
      const auto fetch = regs.GetVertexFetch(0);
      const auto copy_control = regs.Get<reg::RB_COPY_CONTROL>();
      const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
      const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
      const auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
      const auto mode_control = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
      REXGPU_ERROR(
          "FH1 band2 ERROR: D3D11 resolve planning failed failure_id={} swap={} call={} "
          "pass={} window_y={} "
          "vtx_window_offset={} fetch_type={} fetch_size={} fetch_address={:08X} "
          "fetch_endian={} copy_control={:08X} copy_src={} surface_info={:08X} "
          "depth_info={:08X} color_info_0={:08X} dest_base={:08X} dest_pitch={:08X} "
          "dest_info={:08X} empty={}",
          uint32_t(plan.failure), presented_output_frames_, resolve_call_count_,
          current_draw_tiling_pass_, int32_t(window_offset.window_y_offset),
          mode_control.vtx_window_offset_enable, uint32_t(fetch.type), fetch.size, fetch.address,
          uint32_t(fetch.endian), copy_control.value, uint32_t(copy_control.copy_src_select),
          surface_info.value, depth_info.value, regs[XE_GPU_REG_RB_COLOR_INFO],
          regs[XE_GPU_REG_RB_COPY_DEST_BASE], regs[XE_GPU_REG_RB_COPY_DEST_PITCH],
          regs[XE_GPU_REG_RB_COPY_DEST_INFO], plan.empty);
    }
    // Nothing to copy or clear: Xenos does no work either.
    return plan.empty;
  }
  if (trace_band2) {
    REXGPU_INFO(
        "FH1 band2: D3D11 resolve plan call={} copy={} skip={} rect={},{}-{},{} "
        "dest={:08X} extent={:08X}+{} key={} window_y={}",
        resolve_call_count_, plan.copy ? 1 : 0, plan.skip ? plan.skip : "none", plan.x0, plan.y0,
        plan.x1, plan.y1, plan.dest_base, plan.info.copy_dest_extent_start,
        plan.info.copy_dest_extent_length, plan.resolve_key.Describe(),
        int32_t(register_file_->Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset));
  }
  const bool resolve_clear_requested =
      plan.info.IsClearingColor() || plan.info.IsClearingDepth();
  auto apply_resolve_clears = [&]() {
    if (!resolve_clear_requested) return true;
    if (!native_executor_) {
      REXGPU_ERROR("D3D11 FH1 native resolve: resolve clear requested without a native executor");
      return false;
    }
    // Clears execute on the immediate context. Submit any queued draws (and
    // resolve dispatches) first so the clear remains after the guest's copy.
    if (command_lists_enabled_ && !SubmitFrameTape("resolve clear ordering")) return false;
    bool succeeded = true;
    if (plan.info.IsClearingColor()) {
      succeeded &= native_executor_->ResolveClear(
          Fh1SurfaceKey::Color(plan.color_info.color_base, plan.pitch_tiles, plan.msaa,
                               plan.color_info.color_format,
                               host_render_config_->gamma_render_target_as_unorm16()),
          plan.x0, plan.y0, plan.x1, plan.y1, plan.info.rb_color_clear,
          plan.info.rb_color_clear_lo, untile_band);
    }
    if (plan.info.IsClearingDepth()) {
      succeeded &= native_executor_->ResolveClear(
          Fh1SurfaceKey::Depth(plan.depth_info.depth_base, plan.pitch_tiles, plan.msaa,
                               plan.depth_info.depth_format),
          plan.x0, plan.y0, plan.x1, plan.y1, plan.info.rb_depth_clear, 0, untile_band);
    }
    return succeeded;
  };
  // Bounded resolve-clear evidence for one selected presented frame.
  const int32_t clear_trace_swap = REXCVAR_GET(fh1_debug_tile_trace_swap);
  if (!plan.empty && clear_trace_swap > 0 &&
      presented_output_frames_ == uint64_t(clear_trace_swap)) {
    static uint64_t clear_trace_last_swap = UINT64_MAX;
    static uint32_t clear_trace_records = 0;
    if (clear_trace_last_swap != presented_output_frames_) {
      clear_trace_last_swap = presented_output_frames_;
      clear_trace_records = 0;
    }
    if (clear_trace_records < 256) {
      ++clear_trace_records;
      const auto& copy_control = plan.info.rb_copy_control;
      REXGPU_INFO(
          "FH1 resolve clear flags: swap={} resolve={} copy={} skip={} kind={} "
          "rect={},{}-{},{} copy_control={:08X} clear_color={} clear_depth={} "
          "copying_depth={} color_base={} depth_base={} color_clear={:08X} "
          "color_clear_lo={:08X} depth_clear={:08X}",
          presented_output_frames_, resolve_call_count_, plan.copy ? 1 : 0,
          plan.skip ? plan.skip : "none", plan.kind, plan.x0, plan.y0, plan.x1, plan.y1,
          copy_control.value, plan.info.IsClearingColor() ? 1 : 0,
          plan.info.IsClearingDepth() ? 1 : 0, plan.copying_depth ? 1 : 0,
          uint32_t(plan.color_info.color_base), uint32_t(plan.depth_info.depth_base),
          plan.info.rb_color_clear,
          plan.info.rb_color_clear_lo, plan.info.rb_depth_clear);
    }
  }
  if (!plan.copy) {
    apply_resolve_clears();
    REXGPU_WARN("D3D11 FH1 native resolve: skipped ({})", plan.skip ? plan.skip : "unknown");
    return false;
  }
  auto* upload_sink =
      static_cast<D3D11SharedMemoryDeviceUploadSink*>(shared_memory_->upload_sink());
  // fh1_debug_resolve_small_uav: the same dispatch, the same shader, the same
  // binds - only the destination view differs, so a marker that lands there and
  // not in the 512 MB mirror convicts the mirror view's size rather than the
  // resolve code.
  const bool small_uav_probe = REXCVAR_GET(fh1_debug_resolve_small_uav);
  constexpr uint32_t kWindowProbeStart = 0x08000000u;
  const int32_t window_probe_mb = REXCVAR_GET(fh1_debug_resolve_window_mb);
  ID3D11UnorderedAccessView* memory_target = nullptr;
  // Production path: the destination stores go through the resolve staging
  // buffer, not the mirror. Measured on the GTX 570 / driver 391.35: a store at
  // offset 0 of the 512 MB mirror's raw UAV lands, a store at the front
  // buffer's guest address (0x1BDB1000, ~470 MB) never does, and a raw UAV over
  // a mirror window cannot even be created (E_INVALIDARG for any
  // FirstElement != 0). So the dispatch writes the extent into a small staging
  // buffer whose stores are known to land, and the extent is then copied into
  // the mirror at its guest address with CopySubresourceRegion - a copy-engine
  // operation, with no shader-address limit.
  bool use_staging_copy = false;
  if (small_uav_probe) {
    memory_target = upload_sink->GetProbeBufferUAV();
  } else if (window_probe_mb > 0) {
    const uint32_t window_bytes = std::min<uint32_t>(uint32_t(window_probe_mb) << 20,
                                                     SharedMemory::kBufferSize - kWindowProbeStart);
    memory_target = upload_sink->GetHostBufferUAVWindow(kWindowProbeStart, window_bytes);
  } else if (REXCVAR_GET(fh1_debug_resolve_direct_mirror)) {
    memory_target = upload_sink->GetHostBufferUAV();
  } else {
    memory_target = upload_sink->EnsureResolveStaging(plan.info.copy_dest_extent_length);
    use_staging_copy = memory_target != nullptr;
    if (!memory_target) {
      // Extent larger than the staging cap (or its creation failed): fall back
      // to the whole-mirror view, logged once below.
      memory_target = upload_sink->GetHostBufferUAV();
    }
  }
  if (!memory_target) {
    REXGPU_ERROR("D3D11 FH1 native resolve: no raw UAV over the guest-memory mirror");
    return false;
  }
  {
    static uint32_t target_logs = 0;
    if (REXCVAR_GET(fh1_swap_fetch_trace) && target_logs < 6) {
      ++target_logs;
      REXGPU_INFO("FH1 resolve target: call={} staging={} small_uav={} window_mb={} len={}",
                  resolve_call_count_, use_staging_copy ? 1 : 0, small_uav_probe ? 1 : 0,
                  window_probe_mb, plan.info.copy_dest_extent_length);
    }
  }
  // The destination extent must be current in the mirror before the write,
  // and its cached textures invalidated after it (the D3D12 executor does
  // both around the resolve). In a recorded run the upload becomes UploadBuffer
  // packets on the tape (Phase 2 H42), so it stays ordered with the dispatch
  // that follows; every upload outside this scope still executes immediately.
  if (command_lists_enabled_ && upload_sink && command_list_) {
    upload_sink->SetUploadRecorder(command_list_.get());
    shared_memory_->RequestRange(plan.info.copy_dest_extent_start,
                                 plan.info.copy_dest_extent_length);
    upload_sink->ClearUploadRecorder();
  } else {
    shared_memory_->RequestRange(plan.info.copy_dest_extent_start,
                                 plan.info.copy_dest_extent_length);
  }
  // The draw path leaves the shared-memory mirror itself bound as a shader
  // resource (VS/PS t0) for in-shader guest fetches. Direct3D 11 will not host
  // one resource as an SRV and a UAV at the same time: binding the mirror's
  // UAV then makes the runtime drop it silently, and every store of the
  // resolve's destination vanishes - the defect this path had (a marker store
  // placed at the top of the resolve shader never reached the mirror, while
  // nothing in the logs reported a failure). Unbind the draw stages' view
  // first; every draw binds it again.
  {
    ID3D11ShaderResourceView* no_shared_memory_view = nullptr;
    ID3D11DeviceContext* binding_context = GetD3D11Provider().GetImmediateContext();
    binding_context->VSSetShaderResources(0, 1, &no_shared_memory_view);
    binding_context->PSSetShaderResources(0, 1, &no_shared_memory_view);
  }
  D3D11Fh1NativeResolve::SourceRect source;
  source.rect = {plan.x0, plan.y0, plan.x1, plan.y1};
  std::vector<D3D11Fh1NativeResolve::SourceRect> resolve_sources;
  const size_t surfaces_before = native_surfaces_->surface_count();
  const bool resolve_owner_was_draw_target =
      ever_bound_render_target_keys_.count(plan.resolve_key.Pack()) != 0;
  // Split using the same tile geometry and owner table as D3D12/Vulkan. Each
  // dispatch keeps its source and destination pixel coordinates, so a change
  // of owner in the middle of the copy cannot make one surface supply stale
  // pixels for another owner's tiles.
  D3D11Fh1NativeSurface* source_by_owner = nullptr;
  if (native_executor_) {
    const uint32_t tile_width = Fh1D3D11SurfaceTileWidth(plan.resolve_key);
    const uint32_t tile_height =
        xenos::kEdramTileHeightSamples >>
        uint32_t(plan.msaa >= uint32_t(xenos::MsaaSamples::k2X));
    const uint32_t tile_pitch = Fh1D3D11SurfaceTilePitch(plan.resolve_key);
    for (const Fh1EdramTiles::OwnerRect& rect :
         native_executor_->tiles().SplitByOwner(plan.resolve_key.base_tiles, tile_pitch,
                                                int32_t(tile_width), int32_t(tile_height),
                                                plan.x0, plan.y0, plan.x1, plan.y1)) {
      D3D11Fh1NativeResolve::SourceRect region;
      region.rect = {rect.left, rect.top, rect.right, rect.bottom};
      if (rect.owner == Fh1EdramTiles::kNoOwner) {
        // Unclaimed tiles have no alternate owner, so the resolve key's
        // surface remains their source.
        region.surface = native_surfaces_->GetOrCreateSurface(plan.resolve_key);
      } else {
        region.surface = native_surfaces_->FindSurface(rect.owner);
      }
      bool source_rows_valid = true;
      if (region.surface &&
          (rect.bottom < 0 ||
           uint64_t(rect.bottom) + untile_band > region.surface->image_height)) {
        REXGPU_ERROR("D3D11 FH1 resolve: source rows {}..{} exceed {} for owner 0x{:08X}",
                     rect.top + int32_t(untile_band),
                     rect.bottom + int32_t(untile_band), region.surface->image_height,
                     rect.owner);
        region.surface = nullptr;
        source_rows_valid = false;
      }
      if (!region.surface && source_rows_valid) {
        REXGPU_ERROR("D3D11 FH1 resolve: source surface for owner 0x{:08X} unavailable "
                     "for rect {},{}-{},{}",
                     rect.owner, rect.left, rect.top, rect.right, rect.bottom);
      } else if (!source_by_owner && rect.owner != Fh1EdramTiles::kNoOwner &&
                 rect.owner != plan.resolve_key.Pack()) {
        source_by_owner = region.surface;
      }
      resolve_sources.push_back(region);
    }
  } else {
    D3D11Fh1NativeResolve::SourceRect region;
    region.surface = native_surfaces_->GetOrCreateSurface(plan.resolve_key);
    region.rect = source.rect;
    resolve_sources.push_back(region);
  }
  source.surface = source_by_owner ? source_by_owner
                                   : native_surfaces_->GetOrCreateSurface(plan.resolve_key);
  if (source_by_owner) {
    native_executor_->OwnerResolveIssued();
    static uint32_t owner_source_logs = 0;
    if (REXCVAR_GET(fh1_swap_fetch_trace) && owner_source_logs < 8) {
      ++owner_source_logs;
      REXGPU_INFO("FH1 resolve source by ownership: {} for {} ({}x{})",
                  source_by_owner->key.Describe(), plan.resolve_key.Describe(),
                  source_by_owner->width, source_by_owner->height);
    }
  }
  if (source.surface) {
    last_resolve_source_key_ = source.surface->key;
    last_resolve_source_swap_ = presented_output_frames_;
  }
  if (REXCVAR_GET(fh1_swap_fetch_trace) && plan.info.copy_dest_extent_length >= (1u << 20)) {
    REXGPU_INFO(
        "FH1 resolve key: {} rect={},{}-{},{} extent={:08X}+{} surfaces {}->{} ({} {}) "
        "rt_bound={} info={:08X} samples={} sample_select={:08X} src={} by_owner={}",
        plan.resolve_key.Describe(), plan.x0, plan.y0, plan.x1, plan.y1,
        plan.info.copy_dest_extent_start, plan.info.copy_dest_extent_length, surfaces_before,
        native_surfaces_->surface_count(), plan.kind, plan.copying_depth ? "depth" : "color",
        resolve_owner_was_draw_target ? 1 : 0, plan.dest_info,
        source.surface ? source.surface->samples : 0, plan.sample_select,
        source.surface ? source.surface->key.Describe() : "none", source_by_owner ? 1 : 0);
    // Is the surface the resolve samples holding anything at all? Decides
    // between "the draws never reach this surface" and "the resolve shader
    // writes nothing" when the readback comes back black.
    if (source.surface && source.surface->texture && !plan.copying_depth &&
        source.surface->samples == 1) {
      // The surface was written by draws that may still be packets on the tape.
      DrainTapeForDiagnostic("resolve source probe");
      ID3D11Device* device = GetD3D11Provider().GetDevice();
      ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
      D3D11_TEXTURE2D_DESC desc;
      source.surface->texture->GetDesc(&desc);
      D3D11_TEXTURE2D_DESC staging_desc = desc;
      staging_desc.Usage = D3D11_USAGE_STAGING;
      staging_desc.BindFlags = 0;
      staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      staging_desc.MiscFlags = 0;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
      if (SUCCEEDED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
        context->CopyResource(staging.Get(), source.surface->texture);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
          const uint8_t* row = static_cast<const uint8_t*>(mapped.pData);
          uint64_t nonzero_bytes = 0;
          // Content inside the rectangle this resolve reads, and content below
          // it - separates "the surface is blank" from "the surface holds the
          // frame at a row offset the rectangle misses" (predicated tiling).
          uint64_t nonzero_in_rect = 0, nonzero_below_rect = 0;
          const uint32_t rect_bottom = std::min<uint32_t>(desc.Height, plan.y1);
          // Per-60-row band profile at the RESOLVE INSTANT. The presented-pair
          // dump reads this same surface again at the swap; if the frame's
          // halves are drawn after their own resolve, the bands that are empty
          // here are full there, and nothing else can produce that difference.
          const uint32_t band_count = (rect_bottom + 59u) / 60u;
          std::vector<uint64_t> band_nonzero(band_count, 0), band_total(band_count, 0);
          for (uint32_t y = 0; y < desc.Height; y += 7) {
            const uint32_t* texels = reinterpret_cast<const uint32_t*>(row + size_t(mapped.RowPitch) * y);
            for (uint32_t x = 0; x < desc.Width; x += 16) {
              nonzero_bytes += texels[x] != 0;
              // Colour, not alpha: an 8_8_8_8 composite's unused half is
              // 0xC0000000 (RGB 0, alpha 192), so counting whole words reports
              // a black half as full - which is exactly what this probe did
              // before it was corrected.
              const bool has_colour = (texels[x] & 0x00FFFFFFu) != 0;
              if (y < rect_bottom) {
                nonzero_in_rect += has_colour;
                const uint32_t band = y / 60u;
                ++band_total[band];
                band_nonzero[band] += has_colour;
              } else {
                nonzero_below_rect += has_colour;
              }
            }
          }
          REXGPU_INFO(
              "FH1 resolve source surface: {}x{} texture={} nonzero[texels sampled]={} first={:08X} "
              "in_rect={} below_rect={} rect_bottom={}",
              desc.Width, desc.Height, static_cast<const void*>(source.surface->texture), nonzero_bytes,
              reinterpret_cast<const uint32_t*>(row)[0], nonzero_in_rect, nonzero_below_rect,
              rect_bottom);
          std::string band_text;
          for (uint32_t band = 0; band < band_count; ++band) {
            if (!band_text.empty()) band_text += ' ';
            band_text += fmt::format(
                "{:.3f}", band_total[band] ? double(band_nonzero[band]) / double(band_total[band])
                                           : 0.0);
          }
          REXGPU_INFO("FH1 resolve source bands: swap={} key={} rect_bottom={} nz=[{}]",
                      presented_output_frames_, plan.resolve_key.Describe(), rect_bottom,
                      band_text);
          context->Unmap(staging.Get(), 0);
        }
      }
    }
  }
  // fh1_debug_resolve_trace (PHASE 12): the frame's own producer record. A
  // guest range can hold more than one texture over one frame (a pool the game
  // reuses - a 640x360 colour target, a 320x192 one and a 520x520 shadow map
  // all start at 1CA95000 in FH1), so which resolve wrote a sampled texture
  // last is decided by this order, not by the addresses alone. Logged before
  // the dispatch, so the record is one line per copy in issue order.
  {
    const int64_t resolve_trace_from = REXCVAR_GET(fh1_debug_resolve_trace);
    if (resolve_trace_from > 0 && presented_output_frames_ >= uint64_t(resolve_trace_from)) {
      static uint32_t resolve_trace_logs = 0;
      if (resolve_trace_logs < 4000) {
        ++resolve_trace_logs;
        const uint32_t dest_bpp_log2 = (plan.dest_info >> 16) & 3;
        REXGPU_INFO(
            "FH1 resolve trace: swap={} call={} key={} rect={},{}-{},{} dest_base={:08X} "
            "dest_pitch={} info={:08X} pack={} endian={} bytes={} extent={:08X}+{} src={} "
            "src_fmt={} owner={} staging={}",
            presented_output_frames_, resolve_call_count_, plan.resolve_key.Describe(), plan.x0,
            plan.y0, plan.x1, plan.y1, plan.dest_base, plan.dest_pitch, plan.dest_info,
            plan.dest_info & 7u, (plan.dest_info >> 3u) & 7u,
            uint32_t((plan.x1 - plan.x0) * (plan.y1 - plan.y0)) << dest_bpp_log2,
            plan.info.copy_dest_extent_start, plan.info.copy_dest_extent_length,
            source.surface ? source.surface->key.Describe() : "none",
            source.surface ? source.surface->key.format : 0u, source_by_owner ? 1 : 0,
            use_staging_copy ? 1 : 0);
      }
    }
  }
  // fh1_debug_resolve_alias_base (PHASE 13): one guest address can hold the
  // 640x360 8_8_8_8 texture the composite samples and, in the same frame, a
  // 520x520 depth copy and a 320x192 2_10_10_10 copy of other surfaces (the
  // title's pool reuses the range). Which one a fetch sees is then decided by
  // issue order. With a base named, only a resolve whose destination starts at
  // that base and reproduces the fetch's own writer (pack 0, 640x360, pitch
  // 640) may write it; the base's other writers are skipped and logged - the
  // census line above still records every one, so the swap keeps its full
  // writer list. The range is deliberately not the test: the mip chain of a
  // 320x192 surface is allocated inside the 640x360 extent (1CAD1000 and up),
  // and those resolves are live, unrelated surfaces - skipping them stalls the
  // title before free roam (measured, run p14).
  {
    const std::string alias_base_text = REXCVAR_GET(fh1_debug_resolve_alias_base);
    char* alias_base_end = nullptr;
    const uint64_t alias_base = alias_base_text.empty()
                                    ? 0ull
                                    : std::strtoull(alias_base_text.c_str(), &alias_base_end, 16);
    const int64_t alias_bytes_cvar = REXCVAR_GET(fh1_debug_resolve_alias_bytes);
    const uint64_t alias_bytes = alias_bytes_cvar > 0 ? uint64_t(alias_bytes_cvar) : 0ull;
    const uint64_t extent_start = plan.info.copy_dest_extent_start;
    const uint64_t extent_length = plan.info.copy_dest_extent_length;
    const bool alias_at_base =
        !alias_base_text.empty() && alias_base_end != alias_base_text.c_str() && alias_bytes &&
        plan.dest_base == alias_base && extent_length &&
        extent_start < alias_base + alias_bytes && extent_start + extent_length > alias_base;
    const bool is_texture_writer =
        plan.dest_base == alias_base && (plan.dest_info & 7u) == 0u && plan.x0 == 0 &&
        plan.y0 == 0 && plan.x1 == 640 && plan.y1 == 360 && plan.dest_pitch == 640;
    // fh1_debug_resolve_alias_mode: 0 = every base writer, 1 = the depth
    // writer only, 2 = the colour writers only - which of the base's scratch
    // copies the title's next consumer actually needs (the menus stall when
    // either class is skipped, so the two are separated for the measurement).
    const int32_t alias_mode = REXCVAR_GET(fh1_debug_resolve_alias_mode);
    const bool alias_kind_matches = alias_mode == 0 ||
                                    (alias_mode == 1 && (plan.dest_info & 7u) == 4u) ||
                                    (alias_mode == 2 && (plan.dest_info & 7u) != 4u);
    // fh1_debug_resolve_alias_from_swap: let the pre-world frames through (the
    // title needs the c3 copy to boot - measured, run p15) and guard only from
    // this presented-frame index, so a live world swap can be observed with the
    // base reduced to its 640x360 writer. 0 = guard from the first resolve.
    const int64_t alias_from_swap_cvar = REXCVAR_GET(fh1_debug_resolve_alias_from_swap);
    const uint64_t alias_from_swap = alias_from_swap_cvar > 0 ? uint64_t(alias_from_swap_cvar) : 0ull;
    if (alias_at_base && alias_kind_matches && !is_texture_writer &&
        presented_output_frames_ >= alias_from_swap) {
      REXGPU_INFO(
          "FH1 resolve alias guard: skipped call={} key={} rect={},{}-{},{} dest={:08X} "
          "pitch={} pack={} extent={:08X}+{} - not the {}-byte 640x360 8_8_8_8 writer of "
          "{:08X}",
          resolve_call_count_, plan.resolve_key.Describe(), plan.x0, plan.y0, plan.x1, plan.y1,
          plan.dest_base, plan.dest_pitch, plan.dest_info & 7u, plan.info.copy_dest_extent_start,
          plan.info.copy_dest_extent_length, alias_bytes, uint32_t(alias_base));
      const bool clear_ok = apply_resolve_clears();
      ++resolve_call_count_;
      return clear_ok;
    }
  }
  // fh1_swap_fetch_trace: does this resolve's dispatch change the GPU mirror
  // at all? The extent's mirror contents before the dispatch are the guest
  // bytes RequestRange just uploaded, so comparing them with the contents
  // after separates "the store fired and wrote zeros" from "the store never
  // fired" - a readback of zeros alone cannot tell those apart. The gate is
  // 512 KiB, not a mebibyte: the 640x360 front buffer's own copy lands 983,040
  // bytes, so a 1 MiB floor silenced exactly the copy whose ordering matters.
  // The window is a presented-frame index (default 0 = the session's first
  // probes), not the first 40 resolves, for the same reason - that copy is
  // call ~12,000.
  static uint32_t store_probe_logs = 0;
  const int64_t store_probe_from = REXCVAR_GET(fh1_debug_resolve_store_probe_from);
  const bool trace_store_probe =
      REXCVAR_GET(fh1_swap_fetch_trace) && plan.info.copy_dest_extent_length >= (1u << 19) &&
      presented_output_frames_ >= uint64_t(store_probe_from) && store_probe_logs < 160;
  auto mirror_hash = [&](uint32_t start, uint32_t length, uint64_t& hash_out,
                         uint64_t& nonzero_out) -> bool {
    if (!length || start >= SharedMemory::kBufferSize ||
        length > SharedMemory::kBufferSize - start) {
      return false;
    }
    // The staging copy must not race the dispatch it is measuring: a Map on a
    // staging buffer only waits for its own copy, not for the compute work
    // queued before it (measured: without this, "after" equalled "before"
    // even when a later, synchronised read saw the resolve's output).
    if (!WaitForGpu()) {
      return false;
    }
    resolve_readback_scratch_.resize(length);
    if (!upload_sink->ReadbackFromHostBuffer(start, resolve_readback_scratch_.data(), length)) {
      return false;
    }
    hash_out = 1469598103934665603ull;
    nonzero_out = 0;
    const uint8_t* bytes = resolve_readback_scratch_.data();
    for (size_t i = 0; i < length; i += 16) {
      hash_out = (hash_out ^ bytes[i]) * 1099511628211ull;
      nonzero_out += bytes[i] != 0;
    }
    return true;
  };
  if (trace_store_probe) {
    // The destination extent's guest bytes were requested above; on the record
    // path that upload is a packet, so the "before" fingerprint must be taken
    // after the tape has run it (and the draws ordered before it).
    DrainTapeForDiagnostic("resolve store probe upload");
  }
  uint64_t mirror_before_hash = 0;
  uint64_t mirror_before_nonzero = 0;
  const bool mirror_before_ok =
      trace_store_probe && mirror_hash(plan.info.copy_dest_extent_start,
                                       plan.info.copy_dest_extent_length, mirror_before_hash,
                                       mirror_before_nonzero);
  // FH1 resolve probe (Phase 2f): does the coloured EDRAM content make it
  // into the resolved texture? For the first few EDRAM (0/32/4x/c3) band-2
  // resolves at/after fh1_debug_resolve_probe_from, read the mirror at the
  // resolve destination extent before the resolve, and again after it (below).
  // The before/after byte-lane means + sample words distinguish "the dispatch
  // wrote coloured pixels" from "the mirror stayed grey/zero". Purely
  // diagnostic: readbacks + log lines only.
  bool resolve_probe_pending = false;
  uint32_t resolve_probe_index = 0;
  uint32_t resolve_probe_extent_start = 0;
  uint32_t resolve_probe_extent_length = 0;
  uint32_t resolve_probe_dest_base = 0;
  uint32_t resolve_probe_dest_pitch = 0;
  uint32_t resolve_probe_dest_info = 0;
  double resolve_probe_before_mean[4] = {0.0, 0.0, 0.0, 0.0};
  uint64_t resolve_probe_before_words_nz = 0;
  uint32_t resolve_probe_before_first = 0;
  bool resolve_probe_before_ok = false;
  {
    const bool resolve_probe_gate =
        presented_output_frames_ >=
            uint64_t(std::max<int32_t>(REXCVAR_GET(fh1_debug_resolve_probe_from), 0)) &&
        !plan.copying_depth && !plan.resolve_key.is_depth &&
        plan.resolve_key.base_tiles == 0 && plan.resolve_key.pitch_tiles == 32 &&
        plan.resolve_key.msaa == uint32_t(xenos::MsaaSamples::k4X) && untile_band == 2 &&
        plan.info.copy_dest_extent_length > 0 &&
        plan.info.copy_dest_extent_start < SharedMemory::kBufferSize &&
        plan.info.copy_dest_extent_length <=
            SharedMemory::kBufferSize - plan.info.copy_dest_extent_start;
    if (resolve_probe_gate) {
      static uint32_t resolve_probe_count = 0;
      const int32_t resolve_probe_budget = REXCVAR_GET(fh1_debug_resolve_probe_draws);
      if (resolve_probe_budget > 0 && resolve_probe_count < uint32_t(resolve_probe_budget)) {
        // Read the mirror at the destination extent before the resolve.
        // (WaitForGpu first: a Map only waits for its own copy, not the
        // compute work queued before it - same reason as mirror_hash.)
        if (WaitForGpu()) {
          resolve_readback_scratch_.resize(plan.info.copy_dest_extent_length);
          if (upload_sink->ReadbackFromHostBuffer(plan.info.copy_dest_extent_start,
                                                  resolve_readback_scratch_.data(),
                                                  plan.info.copy_dest_extent_length)) {
            const uint8_t* bytes = resolve_readback_scratch_.data();
            const size_t len = plan.info.copy_dest_extent_length;
            uint64_t lane_sum[4] = {0, 0, 0, 0};
            uint64_t lane_count[4] = {0, 0, 0, 0};
            for (size_t i = 0; i < len; ++i) {
              lane_sum[i & 3] += bytes[i];
              ++lane_count[i & 3];
            }
            for (int lane = 0; lane < 4; ++lane) {
              resolve_probe_before_mean[lane] =
                  lane_count[lane] ? double(lane_sum[lane]) / double(lane_count[lane]) : 0.0;
            }
            resolve_probe_before_words_nz = 0;
            for (size_t i = 0; i + 3 < len; i += 4) {
              if (*reinterpret_cast<const uint32_t*>(bytes + i)) {
                ++resolve_probe_before_words_nz;
              }
            }
            resolve_probe_before_first =
                len >= 4 ? *reinterpret_cast<const uint32_t*>(bytes) : 0;
            resolve_probe_before_ok = true;
            resolve_probe_pending = true;
            resolve_probe_index = ++resolve_probe_count;
            resolve_probe_extent_start = plan.info.copy_dest_extent_start;
            resolve_probe_extent_length = plan.info.copy_dest_extent_length;
            resolve_probe_dest_base = plan.dest_base;
            resolve_probe_dest_pitch = plan.dest_pitch;
            resolve_probe_dest_info = plan.dest_info;
          }
        }
      }
    }
  }
  const int32_t minimap_swap = REXCVAR_GET(fh1_debug_minimap_capture_swap);
  const bool capture_minimap_frame = minimap_swap > 0 &&
                                     presented_output_frames_ == uint64_t(minimap_swap);
  bool complete = false;
  bool any_resolved = false;
  uint32_t owner_segments_succeeded = 0;
  bool owner_batch_submit_ok = true;
  const bool staging_copy_required = use_staging_copy;
  const uint32_t resolve_dest_base =
      use_staging_copy ? plan.dest_base - plan.info.copy_dest_extent_start : plan.dest_base;
  // The draw path stores each predicated band at its screen rows in the
  // expanded surface; resolve from those same rows without moving the guest
  // destination range.
  const uint32_t source_row_offset = untile_band;
  const uint32_t resolve_sample_select =
      (plan.sample_select & 0xFFFFu) | (source_row_offset << 16);
  bool trace_resolve = false;
  std::unique_ptr<std::FILE, decltype(&std::fclose)> resolve_trace_file(nullptr, &std::fclose);
  static uint32_t trace_captures = 0;
  static uint64_t selected_trace_swap = UINT64_MAX;
  static uint32_t selected_trace_captures = 0;
  if (selected_trace_swap != presented_output_frames_) {
    selected_trace_swap = presented_output_frames_;
    selected_trace_captures = 0;
  }
  const int32_t trace_swap = REXCVAR_GET(fh1_debug_tile_trace_swap);
  const bool selected_swap_resolve_trace =
      trace_swap > 0 && presented_output_frames_ == uint64_t(trace_swap) && !plan.copying_depth &&
      plan.info.copy_dest_extent_length >= (1u << 20) && selected_trace_captures < 3;
  const uint32_t trace_width = uint32_t(source.rect.right - source.rect.left);
  const uint32_t trace_height = uint32_t(source.rect.bottom - source.rect.top);
  const bool general_resolve_trace =
      REXCVAR_GET(pinyon_resolve_trace) && trace_captures < 3 &&
      uint64_t(trace_width) * trace_height >= 100000u;
  if (selected_swap_resolve_trace || general_resolve_trace) {
    resolve_trace_file.reset(
        std::fopen("E:/PinyonShift/work/.local/resolve-trace.log", "a"));
    if (resolve_trace_file) {
      trace_resolve = true;
      selected_trace_captures += selected_swap_resolve_trace ? 1u : 0u;
      trace_captures += general_resolve_trace ? 1u : 0u;
    } else {
      REXGPU_WARN(
          "FH1 resolve trace: could not open E:/PinyonShift/work/.local/resolve-trace.log");
    }
  }
  auto trace_resolve_line = [&](const std::string& line) {
    if (!resolve_trace_file) return;
    std::fwrite(line.data(), 1, line.size(), resolve_trace_file.get());
    std::fputc('\n', resolve_trace_file.get());
  };
  if (trace_resolve) {
    const auto window_offset = register_file_->Get<reg::PA_SC_WINDOW_OFFSET>();
    const auto mode_cntl = register_file_->Get<reg::PA_SU_SC_MODE_CNTL>();
    D3D11_TEXTURE2D_DESC source_desc{};
    if (source.surface && source.surface->texture) {
      source.surface->texture->GetDesc(&source_desc);
    }
    trace_resolve_line(fmt::format(
        "RESOLVE swap={} call={} base=0x{:X} pitch={} extent=0x{:X}+{} rect={},{}-{},{} "
        "dest_info=0x{:X} sample_select=0x{:X} source_row_offset={} pass={} "
        "resolve_key={} source_key={} source_guest_size={}x{} source_host_size={}x{} "
        "samples={} window_y={} "
        "vtx_window_offset_enable={} readback=ResolveSubresource",
        presented_output_frames_, resolve_call_count_, plan.dest_base, plan.dest_pitch,
        plan.info.copy_dest_extent_start, plan.info.copy_dest_extent_length, plan.x0, plan.y0,
        plan.x1, plan.y1, plan.dest_info, resolve_sample_select, source_row_offset,
        current_draw_tiling_pass_,
        plan.resolve_key.Describe(),
        source.surface ? source.surface->key.Describe() : "none",
        source.surface ? Fh1D3D11SurfaceWidth(source.surface->key) : 0u,
        source.surface ? Fh1D3D11SurfaceHeight(source.surface->key, 1) : 0u,
        source_desc.Width, source_desc.Height, source_desc.SampleDesc.Count,
        int32_t(window_offset.window_y_offset), mode_cntl.vtx_window_offset_enable));
    for (size_t i = 0; i < resolve_sources.size(); ++i) {
      const auto& region = resolve_sources[i];
      D3D11_TEXTURE2D_DESC region_desc{};
      if (region.surface && region.surface->texture) {
        region.surface->texture->GetDesc(&region_desc);
      }
      trace_resolve_line(fmt::format(
          "OWNER call={} index={} key={} rect={},{}-{},{} guest_size={}x{} host_size={}x{} "
          "samples={} format={} source_guest_rows=[{}, {}) source_row_offset={}",
          resolve_call_count_, i,
          region.surface ? region.surface->key.Describe() : "missing", region.rect.left,
          region.rect.top, region.rect.right, region.rect.bottom,
          region.surface ? Fh1D3D11SurfaceWidth(region.surface->key) : 0u,
          region.surface ? Fh1D3D11SurfaceHeight(region.surface->key, 1) : 0u,
          region_desc.Width, region_desc.Height, region_desc.SampleDesc.Count,
          uint32_t(region_desc.Format), region.rect.top + int32_t(source_row_offset),
          region.rect.bottom + int32_t(source_row_offset), source_row_offset));
    }
    if (resolve_trace_file) std::fflush(resolve_trace_file.get());
  }
  if (source.rect.left < source.rect.right && source.rect.top < source.rect.bottom) {
    if (command_lists_enabled_) {
      // Phase 3 M2: the same plan, recorded as a packet batch and replayed
      // through the tape's immediate sink before the diagnostics below read
      // GPU output - the recorded stream is the immediate stream in order.
      complete = !resolve_sources.empty();
      for (const D3D11Fh1NativeResolve::SourceRect& region : resolve_sources) {
        if (!region.surface) {
          complete = false;
          if (trace_resolve) {
            trace_resolve_line(fmt::format("OWNER_RESULT call={} mode=recorded success=0",
                                           resolve_call_count_));
          }
          continue;
        }
        const bool recorded = RecordResolveBatch(plan, region, resolve_sample_select,
                                                 memory_target, use_staging_copy);
        any_resolved |= recorded;
        complete &= recorded;
        owner_segments_succeeded += recorded;
        if (trace_resolve) {
          trace_resolve_line(fmt::format("OWNER_RESULT call={} mode=recorded key={} success={}",
                                         resolve_call_count_, region.surface->key.Describe(),
                                         recorded));
        }
      }
      if (any_resolved) {
        owner_batch_submit_ok = SubmitFrameTape("resolve dispatch");
        complete &= owner_batch_submit_ok;
      }
    } else {
      if (capture_minimap_frame && !plan.copying_depth && source.surface && source.surface->texture &&
          plan.x1 - plan.x0 >= 256 && plan.x1 - plan.x0 <= 512 &&
          plan.y1 - plan.y0 >= 128 && plan.y1 - plan.y0 <= 256 && source.surface->samples > 1) {
        D3D11_TEXTURE2D_DESC source_desc{};
        source.surface->texture->GetDesc(&source_desc);
        D3D11_TEXTURE2D_DESC resolved_desc = source_desc;
        resolved_desc.SampleDesc.Count = 1;
        resolved_desc.SampleDesc.Quality = 0;
        resolved_desc.Usage = D3D11_USAGE_DEFAULT;
        resolved_desc.BindFlags = 0;
        resolved_desc.CPUAccessFlags = 0;
        resolved_desc.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> resolved;
        D3D11_TEXTURE2D_DESC staging_desc = resolved_desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        ID3D11Device* capture_device = GetD3D11Provider().GetDevice();
        ID3D11DeviceContext* capture_context = GetD3D11Provider().GetImmediateContext();
        HRESULT create_resolved = capture_device->CreateTexture2D(&resolved_desc, nullptr, &resolved);
        HRESULT create_staging = capture_device->CreateTexture2D(&staging_desc, nullptr, &staging);
        HRESULT query_result = E_FAIL;
        if (SUCCEEDED(create_resolved) && SUCCEEDED(create_staging)) {
          capture_context->ResolveSubresource(resolved.Get(), 0, source.surface->texture, 0,
                                              source_desc.Format);
          capture_context->CopyResource(staging.Get(), resolved.Get());
          D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
          Microsoft::WRL::ComPtr<ID3D11Query> query;
          HRESULT create_query = capture_device->CreateQuery(&query_desc, &query);
          if (SUCCEEDED(create_query)) {
            capture_context->End(query.Get());
            capture_context->Flush();
            while ((query_result = capture_context->GetData(query.Get(), nullptr, 0, 0)) == S_FALSE) {
              std::this_thread::yield();
            }
          }
          D3D11_MAPPED_SUBRESOURCE mapped{};
          HRESULT map_result = query_result == S_OK
                                   ? capture_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)
                                   : E_FAIL;
          if (SUCCEEDED(map_result)) {
            const uint32_t rows = std::min<uint32_t>(source_desc.Height, 256u);
            std::vector<uint64_t> row_hashes;
            const uint64_t hash = HashRows(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                                           rows, std::min<uint32_t>(mapped.RowPitch, source_desc.Width * 4u),
                                           &row_hashes);
            std::string row_text;
            for (uint32_t row = 0; row < rows; row += 16) {
              if (!row_text.empty()) row_text += ',';
              row_text += fmt::format("{}:{:016X}", row, row_hashes[row]);
            }
            REXGPU_INFO("FH1 minimap source: swap={} rect={},{}-{},{} source={}x{} stride={} "
                        "format={} samples={} hash={:016X} rows={}",
                        presented_output_frames_, plan.x0, plan.y0, plan.x1, plan.y1,
                        source_desc.Width, source_desc.Height, mapped.RowPitch,
                        uint32_t(source_desc.Format), source_desc.SampleDesc.Count, hash, row_text);
            capture_context->Unmap(staging.Get(), 0);
          }
        }
      }
      // The guest-memory mirror is the unscaled destination at any draw scale
      // (the D3D12 executor's unscaled copy pass).
      // Into the staging buffer the destination is relative to the extent's
      // start (which is where the copy into the mirror begins); into the mirror
      // itself it is the guest byte address the shader's layout math expects.
      if (REXCVAR_GET(pinyon_audit)) {
        rex::graphics::render_audit::NoteResolve(
            uint32_t(source.rect.right - source.rect.left),
            uint32_t(source.rect.bottom - source.rect.top), plan.dest_pitch,
            uint32_t(source.surface ? source.surface->samples : 0), plan.dest_info & 7u,
            (plan.dest_info >> 6u) & 1u, (plan.dest_info >> 22u) & 1u);
      }
      complete = !resolve_sources.empty();
      for (const D3D11Fh1NativeResolve::SourceRect& region : resolve_sources) {
        if (!region.surface) {
          complete = false;
          if (trace_resolve) {
            trace_resolve_line(fmt::format("OWNER_RESULT call={} mode=immediate success=0",
                                           resolve_call_count_));
          }
          continue;
        }
        const bool resolved = native_resolve_->ResolveToMemory(
            region, plan.resolve_key, resolve_sample_select, plan.dest_info, resolve_dest_base,
            plan.dest_pitch, memory_target, true);
        any_resolved |= resolved;
        complete &= resolved;
        owner_segments_succeeded += resolved;
        if (trace_resolve) {
          trace_resolve_line(fmt::format("OWNER_RESULT call={} mode=immediate key={} success={}",
                                         resolve_call_count_, region.surface->key.Describe(),
                                         resolved));
        }
      }
    }
  }
  bool staging_copy_ok = !staging_copy_required;
  if (complete && use_staging_copy) {
    const bool capture_minimap_candidate = capture_minimap_frame && !plan.copying_depth &&
        plan.x1 - plan.x0 >= 256 && plan.x1 - plan.x0 <= 512 &&
        plan.y1 - plan.y0 >= 128 && plan.y1 - plan.y0 <= 256 && source.surface &&
        source.surface->texture;
    if (capture_minimap_candidate) {
      D3D11_TEXTURE2D_DESC source_desc{};
      source.surface->texture->GetDesc(&source_desc);
      ID3D11Device* capture_device = GetD3D11Provider().GetDevice();
      ID3D11DeviceContext* capture_context = GetD3D11Provider().GetImmediateContext();
      D3D11_TEXTURE2D_DESC resolved_desc = source_desc;
      resolved_desc.SampleDesc.Count = 1;
      resolved_desc.SampleDesc.Quality = 0;
      resolved_desc.Usage = D3D11_USAGE_DEFAULT;
      resolved_desc.BindFlags = 0;
      resolved_desc.CPUAccessFlags = 0;
      resolved_desc.MiscFlags = 0;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> resolved;
      D3D11_TEXTURE2D_DESC staging_desc = resolved_desc;
      staging_desc.Usage = D3D11_USAGE_STAGING;
      staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
      HRESULT create_resolved = capture_device->CreateTexture2D(&resolved_desc, nullptr, &resolved);
      HRESULT create_staging = capture_device->CreateTexture2D(&staging_desc, nullptr, &staging);
      if (SUCCEEDED(create_resolved) && SUCCEEDED(create_staging)) {
        capture_context->ResolveSubresource(resolved.Get(), 0, source.surface->texture, 0,
                                            source_desc.Format);
        capture_context->CopyResource(staging.Get(), resolved.Get());
        D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        HRESULT create_query = capture_device->CreateQuery(&query_desc, &query);
        HRESULT query_result = E_FAIL;
        if (SUCCEEDED(create_query)) {
          capture_context->End(query.Get());
          capture_context->Flush();
          while ((query_result = capture_context->GetData(query.Get(), nullptr, 0, 0)) == S_FALSE) {
            std::this_thread::yield();
          }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT map_result = E_FAIL;
        if (query_result == S_OK) {
          map_result = capture_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        }
        if (SUCCEEDED(map_result)) {
          const uint32_t width = std::min<uint32_t>(source_desc.Width,
                                                    mapped.RowPitch / 4u);
          const uint32_t rows = std::min<uint32_t>(source_desc.Height, 256u);
          std::vector<uint64_t> row_hashes;
          const uint64_t hash = HashRows(static_cast<const uint8_t*>(mapped.pData),
                                         mapped.RowPitch, rows, width * 4u, &row_hashes);
          std::string row_text;
          for (uint32_t row = 0; row < rows; row += 16) {
            if (!row_text.empty()) row_text += ',';
            row_text += fmt::format("{}:{:016X}", row, row_hashes[row]);
          }
          REXGPU_INFO("FH1 minimap source: swap={} rect={},{}-{},{} source={}x{} stride={} "
                      "format={} samples={} hash={:016X} rows={}",
                      presented_output_frames_, plan.x0, plan.y0, plan.x1, plan.y1,
                      source_desc.Width, source_desc.Height, mapped.RowPitch,
                      uint32_t(source_desc.Format), source_desc.SampleDesc.Count, hash,
                      row_text);
          capture_context->Unmap(staging.Get(), 0);
        } else {
          REXGPU_WARN("FH1 minimap source capture failed: swap={} create_resolved={} "
                      "create_staging={} create_query={} query={} map={}",
                      presented_output_frames_, int32_t(create_resolved), int32_t(create_staging),
                      int32_t(create_query), int32_t(query_result), int32_t(map_result));
        }
      }
      minimap_capture_resolve_count_ = resolve_call_count_;
      minimap_capture_guest_base_ = plan.info.copy_dest_extent_start;
      minimap_capture_guest_pitch_ = plan.dest_pitch;
      minimap_capture_guest_width_ = uint32_t(plan.x1 - plan.x0);
      minimap_capture_guest_height_ = uint32_t(plan.y1 - plan.y0);
      minimap_capture_resolved_ = true;
    }
    // Gate the expensive measurement (WaitForGpu + staging readbacks) to the
    // pass window: without this it runs on every resolve of a visible run and
    // chokes the present path black. SetDrawGeo/NoteDrawSubmission above stay
    // ungated (cheap counters).
    if (REXCVAR_GET(pinyon_audit_passes) &&
        rex::graphics::render_audit::PassWindowActive()) {
      std::vector<uint32_t> pass_src_rows;
      std::vector<uint32_t> pass_dst_rows;
      uint32_t pass_src_w = 0, pass_src_h = 0;
      WaitForGpu();
      ID3D11Device* pass_device = GetD3D11Provider().GetDevice();
      ID3D11DeviceContext* pass_context = GetD3D11Provider().GetImmediateContext();
      if (source.surface && source.surface->texture) {
        D3D11_TEXTURE2D_DESC pass_desc;
        source.surface->texture->GetDesc(&pass_desc);
        pass_src_w = pass_desc.Width;
        pass_src_h = pass_desc.Height;
        // A multi-sample texture cannot be mapped as a 2D staging surface, so
        // occupancy is only measured for single-sample sources and the record
        // says so with an empty tally.
        if (source.surface->samples == 1) {
          D3D11_TEXTURE2D_DESC pass_staging = pass_desc;
          pass_staging.Usage = D3D11_USAGE_STAGING;
          pass_staging.BindFlags = 0;
          pass_staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          pass_staging.MiscFlags = 0;
          Microsoft::WRL::ComPtr<ID3D11Texture2D> pass_readback;
          if (SUCCEEDED(pass_device->CreateTexture2D(&pass_staging, nullptr, &pass_readback))) {
            pass_context->CopyResource(pass_readback.Get(), source.surface->texture);
            D3D11_MAPPED_SUBRESOURCE pass_mapped;
            if (SUCCEEDED(pass_context->Map(pass_readback.Get(), 0, D3D11_MAP_READ, 0,
                                           &pass_mapped))) {
              const uint32_t pass_bottom =
                  std::min<uint32_t>(pass_desc.Height, uint32_t(std::max<int32_t>(plan.y1, 0)));
              for (uint32_t pass_y = 0; pass_y < pass_bottom; pass_y += 16) {
                const uint32_t* pass_texels = reinterpret_cast<const uint32_t*>(
                    static_cast<const uint8_t*>(pass_mapped.pData) +
                    size_t(pass_mapped.RowPitch) * pass_y);
                uint32_t pass_nonzero = 0;
                for (uint32_t pass_x = 0; pass_x < pass_desc.Width; pass_x += 32) {
                  pass_nonzero += pass_texels[pass_x] != 0;
                }
                pass_src_rows.push_back(pass_nonzero);
              }
              pass_context->Unmap(pass_readback.Get(), 0);
            }
          }
        }
      }
      // Destination coverage: which 16-row bands of the resolve output hold
      // non-zero words right after this dispatch.
      resolve_readback_scratch_.resize(plan.info.copy_dest_extent_length);
      if (upload_sink->ReadbackResolveStaging(0, resolve_readback_scratch_.data(),
                                             plan.info.copy_dest_extent_length)) {
        const uint32_t pass_row_words = plan.dest_pitch ? plan.dest_pitch / 4u : 0u;
        const uint8_t* pass_bytes = resolve_readback_scratch_.data();
        if (pass_row_words) {
          const uint32_t pass_words =
              uint32_t(plan.info.copy_dest_extent_length / 4u);
          const uint32_t pass_rows = pass_words / pass_row_words;
          for (uint32_t pass_y = 0; pass_y < pass_rows; pass_y += 16) {
            uint32_t pass_nonzero = 0;
            const uint32_t pass_row_end = std::min<uint32_t>(pass_rows, pass_y + 16);
            for (uint32_t pass_row = pass_y; pass_row < pass_row_end; ++pass_row) {
              for (uint32_t pass_x = 0; pass_x < pass_row_words; pass_x += 8) {
                const uint32_t pass_word = *reinterpret_cast<const uint32_t*>(
                    pass_bytes + (size_t(pass_row) * pass_row_words + pass_x) * 4u);
                pass_nonzero += pass_word != 0;
              }
            }
            pass_dst_rows.push_back(pass_nonzero);
          }
        }
      }
      // A multisampled resource cannot be a staging resource and cannot be
      // mapped. The chain D3D11 requires is: 4x DEFAULT source -> 1x DEFAULT
      // destination (ResolveSubresource needs a single-sampled DEFAULT
      // destination) -> staging -> event query -> Map. Every step is recorded,
      // so a zero reading can be told apart from a broken measurement.
      std::vector<uint32_t> pass_hw_rows;
      const char* pass_evidence = "none";
      if (source.surface && source.surface->texture) {
        if (source.surface->samples == 1) {
          pass_evidence = "raw_map";
        } else if (REXCVAR_GET(pinyon_msaa_probe)) {
          rex::graphics::render_audit::ProbeInfo probe;
          probe.label = "production_4x_source";
          probe.reason = "hardware_resolve_chain";
          D3D11_TEXTURE2D_DESC probe_src{};
          source.surface->texture->GetDesc(&probe_src);
          probe.resource_format = uint32_t(probe_src.Format);
          probe.samples = probe_src.SampleDesc.Count;
          probe.quality = probe_src.SampleDesc.Quality;
          probe.bind_flags = probe_src.BindFlags;
          probe.create_src = 0;
          UINT probe_support = 0;
          if (SUCCEEDED(pass_device->CheckFormatSupport(probe_src.Format, &probe_support))) {
            probe.support_ms_load =
                (probe_support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD) ? 1u : 0u;
            probe.support_ms_resolve =
                (probe_support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE) ? 1u : 0u;
          }
          if (probe_support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) {
            D3D11_SHADER_RESOURCE_VIEW_DESC probe_srv_desc{};
            probe_srv_desc.Format = probe_src.Format;
            probe_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> probe_srv;
            probe.create_srv =
                int32_t(pass_device->CreateShaderResourceView(source.surface->texture,
                                                              &probe_srv_desc, &probe_srv));
            probe.srv_view_format = uint32_t(probe_srv_desc.Format);
          } else {
            probe.create_srv = int32_t(E_INVALIDARG);
          }
          uint32_t probe_max = 0;
          uint32_t probe_nonzero = 0;
          if (probe.support_ms_resolve) {
            D3D11_TEXTURE2D_DESC probe_dst_desc = probe_src;
            probe_dst_desc.SampleDesc.Count = 1;
            probe_dst_desc.SampleDesc.Quality = 0;
            probe_dst_desc.Usage = D3D11_USAGE_DEFAULT;
            probe_dst_desc.BindFlags = 0;
            probe_dst_desc.CPUAccessFlags = 0;
            probe_dst_desc.MiscFlags = 0;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> probe_dst;
            probe.create_dst = int32_t(pass_device->CreateTexture2D(&probe_dst_desc, nullptr,
                                                                    &probe_dst));
            D3D11_TEXTURE2D_DESC probe_staging_desc = probe_dst_desc;
            probe_staging_desc.Usage = D3D11_USAGE_STAGING;
            probe_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> probe_staging;
            if (SUCCEEDED(probe.create_dst)) {
              probe.create_staging =
                  int32_t(pass_device->CreateTexture2D(&probe_staging_desc, nullptr,
                                                       &probe_staging));
            }
            if (SUCCEEDED(probe.create_dst) && SUCCEEDED(probe.create_staging)) {
              pass_context->ResolveSubresource(probe_dst.Get(), 0, source.surface->texture, 0,
                                               probe_dst_desc.Format);
              probe.copy_stage = 1;
              pass_context->CopyResource(probe_staging.Get(), probe_dst.Get());
              D3D11_QUERY_DESC probe_query_desc{};
              probe_query_desc.Query = D3D11_QUERY_EVENT;
              Microsoft::WRL::ComPtr<ID3D11Query> probe_query;
              probe.query_status = 1;
              if (SUCCEEDED(pass_device->CreateQuery(&probe_query_desc, &probe_query))) {
                pass_context->End(probe_query.Get());
                pass_context->Flush();
                for (int probe_spin = 0; probe_spin < 60; ++probe_spin) {
                  const HRESULT probe_poll = pass_context->GetData(probe_query.Get(), nullptr, 0, 0);
                  if (probe_poll == S_OK) {
                    probe.query_status = 0;
                    break;
                  }
                  if (probe_poll != S_FALSE) {
                    probe.query_status = 2;
                    break;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
              }
              if (probe.query_status == 0) {
                D3D11_MAPPED_SUBRESOURCE probe_mapped{};
                probe.map_hresult = int32_t(pass_context->Map(probe_staging.Get(), 0,
                                                              D3D11_MAP_READ, 0, &probe_mapped));
                if (SUCCEEDED(probe.map_hresult)) {
                  probe.map_row_pitch = probe_mapped.RowPitch;
                  for (uint32_t probe_row = 0; probe_row < probe_staging_desc.Height;
                       probe_row += 16) {
                    const uint32_t* probe_texels = reinterpret_cast<const uint32_t*>(
                        static_cast<const uint8_t*>(probe_mapped.pData) +
                        size_t(probe_mapped.RowPitch) * probe_row);
                    uint32_t probe_row_nonzero = 0;
                    for (uint32_t probe_col = 0; probe_col < probe_staging_desc.Width;
                         probe_col += 32) {
                      const uint32_t probe_texel = probe_texels[probe_col];
                      probe_row_nonzero += probe_texel != 0;
                      if (probe_texel != 0) {
                        probe_max = std::max(probe_max, probe_texel);
                      }
                      ++probe.sampled_pixels;
                    }
                    probe_nonzero += probe_row_nonzero;
                    pass_hw_rows.push_back(probe_row_nonzero);
                  }
                  probe.uav_copied = probe_mapped.RowPitch * probe_staging_desc.Height;
                  probe.uav_expected = probe.uav_copied;
                  probe.uav_stride = 4;
                  probe.uav_byte_width = probe_staging_desc.Width * 4;
                  probe.record_complete = true;
                  pass_evidence = "hw_resolve";
                }
                pass_context->Unmap(probe_staging.Get(), 0);
              }
            }
          }
          probe.sample_rows[0] = probe_nonzero;
          probe.max_r = probe_max;
          probe.max_g = probe_max;
          probe.max_b = probe_max;
          probe.max_a = probe_max;
          if (!probe.record_complete) {
            probe.verdict = "invalid_measurement";
            probe.reason = probe.support_ms_resolve ? "query_or_map_failed"
                                                    : "no_hardware_resolve_support";
          } else if (probe_nonzero == 0) {
            probe.verdict = "valid";
            probe.reason = "zero_in_sampled_coordinates";
          } else {
            probe.verdict = "valid";
            probe.reason = "nonzero_in_sampled_coordinates";
          }
          rex::graphics::render_audit::NoteProbe(probe);
        }
      }
      // All-owner provenance: the production probe above only measures the
      // one surface the copy names. If the resolve rect mixes owners, the
      // pixels may live under a different owner, so enumerate every owner
      // intersecting this rect and probe each one's surface the same way.
      // Decision table: selected owner empty + another owner populated =
      // source selection; all owners empty = draw population / target
      // binding; selected owner populated + hardware resolve empty =
      // resolve coordinates / sample selection.
      if (REXCVAR_GET(pinyon_msaa_probe) && native_executor_) {
        const std::map<uint32_t, uint32_t> owners = native_executor_->EnumerateResolveOwners(
            plan.resolve_key, plan.x0, plan.y0, plan.x1, plan.y1);
        for (const auto& owner_entry : owners) {
          const uint32_t owner_key = owner_entry.first;
          const uint32_t owner_tiles = owner_entry.second;
          if (owner_key == Fh1EdramTiles::kNoOwner) {
            continue;
          }
          D3D11Fh1NativeSurface* owner_surface = nullptr;
          if (native_surfaces_) {
            owner_surface = native_surfaces_->FindSurface(owner_key);
          }
          char owner_label[96];
          std::snprintf(owner_label, sizeof(owner_label), "owner_%08X_tiles_%u", owner_key,
                        owner_tiles);
          rex::graphics::render_audit::ProbeInfo owner_probe;
          owner_probe.label = owner_label;
          owner_probe.reason = "all_owner_provenance";
          if (!owner_surface || !owner_surface->texture) {
            owner_probe.verdict = "invalid_measurement";
            owner_probe.reason = "no_surface_for_owner";
            rex::graphics::render_audit::NoteProbe(owner_probe);
            continue;
          }
          if (owner_surface == source.surface) {
            // Already probed as the production source above.
            continue;
          }
          D3D11_TEXTURE2D_DESC owner_desc{};
          owner_surface->texture->GetDesc(&owner_desc);
          owner_probe.resource_format = uint32_t(owner_desc.Format);
          owner_probe.samples = owner_desc.SampleDesc.Count;
          owner_probe.quality = owner_desc.SampleDesc.Quality;
          owner_probe.bind_flags = owner_desc.BindFlags;
          owner_probe.create_src = 0;
          UINT owner_support = 0;
          if (SUCCEEDED(pass_device->CheckFormatSupport(owner_desc.Format, &owner_support))) {
            owner_probe.support_ms_load =
                (owner_support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD) ? 1u : 0u;
            owner_probe.support_ms_resolve =
                (owner_support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE) ? 1u : 0u;
          }
          uint32_t owner_nonzero = 0;
          uint32_t owner_max = 0;
          if (owner_surface->samples == 1) {
            owner_probe.reason = "raw_map";
            D3D11_TEXTURE2D_DESC owner_staging = owner_desc;
            owner_staging.Usage = D3D11_USAGE_STAGING;
            owner_staging.BindFlags = 0;
            owner_staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            owner_staging.MiscFlags = 0;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> owner_readback;
            owner_probe.create_staging =
                int32_t(pass_device->CreateTexture2D(&owner_staging, nullptr, &owner_readback));
            if (SUCCEEDED(owner_probe.create_staging)) {
              pass_context->CopyResource(owner_readback.Get(), owner_surface->texture);
              D3D11_QUERY_DESC owner_query_desc{};
              owner_query_desc.Query = D3D11_QUERY_EVENT;
              Microsoft::WRL::ComPtr<ID3D11Query> owner_query;
              owner_probe.query_status = 1;
              if (SUCCEEDED(pass_device->CreateQuery(&owner_query_desc, &owner_query))) {
                pass_context->End(owner_query.Get());
                pass_context->Flush();
                for (int owner_spin = 0; owner_spin < 60; ++owner_spin) {
                  const HRESULT owner_poll =
                      pass_context->GetData(owner_query.Get(), nullptr, 0, 0);
                  if (owner_poll == S_OK) {
                    owner_probe.query_status = 0;
                    break;
                  }
                  if (owner_poll != S_FALSE) {
                    owner_probe.query_status = 2;
                    break;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
              }
              if (owner_probe.query_status == 0) {
                D3D11_MAPPED_SUBRESOURCE owner_mapped{};
                owner_probe.map_hresult = int32_t(pass_context->Map(
                    owner_readback.Get(), 0, D3D11_MAP_READ, 0, &owner_mapped));
                if (SUCCEEDED(owner_probe.map_hresult)) {
                  owner_probe.map_row_pitch = owner_mapped.RowPitch;
                  for (uint32_t owner_row = 0; owner_row < owner_desc.Height;
                       owner_row += 16) {
                    const uint32_t* owner_texels = reinterpret_cast<const uint32_t*>(
                        static_cast<const uint8_t*>(owner_mapped.pData) +
                        size_t(owner_mapped.RowPitch) * owner_row);
                    uint32_t owner_row_nonzero = 0;
                    for (uint32_t owner_col = 0; owner_col < owner_desc.Width;
                         owner_col += 32) {
                      const uint32_t owner_texel = owner_texels[owner_col];
                      owner_row_nonzero += owner_texel != 0;
                      if (owner_texel != 0) {
                        owner_max = std::max(owner_max, owner_texel);
                      }
                      ++owner_probe.sampled_pixels;
                    }
                    owner_nonzero += owner_row_nonzero;
                  }
                  owner_probe.record_complete = true;
                  pass_context->Unmap(owner_readback.Get(), 0);
                }
              }
            }
          } else if (owner_probe.support_ms_resolve) {
            owner_probe.reason = "hardware_resolve_chain";
            D3D11_TEXTURE2D_DESC owner_dst_desc = owner_desc;
            owner_dst_desc.SampleDesc.Count = 1;
            owner_dst_desc.SampleDesc.Quality = 0;
            owner_dst_desc.Usage = D3D11_USAGE_DEFAULT;
            owner_dst_desc.BindFlags = 0;
            owner_dst_desc.CPUAccessFlags = 0;
            owner_dst_desc.MiscFlags = 0;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> owner_dst;
            owner_probe.create_dst = int32_t(
                pass_device->CreateTexture2D(&owner_dst_desc, nullptr, &owner_dst));
            D3D11_TEXTURE2D_DESC owner_staging_desc = owner_dst_desc;
            owner_staging_desc.Usage = D3D11_USAGE_STAGING;
            owner_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> owner_staging;
            if (SUCCEEDED(owner_probe.create_dst)) {
              owner_probe.create_staging = int32_t(pass_device->CreateTexture2D(
                  &owner_staging_desc, nullptr, &owner_staging));
            }
            if (SUCCEEDED(owner_probe.create_dst) && SUCCEEDED(owner_probe.create_staging)) {
              pass_context->ResolveSubresource(owner_dst.Get(), 0, owner_surface->texture, 0,
                                               owner_dst_desc.Format);
              owner_probe.copy_stage = 1;
              pass_context->CopyResource(owner_staging.Get(), owner_dst.Get());
              D3D11_QUERY_DESC owner_query_desc{};
              owner_query_desc.Query = D3D11_QUERY_EVENT;
              Microsoft::WRL::ComPtr<ID3D11Query> owner_query;
              owner_probe.query_status = 1;
              if (SUCCEEDED(pass_device->CreateQuery(&owner_query_desc, &owner_query))) {
                pass_context->End(owner_query.Get());
                pass_context->Flush();
                for (int owner_spin = 0; owner_spin < 60; ++owner_spin) {
                  const HRESULT owner_poll =
                      pass_context->GetData(owner_query.Get(), nullptr, 0, 0);
                  if (owner_poll == S_OK) {
                    owner_probe.query_status = 0;
                    break;
                  }
                  if (owner_poll != S_FALSE) {
                    owner_probe.query_status = 2;
                    break;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
              }
              if (owner_probe.query_status == 0) {
                D3D11_MAPPED_SUBRESOURCE owner_mapped{};
                owner_probe.map_hresult = int32_t(pass_context->Map(
                    owner_staging.Get(), 0, D3D11_MAP_READ, 0, &owner_mapped));
                if (SUCCEEDED(owner_probe.map_hresult)) {
                  owner_probe.map_row_pitch = owner_mapped.RowPitch;
                  for (uint32_t owner_row = 0; owner_row < owner_staging_desc.Height;
                       owner_row += 16) {
                    const uint32_t* owner_texels = reinterpret_cast<const uint32_t*>(
                        static_cast<const uint8_t*>(owner_mapped.pData) +
                        size_t(owner_mapped.RowPitch) * owner_row);
                    uint32_t owner_row_nonzero = 0;
                    for (uint32_t owner_col = 0; owner_col < owner_staging_desc.Width;
                         owner_col += 32) {
                      const uint32_t owner_texel = owner_texels[owner_col];
                      owner_row_nonzero += owner_texel != 0;
                      if (owner_texel != 0) {
                        owner_max = std::max(owner_max, owner_texel);
                      }
                      ++owner_probe.sampled_pixels;
                    }
                    owner_nonzero += owner_row_nonzero;
                  }
                  owner_probe.record_complete = true;
                  pass_context->Unmap(owner_staging.Get(), 0);
                }
              }
            }
          }
          owner_probe.sample_rows[0] = owner_nonzero;
          owner_probe.max_r = owner_max;
          owner_probe.max_g = owner_max;
          owner_probe.max_b = owner_max;
          owner_probe.max_a = owner_max;
          if (!owner_probe.record_complete) {
            owner_probe.verdict = "invalid_measurement";
            owner_probe.reason = owner_probe.support_ms_resolve ? "query_or_map_failed"
                                                                : "no_hardware_resolve_support";
          } else if (owner_nonzero == 0) {
            owner_probe.verdict = "valid";
            owner_probe.reason = "zero_in_sampled_coordinates";
          } else {
            owner_probe.verdict = "valid";
            owner_probe.reason = "nonzero_in_sampled_coordinates";
          }
          rex::graphics::render_audit::NoteProbe(owner_probe);
        }
      }
      rex::graphics::render_audit::NotePassResolve(
          plan.kind.c_str(), plan.x0, plan.y0, plan.x1, plan.y1,
          plan.resolve_key.Describe().c_str(), pass_src_w, pass_src_h,
          uint32_t(source.surface ? source.surface->samples : 0), resolve_sample_select,
          plan.dest_base, plan.dest_pitch, plan.dest_info, pass_src_rows, pass_evidence,
          pass_hw_rows, pass_dst_rows);
    }
    // The staging readback is the direct measurement of "did the dispatch
    // write the resolved pixels", independent of the copy into the mirror.
    // The first 64 resolves of the session are logged whatever their extent,
    // so the table shows a resolve's surface key and whether it was already
    // resolved (session-first versus surface-first) next to its word counts.
    static uint32_t staging_logs = 0;
    static std::set<uint64_t> resolved_keys;
    const bool log_this_resolve = resolve_call_count_ < 64;
    if (REXCVAR_GET(fh1_swap_fetch_trace) &&
        (log_this_resolve || plan.info.copy_dest_extent_length >= (1u << 20)) &&
        staging_logs < (REXCVAR_GET(fh1_debug_resolve_store_probe) ? 4000 : 600)) {
      ++staging_logs;
      const uint64_t resolve_key_packed = plan.resolve_key.Pack();
      const bool key_seen_before = resolved_keys.count(resolve_key_packed) != 0;
      resolved_keys.insert(resolve_key_packed);
      // Does the dispatch itself land in the staging buffer? Separates a
      // store that never fires from a copy that lands the wrong bytes.
      WaitForGpu();
      resolve_readback_scratch_.resize(plan.info.copy_dest_extent_length);
      if (upload_sink->ReadbackResolveStaging(0, resolve_readback_scratch_.data(),
                                              plan.info.copy_dest_extent_length)) {
        // Non-zero WORDS, not one sampled byte: an 8in32-swapped frame puts a
        // zero byte where an unswapped one has data, so a byte-granular sample
        // reports a written extent as blank.
        uint64_t staging_nonzero = 0;
        uint64_t staging_dwords_nonzero = 0;
        uint64_t staging_zero = 0;
        const uint8_t* bytes = resolve_readback_scratch_.data();
        for (size_t i = 0; i + 3 < plan.info.copy_dest_extent_length; i += 4) {
          const uint32_t word = *reinterpret_cast<const uint32_t*>(bytes + i);
          if (word) {
            ++staging_nonzero;
            if (!(i & 63)) ++staging_dwords_nonzero;

          } else {
            ++staging_zero;
          }
        }
        // Per-resolve source check, paired with the staging read above (the
        // same resolve, not two logs zipped by index): what the surface holds
        // at the moment this exact dispatch runs.
        uint64_t source_in_rect = UINT64_MAX;
        if (staging_logs <= 30 && source.surface && source.surface->texture &&
            source.surface->samples == 1 && !plan.copying_depth) {
          ID3D11Device* device = GetD3D11Provider().GetDevice();
          ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
          D3D11_TEXTURE2D_DESC desc;
          source.surface->texture->GetDesc(&desc);
          D3D11_TEXTURE2D_DESC staging_desc = desc;
          staging_desc.Usage = D3D11_USAGE_STAGING;
          staging_desc.BindFlags = 0;
          staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          staging_desc.MiscFlags = 0;
          Microsoft::WRL::ComPtr<ID3D11Texture2D> readback_texture;
          if (SUCCEEDED(device->CreateTexture2D(&staging_desc, nullptr, &readback_texture))) {
            context->CopyResource(readback_texture.Get(), source.surface->texture);
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(readback_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
              uint64_t nonzero_texels = 0;
              const uint32_t rect_bottom = std::min<uint32_t>(desc.Height, plan.y1);
              for (uint32_t y = 0; y < rect_bottom; y += 16) {
                const uint32_t* texels =
                    reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) +
                                                      size_t(mapped.RowPitch) * y);
                for (uint32_t x = 0; x < desc.Width; x += 32) {
                  nonzero_texels += texels[x] != 0;
                }
              }
              source_in_rect = nonzero_texels;
              context->Unmap(readback_texture.Get(), 0);
            }
          }
        }
        REXGPU_INFO(
            "FH1 resolve staging: call={} key={} seen={} info={:08X} pitch={} len={} "
            "words_nz={} dwords_nz={} zero={} first={:08X} source_in_rect={} "
            "dest_base={:08X} extent_start={:08X} wrap={}",
            resolve_call_count_, plan.resolve_key.Describe(), key_seen_before ? 1 : 0,
            plan.dest_info, plan.dest_pitch, plan.info.copy_dest_extent_length, staging_nonzero,
            staging_dwords_nonzero, staging_zero,
            *reinterpret_cast<const uint32_t*>(bytes),
            source_in_rect == UINT64_MAX ? -1 : int64_t(source_in_rect), plan.dest_base,
            plan.info.copy_dest_extent_start,
            plan.dest_base < plan.info.copy_dest_extent_start ? 1 : 0);
      }
    }
    // Phase 3 M2 verification (fh1_d3d11_command_list_ab): run the same plan
    // through the immediate path on the same source and compare the two
    // dispatches' staging output bit-for-bit. The recorded dispatch already ran
    // above (submitted before the staging readback), so the two outputs are the
    // same resolve computed by the tape's replay sink and by the inline path -
    // the same-session equivalence the cross-run hashes cannot give. The staging
    // buffer is shared by both paths, so it proves the dispatch and nothing
    // about the transfer: the tape's copy packet and the immediate
    // CopyResolveStagingToHost each land the extent in the mirror, and the two
    // destination fingerprints must match as well.
    if (command_lists_enabled_ && REXCVAR_GET(fh1_d3d11_command_list_ab)) {
      const uint32_t ab_dest_base = plan.dest_base - plan.info.copy_dest_extent_start;
      WaitForGpu();
      std::vector<uint8_t> tape_bytes(plan.info.copy_dest_extent_length);
      const bool tape_read =
          upload_sink->ReadbackResolveStaging(0, tape_bytes.data(),
                                              plan.info.copy_dest_extent_length);
      // Land the tape's own copy packet before the immediate resolve overwrites
      // the staging buffer the packet reads.
      uint64_t tape_mirror_hash = 0;
      uint64_t tape_mirror_nonzero = 0;
      const bool tape_mirror_ok =
          RecordResolveStagingCopy(plan.info.copy_dest_extent_start,
                                   plan.info.copy_dest_extent_length) &&
          SubmitFrameTape("A/B tape mirror") &&
          mirror_hash(plan.info.copy_dest_extent_start, plan.info.copy_dest_extent_length,
                      tape_mirror_hash, tape_mirror_nonzero);
      const bool immediate_ok =
          native_resolve_->ResolveToMemory(source, plan.resolve_key, resolve_sample_select,
                                           plan.dest_info, ab_dest_base, plan.dest_pitch,
                                           memory_target, true);
      WaitForGpu();
      std::vector<uint8_t> immediate_bytes(plan.info.copy_dest_extent_length);
      const bool immediate_read =
          upload_sink->ReadbackResolveStaging(0, immediate_bytes.data(),
                                              plan.info.copy_dest_extent_length);
      uint64_t immediate_mirror_hash = 0;
      uint64_t immediate_mirror_nonzero = 0;
      const bool immediate_mirror_ok =
          immediate_ok &&
          upload_sink->CopyResolveStagingToHost(plan.info.copy_dest_extent_start,
                                                plan.info.copy_dest_extent_length) &&
          mirror_hash(plan.info.copy_dest_extent_start, plan.info.copy_dest_extent_length,
                      immediate_mirror_hash, immediate_mirror_nonzero);
      uint64_t tape_words_nz = 0, immediate_words_nz = 0;
      bool equal = false;
      if (tape_read && immediate_read && immediate_ok) {
        for (size_t i = 0; i + 3 < plan.info.copy_dest_extent_length; i += 4) {
          tape_words_nz +=
              *reinterpret_cast<const uint32_t*>(tape_bytes.data() + i) != 0;
          immediate_words_nz +=
              *reinterpret_cast<const uint32_t*>(immediate_bytes.data() + i) != 0;
        }
        equal = std::memcmp(tape_bytes.data(), immediate_bytes.data(),
                            plan.info.copy_dest_extent_length) == 0;
      }
      const bool mirror_equal = tape_mirror_ok && immediate_mirror_ok &&
                                tape_mirror_hash == immediate_mirror_hash &&
                                tape_mirror_nonzero == immediate_mirror_nonzero;
      static uint32_t ab_logs = 0;
      if (ab_logs < 4000) {
        ++ab_logs;
        REXGPU_INFO(
            "FH1 command list A/B: call={} len={} tape_read={} immediate_ok={} immediate_read={} "
            "tape_words_nz={} immediate_words_nz={} tape_first={:08X} immediate_first={:08X} "
            "equal={} tape_mirror={:016X}/{} immediate_mirror={:016X}/{} mirror_equal={}",
            resolve_call_count_, plan.info.copy_dest_extent_length, tape_read ? 1 : 0,
            immediate_ok ? 1 : 0, immediate_read ? 1 : 0, tape_words_nz, immediate_words_nz,
            tape_bytes.size() >= 4 ? *reinterpret_cast<const uint32_t*>(tape_bytes.data()) : 0,
            immediate_bytes.size() >= 4
                ? *reinterpret_cast<const uint32_t*>(immediate_bytes.data())
                : 0,
            equal ? 1 : 0, tape_mirror_hash, tape_mirror_nonzero, immediate_mirror_hash,
            immediate_mirror_nonzero, mirror_equal ? 1 : 0);
      }
    }
    // Land the resolved extent at its guest address. The resolve shader writes
    // the destination offset relative to the extent's start, which is exactly
    // what the staging buffer holds from offset 0. The recorded path appends
    // the copy as a packet and replays it in order after the dispatch.
    staging_copy_ok =
        command_lists_enabled_
            ? (RecordResolveStagingCopy(plan.info.copy_dest_extent_start,
                                        plan.info.copy_dest_extent_length) &&
               SubmitFrameTape("resolve staging copy"))
            : upload_sink->CopyResolveStagingToHost(plan.info.copy_dest_extent_start,
                                                    plan.info.copy_dest_extent_length);
    if (!staging_copy_ok) {
      complete = false;
      static bool staging_copy_failed_logged = false;
      if (!staging_copy_failed_logged) {
        staging_copy_failed_logged = true;
        REXGPU_ERROR("D3D11 FH1 native resolve: staging copy to the mirror failed ({:08X}+{})",
                     plan.info.copy_dest_extent_start, plan.info.copy_dest_extent_length);
      }
    }
  }
  bool trace_gpu_completed = false;
  if (trace_resolve) {
    trace_gpu_completed = WaitForGpu();
    trace_resolve_line(fmt::format(
        "COMPLETE call={} owner_segments={} succeeded={} all_succeeded={} recorded={} "
        "recorded_submit_ok={} resolve_complete={} staging_copy_required={} staging_copy_ok={} "
        "gpu_wait={}",
        resolve_call_count_, uint32_t(resolve_sources.size()), owner_segments_succeeded,
        owner_segments_succeeded == resolve_sources.size(), command_lists_enabled_,
        owner_batch_submit_ok, complete, staging_copy_required, staging_copy_ok,
        trace_gpu_completed));
    if (trace_gpu_completed) {
      std::set<uint32_t> captured_surfaces;
      ID3D11Device* trace_device = GetD3D11Provider().GetDevice();
      ID3D11DeviceContext* trace_context = GetD3D11Provider().GetImmediateContext();
      for (const auto& region : resolve_sources) {
        if (!region.surface || !region.surface->texture ||
            !captured_surfaces.insert(region.surface->key.Pack()).second) {
          continue;
        }
        std::vector<uint8_t> source_rgb;
        uint32_t source_width = 0, source_height = 0;
        SurfaceDumpStats source_stats;
        std::string source_error;
        if (!ReadSurfaceRgb(trace_device, trace_context, region.surface, source_rgb,
                            source_width, source_height, source_stats, source_error)) {
          trace_resolve_line(fmt::format(
              "SOURCE call={} key={} capture_failed={}", resolve_call_count_,
              region.surface->key.Describe(), source_error));
          continue;
        }
        const uint32_t rows =
            selected_swap_resolve_trace ? source_height : std::min(source_height, 512u);
        std::vector<uint64_t> row_hashes;
        const uint64_t source_rgb_hash =
            HashRows(source_rgb.data(), source_width * 3u, rows, source_width * 3u, &row_hashes);
        trace_resolve_line(fmt::format(
            "SOURCE call={} key={} guest_size={}x{} host_size={}x{} host_rows=0..{} "
            "rgb_hash={:016X}",
            resolve_call_count_, region.surface->key.Describe(),
            Fh1D3D11SurfaceWidth(region.surface->key),
            Fh1D3D11SurfaceHeight(region.surface->key, 1), source_width, source_height,
            rows ? rows - 1 : 0, source_rgb_hash));
        for (uint32_t row = 0; row < rows; row += 16) {
          const uint32_t end = std::min(row + 16u, rows);
          uint64_t band_hash = UINT64_C(1469598103934665603);
          for (uint32_t y = row; y < end; ++y) {
            for (uint32_t byte = 0; byte < sizeof(uint64_t); ++byte) {
              band_hash = (band_hash ^ uint8_t(row_hashes[y] >> (byte * 8u))) *
                          UINT64_C(1099511628211);
            }
          }
          trace_resolve_line(fmt::format(
              "SOURCE_ROWS call={} key={} host_rows={}..{} hash={:016X}",
              resolve_call_count_, region.surface->key.Describe(), row, end - 1, band_hash));
        }
      }
      constexpr uint32_t kMaxTraceResolveBytes = 64u << 20;
      const uint32_t length = plan.info.copy_dest_extent_length;
      if (length && length <= kMaxTraceResolveBytes) {
        std::vector<uint8_t> mirror_bytes(length);
        const bool mirror_read = upload_sink->ReadbackFromHostBuffer(
            plan.info.copy_dest_extent_start, mirror_bytes.data(), length);
        uint64_t mirror_hash_value = UINT64_C(1469598103934665603);
        uint64_t mirror_nonzero = 0;
        if (mirror_read) {
          for (uint32_t i = 0; i < length; ++i) {
            mirror_hash_value = (mirror_hash_value ^ mirror_bytes[i]) *
                                UINT64_C(1099511628211);
            mirror_nonzero += mirror_bytes[i] != 0;
          }
        }
        bool staging_read = false;
        bool staging_matches_mirror = false;
        if (use_staging_copy) {
          std::vector<uint8_t> staging_bytes(length);
          staging_read =
              upload_sink->ReadbackResolveStaging(0, staging_bytes.data(), length);
          staging_matches_mirror =
              staging_read && mirror_read &&
              std::memcmp(staging_bytes.data(), mirror_bytes.data(), length) == 0;
        }
        trace_resolve_line(fmt::format(
            "MIRROR call={} range=0x{:X}+{} read={} hash={:016X} nonzero_bytes={} "
            "staging_read={} staging_matches_mirror={}",
            resolve_call_count_, plan.info.copy_dest_extent_start, length, mirror_read,
            mirror_hash_value, mirror_nonzero, staging_read, staging_matches_mirror));
        if (selected_swap_resolve_trace && mirror_read) {
          const uint32_t bytes_per_pixel_log2 = (plan.dest_info >> 16) & 3u;
          const uint64_t row_bytes = uint64_t(plan.dest_pitch) << bytes_per_pixel_log2;
          if (row_bytes && row_bytes <= length) {
            const uint32_t row_count = uint32_t((uint64_t(length) + row_bytes - 1) / row_bytes);
            for (uint32_t row = 0; row < row_count; row += 16) {
              const uint64_t begin = uint64_t(row) * row_bytes;
              const uint64_t end =
                  std::min<uint64_t>(length, uint64_t(row + 16) * row_bytes);
              uint64_t row_hash = UINT64_C(1469598103934665603);
              for (uint64_t byte = begin; byte < end; ++byte) {
                row_hash = (row_hash ^ mirror_bytes[size_t(byte)]) * UINT64_C(1099511628211);
              }
              trace_resolve_line(fmt::format(
                  "MIRROR_ROWS call={} origin=0x{:X} rows={}..{} row_bytes={} hash={:016X}",
                  resolve_call_count_, plan.info.copy_dest_extent_start, row,
                  std::min(row + 15u, row_count - 1u), row_bytes, row_hash));
            }
          } else {
            trace_resolve_line(fmt::format(
                "MIRROR_ROWS call={} unavailable dest_pitch={} bpp_log2={} length={}",
                resolve_call_count_, plan.dest_pitch, bytes_per_pixel_log2, length));
          }
        }
      } else {
        trace_resolve_line(fmt::format("MIRROR call={} range=0x{:X}+{} read=skipped",
                                       resolve_call_count_, plan.info.copy_dest_extent_start,
                                       length));
      }
    }
    if (resolve_trace_file) std::fflush(resolve_trace_file.get());
  }
  if (complete && REXCVAR_GET(fh1_debug_resolve_store_probe)) {
    WaitForGpu();
    uint32_t marker = 0;
    // The probe shader stores at byte address 0 OF THE BOUND VIEW, so the
    // marker is read at the view's own base, not at the resolve's destination.
    const uint32_t marker_offset = window_probe_mb > 0 ? kWindowProbeStart : 0u;
    const bool read =
        small_uav_probe
            ? upload_sink->ReadbackProbeBuffer(0, &marker, sizeof(marker))
            : upload_sink->ReadbackFromHostBuffer(marker_offset, &marker, sizeof(marker));
    static uint32_t marker_logs = 0;
    if (++marker_logs <= 12) {
      REXGPU_INFO(
          "FH1 resolve store marker: target={} window_mb={} offset={:08X} read={} word={:08X} "
          "(expected DEADBEEF)",
          small_uav_probe ? "4MB probe UAV"
                          : (window_probe_mb > 0 ? "mirror window" : "mirror resolve window"),
          window_probe_mb, marker_offset, read, marker);
    }
  }
  if (mirror_before_ok) {
    ++store_probe_logs;
    uint64_t after_hash = 0;
    uint64_t after_nonzero = 0;
    const bool after_ok = mirror_hash(plan.info.copy_dest_extent_start,
                                      plan.info.copy_dest_extent_length, after_hash, after_nonzero);
    uint64_t probe_hash = 0;
    uint64_t probe_nonzero = 0;
    const bool probe_ok =
        mirror_hash(plan.info.copy_dest_extent_start + plan.info.copy_dest_extent_length,
                    plan.info.copy_dest_extent_length, probe_hash, probe_nonzero);
    if (!after_ok || after_hash == mirror_before_hash) {
      // The store left the extent untouched: find where it did land by
      // fingerprinting the whole 512 MB mirror in 4 MB chunks around the
      // dispatch (first few times only - this is a one-off probe).
      static uint32_t sweep_logs = 0;
      if (sweep_logs < 3) {
        ++sweep_logs;
        constexpr uint32_t kChunk = 4u << 20;
        constexpr uint32_t kChunkCount = SharedMemory::kBufferSize / kChunk;
        static std::vector<uint64_t> before_signatures(kChunkCount, 0);
        static bool have_signatures = false;
        std::vector<uint64_t> signatures(kChunkCount, 0);
        std::vector<uint8_t> chunk(kChunk);
        for (uint32_t c = 0; c < kChunkCount; ++c) {
          if (!upload_sink->ReadbackFromHostBuffer(c * kChunk, chunk.data(), kChunk)) {
            continue;
          }
          uint64_t hash = 1469598103934665603ull;
          for (uint32_t i = 0; i < kChunk; i += 64) {
            hash = (hash ^ chunk[i]) * 1099511628211ull;
          }
          signatures[c] = hash;
        }
        std::string changed_chunks;
        if (have_signatures) {
          for (uint32_t c = 0; c < kChunkCount; ++c) {
            if (signatures[c] != before_signatures[c]) {
              changed_chunks += " " + std::to_string(c);
            }
          }
        }
        before_signatures.swap(signatures);
        have_signatures = true;
        REXGPU_INFO("FH1 resolve store sweep: call={} changed mirror chunks ({} MB each):{}",
                    resolve_call_count_, kChunk >> 20,
                    changed_chunks.empty() ? " none" : changed_chunks.c_str());
      }
    }
    REXGPU_INFO(
        "FH1 resolve store probe: call={} range={:08X}+{} rect={},{}x{},{} dest_base={:08X} "
        "pitch={} info={:08X} (pack={} endian={} swap={} bias={} bpp_log2={}) surface={} "
        "samples={} | mirror before={:016X}/{} after={:016X}/{} ok={} changed={} | "
        "next_extent={:016X}/{}",
        resolve_call_count_, plan.info.copy_dest_extent_start,
        plan.info.copy_dest_extent_length, plan.x0, plan.y0, plan.x1, plan.y1, plan.dest_base,
        plan.dest_pitch, plan.dest_info, plan.dest_info & 7u, (plan.dest_info >> 3u) & 7u,
        (plan.dest_info >> 6u) & 1u, int32_t(plan.dest_info << 16) >> 24,
        (plan.dest_info >> 16u) & 3u,
        source.surface ? source.surface->key.Describe() : "none",
        source.surface ? source.surface->samples : 0, mirror_before_hash, mirror_before_nonzero,
        after_hash, after_nonzero, after_ok, after_hash != mirror_before_hash, probe_hash,
        probe_nonzero);
  }
  // FH1 resolve probe (Phase 2f) - after: read the mirror at the destination
  // extent and the staging buffer now that the resolve + staging copy have
  // completed, and log the before/after pixel verdict.
  if (resolve_probe_pending) {
    uint64_t resolve_probe_after_words_nz = 0;
    uint32_t resolve_probe_after_first = 0;
    double resolve_probe_after_mean[4] = {0.0, 0.0, 0.0, 0.0};
    bool resolve_probe_after_ok = false;
    uint64_t resolve_probe_staging_words_nz = 0;
    uint32_t resolve_probe_staging_first = 0;
    bool resolve_probe_staging_ok = false;
    // The mirror is final here: the staging copy ran above and mirror_hash
    // (store probe) already WaitForGpu'd. Read the mirror extent.
    if (WaitForGpu()) {
      resolve_readback_scratch_.resize(resolve_probe_extent_length);
      if (upload_sink->ReadbackFromHostBuffer(resolve_probe_extent_start,
                                              resolve_readback_scratch_.data(),
                                              resolve_probe_extent_length)) {
        const uint8_t* bytes = resolve_readback_scratch_.data();
        const size_t len = resolve_probe_extent_length;
        uint64_t lane_sum[4] = {0, 0, 0, 0};
        uint64_t lane_count[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < len; ++i) {
          lane_sum[i & 3] += bytes[i];
          ++lane_count[i & 3];
        }
        for (int lane = 0; lane < 4; ++lane) {
          resolve_probe_after_mean[lane] =
              lane_count[lane] ? double(lane_sum[lane]) / double(lane_count[lane]) : 0.0;
        }
        for (size_t i = 0; i + 3 < len; i += 4) {
          if (*reinterpret_cast<const uint32_t*>(bytes + i)) {
            ++resolve_probe_after_words_nz;
          }
        }
        resolve_probe_after_first =
            len >= 4 ? *reinterpret_cast<const uint32_t*>(bytes) : 0;
        resolve_probe_after_ok = true;
      }
      // The staging buffer holds the dispatch's own output, independent of
      // the copy into the mirror.
      if (use_staging_copy) {
        resolve_readback_scratch_.resize(resolve_probe_extent_length);
        if (upload_sink->ReadbackResolveStaging(0, resolve_readback_scratch_.data(),
                                                resolve_probe_extent_length)) {
          const uint8_t* sbytes = resolve_readback_scratch_.data();
          for (size_t i = 0; i + 3 < resolve_probe_extent_length; i += 4) {
            if (*reinterpret_cast<const uint32_t*>(sbytes + i)) {
              ++resolve_probe_staging_words_nz;
            }
          }
          resolve_probe_staging_first = resolve_probe_extent_length >= 4
                                            ? *reinterpret_cast<const uint32_t*>(sbytes)
                                            : 0;
          resolve_probe_staging_ok = true;
        }
      }
    }
    REXGPU_INFO(
        "FH1 resolve probe: swap={} probe#{} call={} src={} band={} dest_base={:08X} "
        "dest_pitch={} extent={:08X}+{} bpp_log2={} staging_ok={} staging_words_nz={} "
        "staging_first={:08X} mirror_before_ok={} before_mean={:.1f},{:.1f},{:.1f},{:.1f} "
        "before_words_nz={} before_first={:08X} mirror_after_ok={} after_mean={:.1f},{:.1f},{:.1f},{:.1f} "
        "after_words_nz={} after_first={:08X}",
        presented_output_frames_, resolve_probe_index, resolve_call_count_,
        plan.resolve_key.Describe(), untile_band, resolve_probe_dest_base,
        resolve_probe_dest_pitch, resolve_probe_extent_start, resolve_probe_extent_length,
        (resolve_probe_dest_info >> 16u) & 3u, resolve_probe_staging_ok ? 1 : 0,
        resolve_probe_staging_words_nz, resolve_probe_staging_first,
        resolve_probe_before_ok ? 1 : 0, resolve_probe_before_mean[0],
        resolve_probe_before_mean[1], resolve_probe_before_mean[2], resolve_probe_before_mean[3],
        resolve_probe_before_words_nz, resolve_probe_before_first,
        resolve_probe_after_ok ? 1 : 0, resolve_probe_after_mean[0], resolve_probe_after_mean[1],
        resolve_probe_after_mean[2], resolve_probe_after_mean[3], resolve_probe_after_words_nz,
        resolve_probe_after_first);
  }
  // FH1 follow-up probe (Q2): does the resolve source already contain grey,
  // or does the resolve turn it dark? At resolve time for the scene target
  // (dest 1C4E1000), read the ACTUAL MSAA source texture as the resolve sees
  // it (ResolveSubresource to 1x + staging readback, the minimap-source
  // pattern) and compare its pixel profile for the resolve rect with the
  // mirror bytes for the same rect. One consistent capture: the source
  // readback and the mirror hash are taken back-to-back after the dispatch.
  // Purely diagnostic: readbacks + log lines only.
  {
    const int64_t source_probe_from = REXCVAR_GET(fh1_debug_resolve_source_probe_from);
    static uint32_t source_probe_logs = 0;
    if (source_probe_from > 0 && presented_output_frames_ == uint64_t(source_probe_from) &&
        !plan.copying_depth && source.surface && source.surface->texture &&
        plan.info.copy_dest_extent_start == 0x1C4E1000u && source_probe_logs < 8) {
      ++source_probe_logs;
      DrainTapeForDiagnostic("resolve source probe");
      D3D11_TEXTURE2D_DESC src_desc{};
      source.surface->texture->GetDesc(&src_desc);
      D3D11_TEXTURE2D_DESC res_desc = src_desc;
      res_desc.SampleDesc.Count = 1;
      res_desc.SampleDesc.Quality = 0;
      res_desc.Usage = D3D11_USAGE_DEFAULT;
      res_desc.BindFlags = 0;
      res_desc.CPUAccessFlags = 0;
      res_desc.MiscFlags = 0;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> resolved;
      D3D11_TEXTURE2D_DESC stg_desc = res_desc;
      stg_desc.Usage = D3D11_USAGE_STAGING;
      stg_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
      ID3D11Device* probe_device = GetD3D11Provider().GetDevice();
      ID3D11DeviceContext* probe_context = GetD3D11Provider().GetImmediateContext();
      const HRESULT cr = probe_device->CreateTexture2D(&res_desc, nullptr, &resolved);
      const HRESULT cs = probe_device->CreateTexture2D(&stg_desc, nullptr, &staging);
      auto half_to_float = [](uint16_t h) -> float {
        const uint32_t s = (uint32_t(h) & 0x8000u) << 16;
        const uint32_t e = (uint32_t(h) >> 10) & 0x1Fu;
        const uint32_t m = uint32_t(h) & 0x3FFu;
        uint32_t bits = 0;
        if (e == 0) {
          bits = s;  // Zero (subnormals read as 0: far below the grey tested for).
        } else if (e == 31) {
          bits = s | 0x7F800000u | (m << 13);  // Inf/NaN.
        } else {
          bits = s | ((e + 112u) << 23) | (m << 13);
        }
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
      };
      uint64_t src_nz = 0;
      uint64_t src_total = 0;
      double src_mean0 = 0.0;
      float src_min0 = 0.0f;
      float src_max0 = 0.0f;
      bool src_have_floats = false;
      std::string src_samples;
      bool src_ok = false;
      if (SUCCEEDED(cr) && SUCCEEDED(cs)) {
        probe_context->ResolveSubresource(resolved.Get(), 0, source.surface->texture, 0,
                                          src_desc.Format);
        probe_context->CopyResource(staging.Get(), resolved.Get());
        D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        HRESULT query_result = E_FAIL;
        if (SUCCEEDED(probe_device->CreateQuery(&query_desc, &query))) {
          probe_context->End(query.Get());
          probe_context->Flush();
          while ((query_result = probe_context->GetData(query.Get(), nullptr, 0, 0)) ==
                 S_FALSE) {
            std::this_thread::yield();
          }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT map_result =
            query_result == S_OK
                ? probe_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)
                : E_FAIL;
        if (SUCCEEDED(map_result)) {
          src_ok = true;
          const uint32_t y0 = std::min<uint32_t>(plan.y0 + source_row_offset, src_desc.Height);
          const uint32_t y1 = std::min<uint32_t>(plan.y1 + source_row_offset, src_desc.Height);
          const uint32_t x0 = std::min<uint32_t>(plan.x0, src_desc.Width);
          const uint32_t x1 = std::min<uint32_t>(plan.x1, src_desc.Width);
          if (src_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            src_have_floats = true;
            bool first = true;
            const uint32_t sample_rows[3] = {y0, (y0 + y1) / 2, y1 ? y1 - 1 : y0};
            const uint32_t sample_cols[3] = {x0, (x0 + x1) / 2, x1 ? x1 - 1 : x0};
            for (uint32_t y = y0; y < y1; ++y) {
              const uint16_t* row = reinterpret_cast<const uint16_t*>(
                  static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch);
              for (uint32_t x = x0; x < x1; ++x) {
                const uint16_t* texel = row + size_t(x) * 4;
                const float r = half_to_float(texel[0]);
                if (texel[0] || texel[1] || texel[2] || texel[3]) {
                  ++src_nz;
                }
                ++src_total;
                src_mean0 += r;
                if (first || r < src_min0) {
                  src_min0 = r;
                }
                if (first || r > src_max0) {
                  src_max0 = r;
                }
                first = false;
              }
            }
            if (src_total) {
              src_mean0 /= double(src_total);
            }
            for (int s = 0; s < 3; ++s) {
              const uint16_t* texel = reinterpret_cast<const uint16_t*>(
                  static_cast<const uint8_t*>(mapped.pData) +
                  size_t(sample_rows[s]) * mapped.RowPitch) +
                  size_t(sample_cols[s]) * 4;
              src_samples += fmt::format(
                  " ({},{})=({:.6g},{:.6g},{:.6g},{:.6g})", sample_cols[s], sample_rows[s],
                  half_to_float(texel[0]), half_to_float(texel[1]), half_to_float(texel[2]),
                  half_to_float(texel[3]));
            }
          } else {
            // Unknown source format: byte-level nonzero only (unmeasured here).
            src_total = 0;
            src_nz = 0;
          }
          probe_context->Unmap(staging.Get(), 0);
        }
      }
      // Mirror bytes for the same rect: the store probe's hash (sampled 1 in
      // 16 bytes) plus exact texel stats over the scratch it just filled
      // (dest is 10:10:10:2, 4 bytes/texel, row stride = dest_pitch).
      uint64_t mir_hash = 0;
      uint64_t mir_nz_sampled = 0;
      const bool mir_ok = mirror_hash(plan.info.copy_dest_extent_start,
                                      plan.info.copy_dest_extent_length, mir_hash,
                                      mir_nz_sampled);
      uint64_t mir_nz = 0;
      uint64_t mir_total = 0;
      double mir_mean_r = 0.0;
      double mir_mean_g = 0.0;
      double mir_mean_b = 0.0;
      if (mir_ok && plan.dest_pitch >= 4 && (plan.dest_pitch % 4) == 0 &&
          (plan.info.copy_dest_extent_length % 4) == 0) {
        const uint32_t mir_rows =
            std::min<uint32_t>(plan.y1 - plan.y0,
                               plan.info.copy_dest_extent_length / plan.dest_pitch);
        const uint32_t mir_cols =
            std::min<uint32_t>(plan.x1 - plan.x0, plan.dest_pitch / 4);
        for (uint32_t y = 0; y < mir_rows; ++y) {
          const uint32_t* row = reinterpret_cast<const uint32_t*>(
              resolve_readback_scratch_.data() + size_t(y) * plan.dest_pitch);
          for (uint32_t x = 0; x < mir_cols; ++x) {
            const uint32_t w = row[x];
            if (w != 0) {
              ++mir_nz;
            }
            ++mir_total;
            mir_mean_r += double(w & 0x3FFu) / 1023.0;
            mir_mean_g += double((w >> 10) & 0x3FFu) / 1023.0;
            mir_mean_b += double((w >> 20) & 0x3FFu) / 1023.0;
          }
        }
        if (mir_total) {
          mir_mean_r /= double(mir_total);
          mir_mean_g /= double(mir_total);
          mir_mean_b /= double(mir_total);
        }
      }
      REXGPU_INFO(
          "FH1 resolve source probe: swap={} call={} dest={:08X}+{} rect={},{}-{},{} "
          "srcrowoff={} source={} srcfmt={} src={}x{} samples={} | source ok={} floats={} "
          "nz={}/{} mean0={:.6g} min0={:.6g} max0={:.6g} samples={} | mirror ok={} "
          "hash={:016X}/{} nz={}/{} meanRGB=({:.6g},{:.6g},{:.6g})",
          presented_output_frames_, resolve_call_count_, plan.info.copy_dest_extent_start,
          plan.info.copy_dest_extent_length, plan.x0, plan.y0, plan.x1, plan.y1,
          source_row_offset, source.surface ? source.surface->key.Describe() : "none",
          uint32_t(src_desc.Format), src_desc.Width, src_desc.Height,
          uint32_t(src_desc.SampleDesc.Count), src_ok, src_have_floats ? 1 : 0, src_nz,
          src_total, src_mean0, src_min0, src_max0, src_samples, mir_ok ? 1 : 0, mir_hash,
          mir_nz_sampled, mir_nz, mir_total, mir_mean_r, mir_mean_g, mir_mean_b);
    }
  }
  const bool resolve_complete = complete;
  if (trace_resolve) {
    trace_resolve_line(fmt::format(
        "CLEAR_ORDER call={} clear_requested={} owner_segments={} succeeded={} "
        "all_succeeded={} resolve_complete={} recorded_submit_ok={} gpu_wait={}",
        resolve_call_count_, resolve_clear_requested, uint32_t(resolve_sources.size()),
        owner_segments_succeeded, owner_segments_succeeded == resolve_sources.size(),
        resolve_complete, owner_batch_submit_ok, trace_gpu_completed));
  }
  complete &= apply_resolve_clears();
  if (resolve_complete && !small_uav_probe && window_probe_mb <= 0) {
    texture_cache_->MarkRangeAsResolved(plan.info.copy_dest_extent_start,
                                        plan.info.copy_dest_extent_length);
  }
  // The resolve wrote the guest texture word layout into the GPU mirror of
  // shared memory (a DEFAULT buffer), not into guest memory itself. Guest
  // memory is what the swap's untiling upload reads, so the frame has to be
  // pulled back from the mirror before the guest presents it - that happens at
  // the swap, for exactly the extent the swap fetch describes
  // (fh1_d3d11_resolve_readback, IssueSwap), rather than per resolve extent:
  // a resolve lands one destination's pixels, and pulling every one of them
  // back stalls the GPU once per resolve (~23 resolves per swap in the title),
  // while the presented frame is one buffer at one pitch and height.
  // MarkRangeAsResolved above made the pages GPU-written and valid, which is
  // what CopyReadbackToGuest expects when the swap reads it back.
  // Record where this resolve landed for the swap trace: the guest range the
  // resolve wrote, and the destination base/pitch the guest named for it.
  last_resolve_guest_base_ = plan.info.copy_dest_extent_start;
  last_resolve_length_ = plan.info.copy_dest_extent_length;
  last_resolve_pitch_ = plan.dest_pitch;
  last_resolve_info_ = plan.dest_info;
  last_resolve_swap_ = presented_output_frames_;
  has_last_resolve_ = true;
  ++resolve_call_count_;
  return complete;
}

bool D3D11CommandProcessor::RefreshResolvedGuestRange(uint32_t start, uint32_t length) {
  if (!shared_memory_ || !length || start >= SharedMemory::kBufferSize) {
    return false;
  }
  length = std::min(length, uint32_t(SharedMemory::kBufferSize - start));
  // A resolve marked its destination GPU-written. Pages without that flag were
  // not written by the GPU since the CPU last had them, so guest memory already
  // holds what a CPU-side reader would upload - no readback for those.
  const auto pages = shared_memory_->CountValidPages(start, length);
  if (pages.second == 0) {
    return false;
  }
  auto* upload_sink =
      static_cast<D3D11SharedMemoryDeviceUploadSink*>(shared_memory_->upload_sink());
  if (!upload_sink) {
    return false;
  }
  // The resolve's dispatches may still be queued on the immediate context, and
  // the mirror is about to be read on the CPU. This backend has no submission
  // fences, so the completion point is the swap's own wait. Recorded resolve
  // work is replayed first (a safety net: submission happens at the resolve
  // boundary, so the tape is normally empty here).
  //
  // Wait batching: a swap's texture loads ask for several GPU-written ranges
  // that the same resolves produced, and each one used to pay a full GPU wait
  // here. Once a wait has drained the GPU, a later refresh for a range no
  // newer resolve has written needs no second wait - the mirror data it reads
  // was already final when the last wait returned, and the readback's own Map
  // orders the copy after it. The resolve count is the signal: it only moves
  // when a resolve lands (and marks pages GPU-written), so an unchanged count
  // means no new mirror data exists. Skips the tape replay too - it only
  // matters when a resolve landed, and the tape was flushed by that wait.
  const bool refresh_wait_ready =
      REXCVAR_GET(d3d11_skip_texture_refresh) && refresh_waited_ &&
      refresh_waited_resolve_count_ == resolve_call_count_;
  if (refresh_wait_ready) {
    ++refresh_waits_skipped_;
  } else {
    if (command_lists_enabled_) {
      SubmitFrameTape("resolved range refresh");
    }
    const uint64_t refresh_wait_start = rex::chrono::Clock::QueryHostTickCount();
    if (!WaitForGpu()) {
      return false;
    }
    refresh_wait_ticks_ += rex::chrono::Clock::QueryHostTickCount() - refresh_wait_start;
    ++refresh_waits_done_;
    refresh_waited_ = true;
    refresh_waited_resolve_count_ = resolve_call_count_;
  }
  resolve_readback_scratch_.resize(length);
  if (!upload_sink->ReadbackFromHostBuffer(start, resolve_readback_scratch_.data(), length)) {
    if (!resolve_refresh_failed_logged_) {
      resolve_refresh_failed_logged_ = true;
      REXGPU_ERROR("D3D11 FH1 resolve refresh: readback of {:08X}+{} failed", start, length);
    }
    return false;
  }
  shared_memory_->CopyResolvedRangeToGuest(start, resolve_readback_scratch_.data(), length);
  if (minimap_capture_resolved_ &&
      presented_output_frames_ == uint64_t(REXCVAR_GET(fh1_debug_minimap_capture_swap)) &&
      start <= minimap_capture_guest_base_ &&
      minimap_capture_guest_base_ - start < length) {
    const uint32_t offset = minimap_capture_guest_base_ - start;
    const uint32_t available = length - offset;
    const uint32_t row_stride = minimap_capture_guest_pitch_;
    const uint32_t rows = row_stride ? std::min(minimap_capture_guest_height_, available / row_stride) : 0;
    std::vector<uint64_t> row_hashes;
    const uint64_t hash = row_stride && rows
                              ? HashRows(resolve_readback_scratch_.data() + offset, row_stride,
                                         rows, std::min(row_stride, available), &row_hashes)
                              : 0;
    std::string row_text;
    for (uint32_t row = 0; row < rows; row += 16) {
      if (!row_text.empty()) row_text += ',';
      row_text += fmt::format("{}:{:016X}", row, row_hashes[row]);
    }
    REXGPU_INFO("FH1 minimap guest: swap={} base={:08X} copied_start={:08X} copied_extent={} "
                "offset={} stride={} rows={} width={} height={} hash={:016X} rows={}",
                presented_output_frames_, minimap_capture_guest_base_, start, length, offset,
                row_stride, rows, minimap_capture_guest_width_, minimap_capture_guest_height_,
                hash, row_text);
    minimap_capture_resolved_ = false;
  }
  const auto gpu_written_after = shared_memory_->CountValidPages(start, length).second;
  ++resolve_refresh_count_;
  resolve_refresh_bytes_ += length;
  if (REXCVAR_GET(fh1_swap_fetch_trace) && resolve_refresh_count_ <= 12) {
    REXGPU_INFO(
        "FH1 resolve refresh: range={:08X}+{} pages gpu_written_before={} after={} "
        "total={} ranges {} MB",
        start, length, pages.second, gpu_written_after, resolve_refresh_count_,
        resolve_refresh_bytes_ >> 20);
  }
  return true;
}

bool D3D11CommandProcessor::WaitForGpu() {
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  if (!context) {
    return false;
  }
  if (!gpu_wait_query_) {
    D3D11_QUERY_DESC query_desc = {};
    query_desc.Query = D3D11_QUERY_EVENT;
    if (FAILED(GetD3D11Provider().GetDevice()->CreateQuery(&query_desc, &gpu_wait_query_))) {
      gpu_wait_query_.Reset();
      return false;
    }
  }
  context->End(gpu_wait_query_.Get());
  // Spin rather than Map, which only covers a subresource, not the queue.
  while (context->GetData(gpu_wait_query_.Get(), nullptr, 0, 0) == S_FALSE) {
    std::this_thread::yield();
  }
  return true;
}

void D3D11CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  // The frame counter must advance for any capture that needs it, not just the
  // JSONL audit: the pass trace and the probe are both gated on frame id.
  if (REXCVAR_GET(pinyon_audit) || REXCVAR_GET(pinyon_audit_passes) ||
      REXCVAR_GET(pinyon_transition_log) || REXCVAR_GET(fh1_forensics_capture)) {
    rex::graphics::render_audit::SetContext("d3d11", "GTX 570", 1280, 720);
    rex::graphics::render_audit::EnsureStarted();
    rex::graphics::render_audit::SetFrameWindow(
        int32_t(REXCVAR_GET(pinyon_audit_passes_from)),
        int32_t(REXCVAR_GET(pinyon_audit_passes_to)));
    rex::graphics::render_audit::EndFrame(REXCVAR_GET(pinyon_transition_log));
    // Work after this swap belongs to the frame that is about to be presented,
    // so the recorder clock leads the just-finished audit frame by one.
    rex::graphics::render_forensics::SetFrame(rex::graphics::render_audit::FrameId() + 1);
    rex::graphics::render_forensics::EndFrame();
  }
  // Phase 3 M2: the swap boundary flushes any recorded resolve work before the
  // swap's readback, surface dump or untiling upload reads GPU results. With
  // submission at the resolve boundary this is a safety net (normally empty).
  if (command_lists_enabled_) {
    SubmitFrameTape("swap");
  }
  // fh1_swap_fetch_trace: what the guest asked for (the swap packet's front
  // buffer pointer, and texture fetch constant 0, which VdSwap fills in as the
  // front buffer description) against where the native resolve path last put
  // the frame. Two questions, one line each swap: does the guest's descriptor
  // change across the transition, and does the resolve write the range the
  // descriptor names?
  if (REXCVAR_GET(fh1_swap_fetch_trace)) {
    const xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(0);
    const uint32_t fetch_base = uint32_t(fetch.base_address) << 12;
    const uint32_t fetch_width = uint32_t(fetch.size_2d.width) + 1;
    const uint32_t fetch_height = uint32_t(fetch.size_2d.height) + 1;
    const uint32_t* guest_front_words = reinterpret_cast<const uint32_t*>(
        memory_->TranslatePhysical(fetch_base));
    // What the presented image actually holds right now: the first word is
    // routinely black on a real frame, so count non-zero words too.
    uint64_t fb_nonzero_words = 0;
    if (guest_front_words) {
      const uint64_t fb_bytes = std::min<uint64_t>(
          uint64_t(fetch_width) * uint64_t(fetch_height) * 4u, uint64_t(4u << 20));
      for (uint64_t i = 0; i < fb_bytes / 4u; i += 32u) {
        fb_nonzero_words += guest_front_words[i] != 0;
      }
    }
    REXGPU_INFO(
        "FH1 swap trace: swap={} fb_ptr={:08X} packet={}x{} fetch0=[{:08X} {:08X} {:08X} {:08X}] "
        "base={:08X} pitch={} tiled={} fmt={} stacked={} {}x{} guest_first={:08X} "
        "guest_words_nz={} | last "
        "resolve call={} range={:08X}+{} pitch={} info={:08X} at swap={} ({} ago)",
        presented_output_frames_, frontbuffer_ptr, frontbuffer_width, frontbuffer_height,
        fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch_base,
        uint32_t(fetch.pitch), uint32_t(fetch.tiled), uint32_t(fetch.format),
        uint32_t(fetch.stacked), fetch_width, fetch_height,
        guest_front_words ? guest_front_words[0] : 0, fb_nonzero_words, resolve_call_count_,
        last_resolve_guest_base_, last_resolve_length_, last_resolve_pitch_, last_resolve_info_,
        has_last_resolve_ ? last_resolve_swap_ : uint64_t(0),
        has_last_resolve_ ? presented_output_frames_ - last_resolve_swap_ : uint64_t(0));
  }
  if (native_executor_) {
    native_executor_->OnSwap(presented_output_frames_, frontbuffer_ptr);
  }
  // Hosts count output frames here (render tests pace and stop on them).
  const auto& renderer = d3d11_graphics_system_->native_guest_output_renderer();
  if (renderer.IsRegistered()) {
    system::NativeGuestOutputRenderContext context;
    context.backend = system::NativeGuestOutputBackend::kD3D11;
    context.phase = system::NativeGuestOutputPhase::kPresented;
    context.guest_output_width = frontbuffer_width;
    context.guest_output_height = frontbuffer_height;
    context.display_width = frontbuffer_width;
    context.display_height = frontbuffer_height;
    context.frame_sequence = presented_output_frames_;
    // The draws render into the native EDRAM surfaces and the resolve path
    // writes the frontbuffer into guest memory, so the presented image is the
    // resolved one - the drawing renderer, mirroring the Direct3D 12 swap.
    context.presenter = system::NativeGuestOutputPresenter::kNativeExecutor;
    renderer.Invoke(context);
  }

  // Present the guest frontbuffer: the pixels the resolve path wrote into
  // guest memory, blitted into the guest output image the paint path shows.
  // Until the frontbuffer is resolvable (or on a format this backend cannot
  // host) the output stays the black clear, so presentation never depends on
  // the resolve having happened.
  ::rex::ui::Presenter* presenter = d3d11_graphics_system_->presenter();
  if (presenter && frontbuffer_width && frontbuffer_height) {
    xenos::TextureFormat frontbuffer_format = xenos::TextureFormat::k_8_8_8_8;
    uint32_t swap_width = 0;
    uint32_t swap_height = 0;
    ID3D11ShaderResourceView* swap_srv = nullptr;
    if (texture_cache_) {
      // There are no submission fences on this backend, so wait for the GPU
      // before reading the frontbuffer's memory on the CPU - the resolve
      // dispatches that produced it may still be running. Timed because this
      // is the presentation path's serialisation point (Step E measurement).
      const uint64_t swap_wait_start = rex::chrono::Clock::QueryHostTickCount();
      WaitForGpu();
      swap_wait_ticks_ += rex::chrono::Clock::QueryHostTickCount() - swap_wait_start;
      ++swap_wait_count_;
      // Periodic rather than shutdown-only: a render-test run can be stopped
      // without a clean shutdown, and this is the Step E number.
      if (!(swap_wait_count_ % 600)) {
        LogSwapWait();
        // The resolved-range miss rate: what landing the GPU's resolve output in
        // guest memory costs, per swap (the fix for the world never reaching the
        // composite's texture upload).
        REXGPU_INFO(
            "D3D11 FH1 resolve refresh: {} ranges, {} MB total ({} ranges, {} MB this swap)",
            resolve_refresh_count_, resolve_refresh_bytes_ >> 20,
            resolve_refresh_count_ - resolve_refresh_swap_mark_,
            (resolve_refresh_bytes_ - resolve_refresh_swap_bytes_) >> 20);
        resolve_refresh_swap_mark_ = resolve_refresh_count_;
        resolve_refresh_swap_bytes_ = resolve_refresh_bytes_;
        const double refresh_wait_ms = double(refresh_wait_ticks_) * 1000.0 /
                                       double(rex::chrono::Clock::QueryHostTickFrequency());
        REXGPU_INFO(
            "D3D11 FH1 resolve refresh waits: {} done, {} skipped, {:.1f} ms total "
            "({:.3f} ms mean per wait)",
            refresh_waits_done_, refresh_waits_skipped_, refresh_wait_ms,
            refresh_waits_done_ ? refresh_wait_ms / double(refresh_waits_done_) : 0.0);
      }
      // Phase 3 M3: the tape's accounting, on the same periodic beat (a render-
      // test run is usually stopped before a clean shutdown, and the draw
      // counters only mean something next to how many draws reached the tape).
      if (command_lists_enabled_ && !(swap_wait_count_ % 64)) {
        LogTapeCounters();
      }
      // fh1_debug_surface_dump: the frame is complete at this point (the wait
      // above drained every resolve), so the surfaces are exactly what this
      // swap presents. One-shot, at fh1_debug_surface_dump_swap.
      DumpNativeSurfacesOnce();
      // fh1_d3d11_resolve_readback: land the frame this swap presents, and only
      // that frame, in guest memory. Texture fetch constant 0 names the
      // frontbuffer the guest is about to scan out; the native resolve wrote it
      // into the GPU mirror of shared memory (a DEFAULT buffer), while guest
      // memory holds whatever the CPU last put there - the boot frame - and
      // RequestSwapTexture's untiling upload below reads guest memory. So the
      // fetch's own extent is what has to come back, not a resolve's
      // destination rectangle: the last resolve of a frame may cover only part
      // of the presented buffer, and a resolve of another target doesn't cover
      // it at all. The GPU wait above has already made every resolve of this
      // frame complete, and CopyReadbackToGuest writes only pages the GPU still
      // owns, so a page the CPU wrote is never overwritten from the mirror.
      if (REXCVAR_GET(fh1_d3d11_resolve_readback)) {
        auto* upload_sink =
            static_cast<D3D11SharedMemoryDeviceUploadSink*>(shared_memory_->upload_sink());
        uint32_t frontbuffer_base = 0, frontbuffer_length = 0;
        // The extent is the fetch's, so it is trusted guest data: bound it by the
        // mirror before copying (a bad fetch must not read past the buffer).
        if (upload_sink && texture_cache_->GetSwapGuestExtent(frontbuffer_base, frontbuffer_length) &&
            frontbuffer_length != 0 && frontbuffer_base < SharedMemory::kBufferSize &&
            frontbuffer_length <= SharedMemory::kBufferSize - frontbuffer_base) {
          resolve_readback_scratch_.resize(frontbuffer_length);
          if (upload_sink->ReadbackFromHostBuffer(frontbuffer_base,
                                                  resolve_readback_scratch_.data(),
                                                  frontbuffer_length)) {
            shared_memory_->CopyReadbackToGuest(frontbuffer_base,
                                               resolve_readback_scratch_.data(),
                                               frontbuffer_length);
            ++resolve_readback_count_;
            resolve_readback_bytes_ += frontbuffer_length;
            if (REXCVAR_GET(fh1_swap_fetch_trace)) {
              const auto valid = shared_memory_->CountValidPages(frontbuffer_base,
                                                                 frontbuffer_length);
              const uint32_t* guest_words =
                  reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(frontbuffer_base));
              const uint32_t* readback_words =
                  reinterpret_cast<const uint32_t*>(resolve_readback_scratch_.data());
              // Non-zero WORDS: a byte-swapped frame puts a zero byte where an
              // unswapped one has data, so a byte-granular sample reads it as
              // blank. Counted in the readback and again in guest memory after
              // the copy, so a landed extent is visible as the two counts
              // agreeing rather than as one optimistic number.
              uint64_t readback_words_nz = 0, guest_words_nz = 0;
              const size_t word_count = frontbuffer_length / 4u;
              for (size_t i = 0; i < word_count; i += 32) {
                readback_words_nz += readback_words[i] != 0;
                if (guest_words) guest_words_nz += guest_words[i] != 0;
              }
              REXGPU_INFO(
                  "FH1 frontbuffer readback: swap={} range={:08X}+{} fetch={}x{} "
                  "pages_valid={} gpu_written={} readback_first={:08X} guest_first={:08X} "
                  "readback_words_nz={} guest_words_nz={}",
                  presented_output_frames_, frontbuffer_base, frontbuffer_length,
                  frontbuffer_width, frontbuffer_height, valid.first, valid.second,
                  readback_words[0], guest_words ? guest_words[0] : 0, readback_words_nz,
                  guest_words_nz);
            }
          } else if (!resolve_readback_failed_logged_) {
            resolve_readback_failed_logged_ = true;
            REXGPU_ERROR(
                "D3D11 FH1 native resolve: the frontbuffer readback failed ({:08X}+{}) - "
                "guest memory keeps its previous contents",
                frontbuffer_base, frontbuffer_length);
          }
        }
      }
      swap_srv = texture_cache_->RequestSwapTexture(frontbuffer_format, swap_width, swap_height);
      // fh1_debug_presented_pair: both images of this swap, one call, one
      // counter - the same-moment proof the surface dump could not give.
      DumpPresentedPairOnce();
    }
    if (!swap_texture_logged_) {
      swap_texture_logged_ = true;
      if (swap_srv) {
        REXGPU_INFO("D3D11 IssueSwap: presenting the frontbuffer ({}x{}, format {})", swap_width,
                    swap_height, uint32_t(frontbuffer_format));
      } else {
        REXGPU_WARN(
            "D3D11 IssueSwap: the frontbuffer is not resolvable as a guest texture ({}x{}, "
            "format {}) - presenting black",
            frontbuffer_width, frontbuffer_height, uint32_t(frontbuffer_format));
      }
    }
    auto* d3d11_presenter = static_cast<::rex::ui::d3d11::D3D11Presenter*>(presenter);
    presenter->RefreshGuestOutput(
        frontbuffer_width, frontbuffer_height, frontbuffer_width, frontbuffer_height,
        [this, d3d11_presenter, swap_srv, swap_width,
         swap_height](::rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          if (swap_srv && d3d11_presenter->PresentGuestOutputTexture(context, swap_srv,
                                                                     swap_width, swap_height)) {
            return true;
          }
          // No frontbuffer to show: keep the output black rather than leaving
          // the previous frame's contents in the reused image.
          auto& d3d11_context =
              static_cast<::rex::ui::d3d11::D3D11Presenter::D3D11GuestOutputRefreshContext&>(
                  context);
          ID3D11RenderTargetView* render_target_view = d3d11_context.render_target_view();
          if (!render_target_view) {
            return false;
          }
          auto* provider =
              static_cast<::rex::ui::d3d11::D3D11Provider*>(d3d11_graphics_system_->provider());
          if (!provider) {
            return false;
          }
          static bool fallback_logged = false;
          if (!fallback_logged) {
            fallback_logged = true;
            REXGPU_WARN("D3D11 IssueSwap: no blit, clearing the guest output");
          }
          static constexpr float kClearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
          provider->GetImmediateContext()->ClearRenderTargetView(render_target_view, kClearColor);
          // The clear is queued on the immediate context - flush it so the
          // work is submitted before the refresh succeeds (the contract of
          // GuestOutputRefreshContext::refresher).
          provider->GetImmediateContext()->Flush();
          return true;
        });
    if (swap_srv) {
      swap_srv->Release();
    }
  }
  ++presented_output_frames_;

  // FH1 call logger (Phase 2): frame boundary (joins to the guest XE_SWAP by swap).
  if (Fh1CallogEnabled(presented_output_frames_)) {
    REXGPU_INFO("FH1 callog side=A swap={} kind=present issued={} drawn={}",
                presented_output_frames_, draw_counters_.issued, draw_counters_.drawn);
  }

  // Frame boundary for the converted index buffer ring.
  if (primitive_processor_) {
    primitive_processor_->EndFrame();
  }

  // Per-frame draw accounting for the gate harness.
  if (draw_counters_.issued || draw_counters_.passes || draw_counters_.skipped_memexport ||
      draw_counters_.skipped_only_control || draw_counters_.skipped_both_barriers ||
      draw_counters_.passes_in_color_depth_mode || draw_counters_.passes_in_copy_mode_draw_paths ||
      draw_counters_.dropped_vshader_unbound || draw_counters_.dropped_fspin_unbound ||
      draw_counters_.dropped_both_shaders_unbound || draw_counters_.dropped_with_coherent_destination_in_copy_mode ||
      draw_counters_.dropped_screen_flipped_to_d24s8t || draw_counters_.dropped_both_barriers_unset_at_draw_time ||
      draw_counters_.dropped_tier1_sample_patterns_at_draw_time ||
      draw_counters_.drawn || draw_counters_.drawn_quads_in_color_depth_mode ||
      draw_counters_.dropped_quad_started_twice_in_same_pass ||
      draw_counters_.drawn_quad_started_twice_in_same_pass ||
      draw_counters_.dropped_poly_started_twice_in_same_pass ||
      draw_counters_.drawn_poly_started_twice_in_same_pass ||
      draw_counters_.dropped_invalid_poly_in_pass_with_quad_started ||
      draw_counters_.drawn_nonindex_passes || draw_counters_.drawn_index_passes ||
      draw_counters_.dropped_all || draw_counters_.dropped_other) {
    REXGPU_INFO(
        "D3D11 draws frame={}: passes={} skipped[memexport={} only_control={} both_barriers={}] "
        "color_depth_mode_passes={} copy_mode_draw_paths={} "
        "dropped[vshader_unbound={} fspin_unbound={} both_shaders_unbound={} "
        "coherent_dst_in_copy_mode={} screen_flipped_to_d24s8t={} both_barriers_unset={} "
        "tier1_sample_patterns_at_draw_time={}] "
        "drawn={} quads_in_color_depth_mode={} "
        "quad_started_twice[dropped={} drawn={}] "
        "poly_started_twice[dropped={} drawn={}] "
        "invalid_poly_in_pass_with_quad_started={} "
        "nonindex_passes={} index_passes={} "
        "dropped_all={} other={}], rejected_blobs={}",
        presented_output_frames_,
        draw_counters_.passes,
        draw_counters_.skipped_memexport,
        draw_counters_.skipped_only_control,
        draw_counters_.skipped_both_barriers,
        draw_counters_.passes_in_color_depth_mode,
        draw_counters_.passes_in_copy_mode_draw_paths,
        draw_counters_.dropped_vshader_unbound,
        draw_counters_.dropped_fspin_unbound,
        draw_counters_.dropped_both_shaders_unbound,
        draw_counters_.dropped_with_coherent_destination_in_copy_mode,
        draw_counters_.dropped_screen_flipped_to_d24s8t,
        draw_counters_.dropped_both_barriers_unset_at_draw_time,
        draw_counters_.dropped_tier1_sample_patterns_at_draw_time,
        draw_counters_.drawn,
        draw_counters_.drawn_quads_in_color_depth_mode,
        draw_counters_.dropped_quad_started_twice_in_same_pass,
        draw_counters_.drawn_quad_started_twice_in_same_pass,
        draw_counters_.dropped_poly_started_twice_in_same_pass,
        draw_counters_.drawn_poly_started_twice_in_same_pass,
        draw_counters_.dropped_invalid_poly_in_pass_with_quad_started,
        draw_counters_.drawn_nonindex_passes,
        draw_counters_.drawn_index_passes,
        draw_counters_.dropped_all,
        draw_counters_.dropped_other,
        D3D11Shader::rejected_blob_count());
    draw_counters_ = DrawCounters{};
  }

  if (native_resolve_ && !(presented_output_frames_ % 500)) {
    REXGPU_INFO("D3D11 FH1 native resolve: {} dispatches, {} skipped (last: {}), {} surfaces",
                native_resolve_->dispatch_count(), native_resolve_->skipped_count(),
                native_resolve_->last_skip() ? native_resolve_->last_skip() : "none",
                native_surfaces_ ? native_surfaces_->surface_count() : 0);
  }
}

// fh1_debug_presented_pair: the composite and the presented leg read from one
// call, at one swap. The two images therefore carry the same
// presented_output_frames_ counter on their log lines, which is the proof that
// they are the same moment - the earlier surface dump fired whenever its swap
// was first reached, which was not the moment a capture was taken, so a
// "composite is full, presented frame is half black" pair could not be trusted
// to describe one frame. Called after RequestSwapTexture has uploaded, so the
// second image is the texture the presenter is about to paint.
void D3D11CommandProcessor::DumpPresentedPairOnce() {
  if (!native_surfaces_) {
    return;
  }
  const std::string directory = REXCVAR_GET(fh1_debug_presented_pair);
  if (directory.empty()) {
    return;
  }
  const int64_t at_swap = REXCVAR_GET(fh1_debug_presented_pair_swap);
  if (at_swap > 0 && presented_output_frames_ < uint64_t(at_swap)) {
    return;
  }
  // More than one swap is what tells a stable frame from an alternating one,
  // so the pair repeats for fh1_debug_presented_pair_count swaps (default 1).
  const int32_t dumps_wanted = std::max<int32_t>(1, REXCVAR_GET(fh1_debug_presented_pair_count));
  if (presented_pair_dumps_ >= uint32_t(dumps_wanted)) {
    return;
  }
  ++presented_pair_dumps_;
  DrainTapeForDiagnostic("presented pair");
  WaitForGpu();
  ID3D11Device* device = GetD3D11Provider().GetDevice();
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  if (!device || !context) {
    return;
  }
  char name[96];

  // (a) The surface the presenting resolve sampled.
  D3D11Fh1NativeSurface* composed = native_surfaces_->FindSurface(last_resolve_source_key_.Pack());
  if (composed) {
    std::vector<uint8_t> rgb;
    uint32_t width = 0, height = 0;
    SurfaceDumpStats stats;
    std::string error;
    std::snprintf(name, sizeof(name), "%s/composite-%llu.ppm", directory.c_str(),
                  static_cast<unsigned long long>(presented_output_frames_));
    const bool read = ReadSurfaceRgb(device, context, composed, rgb, width, height, stats, error);
    const bool written = read && WriteRgbPpm(name, rgb, width, height);
    const BandProfile bands = read ? ProfileBands(rgb, width, height) : BandProfile{};
    REXGPU_INFO(
        "FH1 presented pair: swap={} stage=composite key={} {}x{} file={} status={} "
        "nonzero={:.4f} colours={} nz=[{}] mean=[{}]",
        presented_output_frames_, composed->key.Describe(), width, height,
        written ? name : "-", written ? std::string("written") : error,
        stats.total ? double(stats.nonzero) / double(stats.total) : 0.0, stats.distinct,
        bands.nonzero, bands.mean);
  } else {
    REXGPU_INFO("FH1 presented pair: swap={} stage=composite key={} surface=missing",
                presented_output_frames_, last_resolve_source_key_.Describe());
  }

  // (b) The untiled swap texture, i.e. the upload leg the presenter paints.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> presented;
  if (texture_cache_ && texture_cache_->swap_resource()) {
    if (FAILED(texture_cache_->swap_resource()->QueryInterface(
            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(presented.GetAddressOf())))) {
      presented.Reset();
    }
  }
  if (presented) {
    D3D11_TEXTURE2D_DESC desc;
    presented->GetDesc(&desc);
    const DXGI_FORMAT read_format = TypedReadbackFormat(desc.Format);
    std::vector<uint8_t> rgb;
    uint32_t width = 0, height = 0;
    SurfaceDumpStats stats;
    std::string error;
    std::snprintf(name, sizeof(name), "%s/presented-%llu.ppm", directory.c_str(),
                  static_cast<unsigned long long>(presented_output_frames_));
    const bool read =
        ReadTexture2DRgb(device, context, presented.Get(), read_format, rgb, width, height, stats,
                         error);
    const bool written = read && WriteRgbPpm(name, rgb, width, height);
    const BandProfile bands = read ? ProfileBands(rgb, width, height) : BandProfile{};
    REXGPU_INFO(
        "FH1 presented pair: swap={} stage=swap_texture stored_fmt={} read_fmt={} {}x{} file={} "
        "status={} nonzero={:.4f} colours={} nz=[{}] mean=[{}]",
        presented_output_frames_, uint32_t(desc.Format), uint32_t(read_format), width, height,
        written ? name : "-", written ? std::string("written") : error,
        stats.total ? double(stats.nonzero) / double(stats.total) : 0.0, stats.distinct,
        bands.nonzero, bands.mean);
  } else {
    REXGPU_INFO("FH1 presented pair: swap={} stage=swap_texture none", presented_output_frames_);
  }
}

void D3D11CommandProcessor::DumpNativeSurfacesOnce() {
  if (surface_dump_done_ || !native_surfaces_) {
    return;
  }
  const std::string directory = REXCVAR_GET(fh1_debug_surface_dump);
  if (directory.empty()) {
    return;
  }
  const int64_t dump_swap = REXCVAR_GET(fh1_debug_surface_dump_swap);
  if (dump_swap > 0 && presented_output_frames_ < uint64_t(dump_swap)) {
    return;
  }
  // Phase 3 M3: the surface reads below must observe every recorded draw.
  DrainTapeForDiagnostic("surface dump");
  surface_dump_done_ = true;
  WaitForGpu();
  ID3D11Device* device = GetD3D11Provider().GetDevice();
  ID3D11DeviceContext* context = GetD3D11Provider().GetImmediateContext();
  if (!device || !context) {
    return;
  }
  std::ofstream report(directory + "/surfaces.txt", std::ios::trunc);
  report << "swap " << presented_output_frames_ << " resolve_key "
         << last_resolve_source_key_.Describe() << "\n";
  REXGPU_INFO("FH1 surface dump: swap={} resolve_source={}", presented_output_frames_,
              last_resolve_source_key_.Describe());
  // The presented frame's own source first: whatever the last frame-sized
  // resolve sampled is what the front buffer received.
  uint32_t index = 0;
  for (const auto& entry : native_surfaces_->surfaces()) {
    D3D11Fh1NativeSurface* surface = entry.second.get();
    if (!surface || !surface->texture) {
      continue;
    }
    D3D11_TEXTURE2D_DESC desc;
    surface->texture->GetDesc(&desc);
    SurfaceDumpStats stats;
    std::string error;
    const std::string path =
        directory + "/surface-" + std::to_string(index) + "-" + SanitizedKeyName(surface->key) +
        ".ppm";
    const bool drawn = ever_bound_render_target_keys_.count(entry.first) != 0;
    if (surface->key.is_depth) {
      // The depth leg: does this d0 surface hold a scene at all? The resolve
      // that copies it into guest memory can only be as full as the surface is.
      DepthDumpStats depth_stats;
      const bool written =
          DumpDepthSurfacePpm(device, context, surface, path, depth_stats, error);
      const double total = depth_stats.total ? double(depth_stats.total) : 1.0;
      REXGPU_INFO(
          "FH1 surface dump (depth): {} {}x{} fmt={} samples={} drawn={} words={} "
          "zero={:.4f} filled={:.4f} depth_lo mean={:.4f} min={:.4f} max={:.4f} "
          "depth_hi mean={:.4f} first={:08X} {}{}",
          surface->key.Describe(), surface->width, surface->height, uint32_t(desc.Format),
          surface->samples, drawn ? 1 : 0, depth_stats.total,
          double(depth_stats.zero_words) / total, double(depth_stats.filled_words) / total,
          depth_stats.mean_lo, depth_stats.min_lo, depth_stats.max_lo, depth_stats.mean_hi,
          depth_stats.first, written ? "file=" : "error=", written ? path : error);
      report << surface->key.Describe() << " " << surface->width << "x" << surface->height
             << "x" << surface->samples << " depth drawn=" << (drawn ? 1 : 0)
             << " zero=" << (double(depth_stats.zero_words) / total)
             << " filled=" << (double(depth_stats.filled_words) / total)
             << " lo_mean=" << depth_stats.mean_lo << " lo_min=" << depth_stats.min_lo
             << " lo_max=" << depth_stats.max_lo << " hi_mean=" << depth_stats.mean_hi
             << " first=" << fmt::format("{:08X}", depth_stats.first)
             << " file=" << (written ? path : error) << "\n";
      ++index;
      continue;
    }
    const bool written = DumpSurfacePpm(device, context, surface, path, stats, error);
    REXGPU_INFO(
        "FH1 surface dump: {} {}x{}x{} samples={} drawn={} nonzero={} colours={} mean={:.3f},"
        "{:.3f},{:.3f} {}{}",
        surface->key.Describe(), surface->width, surface->height, uint32_t(desc.Format),
        surface->samples, drawn ? 1 : 0,
        stats.total ? double(stats.nonzero) / double(stats.total) : 0.0, stats.distinct,
        stats.mean[0], stats.mean[1], stats.mean[2], written ? "file=" : "error=",
        written ? path : error);
    report << surface->key.Describe() << " " << surface->width << "x" << surface->height
           << "x" << surface->samples << " drawn=" << (drawn ? 1 : 0)
           << " nonzero="
           << (stats.total ? double(stats.nonzero) / double(stats.total) : 0.0)
           << " colours=" << stats.distinct << " file=" << (written ? path : error) << "\n";
    ++index;
  }
  // fh1_debug_dump_guest: what a resolved guest range actually holds where the
  // guest samples it (guest memory, which the texture cache untiles from) and
  // what the GPU mirror holds for the same range, word for word - separates a
  // resolve that never produced the image from a texture load that never got
  // it.
  const std::string guest_dump = REXCVAR_GET(fh1_debug_dump_guest);
  if (!guest_dump.empty()) {
    auto* upload_sink =
        static_cast<D3D11SharedMemoryDeviceUploadSink*>(shared_memory_->upload_sink());
    size_t cursor = 0;
    while (cursor < guest_dump.size()) {
      size_t separator = guest_dump.find(',', cursor);
      if (separator == std::string::npos) {
        separator = guest_dump.size();
      }
      const std::string item = guest_dump.substr(cursor, separator - cursor);
      cursor = separator + 1;
      const size_t plus = item.find('+');
      if (plus == std::string::npos) {
        continue;
      }
      const uint32_t start = uint32_t(std::strtoul(item.substr(0, plus).c_str(), nullptr, 16));
      // Decimal by default so a length reads as the trace prints it; 0x... is hex.
      uint32_t length = uint32_t(std::strtoul(item.substr(plus + 1).c_str(), nullptr, 0));
      if (!length || start >= SharedMemory::kBufferSize) {
        continue;
      }
      length = std::min(length, uint32_t(SharedMemory::kBufferSize - start));
      char name[32];
      std::snprintf(name, sizeof(name), "%08X", start);
      std::vector<uint8_t> guest_bytes(length, 0);
      const uint8_t* guest = reinterpret_cast<const uint8_t*>(memory_->TranslatePhysical(start));
      if (guest) {
        std::memcpy(guest_bytes.data(), guest, length);
      }
      std::vector<uint8_t> mirror_bytes(length, 0);
      const bool have_mirror =
          upload_sink && upload_sink->ReadbackFromHostBuffer(start, mirror_bytes.data(), length);
      auto word_stats = [](const std::vector<uint8_t>& bytes, uint64_t& nonzero,
                           uint64_t& distinct) {
        std::set<uint32_t> seen;
        nonzero = 0;
        for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
          const uint32_t word = *reinterpret_cast<const uint32_t*>(bytes.data() + i);
          nonzero += word != 0;
          if (!(i & 63)) {
            seen.insert(word);
          }
        }
        distinct = seen.size();
      };
      uint64_t guest_nonzero = 0, guest_distinct = 0, mirror_nonzero = 0, mirror_distinct = 0;
      word_stats(guest_bytes, guest_nonzero, guest_distinct);
      if (have_mirror) {
        word_stats(mirror_bytes, mirror_nonzero, mirror_distinct);
      }
      const bool same = have_mirror && guest_bytes == mirror_bytes;
      const std::string bin_path = directory + "/guest-" + name + ".bin";
      {
        std::ofstream bin(bin_path, std::ios::binary | std::ios::trunc);
        bin.write(reinterpret_cast<const char*>(guest_bytes.data()), std::streamsize(length));
      }
      const std::string mirror_path = directory + "/mirror-" + name + ".bin";
      if (have_mirror) {
        std::ofstream bin(mirror_path, std::ios::binary | std::ios::trunc);
        bin.write(reinterpret_cast<const char*>(mirror_bytes.data()), std::streamsize(length));
      }
      // A linear 32-bit view (the layout is tiled, so this is a mosaic of the
      // tile rows, not the image - enough to tell an image from zeros), in both
      // byte orders so a swapped channel does not read as an empty buffer.
      const uint32_t width = (length % (1280u * 4u)) == 0 ? 1280u : 256u;
      const uint32_t height = width ? length / (width * 4u) : 0;
      auto write_ppm = [&](const std::vector<uint8_t>& bytes, const char* suffix,
                           bool red_is_byte0) {
        if (!height) {
          return;
        }
        std::ofstream ppm(directory + "/guest-" + name + suffix, std::ios::binary);
        ppm << "P6\n" << width << " " << height << "\n255\n";
        std::vector<char> row(size_t(width) * 3u);
        for (uint32_t y = 0; y < height; ++y) {
          const uint8_t* texel = bytes.data() + size_t(y) * width * 4u;
          for (uint32_t x = 0; x < width; ++x, texel += 4) {
            row[x * 3 + 0] = char(red_is_byte0 ? texel[0] : texel[2]);
            row[x * 3 + 1] = char(texel[1]);
            row[x * 3 + 2] = char(red_is_byte0 ? texel[2] : texel[0]);
          }
          ppm.write(row.data(), std::streamsize(row.size()));
        }
      };
      write_ppm(guest_bytes, "-rgb.ppm", true);
      write_ppm(guest_bytes, "-bgr.ppm", false);
      REXGPU_INFO(
          "FH1 guest dump: range={:08X}+{} guest_words_nz={} guest_distinct={} mirror_words_nz={} "
          "mirror_distinct={} identical={} bin={}",
          start, length, guest_nonzero, guest_distinct, mirror_nonzero, mirror_distinct,
          same ? 1 : 0, bin_path);
    }
  }
  if (native_executor_) {
    // Which surface owns the EDRAM at the moment the front buffer is resolved:
    // the ownership edge a dropped scene would show up in.
    std::unordered_map<uint32_t, uint32_t> owners;
    for (uint32_t tile = 0; tile < xenos::kEdramTileCount; ++tile) {
      ++owners[native_executor_->tiles().Owner(tile)];
    }
    std::vector<std::pair<uint32_t, uint32_t>> sorted(owners.begin(), owners.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [owner, count] : sorted) {
      char name[64];
      if (owner == Fh1EdramTiles::kNoOwner) {
        std::snprintf(name, sizeof(name), "none");
      } else {
        Fh1SurfaceKey unpacked;
        unpacked.base_tiles = owner & 0x7FFu;
        unpacked.pitch_tiles = (owner >> 11) & 0xFFu;
        unpacked.msaa = (owner >> 19) & 0x3u;
        unpacked.is_depth = ((owner >> 21) & 1u) != 0;
        unpacked.format = (owner >> 22) & 0x3FFu;
        std::snprintf(name, sizeof(name), "%s", unpacked.Describe().c_str());
      }
      REXGPU_INFO("FH1 surface dump: owner {} tiles={}", name, count);
      report << "owner " << name << " tiles=" << count << "\n";
    }
  }
  report.flush();
  REXGPU_INFO("FH1 surface dump: {} surfaces and the ownership histogram written to {}",
              index, directory);
}

}  // namespace rex::graphics::d3d11
