/**
 * @file        graphics/tools/fh1_pack_producer_main.cpp
 * @brief       Headless offline FH1 shader pack producer (no device, no game)
 *
 *              Drives the exact translation and capture flow of the offline
 *              producer plugin (rexgpu-fh1-producer: corpus read -> ucode
 *              analysis -> DxbcShaderTranslator -> geometry shader keys) from
 *              a console process, writing the two files the existing pack
 *              build step consumes:
 *
 *                <output>/dxil.blob           concatenated DXBC entries
 *                <output>/shader-manifest.json  manifest for
 *                    tools/native-shader-pack.py build
 *
 *              Default configuration is the Direct3D 11 backend: SM 5.0
 *              emission (flat D3D11 b/t/s/u registers, feature level 11_0)
 *              under the distinct "d3d11" pack backend identity. Pass
 *              --backend d3d12 for the SM 5.1 mode - the same flow the D3D12
 *              producer uses in-game - which doubles as the regression path
 *              (translator output for SM 5.1 must stay byte-identical).
 *
 *              Shader patterns that cannot be represented in SM 5.0 are never
 *              emitted silently: each failed translation is logged by guest
 *              hash with its translator errors, listed in the summary and in
 *              <output>/translation-failures.txt, and left out of the pack.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <inttypes.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <xxhash.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <rex/filesystem.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/fh1_shader_pack.h>
#include <rex/graphics/pipeline/shader/dxbc_geometry_shaders.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/math.h>
#include <rex/string/buffer.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/graphics_provider.h>

// The corpus and translator types live under rex::graphics; the tool driver is
// a console process outside the rex namespaces.
namespace xenos = rex::graphics::xenos;

namespace {

// The capture limits of src/native_renderer/shader_capture.cpp - the manifest
// this tool writes must stay within what the capture path would accept.
constexpr size_t kMaximumEntries = 65'535;
constexpr size_t kMaximumBytecodeBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumCaptureBytes = 512 * 1024 * 1024;
constexpr size_t kMaximumBindings = 255;

using rex::system::GraphicsShaderStage;

struct ProducerTextureBinding {
  uint32_t bindless_descriptor_index = 0;
  uint32_t fetch_constant = 0;
  uint32_t dimension = 0;
  uint32_t is_signed = 0;
};

struct ProducerSamplerBinding {
  uint32_t bindless_descriptor_index = 0;
  uint32_t fetch_constant = 0;
  uint32_t mag_filter = 0;
  uint32_t min_filter = 0;
  uint32_t mip_filter = 0;
  uint32_t aniso_filter = 0;
};

struct ProducerEntry {
  GraphicsShaderStage stage = GraphicsShaderStage::kVertex;
  uint64_t guest_hash = 0;
  uint64_t specialization_mask = 0;
  std::vector<uint8_t> bytecode;
  std::vector<ProducerTextureBinding> texture_bindings;
  std::vector<ProducerSamplerBinding> sampler_bindings;
  uint32_t used_texture_mask = 0;
};

struct TranslationFailure {
  GraphicsShaderStage stage = GraphicsShaderStage::kVertex;
  uint64_t guest_hash = 0;
  uint64_t specialization_mask = 0;
  std::string reason;
};

// ---------------------------------------------------------------------------
// The FH1 disc corpus modification matrices, mirrored from
// d3d12/pipeline_cache.cpp InitializeShaderStorage (the existing producer):
// the pack entries must pair exactly the way the in-game flow pairs them.
// ---------------------------------------------------------------------------

constexpr uint64_t kFh1VertexModifications[] = {
    0x0, 0x1, 0x3, 0x7, 0xF, 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3F};
constexpr uint64_t kFh1VertexModificationsSource[] = {
    0x0, 0x1, 0x3, 0x7, 0xF, 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3F};
constexpr uint64_t kFh1PixelModifications[] = {
    0x0000000000000000, 0x0000400000000000, 0x0000000000000001,
    0x0000000000010001, 0x0000400000000001, 0x0000400000010001,
    0x0000400300000001, 0x0000000000000003, 0x0000400000000003,
    0x0000400000020003, 0x0000400500000003, 0x0000000000000007,
    0x0000400000000007, 0x0000400000020007, 0x0000400500000007,
    0x000000000000000F, 0x000000000007000F, 0x00000000000A000F,
    0x00000000000E000F, 0x000040000000000F, 0x000040000009000F,
    0x000000000000001F, 0x000000000015001F, 0x00000000001A001F,
    0x00004000000D001F, 0x000040000015001F, 0x000000000000003F,
    0x000000000016003F, 0x00000000002B003F, 0x00000000002D003F,
    0x0000000D0016003F, 0x000040000000003F, 0x000040000019003F,
    0x00004000001B003F, 0x00004000002B003F, 0x000000000016007F,
    0x00000000003B007F, 0x00000000005B007F, 0x00000000006D007F,
    0x000000000075007F, 0x0000000F001D007F, 0x0000000F0035007F,
    0x00004000002B007F, 0x000040000040007F, 0x00004000005B007F,
    0x00000000000700FF, 0x00000000001600FF, 0x0000000000B300FF,
    0x0000000000EB00FF, 0x0000400000B300FF, 0x0000400000EB00FF,
    0x00004011000000FF, 0x0000400001DB01FF, 0x0000400003B303FF,
    0x00004000001B007F, 0x00004000003B00FF, 0x00000000007301FF};

// Runtime-discovered pairings folded into the same corpus jobs by the
// existing producer (mirrored verbatim; see d3d12/pipeline_cache.cpp).
constexpr std::pair<uint64_t, uint64_t> kFh1RuntimeVariants[] = {
    {0xC41DD15CBD361350, 0x00000000000001FF},
    {0xCE81AE65F9C5A57B, 0x000000000000007F},
    {0xD60688109AC80358, 0x000000000000003F},
    {0x81EF4F2E5B5DDBD1, 0x00004000000D003F},
    {0xA81FE6B4247E184B, 0x00004000001B007F},
    {0xD445FABAE890A455, 0x00000000007301FF},
    // Additional retail material pairings with fewer live interpolators than
    // the maximum declared in their shared asset program.
    {0x2399AF5C1A9D6197, 0x000000000000003F},
    {0x32E185F90FED70B1, 0x000000000000003F},
    {0x4018C345028228A2, 0x000000000000007F},
    {0x5808EEF89DBCB2EB, 0x000000000000007F},
    {0x6A9D9846410BB101, 0x000000000000003F},
    {0x74ADCE29056221E0, 0x000000000000003F},
    {0x8107C6FD3171A834, 0x000000000000007F},
    {0x8D8A197476841A9A, 0x0000000000000000},
    {0x9B50C4B2B1CCE0C8, 0x000000000000003F},
    {0xAD2C355A6BE1EE87, 0x0000000000000000},
    {0xD71402231C6EC270, 0x000000000000003F},
    {0xFEA060C603F44C7D, 0x000000000000003F},
    {0x2A69134D6AB961F7, 0x000000000016003F},
    {0x94A6380F93364C93, 0x000000000016007F},
    {0xA4E5DE05DCAF605B, 0x000000000016003F},
    {0xD0EFA731CCB32A71, 0x000000000016003F},
    {0xF6D833A391B89839, 0x000000000016007F},
    // Save-backed Recaro Rush route variants absent from the legacy PSO cache.
    {0x18AB2AE508FD82EB, 0x000000000000007F},
    {0xE67462AC487FB369, 0x0000000000000001},
    {0xD606035F41900757, 0x000000000000003F},
    {0xEC8B8A5D5D0171BE, 0x00000000000000FF},
    // Autoshow and race vertex shaders paired with pixel shaders that read
    // fewer interpolators than the vertex program declares.
    {0x2B2187189DFBFB10, 0x0000000000000007},
    {0x958D2F74344C7B36, 0x000000000000003F},
    {0xAE8FEE9795590D78, 0x000000000000007F},
    {0xC8DB78EC7C219094, 0x000000000000007F},
    {0xFCB6BAC481AA29F2, 0x000000000000007F},
};

std::string Hex64(uint64_t value) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llX",
                static_cast<unsigned long long>(value));
  return buffer;
}

std::string Hex32(uint32_t value) {
  char buffer[9];
  std::snprintf(buffer, sizeof(buffer), "%08X", value);
  return buffer;
}

bool Sha256(std::span<const uint8_t> source, std::array<uint8_t, 32>& digest_out) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) {
    return false;
  }
  bool ok = false;
  if (!BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)) {
    if (!BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(source.data())),
                        static_cast<ULONG>(source.size()), 0) &&
        !BCryptFinishHash(hash, digest_out.data(), static_cast<ULONG>(digest_out.size()), 0)) {
      ok = true;
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return ok;
}

std::string DigestHex(const std::array<uint8_t, 32>& digest) {
  std::string result;
  result.reserve(digest.size() * 2);
  static constexpr char kHex[] = "0123456789abcdef";
  for (uint8_t value : digest) {
    result.push_back(kHex[value >> 4]);
    result.push_back(kHex[value & 15]);
  }
  return result;
}

const char* StageName(GraphicsShaderStage stage) {
  switch (stage) {
    case GraphicsShaderStage::kVertex:
      return "vertex";
    case GraphicsShaderStage::kPixel:
      return "pixel";
    default:
      return "geometry";
  }
}

// ---------------------------------------------------------------------------
// Corpus loading - the FH1C blob and legacy per-program .bin files, exactly as
// tools/extract-fh1-shader-corpus.py writes them and the producer reads them.
// ---------------------------------------------------------------------------

struct CorpusProgram {
  xenos::ShaderType stage = xenos::ShaderType::kVertex;
  uint32_t interpolator_count = 0;
  std::vector<uint32_t> ucode;
};

using ModificationMatrix =
    std::array<std::array<std::set<uint64_t>, xenos::kMaxInterpolators + 1>, 2>;

void BuildModificationMatrix(ModificationMatrix& modifications) {
  for (uint64_t modification : kFh1VertexModificationsSource) {
    const uint32_t interpolator_mask = uint32_t(modification) & 0xFFFF;
    modifications[size_t(xenos::ShaderType::kVertex)][rex::bit_count(interpolator_mask)]
        .insert(modification);
  }
  for (uint64_t modification : kFh1PixelModifications) {
    const uint32_t interpolator_mask = uint32_t(modification) & 0xFFFF;
    if ((interpolator_mask & (interpolator_mask + 1)) == 0) {
      modifications[size_t(xenos::ShaderType::kPixel)]
                   [rex::bit_count(interpolator_mask)]
                       .insert(modification);
    }
  }
}

bool LoadCorpus(const std::filesystem::path& corpus_root,
                const ModificationMatrix& modifications,
                std::vector<CorpusProgram>& programs_out, size_t& failed_out) {
  std::error_code corpus_error;
  std::vector<std::filesystem::path> corpus_files;
  for (std::filesystem::directory_iterator it(corpus_root, corpus_error), end;
       !corpus_error && it != end; it.increment(corpus_error)) {
    if (it->is_regular_file() && it->path().extension() == ".bin") {
      corpus_files.push_back(it->path());
    }
  }
  std::sort(corpus_files.begin(), corpus_files.end());

  if (std::ifstream blob(corpus_root / "corpus.blob", std::ios::binary | std::ios::ate);
      blob) {
    std::vector<uint8_t> data(size_t(std::streamoff(blob.tellg())));
    uint32_t version = 0, count = 0;
    blob.seekg(0);
    if (data.size() < 12 ||
        !blob.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())) ||
        std::memcmp(data.data(), "FH1C", 4)) {
      ++failed_out;
    } else {
      std::memcpy(&version, data.data() + 4, sizeof(version));
      std::memcpy(&count, data.data() + 8, sizeof(count));
    }
    size_t offset = 12;
    for (uint32_t i = 0; version == 1 && i < count; ++i) {
      if (data.size() - offset < 8) {
        ++failed_out;
        break;
      }
      uint32_t size;
      std::memcpy(&size, data.data() + offset + 4, sizeof(size));
      if (data.size() - offset - 8 < size) {
        ++failed_out;
        break;
      }
      const uint8_t stage_index = data[offset];
      const uint32_t interpolator_count = data[offset + 1];
      const uint8_t* ucode_bytes = data.data() + offset + 8;
      offset += 8 + size_t(size);
      const xenos::ShaderType stage =
          stage_index ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex;
      if (stage_index > 1 || interpolator_count > xenos::kMaxInterpolators ||
          modifications[size_t(stage)][interpolator_count].empty()) {
        continue;
      }
      CorpusProgram& program = programs_out.emplace_back();
      program.stage = stage;
      program.interpolator_count = interpolator_count;
      if (size && size <= 0xFFFF * 4 && !(size % 12)) {
        program.ucode.resize(size / sizeof(uint32_t));
        std::memcpy(program.ucode.data(), ucode_bytes, size);
      }
    }
    if (version != 1) {
      ++failed_out;
    }
  } else {
    std::fprintf(stderr, "error: cannot read %s\n",
                 (corpus_root / "corpus.blob").string().c_str());
    return false;
  }

  for (const std::filesystem::path& corpus_file : corpus_files) {
    const std::string name = rex::path_to_utf8(corpus_file.filename());
    xenos::ShaderType stage;
    size_t digit_offset;
    if (name.size() == 78 && name.starts_with("pixel-i")) {
      stage = xenos::ShaderType::kPixel;
      digit_offset = 7;
    } else if (name.size() == 79 && name.starts_with("vertex-i")) {
      stage = xenos::ShaderType::kVertex;
      digit_offset = 8;
    } else {
      continue;
    }
    if (name[digit_offset] < '0' || name[digit_offset] > '9' ||
        name[digit_offset + 1] < '0' || name[digit_offset + 1] > '9' ||
        name[digit_offset + 2] != '-') {
      continue;
    }
    const uint32_t interpolator_count =
        uint32_t(name[digit_offset] - '0') * 10 + uint32_t(name[digit_offset + 1] - '0');
    if (interpolator_count > xenos::kMaxInterpolators ||
        modifications[size_t(stage)][interpolator_count].empty()) {
      continue;
    }
    CorpusProgram& program = programs_out.emplace_back();
    program.stage = stage;
    program.interpolator_count = interpolator_count;
    std::ifstream input(corpus_file, std::ios::binary | std::ios::ate);
    std::streamoff size = input ? std::streamoff(input.tellg()) : std::streamoff(-1);
    if (size <= 0 || size > 0xFFFF * 4 || size % 12) {
      continue;
    }
    program.ucode.resize(size_t(size) / sizeof(uint32_t));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(program.ucode.data()), size)) {
      program.ucode.clear();
    }
  }
  return true;
}

// Pack misses recorded by shipping runs (optional --miss-dir): title-generated
// shaders no disc scan predicts. Mirrors the producer's recorded-miss input.
bool LoadRecordedMisses(const std::filesystem::path& miss_root,
                        const ModificationMatrix& modifications,
                        std::vector<CorpusProgram>& programs_out,
                        std::vector<std::pair<uint64_t, uint64_t>>& recorded_out,
                        size_t& failed_out) {
  std::error_code error;
  if (miss_root.empty() || !std::filesystem::is_directory(miss_root, error)) {
    return true;
  }
  for (std::filesystem::directory_iterator it(miss_root, error), end; !error && it != end;
       it.increment(error)) {
    const std::string name = rex::path_to_utf8(it->path().filename());
    uint64_t hash = 0, modification = 0;
    char stage_name[9] = {};
    if (!it->is_regular_file() ||
        std::sscanf(name.c_str(), "%8[a-z]-%16" SCNx64 "-%16" SCNx64 ".bin", stage_name, &hash,
                    &modification) != 3) {
      continue;
    }
    const std::string stage_string = stage_name;
    if (stage_string == "geometry") {
      if (!hash && modification <= UINT32_MAX) {
        recorded_out.emplace_back(0, modification);
      }
      continue;
    }
    if (stage_string != "vertex" && stage_string != "pixel") {
      continue;
    }
    const xenos::ShaderType stage = stage_string == "vertex"
                                        ? xenos::ShaderType::kVertex
                                        : xenos::ShaderType::kPixel;
    std::ifstream input(it->path(), std::ios::binary | std::ios::ate);
    const std::streamoff size = input ? std::streamoff(input.tellg()) : std::streamoff(-1);
    if (size <= 0 || size > 0xFFFF * 4 || size % 12) {
      ++failed_out;
      continue;
    }
    CorpusProgram& program = programs_out.emplace_back();
    program.stage = stage;
    program.interpolator_count = 0;
    program.ucode.resize(size_t(size) / sizeof(uint32_t));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(program.ucode.data()), size) ||
        XXH3_64bits(program.ucode.data(), size_t(size)) != hash) {
      program.ucode.clear();
      ++failed_out;
      continue;
    }
    recorded_out.emplace_back(hash, modification);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Manifest + blob writing - the format of src/native_renderer/shader_capture.cpp
// (validated by tools/native-shader-pack.py build).
// ---------------------------------------------------------------------------

bool WriteCapture(const std::filesystem::path& output_root, uint32_t translator_version,
                  uint32_t backend_value, const char* backend_name, uint32_t device_features,
                  bool bindless_resources, bool edram_rov, bool gamma_unorm8, bool msaa_2x,
                  uint32_t scale_x, uint32_t scale_y, std::vector<ProducerEntry>& entries,
                  std::string& error_out) {
  std::sort(entries.begin(), entries.end(),
            [](const ProducerEntry& left, const ProducerEntry& right) {
              if (left.stage != right.stage) {
                return left.stage < right.stage;
              }
              if (left.guest_hash != right.guest_hash) {
                return left.guest_hash < right.guest_hash;
              }
              return left.specialization_mask < right.specialization_mask;
            });

  std::error_code error;
  std::filesystem::create_directories(output_root, error);
  if (error) {
    error_out = "cannot create output directory";
    return false;
  }

  const std::filesystem::path blob_path = output_root / "dxil.blob";
  const std::filesystem::path manifest_path = output_root / "shader-manifest.json";
  std::ofstream blob(blob_path, std::ios::binary | std::ios::trunc);
  if (!blob) {
    error_out = "cannot open dxil.blob for writing";
    return false;
  }

  std::string document =
      "{\n"
      "  \"schema\": \"pinyon-shift.native-shader-pack.v3\",\n"
      "  \"backend\": \"" +
      std::string(backend_name) +
      "\",\n"
      "  \"translation\": {\n"
      "    \"translator_version\": \"" +
      Hex32(translator_version) +
      "\",\n"
      "    \"device_features\": " +
      std::to_string(device_features) +
      ",\n"
      "    \"bindless_resources\": " + std::string(bindless_resources ? "true" : "false") +
      ",\n"
      "    \"edram_rov\": " + std::string(edram_rov ? "true" : "false") +
      ",\n"
      "    \"gamma_render_target_as_unorm8\": " + std::string(gamma_unorm8 ? "true" : "false") +
      ",\n"
      "    \"msaa_2x\": " + std::string(msaa_2x ? "true" : "false") +
      ",\n"
      "    \"draw_resolution_scale_x\": " + std::to_string(scale_x) +
      ",\n"
      "    \"draw_resolution_scale_y\": " + std::to_string(scale_y) +
      "\n"
      "  },\n"
      "  \"entries\": [\n";

  size_t bytecode_bytes = 0;
  for (size_t index = 0; index < entries.size(); ++index) {
    ProducerEntry& entry = entries[index];
    if (entry.bytecode.empty() || entry.bytecode.size() > kMaximumBytecodeBytes ||
        entry.texture_bindings.size() > kMaximumBindings ||
        entry.sampler_bindings.size() > kMaximumBindings ||
        bytecode_bytes + entry.bytecode.size() > kMaximumCaptureBytes) {
      error_out = "entry limits exceeded for " + std::string(StageName(entry.stage)) + "/" +
                  Hex64(entry.guest_hash) + "/" + Hex64(entry.specialization_mask);
      return false;
    }
    std::array<uint8_t, 32> digest{};
    if (!Sha256(entry.bytecode, digest)) {
      error_out = "SHA-256 failed for " + Hex64(entry.guest_hash);
      return false;
    }
    blob.write(reinterpret_cast<const char*>(entry.bytecode.data()),
               std::streamsize(entry.bytecode.size()));
    if (!blob) {
      error_out = "dxil.blob write failed";
      return false;
    }
    document += "    {\n      \"stage\": \"" + std::string(StageName(entry.stage)) +
                "\",\n      \"guest_hash\": \"" + Hex64(entry.guest_hash) +
                "\",\n      \"specialization_mask\": \"" + Hex64(entry.specialization_mask) +
                "\",\n      \"bytecode\": \"dxil.blob\",\n      \"bytecode_offset\": " +
                std::to_string(bytecode_bytes) +
                ",\n      \"bytecode_size\": " + std::to_string(entry.bytecode.size()) +
                ",\n      \"sha256\": \"" + DigestHex(digest) +
                "\",\n      \"texture_bindings\": [";
    for (size_t binding_index = 0; binding_index < entry.texture_bindings.size();
         ++binding_index) {
      const ProducerTextureBinding& binding = entry.texture_bindings[binding_index];
      document +=
          std::string(binding_index ? "," : "") + "{\"bindless_descriptor_index\":" +
          std::to_string(binding.bindless_descriptor_index) +
          ",\"fetch_constant\":" + std::to_string(binding.fetch_constant) +
          ",\"dimension\":" + std::to_string(binding.dimension) +
          ",\"is_signed\":" + std::to_string(binding.is_signed) + "}";
    }
    document += "],\n      \"sampler_bindings\": [";
    for (size_t binding_index = 0; binding_index < entry.sampler_bindings.size();
         ++binding_index) {
      const ProducerSamplerBinding& binding = entry.sampler_bindings[binding_index];
      document +=
          std::string(binding_index ? "," : "") + "{\"bindless_descriptor_index\":" +
          std::to_string(binding.bindless_descriptor_index) +
          ",\"fetch_constant\":" + std::to_string(binding.fetch_constant) +
          ",\"mag_filter\":" + std::to_string(binding.mag_filter) +
          ",\"min_filter\":" + std::to_string(binding.min_filter) +
          ",\"mip_filter\":" + std::to_string(binding.mip_filter) +
          ",\"aniso_filter\":" + std::to_string(binding.aniso_filter) + "}";
    }
    document += "],\n      \"used_texture_mask\": " + std::to_string(entry.used_texture_mask) +
                "\n    }" + (index + 1 == entries.size() ? "" : ",") + "\n";
    bytecode_bytes += entry.bytecode.size();
  }
  document += "  ]\n}\n";
  blob.close();

  std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
  if (!manifest.write(document.data(), std::streamsize(document.size())) || !manifest.flush()) {
    error_out = "shader-manifest.json write failed";
    return false;
  }
  (void)backend_value;
  return true;
}

struct ProducerOptions {
  std::filesystem::path corpus_root;
  std::filesystem::path output_root;
  std::filesystem::path miss_root;
  std::string backend = "d3d11";
  std::string vendor = "nvidia";
  uint32_t scale = 1;
  bool gamma_render_target_as_unorm16 = true;
  bool msaa_2x = true;
  size_t thread_count = 0;
};

void PrintUsage() {
  std::fprintf(stderr,
               "usage: rexgpu-fh1-pack-producer --corpus <dir> --output <dir>\n"
               "       [--backend d3d11|d3d12] [--vendor nvidia|amd|intel] [--scale N]\n"
               "       [--gamma-unorm16 0|1] [--msaa-2x 0|1] [--threads N]\n"
               "       [--miss-dir <dir>]\n");
}

}  // namespace

int main(int argc, char** argv) {
  ProducerOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    const auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs a value\n", argv[i]);
        std::exit(2);
      }
      return argv[++i];
    };
    if (argument == "--corpus") {
      options.corpus_root = next();
    } else if (argument == "--output") {
      options.output_root = next();
    } else if (argument == "--miss-dir") {
      options.miss_root = next();
    } else if (argument == "--backend") {
      options.backend = next();
    } else if (argument == "--vendor") {
      options.vendor = next();
    } else if (argument == "--scale") {
      options.scale = uint32_t(std::strtoul(next(), nullptr, 10));
    } else if (argument == "--gamma-unorm16") {
      options.gamma_render_target_as_unorm16 = std::strtoul(next(), nullptr, 10) != 0;
    } else if (argument == "--msaa-2x") {
      options.msaa_2x = std::strtoul(next(), nullptr, 10) != 0;
    } else if (argument == "--threads") {
      options.thread_count = std::strtoul(next(), nullptr, 10);
    } else {
      std::fprintf(stderr, "error: unknown argument %s\n", argv[i]);
      PrintUsage();
      return 2;
    }
  }
  if (options.corpus_root.empty() || options.output_root.empty()) {
    PrintUsage();
    return 2;
  }
  const bool target_sm_50 = options.backend == "d3d11";
  if (options.backend != "d3d11" && options.backend != "d3d12") {
    std::fprintf(stderr, "error: --backend must be d3d11 or d3d12\n");
    return 2;
  }
  if (!options.scale || options.scale > 4) {
    std::fprintf(stderr, "error: --scale must be from 1 through 4\n");
    return 2;
  }
  rex::ui::GraphicsProvider::GpuVendorID vendor_id = rex::ui::GraphicsProvider::GpuVendorID::kNvidia;
  if (options.vendor == "amd") {
    vendor_id = rex::ui::GraphicsProvider::GpuVendorID::kAMD;
  } else if (options.vendor == "intel") {
    vendor_id = rex::ui::GraphicsProvider::GpuVendorID::kIntel;
  } else if (options.vendor != "nvidia") {
    std::fprintf(stderr, "error: --vendor must be nvidia, amd or intel\n");
    return 2;
  }
  if (!options.thread_count) {
    options.thread_count = std::max(size_t(1), size_t(std::thread::hardware_concurrency()));
  }

  const uint32_t device_features =
      vendor_id == rex::ui::GraphicsProvider::GpuVendorID::kIntel
          ? 0u
          : rex::graphics::Fh1ShaderPack::kD3D12FeatureSwitch;

  ModificationMatrix modifications;
  BuildModificationMatrix(modifications);

  size_t failed = 0;
  std::vector<CorpusProgram> corpus_programs;
  if (!LoadCorpus(options.corpus_root, modifications, corpus_programs, failed)) {
    return 1;
  }
  std::vector<std::pair<uint64_t, uint64_t>> recorded_misses;
  if (!LoadRecordedMisses(options.miss_root, modifications, corpus_programs, recorded_misses,
                          failed)) {
    return 1;
  }

  // Group the corpus programs by hash; every variant of one program becomes
  // one job so one thread owns the shader object, as in the producer.
  std::map<uint64_t, std::pair<rex::graphics::d3d12::D3D12Shader*, std::set<uint64_t>>>
      corpus_programs_by_hash;
  std::vector<std::unique_ptr<rex::graphics::d3d12::D3D12Shader>> owned_shaders;
  size_t read_failed = 0;
  for (CorpusProgram& program : corpus_programs) {
    if (program.ucode.empty()) {
      ++read_failed;
      continue;
    }
    const xenos::ShaderType stage = program.stage;
    const uint64_t hash = XXH3_64bits(program.ucode.data(), program.ucode.size() * sizeof(uint32_t));
    auto& [program_shader, program_modifications] = corpus_programs_by_hash[hash];
    if (!program_shader) {
      owned_shaders.push_back(std::make_unique<rex::graphics::d3d12::D3D12Shader>(
          stage, hash, program.ucode.data(), program.ucode.size()));
      program_shader = owned_shaders.back().get();
    }
    program_modifications.insert(modifications[size_t(stage)][program.interpolator_count].begin(),
                                 modifications[size_t(stage)][program.interpolator_count].end());
    // These five FH1 car shaders are paired with fewer live interpolators than
    // their asset metadata declares (same exceptions as the producer).
    if (stage == xenos::ShaderType::kPixel) {
      switch (hash) {
        case 0x1A0763EC031484A5:
        case 0x47B93AEB981C4449:
        case 0x913B2602741D753F:
        case 0xDCE431C38D411674:
          program_modifications.insert(0x000000000016003F);
          break;
        case 0xF0DB93F778565F3C:
          program_modifications.insert(0x000000000016007F);
          break;
        default:
          break;
      }
    }
  }
  failed += read_failed;
  corpus_programs.clear();

  // Runtime variants and recorded misses: extra modifications of shaders the
  // corpus already holds; a recorded miss with an unknown hash is reported.
  // Geometry keys from vertex entries below, plus recorded geometry misses.
  std::set<uint32_t> geometry_keys;
  size_t variant_missing = 0;
  for (const auto& [hash, modification] : kFh1RuntimeVariants) {
    auto it = corpus_programs_by_hash.find(hash);
    if (it == corpus_programs_by_hash.end()) {
      ++variant_missing;
      continue;
    }
    it->second.second.insert(modification);
  }
  for (const auto& [hash, modification] : recorded_misses) {
    if (!hash) {
      // A recorded geometry shader miss: the key is the record.
      geometry_keys.insert(uint32_t(modification));
      continue;
    }
    auto it = corpus_programs_by_hash.find(hash);
    if (it == corpus_programs_by_hash.end()) {
      ++variant_missing;
      continue;
    }
    it->second.second.insert(modification);
  }

  std::vector<std::pair<rex::graphics::d3d12::D3D12Shader*, std::vector<uint64_t>>> jobs;
  jobs.reserve(corpus_programs_by_hash.size());
  for (auto& [hash, program] : corpus_programs_by_hash) {
    jobs.emplace_back(program.first,
                      std::vector<uint64_t>(program.second.begin(), program.second.end()));
  }
  corpus_programs_by_hash.clear();

  const bool bindless_resources = false;
  const bool edram_rov = false;
  const bool gamma_unorm8 = !options.gamma_render_target_as_unorm16;
  const bool msaa_2x = options.msaa_2x;

  std::vector<ProducerEntry> entries;
  std::vector<TranslationFailure> failures;
  std::mutex entries_mutex;
  std::array<std::atomic<size_t>, 2> translated{};
  std::atomic<size_t> rejected{0};

  const auto make_translator = [&]() {
    return std::make_unique<rex::graphics::DxbcShaderTranslator>(
        vendor_id, /*bindless_resources_used=*/false, /*edram_rov_used=*/false, gamma_unorm8,
        msaa_2x, options.scale, options.scale, /*force_emit_source_map=*/false, target_sm_50);
  };

  std::atomic<size_t> next_job{0};
  const auto worker = [&](size_t worker_index) {
    (void)worker_index;
    std::unique_ptr<rex::graphics::DxbcShaderTranslator> translator = make_translator();
    rex::string::StringBuffer disasm_buffer;
    std::vector<ProducerEntry> local_entries;
    std::vector<TranslationFailure> local_failures;
    for (size_t job = next_job.fetch_add(1, std::memory_order_relaxed); job < jobs.size();
         job = next_job.fetch_add(1, std::memory_order_relaxed)) {
      auto& [shader, shader_modifications] = jobs[job];
      if (!shader->is_ucode_analyzed()) {
        shader->AnalyzeUcode(disasm_buffer);
      }
      for (uint64_t modification_value : shader_modifications) {
        const GraphicsShaderStage stage = shader->type() == xenos::ShaderType::kVertex
                                              ? GraphicsShaderStage::kVertex
                                              : GraphicsShaderStage::kPixel;
        auto* translation = static_cast<rex::graphics::d3d12::D3D12Shader::D3D12Translation*>(
            shader->GetOrCreateTranslation(modification_value));
        if (translation->is_translated()) {
          continue;
        }
        const bool ok = translator->TranslateAnalyzedShader(*translation);
        if (!ok || !translation->is_valid() || translation->translated_binary().empty()) {
          std::string reason;
          for (const auto& error : translation->errors()) {
            reason += (reason.empty() ? "" : "; ");
            reason += error.message;
          }
          if (reason.empty()) {
            reason = "translation failed";
          }
          local_failures.push_back(
              {stage, shader->ucode_data_hash(), modification_value, std::move(reason)});
          continue;
        }
        // The capture path's own consistency gate: the used texture mask must
        // be exactly the fetch constants the bindings reference.
        uint32_t observed_texture_mask = 0;
        bool binding_valid = true;
        for (const auto& binding : shader->GetTextureBindingsAfterTranslation()) {
          if (binding.fetch_constant >= 32 || uint32_t(binding.dimension) > 3 ||
              binding.is_signed > 1) {
            binding_valid = false;
            break;
          }
          observed_texture_mask |= 1u << binding.fetch_constant;
        }
        if (!binding_valid ||
            observed_texture_mask != shader->GetUsedTextureMaskAfterTranslation()) {
          local_failures.push_back({stage, shader->ucode_data_hash(), modification_value,
                                    "texture binding layout inconsistent with used texture mask"});
          continue;
        }
        ProducerEntry entry;
        entry.stage = stage;
        entry.guest_hash = shader->ucode_data_hash();
        entry.specialization_mask = modification_value;
        const std::vector<uint8_t>& bytecode = translation->translated_binary();
        if (bytecode.size() > kMaximumBytecodeBytes) {
          local_failures.push_back({stage, entry.guest_hash, modification_value,
                                    "bytecode exceeds " + std::to_string(kMaximumBytecodeBytes) +
                                        " bytes"});
          continue;
        }
        // The capture path's own gate: the Direct3D backends emit DXBC.
        if (bytecode.size() < 4 || std::memcmp(bytecode.data(), "DXBC", 4) != 0) {
          local_failures.push_back({stage, entry.guest_hash, modification_value,
                                    "translation did not produce a DXBC container"});
          continue;
        }
        entry.bytecode.assign(bytecode.begin(), bytecode.end());
        for (const auto& binding : shader->GetTextureBindingsAfterTranslation()) {
          entry.texture_bindings.push_back({binding.bindless_descriptor_index,
                                            binding.fetch_constant,
                                            uint32_t(binding.dimension),
                                            binding.is_signed ? 1u : 0u});
        }
        for (const auto& binding : shader->GetSamplerBindingsAfterTranslation()) {
          entry.sampler_bindings.push_back({binding.bindless_descriptor_index,
                                            binding.fetch_constant, uint32_t(binding.mag_filter),
                                            uint32_t(binding.min_filter),
                                            uint32_t(binding.mip_filter),
                                            uint32_t(binding.aniso_filter)});
        }
        entry.used_texture_mask = shader->GetUsedTextureMaskAfterTranslation();
        if (entry.texture_bindings.size() > kMaximumBindings ||
            entry.sampler_bindings.size() > kMaximumBindings) {
          local_failures.push_back({stage, entry.guest_hash, modification_value,
                                    "binding count exceeds the pack limit"});
          continue;
        }
        local_entries.push_back(std::move(entry));
        translated[size_t(shader->type())].fetch_add(1, std::memory_order_relaxed);
      }
    }
    std::lock_guard<std::mutex> lock(entries_mutex);
    for (ProducerEntry& entry : local_entries) {
      if (entries.size() >= kMaximumEntries) {
        ++rejected;
        failures.push_back({entry.stage, entry.guest_hash, entry.specialization_mask,
                            "entry limit reached"});
        continue;
      }
      entries.push_back(std::move(entry));
    }
    for (TranslationFailure& failure : local_failures) {
      failures.push_back(std::move(failure));
    }
  };
  {
    std::vector<std::thread> threads;
    for (size_t i = 1; i < std::min(options.thread_count, jobs.size()); ++i) {
      threads.emplace_back(worker, i);
    }
    worker(0);
    for (std::thread& thread : threads) {
      thread.join();
    }
  }

  // Geometry shaders: the keys the pipelines drawn with the translated vertex
  // shaders can take (same enumeration and gating as the producer's capture
  // step: GetGeometryShaderKey parity - it has no failure mode beyond kNone).
  for (ProducerEntry& entry : entries) {
    if (entry.stage != GraphicsShaderStage::kVertex) {
      continue;
    }
    rex::graphics::DxbcShaderTranslator::Modification vertex_modification(
        entry.specialization_mask);
    if (vertex_modification.vertex.host_vertex_shader_type !=
        rex::graphics::Shader::HostVertexShaderType::kVertex) {
      continue;
    }
    for (rex::graphics::GeometryShaderType type :
         {rex::graphics::GeometryShaderType::kPointList,
          rex::graphics::GeometryShaderType::kRectangleList,
          rex::graphics::GeometryShaderType::kQuadList}) {
      const bool points = type == rex::graphics::GeometryShaderType::kPointList;
      if (!points && vertex_modification.vertex.output_point_size) {
        continue;
      }
      for (uint32_t point_coordinates = 0; point_coordinates <= uint32_t(points);
           ++point_coordinates) {
        rex::graphics::GeometryShaderKey key;
        key.type = type;
        key.interpolator_count =
            rex::bit_count(vertex_modification.vertex.interpolator_mask);
        key.user_clip_plane_count = vertex_modification.vertex.user_clip_plane_count;
        key.user_clip_plane_cull = vertex_modification.vertex.user_clip_plane_cull;
        key.has_vertex_kill_and = vertex_modification.vertex.vertex_kill_and;
        key.has_point_size = vertex_modification.vertex.output_point_size;
        key.has_point_coordinates = point_coordinates;
        key.point_ps_ucp_mode = vertex_modification.vertex.point_ps_ucp_mode;
        geometry_keys.insert(key.key);
      }
    }
  }
  size_t geometry_entries = 0;
  for (uint32_t key_value : geometry_keys) {
    rex::graphics::GeometryShaderKey key;
    key.key = key_value;
    std::vector<uint32_t> shader;
    rex::graphics::CreateDxbcGeometryShader(key, shader, target_sm_50);
    const size_t shader_bytes = shader.size() * sizeof(uint32_t);
    if (shader_bytes < 4 || std::memcmp(shader.data(), "DXBC", 4) != 0) {
      failures.push_back({GraphicsShaderStage::kGeometry, 0, key_value,
                          "geometry shader generation produced no DXBC container"});
      continue;
    }
    if (entries.size() >= kMaximumEntries) {
      ++rejected;
      failures.push_back({GraphicsShaderStage::kGeometry, 0, key_value, "entry limit reached"});
      continue;
    }
    ProducerEntry entry;
    entry.stage = GraphicsShaderStage::kGeometry;
    entry.guest_hash = 0;
    entry.specialization_mask = key_value;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(shader.data());
    entry.bytecode.assign(bytes, bytes + shader_bytes);
    entries.push_back(std::move(entry));
    ++geometry_entries;
  }

  // Write the capture the existing pack build consumes.
  std::sort(failures.begin(), failures.end(),
            [](const TranslationFailure& left, const TranslationFailure& right) {
              if (left.stage != right.stage) {
                return left.stage < right.stage;
              }
              if (left.guest_hash != right.guest_hash) {
                return left.guest_hash < right.guest_hash;
              }
              return left.specialization_mask != right.specialization_mask
                         ? left.specialization_mask < right.specialization_mask
                         : left.reason < right.reason;
            });
  std::string write_error;
  if (!WriteCapture(options.output_root, rex::graphics::DxbcShaderTranslator::Modification::kVersion,
                    options.backend == "d3d11"
                        ? uint32_t(rex::graphics::Fh1ShaderPack::Backend::kD3D11)
                        : uint32_t(rex::graphics::Fh1ShaderPack::Backend::kD3D12),
                    options.backend.c_str(), device_features, bindless_resources, edram_rov,
                    gamma_unorm8, msaa_2x, options.scale, options.scale, entries,
                    write_error)) {
    std::fprintf(stderr, "error: %s\n", write_error.c_str());
    return 1;
  }

  // Exact offending-hash log (Step D: never silently emit wrong code; every
  // unrepresented shader is listed, everything else is translated).
  if (!failures.empty()) {
    const std::filesystem::path failure_path =
        options.output_root / "translation-failures.txt";
    std::ofstream failure_file(failure_path, std::ios::binary | std::ios::trunc);
    for (const TranslationFailure& failure : failures) {
      const std::string line = std::string(StageName(failure.stage)) + " " +
                               Hex64(failure.guest_hash) + "/" +
                               Hex64(failure.specialization_mask) + ": " + failure.reason + "\n";
      std::fputs(line.c_str(), stdout);
      failure_file.write(line.data(), std::streamsize(line.size()));
    }
  }

  std::printf("FH1 disc corpus translated %zu vertex and %zu pixel shader variants with %zu "
              "failures\n",
              translated[size_t(xenos::ShaderType::kVertex)].load(),
              translated[size_t(xenos::ShaderType::kPixel)].load(), failures.size());
  std::printf("geometry shader entries: %zu (from %zu keys)\n", geometry_entries,
              geometry_keys.size());
  std::printf("runtime variant pairings not in the corpus: %zu\n", variant_missing);
  std::printf("rejected entries: %zu\n", rejected.load());
  std::printf("output: %s\n", (options.output_root / "shader-manifest.json").string().c_str());
  std::printf("output: %s (%zu entries)\n", (options.output_root / "dxil.blob").string().c_str(),
              entries.size());
  return 0;
}
