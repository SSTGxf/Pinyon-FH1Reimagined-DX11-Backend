// Backend-neutral render audit.
//
// One JSONL record per sampled frame, with an identical schema on every
// backend, so a D3D11 run and a Vulkan run can be diffed line for line.
//
// Header-only on purpose: no build-system change, no fmt (fmt inside clang
// templates previously crashed the frontend), and all state is file-static.
//
// Records are written to
//   E:/PinyonShift/work/.local/audit/<backend>-<utc>.jsonl
// and are only produced when the caller checks the pinyon_audit cvar; every
// entry point returns immediately if BeginRun was never called.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace rex {
namespace graphics {
namespace render_audit {

namespace detail {

struct RtKey {
  std::string key;
  uint32_t samples = 0;
  uint32_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0;
  uint32_t hits = 0;
};

struct ResolveEntry {
  uint32_t src_w = 0;
  uint32_t src_h = 0;
  uint32_t dest_pitch = 0;
  uint32_t samples = 0;
  uint32_t pack = 0;
  uint32_t swap_rb = 0;
  uint32_t unscaled = 0;
  uint32_t count = 0;
};

struct ErrorEntry {
  std::string site;
  int32_t code = 0;
  uint32_t count = 0;
};

// The draw-side geometry a pass runs under, captured at the draw that set it.
struct PassGeo {
  int32_t window_offset_x = 0;
  int32_t window_offset_y = 0;
  float viewport_w = 0.0f;
  float viewport_h = 0.0f;
  float scissor_x = 0.0f;
  float scissor_y = 0.0f;
  float scissor_w = 0.0f;
  float scissor_h = 0.0f;
  uint64_t draw_id = 0;
};

struct PassState {
  bool open = false;
  bool truncated = false;
  int32_t frame_from = 0;
  int32_t frame_to = 0;
  uint64_t draws = 0;
  uint64_t wide_draws = 0;
  PassGeo geo;
  uint64_t pass = 0;
  uint64_t records = 0;
  uint64_t frame = 0;
  std::FILE* file = nullptr;
  std::string path;
};

struct State {
  std::mutex mutex;
  bool active = false;
  PassState pass;
  // Context for the lazy open: whatever module records first describes the run.
  std::string pending_backend = "unknown";
  std::string pending_adapter = "unknown";
  uint32_t pending_w = 0;
  uint32_t pending_h = 0;
  std::FILE* file = nullptr;
  std::string path;
  std::string backend;
  std::string adapter;
  uint32_t surface_w = 0;
  uint32_t surface_h = 0;
  uint64_t frame = 0;
  uint64_t draws = 0;
  uint32_t clear_rgb = 0;
  std::vector<RtKey> rts;
  std::vector<ResolveEntry> resolves;
  std::vector<ErrorEntry> errors;
};

inline State& Get() {
  static State state;
  return state;
}

// One record every this many frames, matching the frame-dump indices
// (frame_0300.bmp, frame_0600.bmp, ...) so an image and a record pair up.
constexpr uint64_t kSampleInterval = 300;

inline void JsonEscape(std::FILE* f, const std::string& value) {
  std::fputc('"', f);
  for (char c : value) {
    if (c == '"' || c == '\\') {
      std::fputc('\\', f);
      std::fputc(c, f);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      std::fprintf(f, "\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(c)));
    } else {
      std::fputc(c, f);
    }
  }
  std::fputc('"', f);
}

inline void WriteRecordLocked(State& s) {
  if (!s.file) {
    return;
  }
  std::FILE* f = s.file;
  std::fprintf(f, "{\"frame\":%llu,\"backend\":", static_cast<unsigned long long>(s.frame));
  JsonEscape(f, s.backend);
  std::fprintf(f, ",\"adapter\":");
  JsonEscape(f, s.adapter);
  std::fprintf(f, ",\"surface\":\"%ux%u\",\"draws\":%llu,\"clear\":\"#%06X\"",
               s.surface_w, s.surface_h, static_cast<unsigned long long>(s.draws),
               s.clear_rgb & 0xFFFFFFu);

  std::fprintf(f, ",\"rt\":[");
  for (size_t i = 0; i < s.rts.size(); ++i) {
    const RtKey& rt = s.rts[i];
    if (i) {
      std::fputc(',', f);
    }
    std::fprintf(f, "{\"key\":");
    JsonEscape(f, rt.key);
    std::fprintf(f,
                 ",\"samples\":%u,\"format\":%u,\"dims\":\"%ux%u\",\"pitch\":%u,\"hits\":%u}",
                 rt.samples, rt.format, rt.width, rt.height, rt.pitch, rt.hits);
  }

  std::fprintf(f, "],\"resolves\":[");
  for (size_t i = 0; i < s.resolves.size(); ++i) {
    const ResolveEntry& r = s.resolves[i];
    if (i) {
      std::fputc(',', f);
    }
    std::fprintf(f,
                 "{\"src\":\"%ux%u\",\"dest_pitch\":%u,\"samples\":%u,\"pack\":%u,"
                 "\"swap\":%u,\"unscaled\":%u,\"count\":%u}",
                 r.src_w, r.src_h, r.dest_pitch, r.samples, r.pack, r.swap_rb,
                 r.unscaled, r.count);
  }

  std::fprintf(f, "],\"errors\":[");
  for (size_t i = 0; i < s.errors.size(); ++i) {
    const ErrorEntry& e = s.errors[i];
    if (i) {
      std::fputc(',', f);
    }
    std::fprintf(f, "{\"site\":");
    JsonEscape(f, e.site);
    std::fprintf(f, ",\"code\":%d,\"count\":%u}", e.code, e.count);
  }
  std::fprintf(f, "]}\n");
  std::fflush(f);
}

}  // namespace detail

// Starts a run. Safe to call more than once; only the first call opens a file.
inline void BeginRun(const char* backend, const char* adapter, uint32_t surface_w,
                     uint32_t surface_h) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.active) {
    return;
  }
  std::time_t now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", utc.tm_year + 1900,
                utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
  char path[256];
  std::snprintf(path, sizeof(path), "E:/PinyonShift/work/.local/audit/%s-%s.jsonl",
                backend ? backend : "unknown", stamp);
  s.file = std::fopen(path, "a");
  s.path = path;
  s.backend = backend ? backend : "unknown";
  s.adapter = adapter ? adapter : "unknown";
  s.surface_w = surface_w;
  s.surface_h = surface_h;
  s.frame = 0;
  s.draws = 0;
  s.active = s.file != nullptr;
  if (s.active) {
    std::fprintf(stderr, "[render_audit] writing %s\n", path);
  }
}

// Describes the run before the audit opens its file (safe to call every frame).
inline void SetContext(const char* backend, const char* adapter, uint32_t surface_w,
                       uint32_t surface_h) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.active) {
    return;
  }
  if (backend) {
    s.pending_backend = backend;
  }
  if (adapter) {
    s.pending_adapter = adapter;
  }
  if (surface_w && surface_h) {
    s.pending_w = surface_w;
    s.pending_h = surface_h;
  }
}

