// D3D11 rendering forensics: a bounded, gated event recorder that explains
// which resource, draw, resolve and presentation produced a frame.
//
// Header-only, like render_audit.h, and used from inside the graphics module
// only. The presenter is a separate DLL and must not own this state: statics
// here duplicate per module, so exactly one module may call these functions.
//
// Design rules enforced here:
//  - When every forensics cvar is off, every entry point returns immediately.
//  - The ring buffer is bounded by fh1_forensics_ring_events; overflow is
//    counted and reported, never silently dropped.
//  - The capture file is only created for frames inside the capture window.
//  - No allocation per event, no GPU readback.

#ifndef REX_GRAPHICS_RENDER_FORENSICS_H_
#define REX_GRAPHICS_RENDER_FORENSICS_H_

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>

namespace rex::graphics::render_forensics {

// Resolve-planning failure sites. Frozen: ids are stable across runs so a
// failure kind can be tracked without reading source lines.
enum RfSite : uint8_t {
  kRfNone = 0,
  kRfFetchType = 1,     // RF001
  kRfFetchSize = 2,     // RF002
  kRfFetchMemory = 3,   // RF003
  kRfEmptyRectangle = 4,// RF004
  kRfResolveInfo = 5,   // RF005
  kRfDestFormat = 6,    // RF006
  kRfDestArray = 7,     // RF007
  kRfDestEndian = 8,    // RF008
  kRfSourceFormat = 9,  // RF008 (same endian/format family)
};

inline uint8_t RfIdForSkip(const char* skip) {
  if (!skip) return kRfNone;
  if (std::strcmp(skip, "resolve_info") == 0) return kRfResolveInfo;
  if (std::strcmp(skip, "resolve_dest_format") == 0) return kRfDestFormat;
  if (std::strcmp(skip, "resolve_dest_array") == 0) return kRfDestArray;
  if (std::strcmp(skip, "resolve_dest_endian") == 0) return kRfDestEndian;
  if (std::strcmp(skip, "resolve_source_format") == 0) return kRfSourceFormat;
  return kRfNone;
}

inline uint8_t RfIdForFailure(uint8_t failure) {
  switch (failure) {
    case 1: return kRfFetchType;
    case 2: return kRfFetchSize;
    case 3: return kRfFetchMemory;
    case 4: return kRfEmptyRectangle;
    case 5: return kRfResolveInfo;
    default: return kRfNone;
  }
}

enum ForensicsOp : uint8_t {
  kOpResourceCreate = 0,
  kOpResolveEntry = 1,
  kOpResolvePlanFail = 2,
  kOpResolveSkip = 4,
  kOpFrameSummary = 3,
};

// Fixed 128 bytes, no heap per event.
struct ForensicsEvent {
  uint64_t event_id = 0;
  uint64_t frame = 0;
  uint32_t op = 0;
  uint32_t site = 0;
  uint32_t resource_id = 0;
  uint32_t key = 0;
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  uint32_t samples = 0;
  uint32_t sample_select = 0;
  uint32_t dest_base = 0;
  uint32_t dest_pitch = 0;
  uint64_t aux = 0;
  char note[24] = {};
};

namespace detail {

struct State {
  bool started = false;
  bool capture = false;
  int32_t from = 0;
  int32_t to = 0;
  int32_t mask = -1;
  int32_t ring_capacity = 65536;
  bool include_binds = false;
  bool include_uploads = false;
  uint64_t frame = 0;
  uint64_t next_event_id = 1;
  uint64_t dropped = 0;
  uint64_t emitted = 0;
  uint32_t next_resource_id = 1;
  std::vector<ForensicsEvent> ring;
  std::FILE* file = nullptr;
  std::string path;
};

inline State& Get() {
  static State state;
  return state;
}

inline bool WindowActive(const State& s) {
  if (!s.capture) return false;
  if (s.from == 0 && s.to == 0) return false;  // captures nothing by design
  const int64_t frame = int64_t(s.frame);
  return frame >= s.from && frame <= s.to;
}

inline void EnsureStartedLocked(State& s) {
  if (s.started) return;
  s.started = true;
  s.capture = REXCVAR_GET(fh1_forensics_capture);
  s.from = REXCVAR_GET(fh1_forensics_from);
  s.to = REXCVAR_GET(fh1_forensics_to);
  s.mask = REXCVAR_GET(fh1_forensics_event_mask);
  s.ring_capacity = REXCVAR_GET(fh1_forensics_ring_events);
  s.include_binds = REXCVAR_GET(fh1_forensics_include_binds);
  s.include_uploads = REXCVAR_GET(fh1_forensics_include_uploads);
  if (s.ring_capacity < 256) s.ring_capacity = 256;
  if (s.ring_capacity > 1048576) s.ring_capacity = 1048576;
  s.ring.clear();
  s.ring.reserve(size_t(s.ring_capacity));
}

inline void EnsureFileLocked(State& s) {
  if (s.file) return;
  char stamp[32] = {};
  const uint64_t now = uint64_t(std::time(nullptr));
  std::snprintf(stamp, sizeof(stamp), "%llu", static_cast<unsigned long long>(now));
  s.path = std::string("E:/PinyonShift/work/.local/audit/d3d11-forensics-") + stamp + ".jsonl";
  s.file = std::fopen(s.path.c_str(), "wb");
  if (!s.file) return;
  std::fprintf(s.file,
               "{\"record\":\"header\",\"schema\":1,\"backend\":\"d3d11\","
               "\"capture_from\":%d,\"capture_to\":%d,\"ring_events\":%d}\n",
               s.from, s.to, s.ring_capacity);
}

}  // namespace detail

// True when the current frame is inside the capture window. Callers use this to
// skip building any event payload when capture is off.
inline bool Active() {
  detail::State& s = detail::Get();
  // Hooks run on the GPU thread; initialisation reads cvars once.
  if (!s.started) detail::EnsureStartedLocked(s);
  return detail::WindowActive(s);
}

inline void SetFrame(uint64_t frame) {
  detail::State& s = detail::Get();
  s.frame = frame;
}

inline void Emit(const ForensicsEvent& event) {
  detail::State& s = detail::Get();
  if (!s.started) detail::EnsureStartedLocked(s);
  if (!detail::WindowActive(s)) return;
  ForensicsEvent copy = event;
  copy.frame = s.frame;
  copy.event_id = s.next_event_id++;
  if (s.ring.size() < size_t(s.ring_capacity)) {
    s.ring.push_back(copy);
  } else {
    ++s.dropped;  // Bounded: overflow is counted, never silently lost.
  }
  ++s.emitted;
}

inline uint32_t NoteResourceCreate(uint32_t key, uint64_t aux, const char* note) {
  detail::State& s = detail::Get();
  if (!s.started) detail::EnsureStartedLocked(s);
  const uint32_t id = s.next_resource_id++;
  if (!detail::WindowActive(s)) return id;
  ForensicsEvent event;
  event.op = kOpResourceCreate;
  event.resource_id = id;
  event.key = key;
  event.aux = aux;
  if (note) {
    std::snprintf(event.note, sizeof(event.note), "%s", note);
  }
  Emit(event);
  return id;
}

inline void NoteResolvePlan(uint32_t key, uint8_t failure, const char* skip, int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1) {
  detail::State& s = detail::Get();
  if (!s.started) detail::EnsureStartedLocked(s);
  if (!detail::WindowActive(s)) return;
  // A planning failure and a legitimate skip are different facts: only a real
  // failure is counted as one. Both keep a stable site id.
  // kEmptyRectangle (4) means there was nothing to copy: an expected
  // non-transfer, not a failure. Only the other kinds are failures.
  const bool empty = failure == 4;
  const bool failed = failure != 0 && !empty;
  uint8_t rf = failed ? RfIdForFailure(failure) : RfIdForSkip(skip);
  if (empty) rf = kRfEmptyRectangle;
  if (rf == kRfNone) return;
  ForensicsEvent event;
  event.op = failed ? kOpResolvePlanFail : kOpResolveSkip;
  event.site = rf;
  event.key = key;
  event.x0 = x0;
  event.y0 = y0;
  event.x1 = x1;
  event.y1 = y1;
  if (skip) {
    std::snprintf(event.note, sizeof(event.note), "%s", skip);
  } else if (empty) {
    std::snprintf(event.note, sizeof(event.note), "%s", "empty_rectangle");
  }
  Emit(event);
}

inline void NoteResolveEntry(uint32_t key, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                             uint32_t samples, uint32_t sample_select, uint32_t dest_base,
                             uint32_t dest_pitch) {
  detail::State& s = detail::Get();
  if (!s.started) detail::EnsureStartedLocked(s);
  if (!detail::WindowActive(s)) return;
  ForensicsEvent event;
  event.op = kOpResolveEntry;
  event.key = key;
  event.x0 = x0;
  event.y0 = y0;
  event.x1 = x1;
  event.y1 = y1;
  event.samples = samples;
  event.sample_select = sample_select;
  event.dest_base = dest_base;
  event.dest_pitch = dest_pitch;
  Emit(event);
}

// Writes the frame's events, its summary, and (when the window closes) the
// capture summary. Called at the swap boundary.
inline void EndFrame() {
  detail::State& s = detail::Get();
  if (!s.started) detail::EnsureStartedLocked(s);
  const bool in_window = detail::WindowActive(s);
  if (!in_window && s.ring.empty() && !s.file) {
    return;  // Completely off: one branch, no allocation, no I/O.
  }
  detail::EnsureFileLocked(s);
  if (s.file) {
    for (const ForensicsEvent& event : s.ring) {
      std::fprintf(s.file,
                   "{\"frame\":%llu,\"event_id\":%llu,\"op\":%u,\"site\":%u,"
                   "\"resource_id\":%u,\"key\":%u,\"rect\":[%d,%d,%d,%d],"
                   "\"samples\":%u,\"sample_select\":%u,\"dest_base\":%u,"
                   "\"dest_pitch\":%u,\"aux\":%llu,\"note\":\"%s\"}\n",
                   static_cast<unsigned long long>(event.frame),
                   static_cast<unsigned long long>(event.event_id), event.op, event.site,
                   event.resource_id, event.key, event.x0, event.y0, event.x1, event.y1,
                   event.samples, event.sample_select, event.dest_base, event.dest_pitch,
                   static_cast<unsigned long long>(event.aux), event.note);
    }
    std::fprintf(s.file,
                 "{\"frame\":%llu,\"event\":\"frame_summary\",\"events\":%llu,"
                 "\"dropped\":%llu,\"complete\":%s}\n",
                 static_cast<unsigned long long>(s.frame),
                 static_cast<unsigned long long>(s.ring.size()),
                 static_cast<unsigned long long>(s.dropped),
                 s.dropped == 0 ? "true" : "false");
    std::fflush(s.file);
  }
  s.ring.clear();
  if (s.to != 0 && int64_t(s.frame) > s.to && s.file) {
    // The window has closed: seal the capture and stop writing.
    std::fprintf(s.file,
                 "{\"event\":\"capture_summary\",\"from\":%d,\"to\":%d,\"emitted\":%llu,"
                 "\"dropped\":%llu,\"complete\":%s}\n",
                 s.from, s.to, static_cast<unsigned long long>(s.emitted),
                 static_cast<unsigned long long>(s.dropped),
                 s.dropped == 0 ? "true" : "false");
    std::fflush(s.file);
    std::fclose(s.file);
    s.file = nullptr;
    s.capture = false;
  }
}

}  // namespace rex::graphics::render_forensics

#endif  // REX_GRAPHICS_RENDER_FORENSICS_H_
