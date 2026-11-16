# rexgpu-fh1: Forza Horizon 1-specific D3D12 renderer
# Namespace: rex::graphics
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this
# target. The Xenos format layer is compiled into rexcore.

set(REXGPU_XENOS_SOURCES
    plugin_main.cpp

    # Core GPU types
    flags.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    format/ucode.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    d3d_census_decoder.cpp
    d3d_draw_path.cpp
    d3d_list_compiler.cpp
    d3d_shader_cache.cpp
    fh1_depth_overwrite.cpp
    fh1_edram_resolve.cpp
    fh1_frame_dump.cpp
    fh1_frame_replay.cpp
    fh1_shader_pack.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    primitive_processor.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/texture/replacement.cpp
    # Xenos EDRAM render target cache base (used by the Vulkan cache and the
    # D3D11 render target cache; the D3D12 FH1 native executor owns its render
    # targets).
    pipeline/render_target/cache.cpp
    cube_lut.cpp
    pipeline/shader/interpreter.cpp

    # Null backend (gpu_backend=null): packets without a device
    null/graphics_system.cpp
)

set(REXGPU_FH1_SHADER_ANALYSIS_SOURCES
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGPU_XENOS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/fh1_frame_dump.cpp
    vulkan/fh1_native_executor.cpp
    vulkan/fh1_camera.cpp
    vulkan/command_processor.cpp
    vulkan/scene_post.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shader_overrides.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if(REXGLUE_USE_D3D12)
    # DXBC checksum: needed by the translator and the geometry shader
    # generator (the Direct3D 11 runtime generates geometry shaders itself,
    # Direct3D 12 runtimes take them from the shader pack).
    set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # The runtime needs the DXBC container and the geometry shader generator,
    # but guest translation is confined to the explicit offline producer. The
    # runtime uses only the translator header's register layout, modification
    # bits and system constants.
    set(REXGPU_XENOS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_geometry_shaders.cpp
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )
    set(REXGPU_FH1_SHADER_PRODUCER_SOURCES
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_om.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
    )

    # D3D12 GPU backend
    set(REXGPU_XENOS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/host_render_config.cpp
        d3d12/primitive_processor.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/fh1_frame_census.cpp
        d3d12/fh1_native_executor.cpp
        d3d12/fh1_frame_dump.cpp
        d3d12/pipeline_cache.cpp
    )
endif()

# D3D11 backend (Phase 3: real draws; guest shaders are translated at run
# time like on Vulkan - there is no prebuilt Direct3D 11 shader pack yet)
if(REXGLUE_USE_D3D11)
    set(REXGPU_XENOS_D3D11_SOURCES
        d3d11/graphics_system.cpp
        d3d11/command_list.cpp
        d3d11/command_list_immediate_sink.cpp
        d3d11/command_processor.cpp
        d3d11/fh1_native_executor.cpp
        d3d11/fh1_native_resolve.cpp
        d3d11/fh1_native_surfaces.cpp
        d3d11/host_render_config.cpp
        d3d11/pipeline_cache.cpp
        d3d11/primitive_processor.cpp
        d3d11/render_target_cache.cpp
        d3d11/resource_state_tracker.cpp
        d3d11/shader.cpp
        d3d11/shared_memory.cpp
        d3d11/shared_memory_upload_device.cpp
        d3d11/submission_queue.cpp
        d3d11/texture_cache.cpp
        # The DXBC container and the geometry shader generator, plus the
        # translator the run-time translation drives.
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_geometry_shaders.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_om.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
        # Runtime translation drives ucode analysis (Shader::AnalyzeUcode in
        # translator.cpp) - the D3D11 backend has no offline analysis
        # catalog or prebuilt pack yet.
        ${REXGPU_FH1_SHADER_ANALYSIS_SOURCES}
    )
endif()

if(REXGLUE_USE_VULKAN)
    # The Vulkan backend translates shaders at runtime (no prebuilt pack), so
    # it carries the translator and ucode analysis the D3D12 path only needs
    # in the pack producer.
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_SPIRV_SOURCES} ${REXGPU_XENOS_VULKAN_SOURCES}
        ${REXGPU_FH1_SHADER_ANALYSIS_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
endif()

if(REXGLUE_USE_D3D11)
    # The D3D11 backend translates shaders at run time (no prebuilt pack), so
    # it carries ucode analysis like the Vulkan path.
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_D3D11_SOURCES}
        ${REXGPU_FH1_SHADER_ANALYSIS_SOURCES})
endif()

# Source groups above overlap when multiple Direct3D backends are enabled.
list(REMOVE_DUPLICATES REXGPU_XENOS_SOURCES)

add_library(rexgpu-fh1 SHARED ${REXGPU_XENOS_SOURCES})
add_library(rex::gpu-fh1 ALIAS rexgpu-fh1)
if(REXGLUE_USE_D3D12)
    set(REXGPU_FH1_PRODUCER_SOURCES ${REXGPU_XENOS_SOURCES}
        ${REXGPU_FH1_SHADER_ANALYSIS_SOURCES} ${REXGPU_FH1_SHADER_PRODUCER_SOURCES}
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES})
    list(REMOVE_DUPLICATES REXGPU_FH1_PRODUCER_SOURCES)
    add_library(rexgpu-fh1-producer SHARED EXCLUDE_FROM_ALL ${REXGPU_FH1_PRODUCER_SOURCES})
    target_compile_definitions(rexgpu-fh1-producer PRIVATE
        REXGPU_FH1_SHADER_PRODUCER=1)

    # Headless offline pack producer console tool (tools/fh1_pack_producer_
    # main.cpp): the offline producer's translation sources plus a console
    # driver - the DLL exports only its plugin entry, so the tool compiles the
    # sources directly (minus the plugin glue).
    set(REXGPU_FH1_PACK_PRODUCER_SOURCES ${REXGPU_FH1_PRODUCER_SOURCES})
    list(REMOVE_ITEM REXGPU_FH1_PACK_PRODUCER_SOURCES plugin_main.cpp)
    add_executable(rexgpu-fh1-pack-producer EXCLUDE_FROM_ALL
        tools/fh1_pack_producer_main.cpp ${REXGPU_FH1_PACK_PRODUCER_SOURCES})
    target_compile_definitions(rexgpu-fh1-pack-producer PRIVATE
        REXGPU_FH1_SHADER_PRODUCER=1)