// Opens the file on first use, in whichever module records first.
inline void EnsureStarted() {
  detail::State& s = detail::Get();
  if (s.active) {
    return;
  }
  BeginRun(s.pending_backend.c_str(), s.pending_adapter.c_str(), s.pending_w, s.pending_h);
}

inline bool IsActive() { return detail::Get().active; }

inline const char* Path() { return detail::Get().path.c_str(); }

// Records what a draw binds as its colour target. Deduplicated by key.
inline void NoteDraw(const char* rt_key, uint32_t samples, uint32_t format, uint32_t width,
                     uint32_t height, uint32_t pitch) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.active) {
    return;
  }
  ++s.draws;
  for (detail::RtKey& rt : s.rts) {
    if (rt.key == rt_key) {
      ++rt.hits;
      return;
    }
  }
  if (s.rts.size() >= 16) {
    return;
  }
  detail::RtKey rt;
  rt.key = rt_key ? rt_key : "";
  rt.samples = samples;
  rt.format = format;
  rt.width = width;
  rt.height = height;
  rt.pitch = pitch;
  rt.hits = 1;
  s.rts.push_back(std::move(rt));
}

// Records a resolve that actually dispatches into guest memory. This is the
// other half of the pair that matters: what the draws bind versus what reaches
// the frame.
inline void NoteResolve(uint32_t src_w, uint32_t src_h, uint32_t dest_pitch,
                        uint32_t samples, uint32_t pack, uint32_t swap_rb,
                        uint32_t unscaled) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.active) {
    return;
  }
  for (detail::ResolveEntry& r : s.resolves) {
    if (r.src_w == src_w && r.src_h == src_h && r.samples == samples &&
        r.dest_pitch == dest_pitch && r.pack == pack) {
      ++r.count;
      return;
    }
  }
  if (s.resolves.size() >= 16) {
    return;
  }
  detail::ResolveEntry r;
  r.src_w = src_w;
  r.src_h = src_h;
  r.dest_pitch = dest_pitch;
  r.samples = samples;
  r.pack = pack;
  r.swap_rb = swap_rb;
  r.unscaled = unscaled;
  r.count = 1;
  s.resolves.push_back(r);
}

