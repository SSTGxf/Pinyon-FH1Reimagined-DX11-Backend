/**
 * @file        graphics/d3d11_native_surfaces_test.cpp
 * @brief       Unit tests for the D3D11 native EDRAM surface layer (Phase 4)
 *
 *              Exercises the device-free seams of the FH1 native render
 *              target route: EDRAM surface geometry and owner splitting,
 *              including 64-bpp and MSAA layouts, plus host render target
 *              formats, with no graphics device involved.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <rex/cvar.h>
#include <rex/graphics/d3d11/fh1_native_surfaces.h>
#include <rex/graphics/d3d11/host_render_config.h>
#include <rex/graphics/fh1_edram_surfaces.h>
#include <rex/graphics/fh1_edram_tiles.h>
#include <rex/graphics/xenos.h>

using rex::graphics::Fh1SurfaceKey;
using rex::graphics::d3d11::D3D11HostRenderConfig;
using rex::graphics::d3d11::Fh1D3D11SampleMask;
using rex::graphics::d3d11::Fh1D3D11SurfaceHeight;
using rex::graphics::d3d11::Fh1D3D11SurfaceImageHeight;
using rex::graphics::d3d11::Fh1D3D11SurfaceSamples;
using rex::graphics::d3d11::Fh1D3D11SurfaceTilePitch;
using rex::graphics::d3d11::Fh1D3D11SurfaceTileWidth;
using rex::graphics::d3d11::Fh1D3D11SurfaceWidth;
using rex::graphics::d3d11::Fh1D3D11CopyDownRows;
using rex::graphics::d3d11::Fh1D3D11UntileBand;

namespace {

constexpr uint32_t kMsaa1X = uint32_t(rex::graphics::xenos::MsaaSamples::k1X);
constexpr uint32_t kMsaa2X = uint32_t(rex::graphics::xenos::MsaaSamples::k2X);
constexpr uint32_t kMsaa4X = uint32_t(rex::graphics::xenos::MsaaSamples::k4X);

Fh1SurfaceKey MakeColorKey(uint32_t pitch_tiles, uint32_t msaa) {
  return Fh1SurfaceKey::Color(0, pitch_tiles, msaa,
                              rex::graphics::xenos::ColorRenderTargetFormat::k_8_8_8_8, false);
}

Fh1SurfaceKey MakeColor64Key(uint32_t pitch_tiles, uint32_t msaa) {
  return Fh1SurfaceKey::Color(
      0, pitch_tiles, msaa,
      rex::graphics::xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT, false);
}

}  // namespace

TEST_CASE("D3D11 native surface width follows the EDRAM tile pitch and sample mode") {
  using rex::graphics::xenos::kEdramTileWidthSamples;
  // 1x / 2x: the pitch in 32-bpp tiles is the pixel width over the tile width.
  CHECK(Fh1D3D11SurfaceWidth(MakeColorKey(16, kMsaa1X)) == 16 * kEdramTileWidthSamples);
  CHECK(Fh1D3D11SurfaceWidth(MakeColorKey(16, kMsaa2X)) == 16 * kEdramTileWidthSamples);
  // 4x doubles the sample count in x within the same tiles.
  CHECK(Fh1D3D11SurfaceWidth(MakeColorKey(16, kMsaa4X)) == 16 * (kEdramTileWidthSamples >> 1));
  CHECK(Fh1D3D11SurfaceWidth(MakeColorKey(0, kMsaa1X)) == 0);
}

TEST_CASE("D3D11 native surface height covers the pitch's EDRAM rows, clamped") {
  using rex::graphics::xenos::kEdramTileCount;
  using rex::graphics::xenos::kEdramTileHeightSamples;
  // 2048 tiles over a 16-tile pitch: 128 tile rows.
  CHECK(Fh1D3D11SurfaceHeight(MakeColorKey(16, kMsaa1X), 1) == 128 * kEdramTileHeightSamples);
  // 4x MSAA has half-height tiles.
  CHECK(Fh1D3D11SurfaceHeight(MakeColorKey(16, kMsaa4X), 1) ==
        128 * (kEdramTileHeightSamples >> 1));
  // A one-tile pitch would wrap the EDRAM many times - clamped to the guest
  // texture size limit (the tighter of it and the host texture dimension),
  // like the Xenos render target cache.
  const uint32_t clamped = Fh1D3D11SurfaceHeight(MakeColorKey(1, kMsaa1X), 1);
  CHECK(clamped <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
  CHECK(clamped % kEdramTileHeightSamples == 0);
  CHECK(clamped == (rex::graphics::xenos::kTexture2DCubeMaxWidthHeight /
                    kEdramTileHeightSamples) *
                       kEdramTileHeightSamples);
  // No pitch - no surface.
  CHECK(Fh1D3D11SurfaceHeight(MakeColorKey(0, kMsaa1X), 1) == 0);
  (void)kEdramTileCount;
}

TEST_CASE("D3D11 untile storage expands without changing EDRAM period geometry") {
  const Fh1SurfaceKey scene_32 = MakeColorKey(32, kMsaa4X);
  const Fh1SurfaceKey scene_64 = MakeColor64Key(16, kMsaa4X);
  CHECK(Fh1D3D11SurfaceHeight(scene_32, 1) == 512);
  CHECK(Fh1D3D11SurfaceHeight(scene_64, 1) == 512);
  CHECK(Fh1D3D11SurfaceImageHeight(scene_32, 1, 720) == 720);
  CHECK(Fh1D3D11SurfaceImageHeight(scene_64, 1, 720) == 720);
  CHECK(Fh1D3D11SurfaceImageHeight(MakeColorKey(16, kMsaa4X), 1, 720) == 1024);
  CHECK(Fh1D3D11SurfaceImageHeight(scene_32, 2, 720) == 720);
  CHECK(Fh1D3D11SurfaceImageHeight(scene_32, 0, 720) == 0);
}

TEST_CASE("D3D11 untile bands map to their screen rows and copy down the last band") {
  CHECK(Fh1D3D11UntileBand(0) == 0);
  CHECK(Fh1D3D11UntileBand(-256) == 256);
  CHECK(Fh1D3D11UntileBand(-512) == 512);
  CHECK(Fh1D3D11UntileBand(256) == 0);
  CHECK(Fh1D3D11CopyDownRows(720, 0) == 0);
  CHECK(Fh1D3D11CopyDownRows(720, 256) == 256);
  CHECK(Fh1D3D11CopyDownRows(720, 512) == 208);
  CHECK(Fh1D3D11CopyDownRows(720, 720) == 0);
}

TEST_CASE("D3D11 native surface sample counts emulate 2x with 4x when needed") {
  CHECK(Fh1D3D11SurfaceSamples(kMsaa1X, true) == 1);
  CHECK(Fh1D3D11SurfaceSamples(kMsaa1X, false) == 1);
  CHECK(Fh1D3D11SurfaceSamples(kMsaa2X, true) == 2);
  CHECK(Fh1D3D11SurfaceSamples(kMsaa2X, false) == 4);  // Top-left + bottom-right of 4x.
  CHECK(Fh1D3D11SurfaceSamples(kMsaa4X, true) == 4);
  CHECK(Fh1D3D11SurfaceSamples(kMsaa4X, false) == 4);
}

TEST_CASE("D3D11 clear sample masks preserve the guest 2x sample pattern") {
  CHECK(Fh1D3D11SampleMask(MakeColorKey(16, kMsaa1X),
                           Fh1D3D11SurfaceSamples(kMsaa1X, false)) == 0xFFFFFFFFu);
  CHECK(Fh1D3D11SampleMask(MakeColorKey(16, kMsaa2X),
                           Fh1D3D11SurfaceSamples(kMsaa2X, true)) == 0xFFFFFFFFu);
  CHECK(Fh1D3D11SampleMask(MakeColorKey(16, kMsaa2X),
                           Fh1D3D11SurfaceSamples(kMsaa2X, false)) == 0b1001u);
  CHECK(Fh1D3D11SampleMask(MakeColorKey(16, kMsaa4X),
                           Fh1D3D11SurfaceSamples(kMsaa4X, false)) == 0xFFFFFFFFu);
}

TEST_CASE("D3D11 64-bpp surfaces use half-width tiles and doubled EDRAM pitch") {
  using rex::graphics::xenos::kEdramTileHeightSamples;
  using rex::graphics::xenos::kEdramTileWidthSamples;

  const Fh1SurfaceKey color32 = MakeColorKey(16, kMsaa1X);
  const Fh1SurfaceKey color64 = MakeColor64Key(16, kMsaa1X);
  CHECK_FALSE(color32.Is64bpp());
  CHECK(color64.Is64bpp());
  CHECK(Fh1D3D11SurfaceTilePitch(color32) == 16);
  CHECK(Fh1D3D11SurfaceTilePitch(color64) == 32);
  CHECK(Fh1D3D11SurfaceTileWidth(color32) == kEdramTileWidthSamples);
  CHECK(Fh1D3D11SurfaceTileWidth(color64) == kEdramTileWidthSamples / 2);
  CHECK(Fh1D3D11SurfaceWidth(color64) == Fh1D3D11SurfaceWidth(color32));
  CHECK(Fh1D3D11SurfaceHeight(color64, 1) ==
        Fh1D3D11SurfaceHeight(color32, 1) / 2);

  const Fh1SurfaceKey color64_msaa = MakeColor64Key(16, kMsaa4X);
  CHECK(Fh1D3D11SurfaceTileWidth(color64_msaa) == (kEdramTileWidthSamples / 2) / 2);
  CHECK(Fh1D3D11SurfaceTilePitch(color64_msaa) == 32);
  CHECK(Fh1D3D11SurfaceHeight(color64_msaa, 1) ==
        (rex::graphics::xenos::kEdramTileCount / 32) *
            (kEdramTileHeightSamples / 2));
}

TEST_CASE("D3D11 resolve ownership splits mixed and single-owner tile rectangles") {
  using rex::graphics::Fh1EdramTiles;
  using rex::graphics::xenos::ColorRenderTargetFormat;
  using rex::graphics::xenos::kEdramTileHeightSamples;
  const Fh1SurfaceKey key = MakeColor64Key(2, kMsaa1X);
  const uint32_t tile_pitch = Fh1D3D11SurfaceTilePitch(key);
  const uint32_t tile_width = Fh1D3D11SurfaceTileWidth(key);
  const uint32_t owner_a =
      Fh1SurfaceKey::Color(128, 2, kMsaa1X,
                           ColorRenderTargetFormat::k_16_16_16_16_FLOAT, false)
          .Pack();
  const uint32_t owner_b = Fh1SurfaceKey::Color(
                               256, 2, kMsaa1X,
                               ColorRenderTargetFormat::k_16_16_16_16_FLOAT, false)
                               .Pack();
  Fh1EdramTiles tiles;
  tiles.Reset();
  tiles.Claim(key.base_tiles, 1, owner_a, true);
  tiles.Claim(key.base_tiles + 1, 1, owner_a, true);
  tiles.Claim(key.base_tiles + 2, 1, owner_b, true);
  tiles.Claim(key.base_tiles + 3, 1, owner_b, true);

  const auto mixed = tiles.SplitByOwner(key.base_tiles, tile_pitch, int32_t(tile_width),
                                         int32_t(kEdramTileHeightSamples), 10, 2,
                                         int32_t(tile_width * 4 - 10),
                                         int32_t(kEdramTileHeightSamples - 2));
  REQUIRE(mixed.size() == 2);
  CHECK(mixed[0].owner == owner_a);
  CHECK(mixed[0].left == 10);
  CHECK(mixed[0].right == int32_t(tile_width * 2));
  CHECK(mixed[1].owner == owner_b);
  CHECK(mixed[1].left == int32_t(tile_width * 2));
  CHECK(mixed[1].right == int32_t(tile_width * 4 - 10));

  const auto single = tiles.SplitByOwner(
      key.base_tiles, tile_pitch, int32_t(tile_width), int32_t(kEdramTileHeightSamples),
      int32_t(tile_width * 2), 0, int32_t(tile_width * 4), int32_t(kEdramTileHeightSamples));
  REQUIRE(single.size() == 1);
  CHECK(single[0].owner == owner_b);
  CHECK(single[0].left == int32_t(tile_width * 2));
  CHECK(single[0].right == int32_t(tile_width * 4));

  tiles.Reset();
  tiles.Claim(key.base_tiles, 2, owner_a, true);
  const auto partially_unowned = tiles.SplitByOwner(
      key.base_tiles, tile_pitch, int32_t(tile_width), int32_t(kEdramTileHeightSamples), 0, 0,
      int32_t(tile_width * 4), int32_t(kEdramTileHeightSamples));
  REQUIRE(partially_unowned.size() == 2);
  CHECK(partially_unowned[0].owner == owner_a);
  CHECK(partially_unowned[0].right == int32_t(tile_width * 2));
  CHECK(partially_unowned[1].owner == Fh1EdramTiles::kNoOwner);
  CHECK(partially_unowned[1].left == int32_t(tile_width * 2));
  CHECK(partially_unowned[1].right == int32_t(tile_width * 4));

  const Fh1SurfaceKey msaa_key = MakeColor64Key(2, kMsaa4X);
  const uint32_t msaa_tile_width = Fh1D3D11SurfaceTileWidth(msaa_key);
  const uint32_t msaa_owner_a =
      Fh1SurfaceKey::Color(64, 2, kMsaa4X,
                           ColorRenderTargetFormat::k_16_16_16_16_FLOAT, false)
          .Pack();
  const uint32_t msaa_owner_b =
      Fh1SurfaceKey::Color(256, 2, kMsaa4X,
                           ColorRenderTargetFormat::k_16_16_16_16_FLOAT, false)
          .Pack();
  tiles.Reset();
  tiles.Claim(msaa_key.base_tiles, 2, msaa_owner_a, true);
  tiles.Claim(msaa_key.base_tiles + 2, 2, msaa_owner_b, true);
  const auto msaa_mixed = tiles.SplitByOwner(
      msaa_key.base_tiles, Fh1D3D11SurfaceTilePitch(msaa_key), int32_t(msaa_tile_width),
      int32_t(kEdramTileHeightSamples / 2), 5, 1, int32_t(msaa_tile_width * 4 - 5),
      int32_t(kEdramTileHeightSamples / 2 - 1));
  REQUIRE(msaa_mixed.size() == 2);
  CHECK(msaa_mixed[0].owner == msaa_owner_a);
  CHECK(msaa_mixed[0].right == int32_t(msaa_tile_width * 2));
  CHECK(msaa_mixed[1].owner == msaa_owner_b);
  CHECK(msaa_mixed[1].left == int32_t(msaa_tile_width * 2));
}

TEST_CASE("D3D11 host render color formats mirror the D3D12 executor") {
  using rex::graphics::xenos::ColorRenderTargetFormat;
  // The gamma storage choice is read from the host cvars on initialization
  // (2x MSAA probing off, so no device is needed here). The cvars are
  // process-wide - save and restore them.
  const bool old_native_2x_msaa = REXCVAR_GET(native_2x_msaa);
  const bool old_gamma = REXCVAR_GET(gamma_render_target_as_unorm16);
  REXCVAR_SET(native_2x_msaa, false);
  REXCVAR_SET(gamma_render_target_as_unorm16, false);
  D3D11HostRenderConfig config(1, 1);
  config.Initialize(nullptr);

  CHECK(config.GetColorResourceDXGIFormat(ColorRenderTargetFormat::k_8_8_8_8) ==
        DXGI_FORMAT_R8G8B8A8_UNORM);
  CHECK(config.GetColorResourceDXGIFormat(ColorRenderTargetFormat::k_8_8_8_8_GAMMA) ==
        DXGI_FORMAT_R8G8B8A8_UNORM);
  CHECK(config.GetColorResourceDXGIFormat(ColorRenderTargetFormat::k_2_10_10_10) ==
        DXGI_FORMAT_R10G10B10A2_UNORM);
  CHECK(config.GetColorResourceDXGIFormat(
            ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16) ==
        DXGI_FORMAT_R16G16B16A16_FLOAT);
  CHECK(config.GetColorResourceDXGIFormat(ColorRenderTargetFormat::k_16_16) ==
        DXGI_FORMAT_R16G16_TYPELESS);
  CHECK(config.GetColorResourceDXGIFormat(ColorRenderTargetFormat::k_16_16_16_16_FLOAT) ==
        DXGI_FORMAT_R16G16B16A16_TYPELESS);
  CHECK(config.GetColorResourceDXGIFormat(ColorRenderTargetFormat::k_32_FLOAT) ==
        DXGI_FORMAT_R32_TYPELESS);
  CHECK(config.GetColorResourceDXGIFormat(ColorRenderTargetFormat::k_32_32_FLOAT) ==
        DXGI_FORMAT_R32G32_TYPELESS);

  // Draw views of the typeless storages.
  CHECK(config.GetColorDrawDXGIFormat(ColorRenderTargetFormat::k_16_16) ==
        DXGI_FORMAT_R16G16_SNORM);
  CHECK(config.GetColorDrawDXGIFormat(ColorRenderTargetFormat::k_16_16_16_16) ==
        DXGI_FORMAT_R16G16B16A16_SNORM);
  CHECK(config.GetColorDrawDXGIFormat(ColorRenderTargetFormat::k_16_16_FLOAT) ==
        DXGI_FORMAT_R16G16_FLOAT);
  CHECK(config.GetColorDrawDXGIFormat(ColorRenderTargetFormat::k_32_32_FLOAT) ==
        DXGI_FORMAT_R32G32_FLOAT);
  CHECK(config.GetColorDrawDXGIFormat(ColorRenderTargetFormat::k_8_8_8_8) ==
        DXGI_FORMAT_R8G8B8A8_UNORM);

  // Gamma as 16-bit unorm, when the host wants that storage.
  REXCVAR_SET(gamma_render_target_as_unorm16, true);
  D3D11HostRenderConfig gamma_config(1, 1);
  gamma_config.Initialize(nullptr);
  CHECK(gamma_config.GetColorResourceDXGIFormat(ColorRenderTargetFormat::k_8_8_8_8_GAMMA) ==
        DXGI_FORMAT_R16G16B16A16_UNORM);
  REXCVAR_SET(gamma_render_target_as_unorm16, old_gamma);
  REXCVAR_SET(native_2x_msaa, old_native_2x_msaa);
}

TEST_CASE("D3D11 host render raw and depth format views mirror the D3D12 executor") {
  using rex::graphics::xenos::ColorRenderTargetFormat;
  using rex::graphics::xenos::DepthRenderTargetFormat;
  // Formats whose guest EDRAM words are the host channel bits.
  CHECK(D3D11HostRenderConfig::GetColorUintDXGIFormat(ColorRenderTargetFormat::k_16_16) ==
        DXGI_FORMAT_R16G16_UINT);
  CHECK(D3D11HostRenderConfig::GetColorUintDXGIFormat(ColorRenderTargetFormat::k_16_16_FLOAT) ==
        DXGI_FORMAT_R16G16_UINT);
  CHECK(D3D11HostRenderConfig::GetColorUintDXGIFormat(
            ColorRenderTargetFormat::k_16_16_16_16_FLOAT) == DXGI_FORMAT_R16G16B16A16_UINT);
  CHECK(D3D11HostRenderConfig::GetColorUintDXGIFormat(ColorRenderTargetFormat::k_32_32_FLOAT) ==
        DXGI_FORMAT_R32G32_UINT);
  CHECK(D3D11HostRenderConfig::GetColorUintDXGIFormat(ColorRenderTargetFormat::k_32_FLOAT) ==
        DXGI_FORMAT_UNKNOWN);
  CHECK(D3D11HostRenderConfig::GetColorUintDXGIFormat(ColorRenderTargetFormat::k_8_8_8_8) ==
        DXGI_FORMAT_UNKNOWN);

  // Depth and stencil planes.
  CHECK(D3D11HostRenderConfig::GetDepthResourceDXGIFormat(DepthRenderTargetFormat::kD24S8) ==
        DXGI_FORMAT_R24G8_TYPELESS);
  CHECK(D3D11HostRenderConfig::GetDepthDSVDXGIFormat(DepthRenderTargetFormat::kD24S8) ==
        DXGI_FORMAT_D24_UNORM_S8_UINT);
  CHECK(D3D11HostRenderConfig::GetDepthSRVDepthDXGIFormat(DepthRenderTargetFormat::kD24S8) ==
        DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
  CHECK(D3D11HostRenderConfig::GetDepthSRVStencilDXGIFormat(DepthRenderTargetFormat::kD24S8) ==
        DXGI_FORMAT_X24_TYPELESS_G8_UINT);
  CHECK(D3D11HostRenderConfig::GetDepthResourceDXGIFormat(DepthRenderTargetFormat::kD24FS8) ==
        DXGI_FORMAT_R32G8X24_TYPELESS);
  CHECK(D3D11HostRenderConfig::GetDepthDSVDXGIFormat(DepthRenderTargetFormat::kD24FS8) ==
        DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
  CHECK(D3D11HostRenderConfig::GetDepthSRVDepthDXGIFormat(DepthRenderTargetFormat::kD24FS8) ==
        DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
  CHECK(D3D11HostRenderConfig::GetDepthSRVStencilDXGIFormat(DepthRenderTargetFormat::kD24FS8) ==
        DXGI_FORMAT_X32_TYPELESS_G8X24_UINT);
}

TEST_CASE("D3D11 native surface keys separate color, depth and storage formats") {
  using rex::graphics::xenos::ColorRenderTargetFormat;
  using rex::graphics::xenos::DepthRenderTargetFormat;
  const Fh1SurfaceKey color =
      Fh1SurfaceKey::Color(128, 16, kMsaa1X, ColorRenderTargetFormat::k_8_8_8_8, false);
  const Fh1SurfaceKey color_other =
      Fh1SurfaceKey::Color(128, 16, kMsaa1X, ColorRenderTargetFormat::k_16_16, false);
  const Fh1SurfaceKey depth =
      Fh1SurfaceKey::Depth(128, 16, kMsaa1X, DepthRenderTargetFormat::kD24S8);
  CHECK(color.Pack() != color_other.Pack());
  CHECK(color.Pack() != depth.Pack());
  // The same identity packs the same.
  CHECK(color.Pack() ==
        Fh1SurfaceKey::Color(128, 16, kMsaa1X, ColorRenderTargetFormat::k_8_8_8_8, false).Pack());
  CHECK(!color.Is64bpp());
  CHECK(depth.is_depth);
}
