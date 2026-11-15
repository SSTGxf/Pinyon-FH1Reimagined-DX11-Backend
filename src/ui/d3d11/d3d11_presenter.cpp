/**
 * @file        ui/d3d11/d3d11_presenter.cpp
 * @brief       Direct3D 11 presenter implementation
 *
 *              Mirrors the D3D12 presenter's paint flow on feature level
 *              11_0: flip-model swap chain, guest output mailbox images
 *              (R10G10B10A2), the shared guest-output paint flow with the
 *              existing FSR1/CAS/bilinear output scaling selection, and the
 *              UI draw pass. The bilinear and bilinear-dither passes are
 *              reconstructed SM 5.0 shaders with the original bindings and
 *              noise table; CAS and FSR1 pass blobs are not yet available in
 *              SM 5.0 form and fall back to the bilinear pipeline with a
 *              one-time log until their sources are compiled in.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/d3d11/d3d11_presenter.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <rex/assert.h>
#include <rex/cvar.h>

#include <vector>

REXCVAR_DEFINE_BOOL(pinyon_dump_frames, false, "GPU",
                     "Write the composed back buffer to .local/frame.bmp every 300 paints");
REXCVAR_DEFINE_BOOL(pinyon_present_swap_rb, false, "GPU",
                     "Swap red and blue when copying the guest output to the present "
                     "texture (diagnostic for titles whose 3D arrives channel-swapped)");
#include <rex/logging.h>
#include <rex/ui/surface_win.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/render_audit.h>

namespace shaders {
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_bilinear_dither_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_bilinear_ps.h"
#include "ui/shaders/bytecode/d3d11_5_0/guest_output_triangle_strip_rect_vs.h"
}  // namespace shaders

namespace rex::ui::d3d11 {

namespace {

constexpr float kBackBufferClearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};

struct alignas(16) RectConstants {
  float offset[2];
  float size[2];
};

}  // namespace

D3D11Presenter::~D3D11Presenter() {
  DisconnectPaintingFromSurfaceFromUIThreadImpl();
}

Surface::TypeFlags D3D11Presenter::GetSupportedSurfaceTypes() const {
  Surface::TypeFlags types = 0;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
  types |= Surface::kTypeFlag_Win32Hwnd;
#endif
  return types;
}

bool D3D11Presenter::InitializeSurfaceIndependent() {
  if (!InitializeCommonSurfaceIndependent()) {
    return false;
  }
  ID3D11Device* device = provider_.GetDevice();

  if (FAILED(device->CreateVertexShader(shaders::guest_output_triangle_strip_rect_vs,
                                        sizeof(shaders::guest_output_triangle_strip_rect_vs),
                                        nullptr, &rect_vs_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the rect vertex shader");
    return false;
  }
  if (FAILED(device->CreatePixelShader(shaders::guest_output_bilinear_ps,
                                       sizeof(shaders::guest_output_bilinear_ps), nullptr,
                                       &bilinear_ps_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the bilinear pixel shader");
    return false;
  }
  if (FAILED(device->CreatePixelShader(shaders::guest_output_bilinear_dither_ps,
                                       sizeof(shaders::guest_output_bilinear_dither_ps), nullptr,
                                       &bilinear_dither_ps_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the bilinear dither pixel shader");
    return false;
  }

  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.ByteWidth = sizeof(RectConstants);
  constants_desc.Usage = D3D11_USAGE_DYNAMIC;
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&constants_desc, nullptr, &vs_constants_)) ||
      FAILED(device->CreateBuffer(&constants_desc, nullptr, &ps_constants_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the constant buffers");
    return false;
  }

  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sampler_desc, &guest_output_sampler_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output sampler");
    return false;
  }

  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  rasterizer_desc.ScissorEnable = TRUE;
  if (FAILED(device->CreateRasterizerState(&rasterizer_desc, &rasterizer_state_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the rasterizer state");
    return false;
  }

  D3D11_BLEND_DESC blend_desc = {};
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device->CreateBlendState(&blend_desc, &blend_state_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the blend state");
    return false;
  }

  D3D11_DEPTH_STENCIL_DESC depth_stencil_desc = {};
  depth_stencil_desc.DepthEnable = FALSE;
  depth_stencil_desc.StencilEnable = FALSE;
  if (FAILED(device->CreateDepthStencilState(&depth_stencil_desc, &depth_stencil_state_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the depth stencil state");
    return false;
  }

  return true;
}

bool D3D11Presenter::CreateEffectPipelines() {
  // All effect pipelines share the VS and the constant layouts; the CAS and
  // FSR1 pixel shaders are compiled from their single sources in a later
  // phase - BindEffectPipeline logs and falls back to bilinear until then.
  return true;
}

bool D3D11Presenter::BindEffectPipeline(GuestOutputPaintEffect effect,
                                        ID3D11DeviceContext* device_context,
                                        ID3D11PixelShader* bilinear_ps,
                                        ID3D11PixelShader* bilinear_dither_ps) {
  bool dither = false;
  ID3D11PixelShader* pixel_shader = bilinear_ps;
  switch (effect) {
    case GuestOutputPaintEffect::kBilinear:
      break;
    case GuestOutputPaintEffect::kBilinearDither:
      pixel_shader = bilinear_dither_ps;
      dither = true;
      break;
#if defined(REX_HAS_FIDELITYFX_SDK)
    case GuestOutputPaintEffect::kCasSharpen:
    case GuestOutputPaintEffect::kCasSharpenDither:
    case GuestOutputPaintEffect::kCasResample:
    case GuestOutputPaintEffect::kCasResampleDither:
    case GuestOutputPaintEffect::kFsrEasu:
    case GuestOutputPaintEffect::kFsrRcas:
    case GuestOutputPaintEffect::kFsrRcasDither:
      if (!effect_pipeline_warnings_logged_) {
        effect_pipeline_warnings_logged_ = true;
        REXLOG_WARN(
            "D3D11Presenter: CAS/FSR1 SM 5.0 pixel shaders are not compiled in yet - "
            "falling back to the bilinear output scaling pipeline");
      }
      break;
#endif
    default:
      assert_unhandled_case(effect);
      return false;
  }
  (void)dither;
  device_context->PSSetShader(pixel_shader, nullptr, 0);
  return true;
}

namespace {

// Temporary probe: averages a sparse grid of pixels per channel so a channel
// order or gain difference between the 8-bit frontbuffer and the 10-bit guest
// output shows up as numbers.
// Diagnostics: the composed frame, exactly as the display sees it, as an
// uncompressed 32-bit BMP. The presenter's swap chain cannot be captured from
// outside (flip model), so the frame is written from the back buffer itself.
struct FrameDumpReport {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t sample_count = 0;
  uint32_t row_pitch = 0;
  uint64_t bytes_expected = 0;
  uint64_t bytes_written = 0;
  int32_t create_hresult = 0;
  int32_t copy_query_status = 2;
  int32_t map_hresult = 0;
  const char* failure = "none";
  bool ok = false;
};

// The composed frame, exactly as the display sees it, as an uncompressed 32-bit
// BMP. The swap chain cannot be captured from outside, so the image is taken
// from its back buffer: copy into a staging texture, wait for the copy to
// complete on the GPU, map, then write. Every stage records its own result, and
// ok is true only when the whole image payload reached the file.
bool DumpTextureBmp(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                    const char* path, FrameDumpReport* report) {
  FrameDumpReport local;
  FrameDumpReport& r = report ? *report : local;
  if (!device || !context || !texture) {
    r.failure = "null_resource";
    return false;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  r.width = desc.Width;
  r.height = desc.Height;
  r.format = uint32_t(desc.Format);
  r.sample_count = desc.SampleDesc.Count;

  D3D11_TEXTURE2D_DESC staging_desc = desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;
  staging_desc.MipLevels = 1;
  staging_desc.ArraySize = 1;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  const HRESULT create_result = device->CreateTexture2D(&staging_desc, nullptr, &staging);
  r.create_hresult = int32_t(create_result);
  if (FAILED(create_result)) {
    r.failure = "create_staging_failed";
    return false;
  }

  context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, nullptr);

  D3D11_QUERY_DESC query_desc = {};
  query_desc.Query = D3D11_QUERY_EVENT;
  Microsoft::WRL::ComPtr<ID3D11Query> query;
  if (SUCCEEDED(device->CreateQuery(&query_desc, &query))) {
    context->End(query.Get());
    context->Flush();
    for (int spin = 0; spin < 250; ++spin) {
      const HRESULT poll = context->GetData(query.Get(), nullptr, 0, 0);
      if (poll == S_OK) {
        r.copy_query_status = 0;
        break;
      }
      if (poll != S_FALSE) {
        r.copy_query_status = 3;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  } else {
    r.copy_query_status = 4;
  }
  if (r.copy_query_status != 0) {
    r.failure = "copy_not_complete";
    return false;
  }

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const HRESULT map_result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  r.map_hresult = int32_t(map_result);
  if (FAILED(map_result)) {
    r.failure = "map_failed";
    return false;
  }
  r.row_pitch = mapped.RowPitch;

  const uint32_t row_bytes = desc.Width * 4u;
  r.bytes_expected = 54ull + uint64_t(row_bytes) * uint64_t(desc.Height);
  std::FILE* file = std::fopen(path, "wb");
  if (!file) {
    context->Unmap(staging.Get(), 0);
    r.failure = "open_failed";
    return false;
  }
  uint8_t header[54] = {};
  header[0] = 'B';
  header[1] = 'M';
  const uint32_t image_size = row_bytes * desc.Height;
  const uint32_t file_size = 54 + image_size;
  std::memcpy(header + 2, &file_size, 4);
  const uint32_t offset = 54;
  std::memcpy(header + 10, &offset, 4);
  const uint32_t info_size = 40;
  std::memcpy(header + 14, &info_size, 4);
  std::memcpy(header + 18, &desc.Width, 4);
  const int32_t signed_height = int32_t(desc.Height);
  std::memcpy(header + 22, &signed_height, 4);
  const uint16_t planes = 1, bpp = 32;
  std::memcpy(header + 26, &planes, 2);
  std::memcpy(header + 28, &bpp, 2);
  std::memcpy(header + 34, &image_size, 4);

  uint64_t written = std::fwrite(header, 1, sizeof(header), file);
  for (uint32_t y = 0; y < desc.Height; ++y) {
    // Bottom-up, as BMP expects; the staging pitch may exceed the row width.
    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) +
                         size_t(mapped.RowPitch) * size_t(desc.Height - 1 - y);
    written += std::fwrite(row, 1, size_t(std::min(row_bytes, uint32_t(mapped.RowPitch))), file);
  }
  std::fclose(file);
  context->Unmap(staging.Get(), 0);

  r.bytes_written = written;
  r.ok = written == r.bytes_expected;
  r.failure = r.ok ? "none" : "short_write";
  return r.ok;
}

struct GuestOutputColourMean {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  bool valid = false;
  // Per-channel spread: a means-only sample cannot tell a sky-only frame
  // from a frame with world geometry in it.
  double vmin[3] = {1.0, 1.0, 1.0};
  double vmax[3] = {0.0, 0.0, 0.0};
};

GuestOutputColourMean SampleGuestOutputColour(ID3D11Device* device, ID3D11DeviceContext* context,
                                             ID3D11Texture2D* texture,
                                             Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging,
                                             double y_from = 0.25, double y_to = 0.75) {
  GuestOutputColourMean mean;
  if (!device || !context || !texture) {
    return mean;
  }
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  if (!desc.Width || !desc.Height) {
    return mean;
  }
  if (!staging) {
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
      return mean;
    }
  }
  context->CopyResource(staging.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return mean;
  }
  const bool ten_bit = desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                       desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
  const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
  double sum[3] = {0.0, 0.0, 0.0};
  double vmin[3] = {1.0, 1.0, 1.0};
  double vmax[3] = {0.0, 0.0, 0.0};
  uint32_t sample_count = 0;
  const uint32_t y_begin = uint32_t(double(desc.Height) * y_from);
  const uint32_t y_end = uint32_t(double(desc.Height) * y_to);
  for (uint32_t y = y_begin; y < y_end; y += 16) {
    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * y;
    for (uint32_t x = 0; x < desc.Width; x += 32) {
      double c[3] = {0.0, 0.0, 0.0};
      if (ten_bit) {
        const uint32_t pixel = *reinterpret_cast<const uint32_t*>(row + size_t(x) * 4);
        c[0] = double(pixel & 0x3FFu) / 1023.0;
        c[1] = double((pixel >> 10) & 0x3FFu) / 1023.0;
        c[2] = double((pixel >> 20) & 0x3FFu) / 1023.0;
      } else {
        const uint8_t* pixel = row + size_t(x) * 4;
        const double c0 = double(pixel[0]) / 255.0;
        const double c1 = double(pixel[1]) / 255.0;
        const double c2 = double(pixel[2]) / 255.0;
        // Report in display order regardless of the surface's channel layout.
        c[0] = bgra ? c2 : c0;
        c[1] = c1;
        c[2] = bgra ? c0 : c2;
      }
      for (int i = 0; i < 3; ++i) {
        sum[i] += c[i];
        if (c[i] < vmin[i]) vmin[i] = c[i];
        if (c[i] > vmax[i]) vmax[i] = c[i];
      }
      ++sample_count;
    }
  }
  context->Unmap(staging.Get(), 0);
  if (!sample_count) {
    return mean;
  }
  mean.r = sum[0] / sample_count;
  mean.g = sum[1] / sample_count;
  mean.b = sum[2] / sample_count;
  for (int i = 0; i < 3; ++i) {
    mean.vmin[i] = vmin[i];
    mean.vmax[i] = vmax[i];
  }
  mean.valid = true;
  return mean;
}

}  // namespace

bool D3D11Presenter::EnsureGuestOutputImage(uint32_t mailbox_index, uint32_t width,
                                            uint32_t height) {
  GuestOutputImage& image = guest_output_images_[mailbox_index];
  if (image.texture) {
    D3D11_TEXTURE2D_DESC texture_desc;
    image.texture->GetDesc(&texture_desc);
    if (texture_desc.Width == width && texture_desc.Height == height) {
      return true;
    }
    image.texture.Reset();
    image.srv.Reset();
    image.rtv.Reset();
    image.present_texture.Reset();
    image.present_srv.Reset();
    image.render_mutex.Reset();
    image.present_mutex.Reset();
    image.readback_staging.Reset();
    image.readback_width = 0;
    image.readback_height = 0;
    if (image.shared_handle) CloseHandle(image.shared_handle);
    image.shared_handle = nullptr;
  }
  ID3D11Device* device = provider_.GetDevice();
  D3D11_TEXTURE2D_DESC texture_desc = {};
  texture_desc.Width = width;
  texture_desc.Height = height;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = kGuestOutputFormat;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_DEFAULT;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  // Shared (with a keyed mutex) so a second device - the one that owns the
  // display - can open this texture and paint the frame. The render-device
  // use is unchanged; the share only matters when the swap chain lives on
  // another adapter. NOTE: only SHARED_KEYEDMUTEX - it already implies
  // sharing, and combining it with SHARED makes CreateTexture2D fail with
  // E_INVALIDARG (0x80070057), so no mailbox texture exists at all.
  // Not shared: a surface cannot be opened by a device on another adapter,
  // so the frame reaches the display device through system memory instead
  // (see CopyGuestOutputToPresent).
  texture_desc.MiscFlags = 0;
  const HRESULT texture_hr = device->CreateTexture2D(&texture_desc, nullptr, &image.texture);
  if (FAILED(texture_hr)) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output {}x{} texture hr={:08X}",
                 width, height, static_cast<uint32_t>(texture_hr));
    return false;
  }
  if (FAILED(device->CreateShaderResourceView(image.texture.Get(), nullptr, &image.srv)) ||
      FAILED(device->CreateRenderTargetView(image.texture.Get(), nullptr, &image.rtv))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output {}x{} views", width, height);
    image.texture.Reset();
    image.srv.Reset();
    image.rtv.Reset();
    return false;
  }
  return true;
}

bool D3D11Presenter::CopyGuestOutputToPresent(uint32_t mailbox_index) {
  if (!PresentDeviceIsSeparate() || !present_device_) {
    return false;
  }
  GuestOutputImage& image = guest_output_images_[mailbox_index];
  if (!image.texture) {
    return false;
  }
  D3D11_TEXTURE2D_DESC desc;
  image.texture->GetDesc(&desc);
  if (!desc.Width || !desc.Height) {
    return false;
  }
  auto fail_once = [](const char* reason) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_ERROR("D3D11Presenter: present copy failed: {}", reason);
    }
    return false;
  };
  if (!image.readback_staging || image.readback_width != desc.Width ||
      image.readback_height != desc.Height) {
    image.readback_staging.Reset();
    image.present_texture.Reset();
    image.present_srv.Reset();
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    if (FAILED(provider_.GetDevice()->CreateTexture2D(&staging, nullptr,
                                                      &image.readback_staging))) {
      return fail_once("staging texture");
    }
    D3D11_TEXTURE2D_DESC present_desc = desc;
    present_desc.Usage = D3D11_USAGE_DEFAULT;
    present_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    present_desc.CPUAccessFlags = 0;
    present_desc.MiscFlags = 0;
    if (FAILED(present_device_->CreateTexture2D(&present_desc, nullptr,
                                                &image.present_texture))) {
      return fail_once("present texture");
    }
    if (FAILED(present_device_->CreateShaderResourceView(image.present_texture.Get(), nullptr,
                                                         &image.present_srv))) {
      image.present_texture.Reset();
      return fail_once("present view");
    }
    image.readback_width = desc.Width;
    image.readback_height = desc.Height;
  }
  // The frame costs one round trip through system memory (about 3.7 MB at
  // 1280x720). Mapping the staging texture waits for the render device's blit
  // to finish, which is why this replaces - not joins - the shared-surface
  // path: that one cannot work at all between two adapters.
  ID3D11DeviceContext* render_context = provider_.GetImmediateContext();
  render_context->CopyResource(image.readback_staging.Get(), image.texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(render_context->Map(image.readback_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return fail_once("map staging");
  }
  {
    // One-shot proof that the pulled frame is not blank: sum a few pixels
    // of the middle row (zero means the mailbox itself is black).
    static bool logged_sample = false;
    if (!logged_sample && mapped.pData) {
      logged_sample = true;
      const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) +
                           size_t(mapped.RowPitch) * (desc.Height / 2);
      const uint32_t* pixels = reinterpret_cast<const uint32_t*>(row);
      uint32_t pixel_sum = 0;
      for (uint32_t i = 0; i < 16; ++i) pixel_sum += pixels[i];
      REXLOG_INFO("D3D11Presenter: present copy {}x{} row_pitch={} pixel_sum=0x{:08X}",
                  desc.Width, desc.Height, mapped.RowPitch, pixel_sum);
    }
  }
  const void* upload_data = mapped.pData;
  std::vector<uint8_t> swapped;
  const bool ten_bit = desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                       desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
  if (ten_bit && REXCVAR_GET(pinyon_present_swap_rb)) {
    // Diagnostic: the guest output reaches this point with red and blue
    // exchanged on some titles; swapping here proves it without touching
    // the renderer or the texture cache.
    swapped.resize(size_t(mapped.RowPitch) * desc.Height);
    for (uint32_t y = 0; y < desc.Height; ++y) {
      const uint32_t* src = reinterpret_cast<const uint32_t*>(
          static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * y);
      uint32_t* dst = reinterpret_cast<uint32_t*>(swapped.data() +
                                                   size_t(mapped.RowPitch) * y);
      for (uint32_t x = 0; x < desc.Width; ++x) {
        const uint32_t pixel = src[x];
        const uint32_t r = pixel & 0x3FFu;
        const uint32_t g = (pixel >> 10) & 0x3FFu;
        const uint32_t b = (pixel >> 20) & 0x3FFu;
        const uint32_t a = (pixel >> 30) & 0x3u;
        dst[x] = (a << 30) | (r << 20) | (g << 10) | b;
      }
    }
    upload_data = swapped.data();
  }
  present_context_->UpdateSubresource(image.present_texture.Get(), 0, nullptr, upload_data,
                                      mapped.RowPitch, mapped.DepthPitch);
  render_context->Unmap(image.readback_staging.Get(), 0);
  return true;
}

bool D3D11Presenter::EnsurePresentPaintObjects() {
  if (present_paint_.created) {
    return true;
  }
  ID3D11Device* device = present_device_;
  if (!device) {
    return false;
  }
  if (FAILED(device->CreateVertexShader(shaders::guest_output_triangle_strip_rect_vs,
                                        sizeof(shaders::guest_output_triangle_strip_rect_vs),
                                        nullptr, &present_paint_.rect_vs))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the present rect vertex shader");
    return false;
  }
  if (FAILED(device->CreatePixelShader(shaders::guest_output_bilinear_ps,
                                       sizeof(shaders::guest_output_bilinear_ps), nullptr,
                                       &present_paint_.bilinear_ps))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the present bilinear pixel shader");
    return false;
  }
  if (FAILED(device->CreatePixelShader(shaders::guest_output_bilinear_dither_ps,
                                       sizeof(shaders::guest_output_bilinear_dither_ps),
                                       nullptr, &present_paint_.bilinear_dither_ps))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the present bilinear dither pixel shader");
    return false;
  }
  D3D11_BUFFER_DESC constants_desc = {};
  constants_desc.ByteWidth = sizeof(RectConstants);
  constants_desc.Usage = D3D11_USAGE_DYNAMIC;
  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&constants_desc, nullptr, &present_paint_.vs_constants)) ||
      FAILED(device->CreateBuffer(&constants_desc, nullptr, &present_paint_.ps_constants))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the present constant buffers");
    return false;
  }
  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sampler_desc, &present_paint_.sampler))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the present guest output sampler");
    return false;
  }
  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  rasterizer_desc.ScissorEnable = TRUE;
  if (FAILED(device->CreateRasterizerState(&rasterizer_desc,
                                           &present_paint_.rasterizer_state))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the present rasterizer state");
    return false;
  }
  D3D11_BLEND_DESC blend_desc = {};
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device->CreateBlendState(&blend_desc, &present_paint_.blend_state))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the present blend state");
    return false;
  }
  D3D11_DEPTH_STENCIL_DESC depth_stencil_desc = {};
  depth_stencil_desc.DepthEnable = FALSE;
  depth_stencil_desc.StencilEnable = FALSE;
  if (FAILED(device->CreateDepthStencilState(&depth_stencil_desc,
                                             &present_paint_.depth_stencil_state))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the present depth stencil state");
    return false;
  }
  present_paint_.created = true;
  return true;
}

bool D3D11Presenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  assert_not_zero(frontbuffer_width);
  assert_not_zero(frontbuffer_height);
  if (!EnsureGuestOutputImage(mailbox_index, frontbuffer_width, frontbuffer_height)) {
    return false;
  }
  GuestOutputImage& image = guest_output_images_[mailbox_index];
  D3D11GuestOutputRefreshContext context(is_8bpc_out_ref, image.texture.Get(),
                                         image.rtv.Get());
  return refresher(context);
}

bool D3D11Presenter::PresentGuestOutputTexture(GuestOutputRefreshContext& context,
                                              ID3D11ShaderResourceView* source,
                                              uint32_t source_width, uint32_t source_height) {
  (void)source_width;
  (void)source_height;
  auto fail = [](const char* reason) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("D3D11Presenter: guest output blit failed: {}", reason);
    }
    return false;
  };
  if (!source) {
    return fail("no source");
  }
  auto& d3d11_context = static_cast<D3D11GuestOutputRefreshContext&>(context);
  ID3D11RenderTargetView* render_target_view = d3d11_context.render_target_view();
  ID3D11Texture2D* resource = d3d11_context.resource();
  if (!render_target_view || !resource) {
    return fail("no output image");
  }
  D3D11_TEXTURE2D_DESC texture_desc;
  resource->GetDesc(&texture_desc);
  if (!texture_desc.Width || !texture_desc.Height) {
    return fail("empty output image");
  }
  ID3D11DeviceContext* device_context = provider_.GetImmediateContext();

  // The rect shader takes NDC offset and size, the bilinear shader maps
  // (pixel - offset) / size to the source texture, so a full-image rect with
  // the destination size as the reciprocal stretches the frontbuffer over
  // the whole guest output image.
  // The same fixed-function state the paint path uses - in particular the
  // rasterizer state, which disables culling (the default state's back-face
  // culling drops the rect entirely).
  device_context->OMSetRenderTargets(1, &render_target_view, nullptr);
  device_context->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFF);
  device_context->OMSetDepthStencilState(depth_stencil_state_.Get(), 0);
  device_context->RSSetState(rasterizer_state_.Get());
  D3D11_VIEWPORT viewport = {};
  viewport.Width = float(texture_desc.Width);
  viewport.Height = float(texture_desc.Height);
  viewport.MaxDepth = 1.0f;
  device_context->RSSetViewports(1, &viewport);
  D3D11_RECT scissor = {0, 0, LONG(texture_desc.Width), LONG(texture_desc.Height)};
  device_context->RSSetScissorRects(1, &scissor);
  device_context->IASetInputLayout(nullptr);
  device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

  RectConstants rect_constants;
  rect_constants.offset[0] = -1.0f;
  rect_constants.offset[1] = 1.0f;
  rect_constants.size[0] = 2.0f;
  rect_constants.size[1] = -2.0f;
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(device_context->Map(vs_constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                 &mapped))) {
    return fail("vs constants map");
  }
  std::memcpy(mapped.pData, &rect_constants, sizeof(rect_constants));
  device_context->Unmap(vs_constants_.Get(), 0);

  BilinearConstants bilinear_constants;
  bilinear_constants.output_offset[0] = 0;
  bilinear_constants.output_offset[1] = 0;
  bilinear_constants.output_size_inv[0] = 1.0f / float(texture_desc.Width);
  bilinear_constants.output_size_inv[1] = 1.0f / float(texture_desc.Height);
  if (FAILED(device_context->Map(ps_constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                 &mapped))) {
    return fail("ps constants map");
  }
  std::memcpy(mapped.pData, &bilinear_constants, sizeof(bilinear_constants));
  device_context->Unmap(ps_constants_.Get(), 0);

  device_context->VSSetShader(rect_vs_.Get(), nullptr, 0);
  device_context->PSSetShader(bilinear_ps_.Get(), nullptr, 0);
  device_context->VSSetConstantBuffers(0, 1, vs_constants_.GetAddressOf());
  device_context->PSSetConstantBuffers(0, 1, ps_constants_.GetAddressOf());
  device_context->PSSetShaderResources(0, 1, &source);
  device_context->PSSetSamplers(0, 1, guest_output_sampler_.GetAddressOf());
  // When the mailbox is shared with a present device, hold the keyed mutex
  // across the draw so the present device cannot sample it mid-blit. The
  // mutex starts at key 0 (this side acquires 0, releases 1); the paint
  // side does the reverse. A timeout skips this blit instead of hanging -
  // the mailbox keeps the previous frame.
  Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
  if (PresentDeviceIsSeparate()) {
    resource->QueryInterface(IID_PPV_ARGS(&keyed_mutex));
  }
  if (keyed_mutex && FAILED(keyed_mutex->AcquireSync(0, 500))) {
    return fail("shared mailbox busy");
  }
  device_context->Draw(4, 0);
  // Unbind so the source can be written again (the resolve may target it).
  ID3D11ShaderResourceView* null_srv = nullptr;
  device_context->PSSetShaderResources(0, 1, &null_srv);
  if (keyed_mutex) {
    keyed_mutex->ReleaseSync(1);
  }
  // Temporary colour probe: the 8-bit frontbuffer and the 10-bit guest output
  // are sampled from the same frame, both reported in display order.
  {
    static uint32_t probe_frame = 0;
    static Microsoft::WRL::ComPtr<ID3D11Texture2D> probe_source_staging;
    static Microsoft::WRL::ComPtr<ID3D11Texture2D> probe_output_staging;
    if ((++probe_frame % 300) == 1) {
      device_context->OMSetRenderTargets(0, nullptr, nullptr);
      Microsoft::WRL::ComPtr<ID3D11Resource> source_resource;
      source->GetResource(&source_resource);
      Microsoft::WRL::ComPtr<ID3D11Texture2D> source_texture;
      if (source_resource) {
        source_resource.As(&source_texture);
      }
      {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        source->GetDesc(&srv_desc);
        D3D11_TEXTURE2D_DESC src_desc = {};
        if (source_texture) {
          source_texture->GetDesc(&src_desc);
        }
        REXLOG_INFO("D3D11Presenter: blit formats srv={} tex={} out={} src_usage={} src_bind={}",
                    uint32_t(srv_desc.Format), uint32_t(src_desc.Format),
                    uint32_t(kGuestOutputFormat), uint32_t(src_desc.Usage),
                    uint32_t(src_desc.BindFlags));
      }
      const GuestOutputColourMean source_mean = SampleGuestOutputColour(
          provider_.GetDevice(), device_context, source_texture.Get(), probe_source_staging);
      const GuestOutputColourMean output_mean = SampleGuestOutputColour(
          provider_.GetDevice(), device_context, resource, probe_output_staging);
      const GuestOutputColourMean sky_mean = SampleGuestOutputColour(
          provider_.GetDevice(), device_context, source_texture.Get(), probe_source_staging,
          0.0, 0.25);
      const GuestOutputColourMean road_mean = SampleGuestOutputColour(
          provider_.GetDevice(), device_context, source_texture.Get(), probe_source_staging,
          0.75, 1.0);
      if (sky_mean.valid && road_mean.valid) {
        REXLOG_INFO("D3D11Presenter: blit bands frame={} sky={:.3f}/{:.3f}/{:.3f} "
                    "road={:.3f}/{:.3f}/{:.3f}",
                    probe_frame, sky_mean.r, sky_mean.g, sky_mean.b, road_mean.r,
                    road_mean.g, road_mean.b);
      }
      if (source_mean.valid && output_mean.valid) {
        REXLOG_INFO("D3D11Presenter: blit colours frame={} src={:.3f}/{:.3f}/{:.3f} "
                    "out={:.3f}/{:.3f}/{:.3f} range={:.2f}-{:.2f}/{:.2f}-{:.2f}/"
                    "{:.2f}-{:.2f}",
                    probe_frame, source_mean.r, source_mean.g, source_mean.b, output_mean.r,
                    output_mean.g, output_mean.b, source_mean.vmin[0], source_mean.vmax[0],
                    source_mean.vmin[1], source_mean.vmax[1], source_mean.vmin[2],
                    source_mean.vmax[2]);
        // Duplicated to stderr: the runtime log stays locked while the game
        // runs, and the launcher keeps stderr in a readable file.
        std::fprintf(stderr, "COLOUR blit frame=%u src=%.3f/%.3f/%.3f out=%.3f/%.3f/%.3f\n",
                     probe_frame, source_mean.r, source_mean.g, source_mean.b, output_mean.r,
                     output_mean.g, output_mean.b);
        std::fflush(stderr);
      }
    }
  }
  return true;
}

Presenter::PaintResult D3D11Presenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  PaintContext& paint_context = paint_context_;
  if (!paint_context.swap_chain) {
    return PaintResult::kNotPresentedConnectionOutdated;
  }
  // The paint runs on the device that owns the swap chain. When the swap
  // chain is on a second (present) device, everything the paint touches -
  // the back buffer, the mailbox sample, the paint pipeline - is on that
  // device; the render device only blits into the shared mailbox.
  const bool separate_present = PresentDeviceIsSeparate();
  ID3D11DeviceContext* device_context =
      separate_present ? present_context_ : provider_.GetImmediateContext();
  ID3D11Device* paint_device =
      separate_present ? present_device_ : provider_.GetDevice();
  if (separate_present && !EnsurePresentPaintObjects()) {
    return PaintResult::kNotPresented;
  }
  ID3D11VertexShader* paint_rect_vs =
      separate_present ? present_paint_.rect_vs.Get() : rect_vs_.Get();
  ID3D11PixelShader* paint_bilinear_ps =
      separate_present ? present_paint_.bilinear_ps.Get() : bilinear_ps_.Get();
  ID3D11PixelShader* paint_bilinear_dither_ps = separate_present
                                                    ? present_paint_.bilinear_dither_ps.Get()
                                                    : bilinear_dither_ps_.Get();
  ID3D11Buffer* paint_vs_constants =
      separate_present ? present_paint_.vs_constants.Get() : vs_constants_.Get();
  ID3D11Buffer* paint_ps_constants =
      separate_present ? present_paint_.ps_constants.Get() : ps_constants_.Get();
  ID3D11SamplerState* paint_sampler =
      separate_present ? present_paint_.sampler.Get() : guest_output_sampler_.Get();
  ID3D11BlendState* paint_blend =
      separate_present ? present_paint_.blend_state.Get() : blend_state_.Get();
  ID3D11DepthStencilState* paint_depth =
      separate_present ? present_paint_.depth_stencil_state.Get() : depth_stencil_state_.Get();
  ID3D11RasterizerState* paint_rasterizer =
      separate_present ? present_paint_.rasterizer_state.Get() : rasterizer_state_.Get();
  const uint32_t back_buffer_index = std::min(
      paint_context.swap_chain->GetCurrentBackBufferIndex(),
      PaintContext::kSwapChainBufferCount - 1);
  ID3D11RenderTargetView* back_buffer_rtv =
      paint_context.swap_chain_rtvs[back_buffer_index].Get();
  if (!back_buffer_rtv) {
    return PaintResult::kNotPresented;
  }

  // Acquire the guest output image for this paint, if any.
  uint32_t mailbox_index;
  GuestOutputProperties properties;
  GuestOutputPaintConfig paint_config;
  ID3D11ShaderResourceView* guest_output_srv = nullptr;
  {
    std::unique_lock<std::mutex> guest_output_consumer_lock(
        ConsumeGuestOutput(mailbox_index, &properties, &paint_config));
    if (mailbox_index != UINT32_MAX && guest_output_images_[mailbox_index].srv) {
      if (separate_present) {
        // Sample the shared texture through the present device's view.
        if (CopyGuestOutputToPresent(mailbox_index)) {
          guest_output_srv = guest_output_images_[mailbox_index].present_srv.Get();
        } else {
          mailbox_index = UINT32_MAX;
        }
      } else {
        guest_output_srv = guest_output_images_[mailbox_index].srv.Get();
      }
    } else {
      mailbox_index = UINT32_MAX;
    }
  }

  // When the mailbox is shared, hold the keyed mutex across the paint's
  // reads (reverse of the blit side: acquire 1, release 0). A timeout means
  // the render device is still blitting - paint black this frame instead of
  // sampling a half-written texture.
  IDXGIKeyedMutex* paint_mutex = nullptr;
  bool paint_mutex_held = false;
  if (separate_present && guest_output_srv) {
    paint_mutex = guest_output_images_[mailbox_index].present_mutex.Get();
    // On the readback path there is no mutex at all - the pixels were copied
    // into this device's own texture already, so there is nothing to wait for
    // and the frame must not be discarded.
    if (!paint_mutex) {
      // Paint straight away.
    } else if (SUCCEEDED(paint_mutex->AcquireSync(1, 500))) {
      paint_mutex_held = true;
    } else {
      // Test log: a timeout here means the render device is still blitting
      // into the mailbox (or the mutex is stuck) - the frame paints black
      // instead of sampling a half-written texture.
      static bool paint_mutex_timeout_logged = false;
      if (!paint_mutex_timeout_logged) {
        paint_mutex_timeout_logged = true;
        REXLOG_WARN("D3D11Presenter: present mutex acquire timed out - painting black "
                    "this frame (render device still blitting?)");
      }
      guest_output_srv = nullptr;
      mailbox_index = UINT32_MAX;
    }
  }

  {
    // The first few paints only: which of these being false is what leaves the
    // back buffer black.
    static int logged_paint_inputs = 0;
    if (logged_paint_inputs < 5) {
      ++logged_paint_inputs;
      REXLOG_INFO("D3D11Presenter: paint #{} separate={} active={} srv={} mailbox={}",
                  logged_paint_inputs, separate_present, properties.IsActive(),
                  guest_output_srv != nullptr, mailbox_index);
    }
  }
  // Fixed-function state shared by all painting.
  device_context->OMSetRenderTargets(1, &back_buffer_rtv, nullptr);
  device_context->OMSetBlendState(paint_blend, nullptr, 0xFFFFFFFF);
  device_context->OMSetDepthStencilState(paint_depth, 0);
  device_context->RSSetState(paint_rasterizer);
  D3D11_VIEWPORT viewport;
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = float(paint_context.swap_chain_width);
  viewport.Height = float(paint_context.swap_chain_height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  device_context->RSSetViewports(1, &viewport);
  D3D11_RECT scissor = {0, 0, LONG(paint_context.swap_chain_width),
                        LONG(paint_context.swap_chain_height)};
  device_context->RSSetScissorRects(1, &scissor);

  // Always clear to black first - the guest output (or its letterbox) is
  // painted on top. Simpler and as correct as the D3D12 partial clears.
  device_context->ClearRenderTargetView(back_buffer_rtv, kBackBufferClearColor);

  if (guest_output_srv && properties.IsActive()) {
    GuestOutputPaintFlow guest_output_flow =
        GetGuestOutputPaintFlow(properties, paint_context.swap_chain_width,
                                paint_context.swap_chain_height, paint_context.swap_chain_width,
                                paint_context.swap_chain_height, paint_config);

    ID3D11Device* device = paint_device;
    // Create intermediate images for the non-final effects of the chain on
    // demand (only the flow's chain length matters).
    for (size_t i = 0; i + 1 < guest_output_flow.effect_count; ++i) {
      const std::pair<uint32_t, uint32_t>& intermediate_size =
          guest_output_flow.effect_output_sizes[i];
      GuestOutputImage& intermediate = intermediate_images_[i];
      if (intermediate.texture) {
        D3D11_TEXTURE2D_DESC texture_desc;
        intermediate.texture->GetDesc(&texture_desc);
        if (texture_desc.Width != intermediate_size.first ||
            texture_desc.Height != intermediate_size.second) {
          intermediate.texture.Reset();
          intermediate.srv.Reset();
          intermediate.rtv.Reset();
        }
      }
      if (!intermediate.texture) {
        D3D11_TEXTURE2D_DESC texture_desc = {};
        texture_desc.Width = intermediate_size.first;
        texture_desc.Height = intermediate_size.second;
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = kGuestOutputFormat;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &intermediate.texture)) ||
            FAILED(device->CreateShaderResourceView(intermediate.texture.Get(), nullptr,
                                                    &intermediate.srv)) ||
            FAILED(device->CreateRenderTargetView(intermediate.texture.Get(), nullptr,
                                                  &intermediate.rtv))) {
          REXLOG_ERROR("D3D11Presenter: Failed to create a {}x{} intermediate texture",
                       intermediate_size.first, intermediate_size.second);
          intermediate.texture.Reset();
          intermediate.srv.Reset();
          intermediate.rtv.Reset();
          break;
        }
      }
    }

    device_context->IASetInputLayout(nullptr);
    device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    device_context->VSSetShader(paint_rect_vs, nullptr, 0);
    device_context->PSSetSamplers(0, 1, &paint_sampler);

    for (size_t i = 0; i < guest_output_flow.effect_count; ++i) {
      const bool is_final_effect = i + 1 == guest_output_flow.effect_count;
      const std::pair<uint32_t, uint32_t>& output_size =
          guest_output_flow.effect_output_sizes[i];
      ID3D11ShaderResourceView* effect_source_srv = i ? intermediate_images_[i - 1].srv.Get()
                                                      : guest_output_srv;
      ID3D11RenderTargetView* effect_dest_rtv =
          is_final_effect ? back_buffer_rtv : intermediate_images_[i].rtv.Get();
      if (!effect_source_srv || !effect_dest_rtv) {
        break;
      }

      device_context->OMSetRenderTargets(1, &effect_dest_rtv, nullptr);
      D3D11_VIEWPORT effect_viewport;
      effect_viewport.TopLeftX = 0.0f;
      effect_viewport.TopLeftY = 0.0f;
      effect_viewport.Width = float(output_size.first);
      effect_viewport.Height = float(output_size.second);
      effect_viewport.MinDepth = 0.0f;
      effect_viewport.MaxDepth = 1.0f;
      device_context->RSSetViewports(1, &effect_viewport);

      BindEffectPipeline(guest_output_flow.effects[i], device_context, paint_bilinear_ps,
                         paint_bilinear_dither_ps);

      int32_t effect_rect_x, effect_rect_y;
      guest_output_flow.GetEffectOutputOffset(i, effect_rect_x, effect_rect_y);
      RectConstants rect_constants;
      const float effect_x_to_ndc = 2.0f / float(paint_context.swap_chain_width);
      const float effect_y_to_ndc = 2.0f / float(paint_context.swap_chain_height);
      rect_constants.offset[0] = -1.0f + float(effect_rect_x) * effect_x_to_ndc;
      // +Y is -V.
      rect_constants.offset[1] = 1.0f - float(effect_rect_y) * effect_y_to_ndc;
      rect_constants.size[0] = float(output_size.first) * effect_x_to_ndc;
      rect_constants.size[1] = -float(output_size.second) * effect_y_to_ndc;
      D3D11_MAPPED_SUBRESOURCE mapped;
      if (SUCCEEDED(device_context->Map(paint_vs_constants, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                       &mapped))) {
        std::memcpy(mapped.pData, &rect_constants, sizeof(rect_constants));
        device_context->Unmap(paint_vs_constants, 0);
      }

      // The pixel shader sees SV_Position in destination pixels, matching
      // the original shader's BilinearConstants contract regardless of the
      // effect actually bound (the fallback path stretches like bilinear).
      BilinearConstants effect_constants;
      effect_constants.Initialize(guest_output_flow, i);
      if (SUCCEEDED(device_context->Map(paint_ps_constants, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                       &mapped))) {
        std::memcpy(mapped.pData, &effect_constants, sizeof(effect_constants));
        device_context->Unmap(paint_ps_constants, 0);
      }

      device_context->VSSetConstantBuffers(0, 1, &paint_vs_constants);
      device_context->PSSetConstantBuffers(0, 1, &paint_ps_constants);
      device_context->PSSetShaderResources(0, 1, &effect_source_srv);
      device_context->Draw(4, 0);
    }

    {
      static int logged_draws = 0;
      if (logged_draws < 3) {
        ++logged_draws;
        REXLOG_INFO("D3D11Presenter: paint draw #{} effects={} swap={}x{} back_buffer={}",
                    logged_draws, guest_output_flow.effect_count,
                    paint_context.swap_chain_width, paint_context.swap_chain_height,
                    back_buffer_index);
      }
    }
    // Unbind the source so it can be written by the refresher again.
    ID3D11ShaderResourceView* null_srv = nullptr;
    device_context->PSSetShaderResources(0, 1, &null_srv);
    if (paint_mutex_held) {
      paint_mutex->ReleaseSync(0);
      paint_mutex_held = false;
    }
    SetPaintedGuestOutputRect(&guest_output_flow);
  } else {
    SetPaintedGuestOutputRect(nullptr);
  }

  if (execute_ui_drawers) {
    D3D11UIDrawContext ui_draw_context(*this, paint_context.swap_chain_width,
                                       paint_context.swap_chain_height, device_context,
                                       back_buffer_rtv);
    ExecuteUIDrawersFromUIThread(ui_draw_context);
    // The immediate drawer may have changed state.
    device_context->OMSetRenderTargets(1, &back_buffer_rtv, nullptr);
    device_context->RSSetViewports(1, &viewport);
    device_context->RSSetScissorRects(1, &scissor);
  }

  {
    // Temporary: what the display actually receives, in the same shape as the
    // guest-output probe so the two can be compared channel by channel.
    static uint32_t paint_probe_frame = 0;
    static Microsoft::WRL::ComPtr<ID3D11Texture2D> paint_probe_staging;
    if ((++paint_probe_frame % 300) == 1) {
      Microsoft::WRL::ComPtr<ID3D11Resource> back_buffer_resource;
      back_buffer_rtv->GetResource(&back_buffer_resource);
      Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer_texture;
      if (back_buffer_resource) {
        back_buffer_resource.As(&back_buffer_texture);
      }
      const GuestOutputColourMean back_mean = SampleGuestOutputColour(
          paint_device, device_context, back_buffer_texture.Get(), paint_probe_staging);
      if (back_mean.valid) {
        REXLOG_INFO("D3D11Presenter: paint colours frame={} back={:.3f}/{:.3f}/{:.3f}",
                    paint_probe_frame, back_mean.r, back_mean.g, back_mean.b);
        std::fprintf(stderr, "COLOUR paint frame=%u back=%.3f/%.3f/%.3f\n", paint_probe_frame,
                     back_mean.r, back_mean.g, back_mean.b);
        std::fflush(stderr);
      }
    }
  }
  // Diagnostics: write the composed frame and the guest output it came from, so
  // the presented image can be inspected without capturing the flip-model chain.
  if (REXCVAR_GET(pinyon_dump_frames)) {
    static uint32_t dump_frame = 0;
    if ((++dump_frame % 30) == 1) {
      Microsoft::WRL::ComPtr<ID3D11Resource> back_resource;
      back_buffer_rtv->GetResource(&back_resource);
      Microsoft::WRL::ComPtr<ID3D11Texture2D> back_texture;
      if (back_resource) {
        back_resource.As(&back_texture);
      }
      char frame_path[256];
      std::snprintf(frame_path, sizeof(frame_path),
                    "E:/PinyonShift/work/.local/frames/frame_%06u.bmp", dump_frame);
      FrameDumpReport back_report;
      const bool dumped = DumpTextureBmp(paint_device, device_context, back_texture.Get(),
                                         frame_path, &back_report);
      REXLOG_INFO(
          "D3D11Presenter: frame dump {} {}x{} format {} samples {} pitch {} expected {} "
          "written {} create 0x{:08X} query {} map 0x{:08X} ({})",
          frame_path, back_report.width, back_report.height, back_report.format,
          back_report.sample_count, back_report.row_pitch, back_report.bytes_expected,
          back_report.bytes_written, uint32_t(back_report.create_hresult),
          back_report.copy_query_status, uint32_t(back_report.map_hresult), back_report.failure);
      if (guest_output_srv) {
        Microsoft::WRL::ComPtr<ID3D11Resource> guest_resource;
        guest_output_srv->GetResource(&guest_resource);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> guest_texture;
        if (guest_resource) {
          guest_resource.As(&guest_texture);
        }
        char guest_path[256];
        std::snprintf(guest_path, sizeof(guest_path),
                      "E:/PinyonShift/work/.local/frames/guest_%06u.bmp", dump_frame);
        FrameDumpReport guest_report;
        DumpTextureBmp(paint_device, device_context, guest_texture.Get(), guest_path,
                       &guest_report);
        REXLOG_INFO(
            "D3D11Presenter: guest dump {} {}x{} format {} pitch {} expected {} written {} ({})",
            guest_path, guest_report.width, guest_report.height, guest_report.format,
            guest_report.row_pitch, guest_report.bytes_expected, guest_report.bytes_written,
            guest_report.failure);
      }
      REXLOG_INFO("D3D11Presenter: dumped frame {} ({})", dump_frame, dumped ? "ok" : "failed");
    }
  }
  const bool vsync = rex::cvar::GetFlagByName("vsync") != "false";
  HRESULT present_result = paint_context.swap_chain->Present(vsync ? 1 : 0, 0);
  if (present_result == DXGI_ERROR_DEVICE_REMOVED ||
      present_result == DXGI_ERROR_DEVICE_RESET) {
    REXLOG_ERROR("D3D11Presenter: device lost on Present (HRESULT 0x{:08X})",
                 uint32_t(present_result));
    return PaintResult::kGpuLostExternally;
  }
  if (FAILED(present_result)) {
    REXLOG_ERROR("D3D11Presenter: Present failed (HRESULT 0x{:08X})", uint32_t(present_result));
    return PaintResult::kNotPresented;
  }
  // Presented-frame census for the Phase 1 gate and later performance passes.
  uint64_t presented_frames = ++presented_frame_count_;
  if (presented_frames % 300 == 0) {
    REXLOG_INFO("D3D11Presenter: presented {} frames (cleared-frame pace)", presented_frames);
  }
  return PaintResult::kPresented;
}

bool D3D11Presenter::CaptureGuestOutput(RawImage& image_out) {
  uint32_t mailbox_index;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> guest_output_texture;
  {
    std::unique_lock<std::mutex> guest_output_consumer_lock(
        ConsumeGuestOutput(mailbox_index, nullptr, nullptr));
    if (mailbox_index != UINT32_MAX) {
      guest_output_texture = guest_output_images_[mailbox_index].texture;
    }
  }
  if (!guest_output_texture) {
    REXLOG_ERROR("D3D11Presenter: capture found no guest output texture");
    return false;
  }
  D3D11_TEXTURE2D_DESC texture_desc;
  guest_output_texture->GetDesc(&texture_desc);

  ID3D11Device* device = provider_.GetDevice();
  ID3D11DeviceContext* device_context = provider_.GetImmediateContext();

  D3D11_TEXTURE2D_DESC staging_desc = texture_desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  const HRESULT staging_hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
  if (FAILED(staging_hr)) {
    REXLOG_ERROR(
        "D3D11Presenter: Failed to create the guest output capture staging texture hr={:08X}",
        static_cast<uint32_t>(staging_hr));
    return false;
  }
  device_context->CopyResource(staging.Get(), guest_output_texture.Get());

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(device_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    REXLOG_ERROR("D3D11Presenter: Failed to map the guest output capture staging texture");
    return false;
  }

  image_out.width = texture_desc.Width;
  image_out.height = texture_desc.Height;
  image_out.stride = size_t(texture_desc.Width) * 4;
  image_out.data.resize(image_out.stride * texture_desc.Height);
  for (uint32_t y = 0; y < texture_desc.Height; ++y) {
    const uint32_t* source_row =
        reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) +
                                          mapped.RowPitch * y);
    uint8_t* dest_row = image_out.data.data() + image_out.stride * y;
    for (uint32_t x = 0; x < texture_desc.Width; ++x) {
      uint32_t packed8bpc = Packed10bpcRGBTo8bpcBytes(source_row[x]);
      std::memcpy(dest_row + x * 4, &packed8bpc, 4);
    }
  }
  device_context->Unmap(staging.Get(), 0);
  return true;
}

Presenter::SurfacePaintConnectResult
D3D11Presenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
    bool was_paintable, bool& is_vsync_implicit_out) {
  constexpr uint32_t kMaxSwapChainDimension = 16384;
  const uint32_t new_swap_chain_width = std::min(new_surface_width, kMaxSwapChainDimension);
  const uint32_t new_swap_chain_height = std::min(new_surface_height, kMaxSwapChainDimension);

  PaintContext& paint_context = paint_context_;
  if (paint_context.swap_chain) {
    if (was_paintable && paint_context.swap_chain_width == new_swap_chain_width &&
        paint_context.swap_chain_height == new_swap_chain_height) {
      is_vsync_implicit_out = false;
      return SurfacePaintConnectResult::kSuccessUnchanged;
    }
    // Release the buffer views before resizing.
    for (Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv_ref :
         paint_context.swap_chain_rtvs) {
      rtv_ref.Reset();
    }
    for (Microsoft::WRL::ComPtr<ID3D11Texture2D>& buffer_ref : paint_context.swap_chain_buffers) {
      buffer_ref.Reset();
    }
    bool swap_chain_resized =
        SUCCEEDED(paint_context.swap_chain->ResizeBuffers(
            0, UINT(new_swap_chain_width), UINT(new_swap_chain_height), DXGI_FORMAT_UNKNOWN, 0));
    if (swap_chain_resized) {
      swap_chain_resized = AcquireSwapChainBuffers(paint_context);
      if (swap_chain_resized) {
        paint_context.swap_chain_width = new_swap_chain_width;
        paint_context.swap_chain_height = new_swap_chain_height;
      }
    }
    if (!swap_chain_resized) {
      REXLOG_ERROR("D3D11Presenter: Failed to resize the swap chain");
      paint_context.DestroySwapChain();
    }
  }

  if (!paint_context.swap_chain) {
    Surface::TypeIndex surface_type = new_surface.GetType();
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc;
    swap_chain_desc.Width = UINT(new_swap_chain_width);
    swap_chain_desc.Height = UINT(new_swap_chain_height);
    swap_chain_desc.Format = kSwapChainFormat;
    swap_chain_desc.Stereo = FALSE;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.SampleDesc.Quality = 0;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = UINT(PaintContext::kSwapChainBufferCount);
    swap_chain_desc.Scaling =
        (new_swap_chain_width == new_surface_width && new_swap_chain_height == new_surface_height)
            ? DXGI_SCALING_NONE
            : DXGI_SCALING_STRETCH;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swap_chain_desc.Flags = 0;

    IDXGIFactory2* dxgi_factory = provider_.GetDXGIFactory();
    bool acquired = false;
    switch (surface_type) {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
      case Surface::kTypeIndex_Win32Hwnd: {
        HWND surface_hwnd = static_cast<const Win32HwndSurface&>(new_surface).hwnd();
        // The swap chain must live on the adapter that owns this window's
        // monitor. The render adapter (GTX 570) is headless; a flip chain
        // on it cannot acquire buffers for a window on the other adapter,
        // so Present succeeds and nothing reaches the screen. The render
        // device keeps rendering - only the swap chain and the paint move
        // to the display adapter, fed by the shared mailbox texture.
        ID3D11Device* present_device = provider_.GetOrCreatePresentDevice(surface_hwnd);
        present_device_ = present_device;
        present_context_ = (present_device == provider_.GetDevice())
                               ? provider_.GetImmediateContext()
                               : provider_.GetPresentImmediateContext();
        // Try the swap effect tiers in order. A flip model is preferred, but
        // some valid combinations (an older WDDM driver behind a window owned
        // by another adapter, such as the GTX 570 target) create a flip chain
        // whose buffers beyond the first cannot be acquired - fall back until
        // both creation and buffer acquisition succeed, down to a
        // single-buffer blt chain, which works everywhere.
        struct SwapChainTier {
          DXGI_SWAP_EFFECT swap_effect;
          uint32_t buffer_count;
          bool scaling_none_allowed;
          const char* name;
        };
        constexpr SwapChainTier kTiers[] = {
            {DXGI_SWAP_EFFECT_FLIP_DISCARD, PaintContext::kSwapChainBufferCount, true,
             "FLIP_DISCARD"},
            {DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, PaintContext::kSwapChainBufferCount, true,
             "FLIP_SEQUENTIAL"},
            {DXGI_SWAP_EFFECT_DISCARD, 1, false, "blt"},
        };
        const DXGI_SCALING scaling_by_size =
            (new_swap_chain_width == new_surface_width &&
             new_swap_chain_height == new_surface_height)
                ? DXGI_SCALING_NONE
                : DXGI_SCALING_STRETCH;
        for (const SwapChainTier& tier : kTiers) {
          swap_chain_desc.SwapEffect = tier.swap_effect;
          swap_chain_desc.BufferCount = UINT(tier.buffer_count);
          swap_chain_desc.Scaling =
              tier.scaling_none_allowed ? scaling_by_size : DXGI_SCALING_STRETCH;
          Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_1;
          HRESULT create_result = dxgi_factory->CreateSwapChainForHwnd(
              present_device, surface_hwnd, &swap_chain_desc, nullptr, nullptr,
              &swap_chain_1);
          if (FAILED(create_result)) {
            REXLOG_WARN("D3D11Presenter: {} swap chain creation failed (HRESULT 0x{:08X})",
                        tier.name, uint32_t(create_result));
            continue;
          }
          if (FAILED(swap_chain_1->QueryInterface(IID_PPV_ARGS(&paint_context.swap_chain)))) {
            REXLOG_WARN("D3D11Presenter: no version 3 swap chain interface for the {} tier",
                        tier.name);
            continue;
          }
          // From now on, in case of any failure, DestroySwapChain must be called.
          paint_context.swap_chain_width = new_swap_chain_width;
          paint_context.swap_chain_height = new_swap_chain_height;
          if (AcquireSwapChainBuffers(paint_context)) {
            acquired = true;
            if (tier.swap_effect != kTiers[0].swap_effect) {
              REXLOG_WARN("D3D11Presenter: using the {} swap effect tier ({}x{})", tier.name,
                          new_swap_chain_width, new_swap_chain_height);
            }
            // Test log: one line that says exactly how the frame gets to the
            // screen - which adapter renders, which one presents, which swap
            // tier survived, and at what size. If the monitor is black, this
            // line plus the tier warnings above say why.
            DXGI_SWAP_CHAIN_DESC1 acquired_desc{};
            paint_context.swap_chain->GetDesc1(&acquired_desc);
            REXGPU_INFO(
                "D3D11Presenter: TEST SUMMARY render=\"{}\" present=\"{}\" tier={} "
                "swap={}x{} buffers={} (separate present device: {})",
                std::string(provider_.GetAdapterDescription().begin(),
                            provider_.GetAdapterDescription().end()),
                std::string(provider_.GetPresentAdapterDescription().begin(),
                            provider_.GetPresentAdapterDescription().end()),
                tier.name, new_swap_chain_width, new_swap_chain_height,
                acquired_desc.BufferCount,
                (present_device_ != provider_.GetDevice()) ? "yes" : "no");
            break;
          }
          paint_context.DestroySwapChain();
        }
        if (acquired) {
          dxgi_factory->MakeWindowAssociation(surface_hwnd, DXGI_MWA_NO_ALT_ENTER);
        }
      } break;
#endif
      default:
        assert_unhandled_case(surface_type);
        REXLOG_ERROR(
            "D3D11Presenter: Tried to create a swap chain for an unsupported surface type");
        return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
    if (!acquired) {
      REXLOG_ERROR("D3D11Presenter: could not create and acquire a swap chain on any tier");
      return SurfacePaintConnectResult::kFailure;
    }
  }

  is_vsync_implicit_out = false;
  return SurfacePaintConnectResult::kSuccess;
}

bool D3D11Presenter::AcquireSwapChainBuffers(PaintContext& paint_context) {
  DXGI_SWAP_CHAIN_DESC1 desc = {};
  if (FAILED(paint_context.swap_chain->GetDesc1(&desc))) {
    REXLOG_ERROR("D3D11Presenter: Failed to query the swap chain description");
    return false;
  }
  const uint32_t buffer_count =
      std::min<uint32_t>(desc.BufferCount, PaintContext::kSwapChainBufferCount);
  for (uint32_t i = 0; i < buffer_count; ++i) {
    HRESULT get_result = paint_context.swap_chain->GetBuffer(
        i, IID_PPV_ARGS(&paint_context.swap_chain_buffers[i]));
    if (FAILED(get_result)) {
      REXLOG_ERROR(
          "D3D11Presenter: GetBuffer({}) failed (chain has {} buffers, {}x{}, swap effect {}, "
          "HRESULT 0x{:08X})",
          i, desc.BufferCount, desc.Width, desc.Height, uint32_t(desc.SwapEffect),
          uint32_t(get_result));
      return false;
    }
    HRESULT rtv_result = (present_device_ ? present_device_ : provider_.GetDevice())
                             ->CreateRenderTargetView(
                                 paint_context.swap_chain_buffers[i].Get(), nullptr,
                                 &paint_context.swap_chain_rtvs[i]);
    if (FAILED(rtv_result)) {
      REXLOG_ERROR(
          "D3D11Presenter: CreateRenderTargetView for buffer {} failed (chain {}x{}, format {}, "
          "HRESULT 0x{:08X})",
          i, desc.Width, desc.Height, uint32_t(desc.Format), uint32_t(rtv_result));
      return false;
    }
  }
  paint_context.swap_chain_buffer_count = buffer_count;
  REXGPU_INFO(
      "D3D11Presenter: swap chain ready ({}x{}, {} buffers acquired, swap effect {})",
      desc.Width, desc.Height, buffer_count, uint32_t(desc.SwapEffect));
  return true;
}

void D3D11Presenter::PaintContext::DestroySwapChain() {
  for (Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv_ref : swap_chain_rtvs) {
    rtv_ref.Reset();
  }
  for (Microsoft::WRL::ComPtr<ID3D11Texture2D>& buffer_ref : swap_chain_buffers) {
    buffer_ref.Reset();
  }
  swap_chain.Reset();
  swap_chain_height = 0;
  swap_chain_width = 0;
  swap_chain_buffer_count = 0;
}

void D3D11Presenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  paint_context_.DestroySwapChain();
}

}  // namespace rex::ui::d3d11