// Records a failure so it is counted rather than buried in the log.
inline void NoteError(const char* site, int32_t code) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.active) {
    return;
  }
  for (detail::ErrorEntry& e : s.errors) {
    if (e.site == site && e.code == code) {
      ++e.count;
      return;
    }
  }
  if (s.errors.size() >= 32) {
    return;
  }
  detail::ErrorEntry e;
  e.site = site ? site : "";
  e.code = code;
  e.count = 1;
  s.errors.push_back(std::move(e));
}

inline void NoteClear(uint32_t r, uint32_t g, uint32_t b) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.active) {
    return;
  }
  s.clear_rgb = ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
}

// Called once per presented frame. Emits a record every kSampleInterval frames
// and keeps the per-frame tallies from growing without bound.
inline void EndFrame(bool log_transition) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  ++s.frame;
  // Transition log: whether the world pass (the 8192-wide clip-disabled
  // viewport) ran in this frame. A disappearance across Start is a load or
  // animation stall, not a resolve defect. Independent of the audit file, so
  // it works with the audit switched off.
  if (log_transition) {
    std::FILE* transition = std::fopen("E:/PinyonShift/work/.local/transition.log", "a");
    if (transition) {
      std::fprintf(transition, "frame=%llu wide=%llu",
                   static_cast<unsigned long long>(s.frame),
                   static_cast<unsigned long long>(s.pass.wide_draws));
      std::fputc(10, transition);
      std::fclose(transition);
    }
  }
  s.pass.wide_draws = 0;
  if (!s.active) {
    return;
  }
  if ((s.frame % detail::kSampleInterval) == 0) {
    detail::WriteRecordLocked(s);
  }
  s.draws = 0;
  s.rts.clear();
  s.resolves.clear();
}

inline void EndRun() {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.file) {
    std::fflush(s.file);
    std::fclose(s.file);
    s.file = nullptr;
  }
  s.active = false;
}

//
// Per-pass trace (pinyon_audit_passes): one logical tile pass from the claim
// that takes EDRAM ownership through the resolve that writes it out. Records
// share a pass id so a claim and its resolve pair up, and every record carries
// the frame id so one gameplay frame can be selected from the file.
//

