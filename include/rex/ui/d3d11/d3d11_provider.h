/**
 * @file        ui/d3d11/d3d11_provider.h
 * @brief       Direct3D 11 graphics provider (device + DXGI factory)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/ui/d3d11/d3d11_api.h>
#include <rex/ui/graphics_provider.h>

namespace rex::ui::d3d11 {

class D3D11Provider : public GraphicsProvider {
 public:
  ~D3D11Provider();

  static bool IsD3D11APIAvailable();

  static std::unique_ptr<D3D11Provider> Create();

  std::unique_ptr<Presenter> CreatePresenter(Presenter::HostGpuLossCallback host_gpu_loss_callback =
                                                 Presenter::FatalErrorHostGpuLossCallback) override;

  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

  ID3D11Device* GetDevice() const { return device_; }
  ID3D11DeviceContext* GetImmediateContext() const { return immediate_context_; }
  IDXGIFactory2* GetDXGIFactory() const { return dxgi_factory_; }
  IDXGIAdapter1* GetAdapter() const { return adapter_; }
  const std::wstring& GetAdapterDescription() const { return adapter_description_; }

  // The device on the adapter that owns the display for a window (created
  // lazily on first use, cached after). When the render adapter is headless
  // and the window lives on another adapter, the swap chain must be created
  // on this device: a flip chain on the headless adapter cannot acquire
  // buffers for a window on the other adapter, so Present goes nowhere.
  // Returns the render device when no second adapter with an output exists,
  // or when the render adapter already owns the window's monitor.
  ID3D11Device* GetOrCreatePresentDevice(HWND hwnd) const;
  ID3D11Device* GetPresentDevice() const { return present_device_; }
  ID3D11DeviceContext* GetPresentImmediateContext() const {
    return present_immediate_context_;
  }
  const std::wstring& GetPresentAdapterDescription() const {
    return present_adapter_description_;
  }

  // Adapter info.
  GpuVendorID GetAdapterVendorID() const { return adapter_vendor_id_; }

 private:
  D3D11Provider() = default;

  bool Initialize();

  IDXGIFactory2* dxgi_factory_ = nullptr;
  IDXGIAdapter1* adapter_ = nullptr;
  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* immediate_context_ = nullptr;
  // Stable identity of the render adapter (pointer identity is not reliable
  // across factory instances, so the present-device search compares LUIDs).
  LUID adapter_luid_{};

  // Present device (the adapter that owns the display), created lazily by
  // GetOrCreatePresentDevice. Equal to the render device/context/adapter
  // when no second adapter is needed; null until first use.
  mutable IDXGIAdapter1* present_adapter_ = nullptr;
  mutable ID3D11Device* present_device_ = nullptr;
  mutable ID3D11DeviceContext* present_immediate_context_ = nullptr;
  mutable std::wstring present_adapter_description_;

  std::wstring adapter_description_;
  GpuVendorID adapter_vendor_id_;
};

}  // namespace rex::ui::d3d11
