/**
 * @file        ui/d3d11/d3d11_immediate_drawer.cpp
 * @brief       D3D11 immediate-mode drawer (Phase 1)
 *
 *              Textures are created for real (the UI texture bookkeeping and
 *              lifetimes work), while draws are logged once and dropped: the
 *              overlay vertex pipeline lands with the drawing backend. The
 *              Phase 1 gate (cleared frames presenting) does not exercise
 *              it, and nothing accumulates GPU state in the dropped path.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/d3d11/d3d11_immediate_drawer.h>

#include <rex/logging.h>

namespace rex::ui::d3d11 {

std::unique_ptr<ImmediateTexture> D3D11ImmediateDrawer::CreateTexture(
    uint32_t width, uint32_t height, ImmediateTextureFilter filter, bool is_repeated,
    const uint8_t* data) {
  (void)filter;
  (void)is_repeated;
  ID3D11Device* device = provider_.GetDevice();
  D3D11_TEXTURE2D_DESC texture_desc = {};
  texture_desc.Width = width;
  texture_desc.Height = height;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_DEFAULT;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial_data = {};
  initial_data.pSysMem = data;
  initial_data.SysMemPitch = width * 4;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  if (FAILED(device->CreateTexture2D(&texture_desc, data ? &initial_data : nullptr, &texture))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create a {}x{} texture", width, height);
    return nullptr;
  }
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create a texture view");
    return nullptr;
  }
  return std::make_unique<D3D11ImmediateTexture>(width, height, std::move(texture),
                                                 std::move(srv));
}

void D3D11ImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  (void)batch;
}

void D3D11ImmediateDrawer::Draw(const ImmediateDraw& draw) {
  (void)draw;
  if (!draws_stubbed_logged_) {
    draws_stubbed_logged_ = true;
    REXGPU_INFO(
        "D3D11ImmediateDrawer: draws are dropped in the Phase 1 skeleton "
        "(the overlay vertex pipeline lands with the drawing backend)");
  }
}

void D3D11ImmediateDrawer::EndDrawBatch() {}

}  // namespace rex::ui::d3d11