namespace detail {

inline bool EnsurePassFileLocked(State& s) {
  if (s.pass.open) {
    return true;
  }
  std::time_t now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", utc.tm_year + 1900,
                utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
  char path[256];
  std::snprintf(path, sizeof(path), "E:/PinyonShift/work/.local/audit/%s-passes-%s.jsonl",
                s.pending_backend.c_str(), stamp);
  s.pass.file = std::fopen(path, "a");
  s.pass.path = path;
  s.pass.open = s.pass.file != nullptr;
  if (s.pass.open) {
    std::fprintf(stderr, "[render_audit] writing %s\n", path);
  }
  return s.pass.open;
}

inline void WriteTally(std::FILE* f, const std::vector<uint32_t>& tally) {
  std::fputc('[', f);
  for (size_t i = 0; i < tally.size(); ++i) {
    if (i) {
      std::fputc(',', f);
    }
    std::fprintf(f, "%u", tally[i]);
  }
  std::fputc(']', f);
}

inline void JKey(std::FILE* f, const char* key, bool first) {
  if (!first) {
    std::fputc(44, f);
  }
  JsonEscape(f, key ? key : "");
  std::fputc(58, f);
}
inline void JStr(std::FILE* f, const char* value) { JsonEscape(f, value ? value : ""); }
inline void JInt(std::FILE* f, int64_t value) { std::fprintf(f, "%lld", static_cast<long long>(value)); }
inline void JU64(std::FILE* f, uint64_t value) {
  std::fprintf(f, "%llu", static_cast<unsigned long long>(value));
}
inline void JF(std::FILE* f, double value) { std::fprintf(f, "%.1f", value); }
inline void JRaw(std::FILE* f, const char* text) { std::fprintf(f, "%s", text); }
inline void JEnd(std::FILE* f) {
  std::fputc(125, f);
  std::fputc(10, f);
}

}  // namespace detail

// A tile claim: the rectangle a pass takes ownership of, and the owner it took
// the tiles from. Emitted before the pass draws, so ownership is recorded on
// both sides of it.
inline void NotePassClaim(const char* key, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                          uint32_t col0, uint32_t col1, uint32_t row0, uint32_t row1,
                          uint32_t owner_before, uint32_t owner_after, bool per_row) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!detail::EnsurePassFileLocked(s) || s.pass.records >= 400) {
    if (s.pass.records >= 400) {
      s.pass.truncated = true;
    }
    return;
  }
  const int64_t pass_frame = int64_t(s.frame);
  if (pass_frame < s.pass.frame_from || pass_frame > s.pass.frame_to) {
    return;
  }
  ++s.pass.pass;
  ++s.pass.records;
  std::FILE* f = s.pass.file;
  std::fputc(123, f);
  detail::JKey(f, "ev", true);
  detail::JStr(f, "claim");
  detail::JKey(f, "frame", false);
  detail::JU64(f, s.frame);
  detail::JKey(f, "pass", false);
  detail::JU64(f, s.pass.pass);
  detail::JKey(f, "draw_id", false);
  detail::JU64(f, s.pass.geo.draw_id);
  detail::JKey(f, "source", false);
  detail::JStr(f, key);
  detail::JKey(f, "rect", false);
  std::fprintf(f, "[%d,%d,%d,%d]", x0, y0, x1, y1);
  detail::JKey(f, "tiles", false);
  std::fprintf(f, "[%u,%u,%u,%u]", col0, col1, row0, row1);
  detail::JKey(f, "owner_before", false);
  detail::JU64(f, owner_before);
  detail::JKey(f, "owner_after", false);
  detail::JU64(f, owner_after);
  detail::JKey(f, "per_row", false);
  detail::JU64(f, per_row ? 1u : 0u);
  detail::JKey(f, "window_offset", false);
  std::fprintf(f, "[%d,%d]", s.pass.geo.window_offset_x, s.pass.geo.window_offset_y);
  detail::JKey(f, "viewport", false);
  std::fprintf(f, "[");
  detail::JF(f, double(s.pass.geo.viewport_w));
  std::fputc(44, f);
  detail::JF(f, double(s.pass.geo.viewport_h));
  std::fputc(93, f);
  detail::JKey(f, "scissor", false);
  std::fprintf(f, "[");
  detail::JF(f, double(s.pass.geo.scissor_x));
  std::fputc(44, f);
  detail::JF(f, double(s.pass.geo.scissor_y));
  std::fputc(44, f);
  detail::JF(f, double(s.pass.geo.scissor_w));
  std::fputc(44, f);
  detail::JF(f, double(s.pass.geo.scissor_h));
  std::fputc(93, f);
  detail::JEnd(f);
  std::fflush(f);
}

