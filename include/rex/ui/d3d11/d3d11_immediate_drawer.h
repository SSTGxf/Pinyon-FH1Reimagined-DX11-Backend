/**
 * @file        ui/d3d11/d3d11_immediate_drawer.h
 * @brief       D3D11 immediate-mode drawer (Phase 1: textures real, draws stubbed)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

#include <rex/ui/d3d11/d3d11_api.h>
#include <rex/ui/d3d11/d3d11_provider.h>
#include <rex/ui/immediate_drawer.h>

namespace rex::ui::d3d11 {

class D3D11ImmediateTexture final : public ImmediateTexture {
 public:
  D3D11ImmediateTexture(uint32_t width, uint32_t height,
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
                        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv)
      : ImmediateTexture(width, height), texture_(std::move(texture)), srv_(std::move(srv)) {}

  ID3D11ShaderResourceView* srv() const { return srv_.Get(); }

 private:
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
};

class D3D11ImmediateDrawer final : public ImmediateDrawer {
 public:
  static std::unique_ptr<D3D11ImmediateDrawer> Create(const D3D11Provider& provider) {
    return std::unique_ptr<D3D11ImmediateDrawer>(new D3D11ImmediateDrawer(provider));
  }

  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                  ImmediateTextureFilter filter, bool is_repeated,
                                                  const uint8_t* data) override;

  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;

 private:
  explicit D3D11ImmediateDrawer(const D3D11Provider& provider) : provider_(provider) {}

  const D3D11Provider& provider_;
  bool draws_stubbed_logged_ = false;
};

}  // namespace rex::ui::d3d11
