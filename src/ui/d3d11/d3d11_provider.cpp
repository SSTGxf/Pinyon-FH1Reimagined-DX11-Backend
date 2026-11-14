/**
 * @file        ui/d3d11/d3d11_provider.cpp
 * @brief       Direct3D 11 graphics provider: FL11_0 device creation
 *
 *              The device is created at D3D_FEATURE_LEVEL_11_0 on the adapter
 *              whose description contains the "d3d11_adapter" cvar string
 *              (for the GTX 570 target: "GTX 570"). Adapter selection is by
 *              description substring, never by hardcoded index. With an empty
 *              cvar the first hardware adapter supporting feature level 11_0
 *              is chosen and the full adapter list is logged.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d11/d3d11_immediate_drawer.h>
#include <rex/ui/d3d11/d3d11_presenter.h>
#include <rex/ui/d3d11/d3d11_provider.h>
#include <rex/ui/flags.h>

REXCVAR_DEFINE_STRING(d3d11_adapter, "",
                      "UI/D3D11",
                      "Adapter description substring for the d3d11 backend (for example "
                      "\"GTX 570\"); empty selects the first feature level 11_0 hardware "
                      "adapter.");

namespace rex::ui::d3d11 {

namespace {

std::wstring Lowercase(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](wchar_t c) { return wchar_t(std::towlower(c)); });
  return text;
}

bool DescriptionMatches(const wchar_t* description) {
  std::string needle = REXCVAR_GET(d3d11_adapter);
  if (needle.empty()) {
    return true;
  }
  std::wstring haystack = Lowercase(description ? description : L"");
  std::wstring needle_lower(needle.size(), L'\0');
  std::transform(needle.begin(), needle.end(), needle_lower.begin(),
                 [](char c) { return wchar_t(std::towlower(wchar_t((unsigned char)c))); });
  return haystack.find(needle_lower) != std::wstring::npos;
}

}  // namespace

bool D3D11Provider::IsD3D11APIAvailable() {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
}

std::unique_ptr<D3D11Provider> D3D11Provider::Create() {
  std::unique_ptr<D3D11Provider> provider(new D3D11Provider);
  if (!provider->Initialize()) {
    return nullptr;
  }
  return provider;
}

D3D11Provider::~D3D11Provider() {
  // The present device may alias the render device (single-adapter setup):
  // only release it when it is a distinct object.
  if (present_immediate_context_ && present_immediate_context_ != immediate_context_) {
    present_immediate_context_->ClearState();
    present_immediate_context_->Flush();
    present_immediate_context_->Release();
    present_immediate_context_ = nullptr;
  }
  if (present_device_ && present_device_ != device_) {
    present_device_->Release();
    present_device_ = nullptr;
  }
  if (present_adapter_ && present_adapter_ != adapter_) {
    present_adapter_->Release();
    present_adapter_ = nullptr;
  }
  if (immediate_context_) {
    immediate_context_->ClearState();
    immediate_context_->Flush();
    immediate_context_->Release();
    immediate_context_ = nullptr;
  }
  if (device_) {
    device_->Release();
    device_ = nullptr;
  }
  if (adapter_) {
    adapter_->Release();
    adapter_ = nullptr;
  }
  if (dxgi_factory_) {
    dxgi_factory_->Release();
    dxgi_factory_ = nullptr;
  }
}

ID3D11Device* D3D11Provider::GetOrCreatePresentDevice(HWND hwnd) const {
  if (present_device_) {
    return present_device_;
  }
  // Default: present on the render device (single-adapter setups, or when
  // the render adapter already owns the window's monitor).
  present_device_ = device_;
  present_immediate_context_ = immediate_context_;
  present_adapter_ = adapter_;
  present_adapter_description_ = adapter_description_;

  // Which monitor is the window on? Match it to the adapter that drives it.
  wchar_t monitor_device_name[32] = {};
  if (hwnd) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (monitor) {
      MONITORINFOEXW monitor_info{};
      monitor_info.cbSize = sizeof(monitor_info);
      if (GetMonitorInfoW(monitor, &monitor_info)) {
        wcsncpy_s(monitor_device_name, monitor_info.szDevice, _TRUNCATE);
      }
    }
  }
  {
    std::string monitor_narrow(monitor_device_name,
                               monitor_device_name + wcslen(monitor_device_name));
    // Test log: in a headless/SSH session there may be no monitor at all,
    // and the display adapter may report no outputs - that is expected
    // there, and the fallback below still has to pick the display adapter.
    REXGPU_INFO("D3D11Provider: present-device search - window monitor=\"{}\" "
                "render adapter LUID={}:{} (ptr {})",
                monitor_narrow, adapter_luid_.HighPart, adapter_luid_.LowPart,
                static_cast<const void*>(adapter_));
  }

  Microsoft::WRL::ComPtr<IDXGIAdapter1> display_adapter;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> first_output_adapter;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> first_other_adapter;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> enumerated;
  for (UINT adapter_index = 0;
       dxgi_factory_->EnumAdapters1(adapter_index, &enumerated) != DXGI_ERROR_NOT_FOUND;
       ++adapter_index) {
    DXGI_ADAPTER_DESC1 adapter_desc{};
    enumerated->GetDesc1(&adapter_desc);
    if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
      enumerated.Reset();
      continue;
    }
    const bool is_render_adapter =
        (adapter_desc.AdapterLuid.LowPart == adapter_luid_.LowPart &&
         adapter_desc.AdapterLuid.HighPart == adapter_luid_.HighPart);
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    bool has_output = SUCCEEDED(enumerated->EnumOutputs(0, &output));
    {
      std::string description_narrow(adapter_desc.Description,
                                     adapter_desc.Description + wcslen(adapter_desc.Description));
      REXGPU_INFO("D3D11Provider: present-device search - adapter {}: \"{}\" "
                  "is_render={} has_output={} (ptr {})",
                  adapter_index, description_narrow, is_render_adapter ? "yes" : "no",
                  has_output ? "yes" : "no",
                  static_cast<const void*>(enumerated.Get()));
    }
    if (!is_render_adapter && has_output && !first_output_adapter) {
      first_output_adapter = enumerated;
    }
    if (!is_render_adapter && !first_other_adapter) {
      first_other_adapter = enumerated;
    }
    if (monitor_device_name[0] && has_output) {
      for (UINT output_index = 0;
           enumerated->EnumOutputs(output_index, &output) != DXGI_ERROR_NOT_FOUND;
           ++output_index) {
        DXGI_OUTPUT_DESC output_desc{};
        output->GetDesc(&output_desc);
        if (wcscmp(output_desc.DeviceName, monitor_device_name) == 0) {
          display_adapter = enumerated;
          break;
        }
        output.Reset();
      }
      if (display_adapter) {
        break;
      }
    }
    enumerated.Reset();
  }

  Microsoft::WRL::ComPtr<IDXGIAdapter1> chosen = display_adapter;
  const char* chosen_branch = "monitor match";
  if (!chosen) {
    chosen = first_output_adapter;
    chosen_branch = "first non-render adapter with an output";
  }
  if (!chosen) {
    chosen = first_other_adapter;
    chosen_branch = "first non-render adapter";
  }
  DXGI_ADAPTER_DESC1 chosen_desc{};
  if (chosen) {
    chosen->GetDesc1(&chosen_desc);
  }
  const bool chosen_is_render =
      chosen && (chosen_desc.AdapterLuid.LowPart == adapter_luid_.LowPart &&
                 chosen_desc.AdapterLuid.HighPart == adapter_luid_.HighPart);
  {
    std::string chosen_narrow(chosen_desc.Description,
                              chosen_desc.Description + wcslen(chosen_desc.Description));
    REXGPU_INFO("D3D11Provider: present-device search - chosen=\"{}\" via {} "
                "(is_render={})",
                chosen_narrow, chosen_branch, chosen_is_render ? "yes" : "no");
  }
  if (!chosen || chosen_is_render) {
    REXGPU_INFO("D3D11Provider: presenting on the render adapter");
    return present_device_;
  }

  const D3D_FEATURE_LEVEL requested_feature_level = D3D_FEATURE_LEVEL_11_0;
  D3D_FEATURE_LEVEL created_feature_level = D3D_FEATURE_LEVEL_11_0;
  Microsoft::WRL::ComPtr<ID3D11Device> present_device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> present_context;
  HRESULT create_result = D3D11CreateDevice(
      chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      &requested_feature_level, 1, D3D11_SDK_VERSION, &present_device,
      &created_feature_level, &present_context);
  if (FAILED(create_result) || created_feature_level != D3D_FEATURE_LEVEL_11_0) {
    REXLOG_ERROR(
        "D3D11Provider: present device creation failed (HRESULT 0x{:08X}) - falling back "
        "to the render device",
        uint32_t(create_result));
    return present_device_;
  }
  // chosen_desc was already filled in by the search above.
  {
    std::string description_narrow(chosen_desc.Description,
                                   chosen_desc.Description + wcslen(chosen_desc.Description));
    REXGPU_INFO(
        "D3D11Provider: presenting on \"{}\" (vendor 0x{:04X}, device 0x{:04X}, {} MB) - "
        "swap chain lives on the display adapter, render stays on the render adapter",
        description_narrow, chosen_desc.VendorId, chosen_desc.DeviceId,
        chosen_desc.DedicatedVideoMemory / (1024 * 1024));
  }
  present_adapter_ = chosen.Detach();
  present_device_ = present_device.Detach();
  present_immediate_context_ = present_context.Detach();
  present_adapter_description_ = chosen_desc.Description;
  return present_device_;
}

bool D3D11Provider::Initialize() {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory1;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1)))) {
    REXLOG_ERROR("D3D11Provider: Failed to create the DXGI factory");
    return false;
  }
  if (FAILED(factory1->QueryInterface(IID_PPV_ARGS(&dxgi_factory_)))) {
    // IDXGIFactory2 is needed for swap chains for HWND - Windows 8+.
    REXLOG_ERROR("D3D11Provider: IDXGIFactory2 is unavailable");
    return false;
  }

  // Enumerate and log every adapter, and pick the requested one by
  // description substring (never by index).
  Microsoft::WRL::ComPtr<IDXGIAdapter1> enumerated_adapter;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> matched_adapter;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> first_hardware_adapter;
  for (UINT adapter_index = 0;
       factory1->EnumAdapters1(adapter_index, &enumerated_adapter) != DXGI_ERROR_NOT_FOUND;
       ++adapter_index) {
    DXGI_ADAPTER_DESC1 adapter_desc;
    enumerated_adapter->GetDesc1(&adapter_desc);
    std::string description_narrow(adapter_desc.Description,
                                  adapter_desc.Description + wcslen(adapter_desc.Description));
    REXGPU_INFO("D3D11Provider: adapter {}: \"{}\" (vendor 0x{:04X}, device 0x{:04X}, {} MB)",
                adapter_index, description_narrow, adapter_desc.VendorId, adapter_desc.DeviceId,
                adapter_desc.DedicatedVideoMemory / (1024 * 1024));
    if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
      continue;
    }
    if (!first_hardware_adapter) {
      first_hardware_adapter = enumerated_adapter;
    }
    if (!matched_adapter && DescriptionMatches(adapter_desc.Description)) {
      matched_adapter = enumerated_adapter;
    }
    enumerated_adapter.Reset();
  }

  if (!matched_adapter && !REXCVAR_GET(d3d11_adapter).empty()) {
    REXLOG_ERROR(
        "D3D11Provider: no adapter matches d3d11_adapter=\"{}\" - check the description "
        "substring (the full adapter list is logged above)",
        REXCVAR_GET(d3d11_adapter));
    return false;
  }
  if (!matched_adapter) {
    matched_adapter = first_hardware_adapter;
  }
  if (!matched_adapter) {
    REXLOG_ERROR("D3D11Provider: no hardware Direct3D 11 adapter found");
    return false;
  }

  DXGI_ADAPTER_DESC1 selected_desc;
  matched_adapter->GetDesc1(&selected_desc);
  adapter_description_ = selected_desc.Description;
  adapter_luid_ = selected_desc.AdapterLuid;
  adapter_vendor_id_ = GpuVendorID(selected_desc.VendorId);
  {
    std::string description_narrow(adapter_description_.begin(), adapter_description_.end());
    REXGPU_INFO("D3D11Provider: using adapter \"{}\" (vendor 0x{:04X}, device 0x{:04X}, {} MB)",
                description_narrow, selected_desc.VendorId, selected_desc.DeviceId,
                selected_desc.DedicatedVideoMemory / (1024 * 1024));
  }

  // The port targets feature level 11_0 hardware - request exactly that so
  // the device cannot silently come up at a lower level.
  const D3D_FEATURE_LEVEL requested_feature_level = D3D_FEATURE_LEVEL_11_0;
  D3D_FEATURE_LEVEL created_feature_level = D3D_FEATURE_LEVEL_11_0;
  UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate_context;
  HRESULT create_result = D3D11CreateDevice(
      matched_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, create_flags,
      &requested_feature_level, 1, D3D11_SDK_VERSION, &device, &created_feature_level,
      &immediate_context);
  if (FAILED(create_result)) {
    REXLOG_ERROR("D3D11Provider: D3D11CreateDevice failed with HRESULT 0x{:08X}",
                 uint32_t(create_result));
    return false;
  }
  if (created_feature_level != D3D_FEATURE_LEVEL_11_0) {
    REXLOG_ERROR("D3D11Provider: unexpected feature level 0x{:04X}",
                 uint32_t(created_feature_level));
    return false;
  }
  REXGPU_INFO("D3D11Provider: device created at feature level 11_0");

  adapter_ = matched_adapter.Detach();
  device_ = device.Detach();
  immediate_context_ = immediate_context.Detach();
  return true;
}

std::unique_ptr<Presenter> D3D11Provider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return D3D11Presenter::Create(std::move(host_gpu_loss_callback), *this);
}

std::unique_ptr<ImmediateDrawer> D3D11Provider::CreateImmediateDrawer() {
  return D3D11ImmediateDrawer::Create(*this);
}

}  // namespace rex::ui::d3d11