inline void NotePassResolve(const char* kind, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            const char* src_key, uint32_t src_w, uint32_t src_h,
                            uint32_t samples, uint32_t sample_select, uint32_t dest_base,
                            uint32_t dest_pitch, uint32_t dest_info,
                            const std::vector<uint32_t>& src_rows, const char* src_evidence,
                            const std::vector<uint32_t>& hw_src_rows,
                            const std::vector<uint32_t>& dst_rows) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!detail::EnsurePassFileLocked(s) || s.pass.records >= 400) {
    if (s.pass.records >= 400) {
      s.pass.truncated = true;
    }
    return;
  }
  const int64_t pass_frame = int64_t(s.frame);
  if (pass_frame < s.pass.frame_from || pass_frame > s.pass.frame_to) {
    return;
  }
  ++s.pass.records;
  std::FILE* f = s.pass.file;
  std::fputc(123, f);
  detail::JKey(f, "ev", true);
  detail::JStr(f, "resolve");
  detail::JKey(f, "frame", false);
  detail::JU64(f, s.frame);
  detail::JKey(f, "pass", false);
  detail::JU64(f, s.pass.pass);
  detail::JKey(f, "draw_id", false);
  detail::JU64(f, s.pass.geo.draw_id);
  detail::JKey(f, "kind", false);
  detail::JStr(f, kind);
  detail::JKey(f, "rect", false);
  std::fprintf(f, "[%d,%d,%d,%d]", x0, y0, x1, y1);
  detail::JKey(f, "source", false);
  detail::JStr(f, src_key);
  detail::JKey(f, "src_dim", false);
  std::fprintf(f, "[%u,%u]", src_w, src_h);
  detail::JKey(f, "samples", false);
  detail::JU64(f, samples);
  detail::JKey(f, "sample_select", false);
  detail::JU64(f, sample_select);
  detail::JKey(f, "dest_base", false);
  detail::JU64(f, dest_base);
  detail::JKey(f, "dest_pitch", false);
  detail::JU64(f, dest_pitch);
  detail::JKey(f, "dest_info", false);
  detail::JU64(f, dest_info);
  detail::JKey(f, "pack", false);
  detail::JU64(f, dest_info & 7u);
  detail::JKey(f, "endian", false);
  detail::JU64(f, (dest_info >> 3u) & 7u);
  detail::JKey(f, "swap", false);
  detail::JU64(f, (dest_info >> 6u) & 1u);
  detail::JKey(f, "unscaled", false);
  detail::JU64(f, (dest_info >> 22u) & 1u);
  detail::JKey(f, "src_evidence", false);
  detail::JStr(f, src_evidence);
  detail::JKey(f, "raw_src_rows", false);
  detail::WriteTally(f, src_rows);
  detail::JKey(f, "hw_src_rows", false);
  detail::WriteTally(f, hw_src_rows);
  detail::JKey(f, "dst_rows", false);
  detail::WriteTally(f, dst_rows);
  detail::JEnd(f);
  std::fflush(f);
}

// One real draw submission: the id a pass records is the submission it belongs
// to, so repeated geometry shows as repeated ids rather than repeated bindings.
inline void NoteDrawSubmission() {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  ++s.pass.geo.draw_id;
  ++s.pass.draws;
}

// Draw-side geometry, captured at the draw that set it (same draw, before the
// registers can change).
inline void SetDrawGeo(int32_t window_offset_x, int32_t window_offset_y, float viewport_w,
                       float viewport_h, float scissor_x, float scissor_y, float scissor_w,
                       float scissor_h) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.pass.geo.window_offset_x = window_offset_x;
  s.pass.geo.window_offset_y = window_offset_y;
  if (viewport_w >= 8000.0f) {
    ++s.pass.wide_draws;
  }
  s.pass.geo.viewport_w = viewport_w;
  s.pass.geo.viewport_h = viewport_h;
  s.pass.geo.scissor_x = scissor_x;
  s.pass.geo.scissor_y = scissor_y;
  s.pass.geo.scissor_w = scissor_w;
  s.pass.geo.scissor_h = scissor_h;
}

// Frames outside [from, to] are not recorded; from == to == 0 records nothing.
inline void SetFrameWindow(int32_t from, int32_t to) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.pass.frame_from = from;
  s.pass.frame_to = to;
}

