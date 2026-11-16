/**
 * @file        graphics/d3d11_shared_memory_upload_test.cpp
 * @brief       Unit tests for the D3D11 shared memory upload harness
 *
 *              Exercises the real SharedMemory request pipeline (RequestRange
 *              and RequestRanges down to UploadRanges) against a recording
 *              upload sink, so the guest -> host mirror upload path is
 *              verified without a D3D11 device.
 *
 *              Each test case seeds and requests its own guest physical
 *              region: uploaded pages are armed against CPU writes by the
 *              shared memory, and the process-wide test Memory keeps that
 *              protection after the test's shared memory is gone, so tests
 *              never touch a region another test has used.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/d3d11/shared_memory_upload.h>

#include "test_memory.h"

namespace {

using rex::graphics::SharedMemory;
using rex::graphics::d3d11::D3D11SharedMemory;
using rex::graphics::d3d11::D3D11SharedMemoryUploadSink;
using rex::graphics::d3d11::kSharedMemoryUploadChunkBytes;

constexpr uint32_t kPageBytes = 1 << 12;

// Records the host mirror traffic and mirrors the uploaded bytes, so the
// tests can assert on the exact upload calls and their content.
class RecordingUploadSink : public D3D11SharedMemoryUploadSink {
 public:
  struct UploadCall {
    uint32_t offset_bytes;
    uint32_t length_bytes;
  };

  bool CreateHostBuffer(uint32_t size_bytes) override {
    ++create_calls;
    created_size_bytes = size_bytes;
    buffer_alive = true;
    return create_succeeds;
  }

  bool UploadToHostBuffer(uint32_t offset_bytes, const void* source,
                          uint32_t length_bytes) override {
    upload_calls.push_back({offset_bytes, length_bytes});
    if (on_upload) {
      on_upload(offset_bytes, length_bytes);
    }
    if (upload_calls.size() == fail_on_upload_call) {
      return false;
    }
    std::memcpy(mirror.data() + offset_bytes, source, length_bytes);
    return true;
  }

  void DestroyHostBuffer() override {
    ++destroy_calls;
    buffer_alive = false;
  }

  std::vector<uint8_t> mirror = std::vector<uint8_t>(SharedMemory::kBufferSize);
  std::vector<UploadCall> upload_calls;
  std::function<void(uint32_t, uint32_t)> on_upload;
  uint32_t create_calls = 0;
  uint32_t destroy_calls = 0;
  uint32_t created_size_bytes = 0;
  size_t fail_on_upload_call = 0;
  bool create_succeeds = true;
  bool buffer_alive = false;
};

class UploadHarness {
 public:
  explicit UploadHarness(uint32_t physical_base)
      : memory_(rex::testing::GetTestMemory()), physical_base_(physical_base) {
    auto sink = std::make_unique<RecordingUploadSink>();
    sink_ = sink.get();
    shared_memory_ =
        std::make_unique<D3D11SharedMemory>(memory_, std::move(sink));
  }

  bool Initialize() { return shared_memory_->Initialize(); }

  uint32_t physical(uint32_t offset) const { return physical_base_ + offset; }

  // Writes guest physical memory. Only safe for pages the test has not
  // uploaded yet: uploads arm the pages against CPU writes.
  void SeedGuestBytes(uint32_t offset, uint32_t length) {
    uint8_t* guest = memory_.TranslatePhysical(physical(offset));
    for (uint32_t i = 0; i < length; ++i) {
      guest[i] = uint8_t(i * 31 + 7);
    }
  }

  bool MirrorMatchesGuest(uint32_t offset, uint32_t length) {
    const uint8_t* guest = memory_.TranslatePhysical(physical(offset));
    return std::memcmp(sink_->mirror.data() + physical(offset), guest,
                       length) == 0;
  }

  std::pair<uint32_t, uint32_t> CountValid(uint32_t offset, uint32_t length) {
    return shared_memory_->CountValidPages(physical(offset), length);
  }

  D3D11SharedMemory& shared_memory() { return *shared_memory_; }
  RecordingUploadSink& sink() { return *sink_; }

 private:
  rex::memory::Memory& memory_;
  uint32_t physical_base_;
  RecordingUploadSink* sink_ = nullptr;
  std::unique_ptr<D3D11SharedMemory> shared_memory_;
};

}  // namespace

TEST_CASE("d3d11 shared memory creates and destroys the host mirror") {
  UploadHarness harness(0x10000000);
  REQUIRE(harness.Initialize());
  CHECK(harness.sink().create_calls == 1);
  CHECK(harness.sink().created_size_bytes == SharedMemory::kBufferSize);
  CHECK(harness.sink().buffer_alive);

  harness.shared_memory().Shutdown();
  CHECK(harness.sink().destroy_calls == 1);
  CHECK_FALSE(harness.sink().buffer_alive);
}

TEST_CASE("d3d11 shared memory fails initialization without a host buffer") {
  auto sink = std::make_unique<RecordingUploadSink>();
  RecordingUploadSink* sink_ptr = sink.get();
  sink_ptr->create_succeeds = false;
  D3D11SharedMemory shared_memory(rex::testing::GetTestMemory(), std::move(sink));
  CHECK_FALSE(shared_memory.Initialize());
  CHECK(sink_ptr->destroy_calls == 1);
}

TEST_CASE("d3d11 shared memory uploads requested guest pages into the mirror") {
  UploadHarness harness(0x11000000);
  REQUIRE(harness.Initialize());
  harness.SeedGuestBytes(0, 0x6000);

  // Sub-page request bounds: page 0 to page 5 (0x123 + 0x5555 = 0x5678).
  CHECK(harness.shared_memory().RequestRange(harness.physical(0x123), 0x5555));

  REQUIRE(harness.sink().upload_calls.size() == 1);
  CHECK(harness.sink().upload_calls[0].offset_bytes == harness.physical(0));
  CHECK(harness.sink().upload_calls[0].length_bytes == 0x6000);
  CHECK(harness.MirrorMatchesGuest(0, 0x6000));
  CHECK(harness.CountValid(0, 0x6000) == std::make_pair(uint32_t(6), uint32_t(0)));
  CHECK(harness.shared_memory().upload_request_count() == 1);
}

TEST_CASE("d3d11 shared memory uploads each valid page only once") {
  UploadHarness harness(0x12000000);
  REQUIRE(harness.Initialize());
  harness.SeedGuestBytes(0, 0x2000);

  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x2000));
  REQUIRE(harness.sink().upload_calls.size() == 1);

  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x2000));
  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x2000));
  CHECK(harness.sink().upload_calls.size() == 1);
  CHECK(harness.shared_memory().upload_request_count() == 1);
}

TEST_CASE("d3d11 shared memory re-uploads only CPU-invalidated pages") {
  UploadHarness harness(0x13000000);
  REQUIRE(harness.Initialize());
  harness.SeedGuestBytes(0, 0x4000);

  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x4000));
  REQUIRE(harness.sink().upload_calls.size() == 1);

  // A CPU write to page 1 only, as the guest memory write fault reports it.
  harness.shared_memory().MemoryInvalidationCallback(harness.physical(0x1000),
                                                     kPageBytes, true);
  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x4000));

  REQUIRE(harness.sink().upload_calls.size() == 2);
  CHECK(harness.sink().upload_calls[1].offset_bytes == harness.physical(0x1000));
  CHECK(harness.sink().upload_calls[1].length_bytes == kPageBytes);
  CHECK(harness.MirrorMatchesGuest(0, 0x4000));
  CHECK(harness.CountValid(0, 0x4000) == std::make_pair(uint32_t(4), uint32_t(0)));
}

TEST_CASE("d3d11 shared memory keeps GPU-written pages out of CPU uploads") {
  UploadHarness harness(0x14000000);
  REQUIRE(harness.Initialize());

  harness.shared_memory().RangeWrittenByGpu(harness.physical(0), 0x2000);
  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x2000));

  CHECK(harness.sink().upload_calls.empty());
  CHECK(harness.CountValid(0, 0x2000) == std::make_pair(uint32_t(2), uint32_t(2)));
}

TEST_CASE("d3d11 shared memory coalesces unordered requests into page runs") {
  UploadHarness harness(0x15000000);
  REQUIRE(harness.Initialize());
  harness.SeedGuestBytes(0, 0x3000);

  // Unsorted and byte-disjoint: the pages end up as one ascending,
  // contiguous upload run.
  std::pair<uint32_t, uint32_t> ranges[3] = {
      {harness.physical(0x2000), 0x100},
      {harness.physical(0x0100), 0x200},
      {harness.physical(0x1000), 0x200},
  };
  CHECK(harness.shared_memory().RequestRanges(ranges, 3));

  REQUIRE(harness.sink().upload_calls.size() == 1);
  CHECK(harness.sink().upload_calls[0].offset_bytes == harness.physical(0));
  CHECK(harness.sink().upload_calls[0].length_bytes == 0x3000);
  CHECK(harness.MirrorMatchesGuest(0, 0x3000));
}

TEST_CASE("d3d11 shared memory merges overlapping request ranges") {
  UploadHarness harness(0x15100000);
  REQUIRE(harness.Initialize());
  harness.SeedGuestBytes(0, 0x1000);

  // Overlapping byte ranges merge before the upload scan, so page 0 is
  // requested and uploaded once.
  std::pair<uint32_t, uint32_t> ranges[2] = {
      {harness.physical(0x100), 0x300},
      {harness.physical(0x200), 0x300},
  };
  CHECK(harness.shared_memory().RequestRanges(ranges, 2));

  REQUIRE(harness.sink().upload_calls.size() == 1);
  CHECK(harness.sink().upload_calls[0].offset_bytes == harness.physical(0));
  CHECK(harness.sink().upload_calls[0].length_bytes == 0x1000);
  CHECK(harness.MirrorMatchesGuest(0, 0x1000));
}

TEST_CASE("d3d11 shared memory uploads pages shared by merged ranges per run") {
  UploadHarness harness(0x15200000);
  REQUIRE(harness.Initialize());
  harness.SeedGuestBytes(0, 0x3000);

  // When separate merged ranges share a page (page 1 here), the upload runs
  // are still ascending but overlap on it - the base request pipeline
  // coalesces only contiguously queued runs, the same behavior the Direct3D
  // 12 path has. The shared page is copied twice and the mirror stays exact.
  std::pair<uint32_t, uint32_t> ranges[3] = {
      {harness.physical(0x1C00), 0x800},
      {harness.physical(0x0100), 0x200},
      {harness.physical(0x1000), 0x200},
  };
  CHECK(harness.shared_memory().RequestRanges(ranges, 3));

  REQUIRE(harness.sink().upload_calls.size() == 2);
  CHECK(harness.sink().upload_calls[0].offset_bytes == harness.physical(0));
  CHECK(harness.sink().upload_calls[0].length_bytes == 0x2000);
  CHECK(harness.sink().upload_calls[1].offset_bytes == harness.physical(0x1000));
  CHECK(harness.sink().upload_calls[1].length_bytes == 0x2000);
  CHECK(harness.MirrorMatchesGuest(0, 0x3000));
}

TEST_CASE("d3d11 shared memory chunks large uploads into bounded copies") {
  UploadHarness harness(0x16000000);
  REQUIRE(harness.Initialize());
  harness.SeedGuestBytes(0, 0x500000);

  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x500000));

  REQUIRE(harness.sink().upload_calls.size() == 2);
  CHECK(harness.sink().upload_calls[0].offset_bytes == harness.physical(0));
  CHECK(harness.sink().upload_calls[0].length_bytes == kSharedMemoryUploadChunkBytes);
  CHECK(harness.sink().upload_calls[1].offset_bytes ==
        harness.physical(kSharedMemoryUploadChunkBytes));
  CHECK(harness.sink().upload_calls[1].length_bytes == 0x500000 - kSharedMemoryUploadChunkBytes);
  for (const RecordingUploadSink::UploadCall& call : harness.sink().upload_calls) {
    CHECK(call.length_bytes <= kSharedMemoryUploadChunkBytes);
    CHECK(call.offset_bytes % kPageBytes == 0);
    CHECK(call.length_bytes % kPageBytes == 0);
  }
  CHECK(harness.MirrorMatchesGuest(0, 0x500000));
}

TEST_CASE("d3d11 shared memory marks pages valid before the copy") {
  UploadHarness harness(0x17000000);
  REQUIRE(harness.Initialize());
  harness.SeedGuestBytes(0, 0x2000);

  // Observed from inside the copy itself: if validity were marked after it, a
  // CPU write during the copy would be lost, so the pages must already read
  // as valid here.
  std::vector<std::pair<uint32_t, uint32_t>> observed;
  harness.sink().on_upload = [&](uint32_t offset_bytes, uint32_t length_bytes) {
    observed.push_back(harness.shared_memory().CountValidPages(
        offset_bytes, length_bytes));
  };
  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x2000));

  REQUIRE(observed.size() == 1);
  CHECK(observed[0] == std::make_pair(uint32_t(0x2000 / kPageBytes), uint32_t(0)));
}

TEST_CASE("d3d11 shared memory retries a range after a failed upload") {
  UploadHarness harness(0x18000000);
  REQUIRE(harness.Initialize());
  harness.SeedGuestBytes(0, 0x500000);

  harness.sink().fail_on_upload_call = 2;
  CHECK_FALSE(harness.shared_memory().RequestRange(harness.physical(0), 0x500000));
  // The request stops at the failure instead of continuing into the mirror.
  REQUIRE(harness.sink().upload_calls.size() == 2);

  harness.sink().fail_on_upload_call = 0;
  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x500000));

  // The failure dropped the CPU-uploaded validity again, so both chunks are
  // retried and the mirror ends up complete.
  REQUIRE(harness.sink().upload_calls.size() == 4);
  CHECK(harness.sink().upload_calls[2].offset_bytes ==
        harness.sink().upload_calls[0].offset_bytes);
  CHECK(harness.sink().upload_calls[2].length_bytes ==
        harness.sink().upload_calls[0].length_bytes);
  CHECK(harness.sink().upload_calls[3].offset_bytes ==
        harness.sink().upload_calls[1].offset_bytes);
  CHECK(harness.sink().upload_calls[3].length_bytes ==
        harness.sink().upload_calls[1].length_bytes);
  CHECK(harness.MirrorMatchesGuest(0, 0x500000));
  CHECK(harness.CountValid(0, 0x500000) ==
        std::make_pair(uint32_t(0x500000 / kPageBytes), uint32_t(0)));
}

TEST_CASE("d3d11 shared memory accounts uploads by kind and frame") {
  UploadHarness harness(0x19000000);
  REQUIRE(harness.Initialize());
  harness.SeedGuestBytes(0, 0x2000);

  SharedMemory::UploadKindScope kind_scope(harness.shared_memory(),
                                           SharedMemory::UploadKind::kTexture);
  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x2000));
  const SharedMemory::UploadStats& stats =
      harness.shared_memory().upload_stats(SharedMemory::UploadKind::kTexture);
  CHECK(stats.requests == 1);
  CHECK(stats.ranges == 1);
  CHECK(stats.bytes == 0x2000);
  CHECK(stats.repeat_bytes == 0);

  // Page 0 re-uploaded inside the same frame is a repeat.
  harness.shared_memory().MemoryInvalidationCallback(harness.physical(0),
                                                     kPageBytes, true);
  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x2000));
  CHECK(stats.requests == 2);
  CHECK(stats.ranges == 2);
  CHECK(stats.bytes == 0x3000);
  CHECK(stats.repeat_bytes == kPageBytes);

  // The same re-upload in a new frame is not a repeat.
  harness.shared_memory().BeginUploadFrame();
  harness.shared_memory().MemoryInvalidationCallback(harness.physical(0),
                                                     kPageBytes, true);
  CHECK(harness.shared_memory().RequestRange(harness.physical(0), 0x2000));
  CHECK(stats.requests == 3);
  CHECK(stats.bytes == 0x4000);
  CHECK(stats.repeat_bytes == kPageBytes);

  // Uploads are only counted against their own kind.
  CHECK(harness.shared_memory()
            .upload_stats(SharedMemory::UploadKind::kVertex)
            .requests == 0);
}

TEST_CASE("d3d11 shared memory rejects requests beyond the buffer") {
  UploadHarness harness(0x1A000000);
  REQUIRE(harness.Initialize());

  CHECK_FALSE(harness.shared_memory().RequestRange(SharedMemory::kBufferSize,
                                                   kPageBytes));
  std::pair<uint32_t, uint32_t> crossing[1] = {
      {SharedMemory::kBufferSize - kPageBytes, 2 * kPageBytes},
  };
  CHECK_FALSE(harness.shared_memory().RequestRanges(crossing, 1));
  CHECK(harness.sink().upload_calls.empty());

  // Empty requests are no-ops.
  CHECK(harness.shared_memory().RequestRange(0, 0));
  CHECK(harness.shared_memory().RequestRanges(nullptr, 0));
  CHECK(harness.sink().upload_calls.empty());
}