endif()

foreach(rexgpu_fh1_target IN ITEMS rexgpu-fh1 rexgpu-fh1-producer rexgpu-fh1-pack-producer)
    if(NOT TARGET ${rexgpu_fh1_target})
        continue()
    endif()
    set_target_properties(${rexgpu_fh1_target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    target_include_directories(${rexgpu_fh1_target} PRIVATE
        ${REXGLUE_ROOT}/thirdparty/renderdoc
        ${REXGLUE_ROOT})
    target_compile_options(${rexgpu_fh1_target} PRIVATE -Wno-unused-variable)
    # presenter.h's class layout depends on REX_HAS_FIDELITYFX_SDK, which rexui
    # gives its users; the plugin does not link rexui, so it must set it too
    # to see the Presenter the runtime built.
    target_compile_definitions(${rexgpu_fh1_target} PRIVATE REX_HAS_FIDELITYFX_SDK=1)
    rex_target_pch(${rexgpu_fh1_target}
        <mutex>
        <unordered_map>
        <rex/math.h>
        <rex/memory.h>
        <rex/thread/mutex.h>
        <rex/system/xmemory.h>
        <rex/graphics/xenos.h>
        <rex/graphics/registers.h>
        <rex/graphics/format/ucode.h>
        <rex/graphics/pipeline/shader/shader.h>)
    target_link_libraries(${rexgpu_fh1_target} PRIVATE
        rexruntime
        xxHash::xxhash)
    if(WIN32)
        target_link_libraries(${rexgpu_fh1_target} PRIVATE bcrypt)
    endif()
    if(REXGLUE_USE_D3D12)
        target_link_libraries(${rexgpu_fh1_target} PRIVATE d3d12 dxgi dxguid)
    endif()
    if(REXGLUE_USE_D3D11)
        target_link_libraries(${rexgpu_fh1_target} PRIVATE d3d11 dxgi dxguid)
    endif()
endforeach()
# The D3D11 backend translates guest shaders at runtime (no prebuilt pack),
# so it needs the same translator gate the Vulkan backend uses.
if(REXGLUE_USE_D3D11)
    foreach(rexgpu_fh1_target IN ITEMS rexgpu-fh1 rexgpu-fh1-producer)
        if(NOT TARGET ${rexgpu_fh1_target})
            continue()
        endif()
        target_compile_definitions(${rexgpu_fh1_target} PRIVATE REXGPU_SHADER_TRANSLATOR=1)
    endforeach()
endif()
set_target_properties(rexgpu-fh1 PROPERTIES EXPORT_NAME gpu-fh1)

# Symbols for profiling the optimized plugin: a RelWithDebInfo plugin links
# its own runtime and does not load into a Release executable, so this keeps
# the Release build and only adds CodeView debug info and a PDB.
option(REXGPU_FH1_DEBUG_INFO "Build rexgpu-fh1 with a PDB in every configuration" OFF)
if(REXGPU_FH1_DEBUG_INFO)
    target_compile_options(rexgpu-fh1 PRIVATE -g -gcodeview)
    target_link_options(rexgpu-fh1 PRIVATE -g)
endif()
# ThinLTO within the plugin in Release: the recorder's per-draw path crosses
# many small functions in the command processor, texture cache and executor
# units (4.3 % less recorder time a draw on the 1x race start).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_rexgpu_fh1_lto_default ON)
else()
    set(_rexgpu_fh1_lto_default OFF)
endif()
option(REXGPU_FH1_LTO "Build rexgpu-fh1 with ThinLTO in Release" ${_rexgpu_fh1_lto_default})
if(REXGPU_FH1_LTO)
    target_compile_options(rexgpu-fh1 PRIVATE $<$<CONFIG:Release>:-flto=thin>)
    target_link_options(rexgpu-fh1 PRIVATE $<$<CONFIG:Release>:-flto=thin>)
endif()

if(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES)
    set_source_files_properties(${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# Everything PRIVATE: the plugin has no consumers. Never link the OBJECT
# libraries (rexcore/rexui) here; rexruntime already contains those objects
# and relinking them would duplicate global state.
if(REXGLUE_USE_VULKAN)
    # Runtime shader translation (translator.cpp keeps it out of D3D12-only
    # runtimes, which load prebuilt packs).
    target_compile_definitions(rexgpu-fh1 PRIVATE REXGPU_SHADER_TRANSLATOR=1)
    # The offline producer is built from the plugin's sources, which then
    # include the Vulkan backend and the SPIR-V translator too - and so is the
    # console pack producer, since it compiles the same source list.
    foreach(rexgpu_fh1_target IN ITEMS rexgpu-fh1 rexgpu-fh1-producer rexgpu-fh1-pack-producer)
        if(NOT TARGET ${rexgpu_fh1_target})
            continue()
        endif()
        target_link_libraries(${rexgpu_fh1_target} PRIVATE
            Vulkan::Headers
            glslang::SPIRV
            GPUOpen::VulkanMemoryAllocator
            glslang
            MachineIndependent
            GenericCodeGen
            OSDependent
            OGLCompiler
        )
    endforeach()
endif()