// True while the current presented-frame count is inside the pass window,
// using the same check the record writers apply. Callers use this to gate
// expensive per-resolve measurement (WaitForGpu, staging readbacks) so it
// only runs on frames that will actually be recorded, instead of choking
// every frame of a visible run.
inline bool PassWindowActive() {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  const int64_t pass_frame = int64_t(s.frame);
  return pass_frame >= s.pass.frame_from && pass_frame <= s.pass.frame_to;
}

// One provenance probe record. Every field that can invalidate the
// measurement is carried, so a zero can never be read as "empty surface".
struct ProbeInfo {
  const char* label = "";
  const char* reason = "";
  const char* verdict = "invalid_measurement";
  uint32_t resource_format = 0;
  uint32_t srv_view_format = 0;
  uint32_t samples = 0;
  uint32_t quality = 0;
  uint32_t bind_flags = 0;
  uint32_t support_ms_load = 0;
  uint32_t support_ms_resolve = 0;
  int32_t create_src = 0;
  int32_t create_dst = 0;
  int32_t create_staging = 0;
  int32_t create_srv = 0;
  int32_t create_uav = 0;
  int32_t copy_stage = 0;
  int32_t query_status = 0;
  int32_t map_hresult = 0;
  uint32_t map_row_pitch = 0;
  uint32_t uav_byte_width = 0;
  uint32_t uav_stride = 0;
  uint32_t uav_expected = 0;
  uint32_t uav_copied = 0;
  uint32_t subresource = 0;
  uint32_t rows_stride = 16;
  uint32_t cols_stride = 32;
  uint32_t sampled_pixels = 0;
  uint32_t sample_rows[4] = {0, 0, 0, 0};
  uint32_t max_r = 0;
  uint32_t max_g = 0;
  uint32_t max_b = 0;
  uint32_t max_a = 0;
  bool record_complete = false;
};

