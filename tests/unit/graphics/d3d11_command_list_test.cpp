/**
 * @file        graphics/d3d11_command_list_test.cpp
 * @brief       Device-free unit tests for the D3D11 command tape (Phase 3 M1/M2)
 *
 *              Exercises the packet stream, byte arena, COM reference lifetime,
 *              replay loop and resource-state map without creating a D3D11
 *              device. The immediate-context sink is compiled into the backend
 *              but cannot be executed here; replay is verified against a
 *              recording sink instead.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include <rex/graphics/d3d11/command_list.h>
#include <rex/graphics/d3d11/resource_state_tracker.h>

namespace {

using rex::graphics::d3d11::D3D11ArenaSpan;
using rex::graphics::d3d11::D3D11BarrierPacket;
using rex::graphics::d3d11::D3D11ClearRenderTargetPacket;
using rex::graphics::d3d11::D3D11Command;
using rex::graphics::d3d11::D3D11CommandList;
using rex::graphics::d3d11::D3D11CommandSink;
using rex::graphics::d3d11::D3D11DrawPacket;
using rex::graphics::d3d11::D3D11ResourceState;
using rex::graphics::d3d11::D3D11ResourceStateTracker;
using rex::graphics::d3d11::D3D11UavBarrierPacket;
using rex::graphics::d3d11::D3D11UnbindGraphicsSrvPacket;
using rex::graphics::d3d11::D3D11UnbindRenderTargetsPacket;
using rex::graphics::d3d11::D3D11UploadBufferPacket;

// Counts every AddRef/Release so the tests can prove packets own their COM
// references and Reset drops them.
class RefCountedUnknown final : public IUnknown {
 public:
  explicit RefCountedUnknown(std::atomic<uint32_t>& references) : references_(references) {
    ++references_;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown)) {
      *object = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override { return --references_; }

 private:
  std::atomic<uint32_t>& references_;
};

struct SeenCall {
  bool is_clear = false;
  bool is_upload = false;
  bool is_unbind = false;
  bool is_draw = false;
  std::vector<uint8_t> upload_bytes;
  std::vector<uint8_t> draw_system_constants;
  std::vector<uint8_t> draw_float_constants;
  uint32_t draw_vertex_count = 0;
  bool draw_indexed = false;
  D3D11ResourceState before = D3D11ResourceState::kCommon;
  D3D11ResourceState after = D3D11ResourceState::kCommon;
};

// Records what the tape handed it, resolving arena spans exactly as the
// immediate sink does.
class RecordingSink final : public D3D11CommandSink {
 public:
  void Execute(const D3D11Command& command, const D3D11CommandList& tape) override {
    SeenCall seen;
    seen.is_clear = std::holds_alternative<D3D11ClearRenderTargetPacket>(command);
    seen.is_upload = std::holds_alternative<D3D11UploadBufferPacket>(command);
    seen.is_unbind = std::holds_alternative<D3D11UnbindGraphicsSrvPacket>(command);
    if (const auto* upload = std::get_if<D3D11UploadBufferPacket>(&command)) {
      const uint8_t* data = tape.Data(upload->data);
      if (data) {
        seen.upload_bytes.assign(data, data + upload->data.size);
      }
    } else if (const auto* draw = std::get_if<D3D11DrawPacket>(&command)) {
      seen.is_draw = true;
      seen.draw_vertex_count = draw->vertex_count;
      seen.draw_indexed = draw->indexed;
      for (size_t i = 0; i < draw->constant_buffers.size(); ++i) {
        const D3D11ArenaSpan span = draw->constant_buffers[i].data;
        const uint8_t* data = tape.Data(span);
        if (!data) {
          continue;
        }
        if (i == 0) {
          seen.draw_system_constants.assign(data, data + span.size);
        } else if (i == 1) {
          seen.draw_float_constants.assign(data, data + span.size);
        }
      }
    } else if (const auto* barrier = std::get_if<D3D11BarrierPacket>(&command)) {
      seen.before = barrier->before;
      seen.after = barrier->after;
    }
    calls.push_back(std::move(seen));
  }

  std::vector<SeenCall> calls;
};

TEST_CASE("Command tape replays packets in order with arena payloads", "[d3d11][command-list]") {
  D3D11CommandList tape(64, 4096);

  auto* clear = tape.Append<D3D11ClearRenderTargetPacket>();
  REQUIRE(clear != nullptr);
  clear->color = {0.25f, 0.5f, 0.75f, 1.0f};

  auto* upload = tape.Append<D3D11UploadBufferPacket>();
  REQUIRE(upload != nullptr);
  const uint8_t payload[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
  REQUIRE(tape.AppendBytes(payload, sizeof(payload), upload->data));
  REQUIRE(upload->data.size == sizeof(payload));
  upload->destination_offset = 32;

  REQUIRE(tape.command_count() == 2);
  REQUIRE(tape.arena_bytes() >= sizeof(payload));
  REQUIRE_FALSE(tape.overflowed());

  RecordingSink sink;
  tape.Replay(sink);
  REQUIRE(sink.calls.size() == 2);
  REQUIRE(sink.calls[0].is_clear);
  REQUIRE(sink.calls[1].is_upload);
  REQUIRE(sink.calls[1].upload_bytes.size() == sizeof(payload));
  REQUIRE(std::memcmp(sink.calls[1].upload_bytes.data(), payload, sizeof(payload)) == 0);
}

TEST_CASE("Upload packets copy bytes into the arena and fail atomically",
          "[d3d11][command-list]") {
  D3D11CommandList tape(8, 40);
  const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                             11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
  std::atomic<uint32_t> references = 0;
  Microsoft::WRL::ComPtr<RefCountedUnknown> buffer;
  buffer.Attach(new RefCountedUnknown(references));
  // Only stored and reference-counted, never called through: the packet holds
  // it as an ID3D11Buffer the same way a real one would be held.
  ID3D11Buffer* fake_buffer = reinterpret_cast<ID3D11Buffer*>(buffer.Get());

  REQUIRE_FALSE(tape.AppendUploadBufferPacket(nullptr, 0, payload, sizeof(payload)));
  REQUIRE_FALSE(tape.AppendUploadBufferPacket(fake_buffer, 0, nullptr, sizeof(payload)));
  REQUIRE_FALSE(tape.AppendUploadBufferPacket(fake_buffer, 0, payload, 0));
  REQUIRE(tape.command_count() == 0);

  REQUIRE(tape.AppendUploadBufferPacket(fake_buffer, 32, payload, sizeof(payload)));
  REQUIRE(tape.command_count() == 1);
  const auto* packet = std::get_if<D3D11UploadBufferPacket>(&tape.commands()[0]);
  REQUIRE(packet != nullptr);
  REQUIRE(packet->destination.Get() == fake_buffer);
  REQUIRE(packet->destination_offset == 32);
  REQUIRE(packet->data.size == sizeof(payload));
  const uint8_t* data = tape.Data(packet->data);
  REQUIRE(data != nullptr);
  REQUIRE(std::memcmp(data, payload, sizeof(payload)) == 0);

  // The arena cannot hold a second 20-byte payload: the append must leave no
  // packet behind, so the tape stays a description of what actually runs.
  REQUIRE_FALSE(tape.AppendUploadBufferPacket(fake_buffer, 64, payload, sizeof(payload)));
  REQUIRE(tape.command_count() == 1);
  REQUIRE(tape.overflowed());
  REQUIRE(references == 2);  // the local ComPtr and the tape's packet

  tape.Reset();
  REQUIRE(tape.command_count() == 0);
  REQUIRE_FALSE(tape.overflowed());
  REQUIRE(references == 1);
}

TEST_CASE("Draw packets carry record-time constant blobs and draw arguments",
          "[d3d11][command-list]") {
  D3D11CommandList tape(64, 4096);
  const uint8_t system_constants[32] = {1, 2, 3, 4, 5, 6, 7, 8};
  const uint8_t float_constants[16] = {0x10, 0x20, 0x30, 0x40};

  auto* draw = tape.Append<D3D11DrawPacket>();
  REQUIRE(draw != nullptr);
  REQUIRE(tape.AppendBytes(system_constants, sizeof(system_constants),
                           draw->constant_buffers[0].data));
  REQUIRE(tape.AppendBytes(float_constants, sizeof(float_constants),
                           draw->constant_buffers[1].data));
  draw->vertex_count = 3;
  draw->indexed = false;
  draw->topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  draw->vs_srv_count = 1;  // the null mirror SRV still counts as a slot
  REQUIRE_FALSE(draw->constant_buffers[1].data.empty());
  REQUIRE(draw->constant_buffers[2].data.empty());

  RecordingSink sink;
  tape.Replay(sink);
  REQUIRE(sink.calls.size() == 1);
  REQUIRE(sink.calls[0].is_draw);
  REQUIRE(sink.calls[0].draw_vertex_count == 3);
  REQUIRE_FALSE(sink.calls[0].draw_indexed);
  REQUIRE(sink.calls[0].draw_system_constants.size() == sizeof(system_constants));
  REQUIRE(std::memcmp(sink.calls[0].draw_system_constants.data(), system_constants,
                      sizeof(system_constants)) == 0);
  REQUIRE(sink.calls[0].draw_float_constants.size() == sizeof(float_constants));
  REQUIRE(std::memcmp(sink.calls[0].draw_float_constants.data(), float_constants,
                      sizeof(float_constants)) == 0);
}

TEST_CASE("Truncate drops packets and their COM references", "[d3d11][command-list]") {
  D3D11CommandList tape(8, 1024);
  std::atomic<uint32_t> references = 0;
  Microsoft::WRL::ComPtr<RefCountedUnknown> resource;
  resource.Attach(new RefCountedUnknown(references));

  auto* first = tape.Append<D3D11BarrierPacket>();
  REQUIRE(first != nullptr);
  first->resource = resource.Get();
  REQUIRE(references == 2);
  REQUIRE(tape.Append<D3D11UnbindGraphicsSrvPacket>() != nullptr);
  REQUIRE(tape.Append<D3D11UnbindRenderTargetsPacket>() != nullptr);
  REQUIRE(tape.command_count() == 3);

  tape.Truncate(1);
  REQUIRE(tape.command_count() == 1);
  REQUIRE(references == 2);
  // Truncating past the end is a no-op.
  tape.Truncate(4);
  REQUIRE(tape.command_count() == 1);

  tape.Reset();
  REQUIRE(references == 1);
}

TEST_CASE("Unbind and upload packets replay in recorded order", "[d3d11][command-list]") {
  D3D11CommandList tape(8, 256);
  const uint8_t payload[] = {0x5A, 0xA5};
  std::atomic<uint32_t> references = 0;
  Microsoft::WRL::ComPtr<RefCountedUnknown> buffer;
  buffer.Attach(new RefCountedUnknown(references));
  ID3D11Buffer* fake_buffer = reinterpret_cast<ID3D11Buffer*>(buffer.Get());

  REQUIRE(tape.Append<D3D11UnbindGraphicsSrvPacket>() != nullptr);
  REQUIRE(tape.AppendUploadBufferPacket(fake_buffer, 16, payload, sizeof(payload)));
  REQUIRE(tape.Append<D3D11UnbindRenderTargetsPacket>() != nullptr);
  REQUIRE(std::holds_alternative<D3D11UnbindRenderTargetsPacket>(tape.commands()[2]));

  RecordingSink sink;
  tape.Replay(sink);
  REQUIRE(sink.calls.size() == 3);
  REQUIRE(sink.calls[0].is_unbind);
  REQUIRE(sink.calls[1].is_upload);
  REQUIRE(sink.calls[1].upload_bytes == std::vector<uint8_t>(payload, payload + sizeof(payload)));
  REQUIRE(sink.calls[2].is_unbind == false);
  REQUIRE(sink.calls[2].is_clear == false);
}

TEST_CASE("Arena aligns allocations and rejects overflow", "[d3d11][command-list]") {
  D3D11CommandList tape(8, 40);
  const uint8_t five[] = {1, 2, 3, 4, 5};

  D3D11ArenaSpan first;
  REQUIRE(tape.AppendBytes(five, sizeof(five), first));
  REQUIRE(first.offset == 0);
  REQUIRE(first.size == 5);
  REQUIRE(tape.Data(first) != nullptr);
  REQUIRE(std::memcmp(tape.Data(first), five, sizeof(five)) == 0);

  D3D11ArenaSpan second;
  REQUIRE(tape.AppendBytes(five, sizeof(five), second));
  REQUIRE(second.offset == D3D11CommandList::kArenaAlignment);

  D3D11ArenaSpan third;
  REQUIRE(tape.AppendBytes(five, sizeof(five), third));
  REQUIRE(third.offset == 2 * D3D11CommandList::kArenaAlignment);

  // 8 bytes at the next aligned offset would pass the 40-byte cap.
  D3D11ArenaSpan rejected;
  REQUIRE_FALSE(tape.AppendBytes(five, 8, rejected));
  REQUIRE(rejected.empty());
  REQUIRE(tape.overflowed());
  REQUIRE(tape.overflow_count() == 1);

  D3D11ArenaSpan rejected_again;
  REQUIRE_FALSE(tape.AppendBytes(five, 8, rejected_again, "TestPacket"));
  REQUIRE(tape.overflow_count() == 1);

  // Out-of-range spans are rejected when resolving, not just when appending.
  REQUIRE(tape.Data(D3D11ArenaSpan{100, 4}) == nullptr);
  REQUIRE(tape.Data(D3D11ArenaSpan{}) == nullptr);
  REQUIRE(tape.Data(D3D11ArenaSpan{36, 8}) == nullptr);

  tape.Reset();
  REQUIRE_FALSE(tape.overflowed());
  REQUIRE(tape.overflow_count() == 1);
  REQUIRE(tape.empty());
  REQUIRE(tape.arena_bytes() == 0);
  D3D11ArenaSpan rejected_after_reset;
  REQUIRE_FALSE(tape.AppendBytes(five, 41, rejected_after_reset, "TestPacket"));
  REQUIRE(tape.overflow_count() == 2);

  tape.Reset();
  D3D11ArenaSpan after_reset;
  REQUIRE(tape.AppendBytes(five, sizeof(five), after_reset));
  REQUIRE(after_reset.offset == 0);
}

TEST_CASE("Packet cap stops recording and Reset clears it", "[d3d11][command-list]") {
  D3D11CommandList tape(2, 1024);

  REQUIRE(tape.Append<D3D11ClearRenderTargetPacket>() != nullptr);
  REQUIRE(tape.Append<D3D11ClearRenderTargetPacket>() != nullptr);
  REQUIRE(tape.Append<D3D11ClearRenderTargetPacket>() == nullptr);
  REQUIRE(tape.overflowed());
  REQUIRE(tape.overflow_count() == 1);
  REQUIRE(tape.command_count() == 2);

  tape.Reset();
  REQUIRE_FALSE(tape.overflowed());
  REQUIRE(tape.overflow_count() == 1);
  REQUIRE(tape.command_count() == 0);
  REQUIRE(tape.Append<D3D11ClearRenderTargetPacket>() != nullptr);
}

TEST_CASE("Packets hold COM references until Reset", "[d3d11][command-list]") {
  std::atomic<uint32_t> references = 0;
  {
    Microsoft::WRL::ComPtr<RefCountedUnknown> object;
    object.Attach(new RefCountedUnknown(references));  // 1
    REQUIRE(references == 1);
    {
      D3D11CommandList tape(8, 1024);
      auto* packet = tape.Append<D3D11BarrierPacket>();
      REQUIRE(packet != nullptr);
      packet->resource = object.Get();
      REQUIRE(references == 2);
      tape.Reset();
      REQUIRE(references == 1);
      REQUIRE(tape.empty());
    }
    REQUIRE(references == 1);
  }
  REQUIRE(references == 0);
}

TEST_CASE("Tracker coalesces state transitions and records barriers", "[d3d11][command-list]") {
  D3D11CommandList tape(32, 1024);
  D3D11ResourceStateTracker tracker;
  std::atomic<uint32_t> references = 0;
  Microsoft::WRL::ComPtr<RefCountedUnknown> resource;
  resource.Attach(new RefCountedUnknown(references));
  IUnknown* raw = resource.Get();

  REQUIRE(tracker.Transition(tape, raw, D3D11ResourceState::kRenderTarget));
  REQUIRE(tape.command_count() == 1);
  REQUIRE(tracker.State(raw) == D3D11ResourceState::kRenderTarget);
  REQUIRE(tracker.IsTracked(raw));
  REQUIRE(references == 3);  // local + tracker + barrier packet

  // Same state: no packet.
  REQUIRE_FALSE(tracker.Transition(tape, raw, D3D11ResourceState::kRenderTarget));
  REQUIRE(tape.command_count() == 1);

  // A new state appends a barrier carrying both sides.
  REQUIRE(tracker.Transition(tape, raw, D3D11ResourceState::kShaderResource));
  REQUIRE(tape.command_count() == 2);
  const auto* barrier = std::get_if<D3D11BarrierPacket>(&tape.commands()[1]);
  REQUIRE(barrier != nullptr);
  REQUIRE(barrier->before == D3D11ResourceState::kRenderTarget);
  REQUIRE(barrier->after == D3D11ResourceState::kShaderResource);
  REQUIRE(barrier->resource.Get() == raw);

  // A combined read state is one state, and the barrier is recorded once.
  REQUIRE(tracker.Transition(
      tape, raw, D3D11ResourceState::kShaderResource | D3D11ResourceState::kVertexBuffer));
  REQUIRE(tracker.State(raw) ==
          (D3D11ResourceState::kShaderResource | D3D11ResourceState::kVertexBuffer));
  REQUIRE(tracker.transition_count(raw) == 3);

  tracker.Reset();
  REQUIRE_FALSE(tracker.IsTracked(raw));
  REQUIRE(tracker.tracked_count() == 0);
  REQUIRE(references == 4);  // local + three barrier packets on the tape
  tape.Reset();
  REQUIRE(references == 1);
}

TEST_CASE("Tracker adopts untracked resources and tracks UAV barriers", "[d3d11][command-list]") {
  D3D11CommandList tape(32, 1024);
  D3D11ResourceStateTracker tracker;
  std::atomic<uint32_t> references = 0;
  Microsoft::WRL::ComPtr<RefCountedUnknown> resource;
  resource.Attach(new RefCountedUnknown(references));
  IUnknown* raw = resource.Get();

  // A known initial state is registered without a barrier.
  REQUIRE(tracker.Track(raw, D3D11ResourceState::kCopySource));
  REQUIRE(tape.command_count() == 0);
  REQUIRE(tracker.State(raw) == D3D11ResourceState::kCopySource);

  // Untracked adoption still puts the first state on the tape.
  std::atomic<uint32_t> other_references = 0;
  Microsoft::WRL::ComPtr<RefCountedUnknown> other;
  other.Attach(new RefCountedUnknown(other_references));
  REQUIRE(tracker.Transition(tape, other.Get(), D3D11ResourceState::kUnorderedAccess));
  REQUIRE(tape.command_count() == 1);
  REQUIRE(tracker.State(other.Get()) == D3D11ResourceState::kUnorderedAccess);

  REQUIRE(tracker.TransitionUav(tape, raw));
  REQUIRE(tape.command_count() == 2);
  REQUIRE(std::holds_alternative<D3D11UavBarrierPacket>(tape.commands()[1]));
  REQUIRE(tracker.recorded_barrier_count() == 1);
  REQUIRE(tracker.recorded_uav_barrier_count() == 1);

  REQUIRE(tracker.Transition(tape, nullptr, D3D11ResourceState::kCommon) == false);
  REQUIRE(tracker.TransitionUav(tape, nullptr) == false);
  REQUIRE(tracker.Track(nullptr, D3D11ResourceState::kCommon) == false);

  tracker.Reset();
  REQUIRE(tracker.recorded_barrier_count() == 0);
  REQUIRE(tracker.recorded_uav_barrier_count() == 0);
  REQUIRE(references == 2);        // local + the tape's UAV barrier packet
  REQUIRE(other_references == 2);  // local + the tape's transition packet
  tape.Reset();
  REQUIRE(references == 1);
  REQUIRE(other_references == 1);
}

TEST_CASE("Tracker stops recording when the tape is full", "[d3d11][command-list]") {
  D3D11CommandList tape(1, 1024);
  D3D11ResourceStateTracker tracker;
  std::atomic<uint32_t> references = 0;
  Microsoft::WRL::ComPtr<RefCountedUnknown> resource;
  resource.Attach(new RefCountedUnknown(references));
  IUnknown* raw = resource.Get();

  REQUIRE(tracker.Transition(tape, raw, D3D11ResourceState::kRenderTarget));
  REQUIRE(tape.overflowed() == false);
  // The tape cap is reached; the tracker must not claim the state changed.
  D3D11CommandList full(1, 1024);
  REQUIRE(full.Append<D3D11ClearRenderTargetPacket>() != nullptr);
  REQUIRE_FALSE(tracker.Transition(full, raw, D3D11ResourceState::kShaderResource));
  REQUIRE(full.overflowed());
  REQUIRE(tracker.State(raw) == D3D11ResourceState::kRenderTarget);
}

}  // namespace
