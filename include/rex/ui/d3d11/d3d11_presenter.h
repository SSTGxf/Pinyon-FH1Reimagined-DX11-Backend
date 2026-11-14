/**
 * @file        ui/d3d11/d3d11_presenter.h
 * @brief       Direct3D 11 presenter: swap chain + guest output scaling
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <memory>
#include <utility>

#include <rex/ui/d3d11/d3d11_api.h>
#include <rex/ui/d3d11/d3d11_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/surface.h>

namespace rex::ui::d3d11 {

class D3D11UIDrawContext final : public UIDrawContext {
 public:
  D3D11UIDrawContext(Presenter& presenter, uint32_t render_target_width,
                     uint32_t render_target_height, ID3D11DeviceContext* device_context,
                     ID3D11RenderTargetView* render_target_view)
      : UIDrawContext(presenter, render_target_width, render_target_height),
        device_context_(device_context),
        render_target_view_(render_target_view) {}

  ID3D11DeviceContext* device_context() const { return device_context_; }
  ID3D11RenderTargetView* render_target_view() const { return render_target_view_; }

 private:
  ID3D11DeviceContext* device_context_;
  ID3D11RenderTargetView* render_target_view_;
};

class D3D11Presenter final : public Presenter {
 public:
  static constexpr DXGI_FORMAT kGuestOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

  // The format used internally by Windows composition.
  static constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

  // The refresher writes into the guest output image (initially undefined
  // contents) through the render target view - feature level 11_0 has no
  // typed unordered stores for R10G10B10A2, so unlike D3D12 there is no UAV.
  class D3D11GuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    D3D11GuestOutputRefreshContext(bool& is_8bpc_out_ref, ID3D11Texture2D* resource,
                                   ID3D11RenderTargetView* render_target_view)
        : GuestOutputRefreshContext(is_8bpc_out_ref),
          resource_(resource),
          render_target_view_(render_target_view) {}

    ID3D11Texture2D* resource() const { return resource_; }
    ID3D11RenderTargetView* render_target_view() const { return render_target_view_; }

   private:
    ID3D11Texture2D* resource_;
    ID3D11RenderTargetView* render_target_view_;
  };

  static std::unique_ptr<D3D11Presenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                const D3D11Provider& provider) {
    auto presenter =
        std::unique_ptr<D3D11Presenter>(new D3D11Presenter(host_gpu_loss_callback, provider));
    if (!presenter->InitializeSurfaceIndependent()) {
      return nullptr;
    }
    return presenter;
  }

  ~D3D11Presenter();

  const D3D11Provider& provider() const { return provider_; }

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  bool CaptureGuestOutput(RawImage& image_out) override;

  // Fills this refresh's guest output image with a guest frontbuffer texture,
  // using the same rect vertex shader and bilinear pixel shader as the paint
  // path (the refresh-context sibling of the Direct3D 12 refresher's swap
  // pass, without its gamma and post-effect stages). Returns whether anything
  // was drawn; the caller falls back to its own clear otherwise.
  bool PresentGuestOutputTexture(GuestOutputRefreshContext& context,
                                 ID3D11ShaderResourceView* source, uint32_t source_width,
                                 uint32_t source_height);

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;

  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out) override;

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct GuestOutputImage {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;

    // Cross-adapter share: when the swap chain lives on a second (present)
    // device, the mailbox texture is created shared and opened there, so the
    // present device can sample the frame the render device blitted. Null
    // when the present device is the render device.
    HANDLE shared_handle = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> present_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> present_srv;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> render_mutex;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> present_mutex;

    // CPU readback path: the render device copies the mailbox into a
    // staging texture and the present device receives the pixels through
    // UpdateSubresource, so no surface is shared between the adapters.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> readback_staging;
    uint32_t readback_width = 0;
    uint32_t readback_height = 0;
  };

  // Paint objects on the present device (the rect/bilinear pipeline the
  // paint path uses). The refresher blit keeps using the render-device set
  // above; the paint uses this set when the swap chain is on a second
  // device, because D3D11 objects are bound to the device that made them.
  struct PresentPaintObjects {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> rect_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> bilinear_ps;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> bilinear_dither_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vs_constants;
    Microsoft::WRL::ComPtr<ID3D11Buffer> ps_constants;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_state;
    bool created = false;
  };

  struct PaintContext {
    static constexpr uint32_t kSwapChainBufferCount = 3;

    void DestroySwapChain();

    // Buffers actually acquired into the arrays (1...kSwapChainBufferCount).
    uint32_t swap_chain_buffer_count = 0;

    uint32_t swap_chain_width = 0;
    uint32_t swap_chain_height = 0;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, kSwapChainBufferCount> swap_chain_buffers;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, kSwapChainBufferCount>
        swap_chain_rtvs;
  };

  explicit D3D11Presenter(HostGpuLossCallback host_gpu_loss_callback, const D3D11Provider& provider)
      : Presenter(host_gpu_loss_callback), provider_(provider) {}

  bool InitializeSurfaceIndependent();

  bool EnsureGuestOutputImage(uint32_t mailbox_index, uint32_t width, uint32_t height);
  // Opens the mailbox texture on the present device (shared handle + SRV)
  // when the swap chain is on a second device. False when the image cannot
  // be shared (the paint then skips the guest output that frame).
  // Copies the render device's mailbox frame into the present device's own
  // texture through system memory (the cross-adapter path this replaces
  // cannot open a shared surface at all).
  bool CopyGuestOutputToPresent(uint32_t mailbox_index);
  // Creates the paint pipeline objects on the present device (once).
  bool EnsurePresentPaintObjects();
  // Whether the swap chain is on a second device (vs the render device).
  bool PresentDeviceIsSeparate() const {
    return present_device_ && present_device_ != provider_.GetDevice();
  }
  // Fills the swap chain buffer and RTV arrays from the current back buffers.
  bool AcquireSwapChainBuffers(PaintContext& paint_context);
  // Returns whether a pipeline is bound for the effect; unsupported effects
  // fall back to the bilinear pipeline with a one-time log (Phase 1).
  bool BindEffectPipeline(GuestOutputPaintEffect effect, ID3D11DeviceContext* device_context,
                          ID3D11PixelShader* bilinear_ps,
                          ID3D11PixelShader* bilinear_dither_ps);
  bool CreateEffectPipelines();

  const D3D11Provider& provider_;

  // The device the swap chain is created on (the display adapter; provider-
  // owned, non-owning here). Equal to the render device when no second
  // adapter is needed. Null until the first surface connect.
  ID3D11Device* present_device_ = nullptr;
  ID3D11DeviceContext* present_context_ = nullptr;
  PresentPaintObjects present_paint_;

  // Presented-frame census (logged periodically for pacing verification).
  uint64_t presented_frame_count_ = 0;

  // Surface-independent paint objects.
  Microsoft::WRL::ComPtr<ID3D11VertexShader> rect_vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> bilinear_ps_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> bilinear_dither_ps_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> vs_constants_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> ps_constants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> guest_output_sampler_;

  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_state_;

  // Guest output mailbox images (written by the refresher, sampled when
  // painting).
  std::array<GuestOutputImage, kGuestOutputMailboxSize> guest_output_images_;

  // Intermediate images for effect chains (kMaxGuestOutputPaintEffects - 1).
  std::array<GuestOutputImage, kMaxGuestOutputPaintEffects - 1> intermediate_images_;

  PaintContext paint_context_;

  bool effect_pipeline_warnings_logged_ = false;
};

}  // namespace rex::ui::d3d11