// Provenance probe record. Fields are only meaningful together with the
// verdict: probe_invalid or invalid_measurement means no field below may be
// read as evidence about the surface.
inline void NoteProbe(const ProbeInfo& info) {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!detail::EnsurePassFileLocked(s)) {
    return;
  }
  // Probes follow the same one-frame window as the pass trace, so a probe can
  // never stall a frame that was not meant to be captured.
  const int64_t probe_frame = int64_t(s.frame);
  if (probe_frame < s.pass.frame_from || probe_frame > s.pass.frame_to) {
    return;
  }
  std::FILE* f = s.pass.file;
  std::fputc(123, f);
  detail::JKey(f, "ev", true);
  detail::JStr(f, "probe");
  detail::JKey(f, "frame", false);
  detail::JU64(f, s.frame);
  detail::JKey(f, "label", false);
  detail::JStr(f, info.label);
  detail::JKey(f, "probe_verdict", false);
  detail::JStr(f, info.verdict);
  detail::JKey(f, "reason", false);
  detail::JStr(f, info.reason);
  detail::JKey(f, "record_complete", false);
  detail::JStr(f, info.record_complete ? "true" : "false");
  detail::JKey(f, "resource_format", false);
  detail::JU64(f, info.resource_format);
  detail::JKey(f, "srv_view_format", false);
  detail::JU64(f, info.srv_view_format);
  detail::JKey(f, "sample_count", false);
  detail::JU64(f, info.samples);
  detail::JKey(f, "sample_quality", false);
  detail::JU64(f, info.quality);
  detail::JKey(f, "bind_flags", false);
  detail::JU64(f, info.bind_flags);
  detail::JKey(f, "format_support_multisample_load", false);
  detail::JU64(f, info.support_ms_load);
  detail::JKey(f, "format_support_multisample_resolve", false);
  detail::JU64(f, info.support_ms_resolve);
  detail::JKey(f, "create_source_hresult", false);
  detail::JInt(f, info.create_src);
  detail::JKey(f, "create_resolve_hresult", false);
  detail::JInt(f, info.create_dst);
  detail::JKey(f, "create_staging_hresult", false);
  detail::JInt(f, info.create_staging);
  detail::JKey(f, "create_srv_hresult", false);
  detail::JInt(f, info.create_srv);
  detail::JKey(f, "create_uav_hresult", false);
  detail::JInt(f, info.create_uav);
  detail::JKey(f, "copy_stage", false);
  detail::JInt(f, info.copy_stage);
  detail::JKey(f, "query_status", false);
  detail::JInt(f, info.query_status);
  detail::JKey(f, "query_status_text", false);
  detail::JStr(f, info.query_status == 0 ? "complete"
                : (info.query_status == 1 ? "pending" : "timeout"));
  detail::JKey(f, "map_hresult", false);
  detail::JInt(f, info.map_hresult);
  detail::JKey(f, "map_row_pitch", false);
  detail::JU64(f, info.map_row_pitch);
  detail::JKey(f, "uav_byte_width", false);
  detail::JU64(f, info.uav_byte_width);
  detail::JKey(f, "uav_stride", false);
  detail::JU64(f, info.uav_stride);
  detail::JKey(f, "uav_expected_bytes", false);
  detail::JU64(f, info.uav_expected);
  detail::JKey(f, "uav_copied_bytes", false);
  detail::JU64(f, info.uav_copied);
  detail::JKey(f, "subresource_index", false);
  detail::JU64(f, info.subresource);
  detail::JKey(f, "sampled_rows_stride", false);
  detail::JU64(f, info.rows_stride);
  detail::JKey(f, "sampled_cols_stride", false);
  detail::JU64(f, info.cols_stride);
  detail::JKey(f, "sampled_pixel_count", false);
  detail::JU64(f, info.sampled_pixels);
  detail::JKey(f, "sample0_nonzero", false);
  detail::JU64(f, info.sample_rows[0]);
  detail::JKey(f, "sample1_nonzero", false);
  detail::JU64(f, info.sample_rows[1]);
  detail::JKey(f, "sample2_nonzero", false);
  detail::JU64(f, info.sample_rows[2]);
  detail::JKey(f, "sample3_nonzero", false);
  detail::JU64(f, info.sample_rows[3]);
  detail::JKey(f, "max_r", false);
  detail::JU64(f, info.max_r);
  detail::JKey(f, "max_g", false);
  detail::JU64(f, info.max_g);
  detail::JKey(f, "max_b", false);
  detail::JU64(f, info.max_b);
  detail::JKey(f, "max_a", false);
  detail::JU64(f, info.max_a);
  detail::JEnd(f);
  std::fflush(f);
}

inline const char* PassPath() { return detail::Get().pass.path.c_str(); }

inline void WriteCaptureSummary() {
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.pass.file) {
    return;
  }
  // Built without escape sequences: the shell layer mangles backslashes.
  static const char kQuote[2] = {34, 0};
  const char* q = kQuote;
  std::FILE* f = s.pass.file;
  std::fputc(123, f);
  std::fprintf(f, "%sev%s:%s%scapture%s,", q, q, q, q);
  std::fprintf(f, "%sframe_from%s:%d,", q, q, s.pass.frame_from);
  std::fprintf(f, "%sframe_to%s:%d,", q, q, s.pass.frame_to);
  std::fprintf(f, "%srecords%s:%llu,", q, q, static_cast<unsigned long long>(s.pass.records));
  std::fprintf(f, "%sdraws_submitted%s:%llu,", q, q,
               static_cast<unsigned long long>(s.pass.draws));
  std::fprintf(f, "%scomplete%s:%s,", q, q, s.pass.truncated ? "false" : "true");
  std::fprintf(f, "%struncated%s:%s}", q, q, s.pass.truncated ? "true" : "false");
  std::fputc(10, f);
  std::fflush(f);
}

inline void EndPassRun() {
  WriteCaptureSummary();
  detail::State& s = detail::Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.pass.file) {
    std::fflush(s.pass.file);
    std::fclose(s.pass.file);
    s.pass.file = nullptr;
  }
  s.pass.open = false;
}

}  // namespace render_audit
}  // namespace graphics
}  // namespace rex
