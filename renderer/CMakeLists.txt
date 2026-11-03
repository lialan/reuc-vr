add_library(uc_gx STATIC gx_vertices.cpp gx_packets.cpp gx_scene.cpp gx_textures.cpp texture_replacement.cpp gx_transforms.cpp gx_materials.cpp prompt_draw.cpp scene_boundary.cpp motion_tracker.cpp scene_lights.cpp)
target_include_directories(uc_gx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_gx PUBLIC uc_input_prompts)

enable_language(C)
find_package(Vulkan 1.3 REQUIRED)
if(UC_VENDOR_UPSCALERS)
  include(${CMAKE_SOURCE_DIR}/cmake/upscaler_sdks.cmake)
else()
  set(UC_UPSCALER_RUNTIME "" CACHE INTERNAL "")
  set(UC_DLSS5_RUNTIME_FILES "" CACHE INTERNAL "")
endif()
find_program(UC_SLANGC slangc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
include(FetchContent)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(sdl_archive "https://github.com/libsdl-org/SDL/releases/download/release-3.2.0/SDL3-3.2.0.tar.gz")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/SDL3-3.2.0.tar.gz")
  set(sdl_archive "${CMAKE_SOURCE_DIR}/build/SDL3-3.2.0.tar.gz")
endif()
FetchContent_Declare(SDL3 URL "${sdl_archive}"
  URL_HASH SHA256=bf308f92c5688b1479faf5cfe24af72f3cd4ce08d0c0670d6ce55bc2ec1e9a5e
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(SDL3)
if(WIN32)
  include(patch_sdl.cmake)
endif()
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
# Every game vertex stage gets an Invariant position (tools/spirv_invariant_position.py): layered draws over the same vertices test depth
# EQUAL/LEQUAL, and only invariance makes the position bit-identical across the pipelines the driver compiles.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
macro(uc_invariant_step out file)
  set(${out} COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/spirv_invariant_position.py ${file})
endmacro()
foreach(stage vert frag)
  if(stage STREQUAL "vert")
    set(entry vertexMain)
    set(shader_stage vertex)
    uc_invariant_step(invariant ${shader_dir}/smoke.${stage}.spv)
  else()
    set(entry fragmentMain)
    set(shader_stage fragment)
    set(invariant)
  endif()
  add_custom_command(OUTPUT ${shader_dir}/smoke.${stage}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -target spirv -profile spirv_1_5 -entry ${entry} -stage ${shader_stage} -fvk-use-entrypoint-name -o ${shader_dir}/smoke.${stage}.spv
    ${invariant}
    DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
endforeach()
# The headset's vertex stage for draws without per-vertex texture matrices (pipeline key word 0 bit 17 clear).
add_custom_command(OUTPUT ${shader_dir}/smoke.stereo.nobank.vert.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DSTEREO -DTEV_FAST -DNO_BANK -entry vertexMain -stage vertex -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smoke.stereo.nobank.vert.spv
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/spirv_invariant_position.py ${shader_dir}/smoke.stereo.nobank.vert.spv
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/spirv_invariant_position.py shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
# The headset's vertex pre-pass (smoke.slang xformMain): every vertex of a packet shaded once in a compute dispatch, and the draws' vertex
# stage that reads the result (UC_NO_XFORM=1 at run time: the full vertex stage per draw, as before).
add_custom_command(OUTPUT ${shader_dir}/smoke.stereo.xform.comp.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DSTEREO -DTEV_FAST -DXFORM -entry xformMain -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smoke.stereo.xform.comp.spv
  DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
add_custom_command(OUTPUT ${shader_dir}/smoke.stereo.xform.vert.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DSTEREO -DTEV_FAST -DXFORM_DRAW -entry vertexMain -stage vertex -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smoke.stereo.xform.vert.spv
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/spirv_invariant_position.py ${shader_dir}/smoke.stereo.xform.vert.spv
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/spirv_invariant_position.py shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
# Diagnostics (UC_PROBE_VS / UC_PROBE_NOINV / UC_PROBE_FS): the headset's vertex stage without lighting and texgen, without the Invariant
# position, and a fragment stage that outputs the vertex colour; they split the scene pass's GPU time on the Quest.
add_custom_command(OUTPUT ${shader_dir}/smoke.stereo.probe.vert.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DSTEREO -DTEV_FAST -DNO_BANK -DPROBE_VS -entry vertexMain -stage vertex -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smoke.stereo.probe.vert.spv
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/spirv_invariant_position.py ${shader_dir}/smoke.stereo.probe.vert.spv
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/spirv_invariant_position.py shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
add_custom_command(OUTPUT ${shader_dir}/smoke.stereo.nobank.noinv.vert.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DSTEREO -DTEV_FAST -DNO_BANK -entry vertexMain -stage vertex -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smoke.stereo.nobank.noinv.vert.spv
  DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
add_custom_command(OUTPUT ${shader_dir}/smoke.stereo.probe.frag.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DSTEREO -DTEV_FAST -DEARLY_Z -DPROBE_FS -entry fragmentMain -stage fragment -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smoke.stereo.probe.frag.spv
  DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
# Early-Z variants: the specialized fragment shader without the alpha test (pipelines that cannot discard), early fragment tests declared.
foreach(family "" motion stereo)
  if(family STREQUAL "")
    set(early_name smoke.early.frag.spv)
    set(early_defs "")
  else()
    set(early_name smoke.${family}.early.frag.spv)
    string(TOUPPER ${family} early_upper)
    set(early_defs -D${early_upper})
    if(family STREQUAL "stereo")
      list(APPEND early_defs -DTEV_FAST)   # the headset's shaders: float TEV and fog
    endif()
  endif()
  add_custom_command(OUTPUT ${shader_dir}/${early_name}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DEARLY_Z ${early_defs} -target spirv -profile spirv_1_5 -entry fragmentMain -stage fragment -fvk-use-entrypoint-name -o ${shader_dir}/${early_name}
    DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
endforeach()
# Ubershader: the fragment shader with its TEV state read from the draw uniforms; used while a specialized pipeline compiles.
add_custom_command(OUTPUT ${shader_dir}/smoke.uber.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DUBER -target spirv -profile spirv_1_5 -entry fragmentMain -stage fragment -fvk-use-entrypoint-name -o ${shader_dir}/smoke.uber.spv
  DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
# Upscaling variants: jitter in the vertex shader, motion vectors as a second color output.
foreach(stage vert frag uber)
  if(stage STREQUAL "vert")
    set(motion_args -entry vertexMain -stage vertex)
    uc_invariant_step(invariant ${shader_dir}/smoke.motion.${stage}.spv)
  elseif(stage STREQUAL "frag")
    set(motion_args -entry fragmentMain -stage fragment)
  else()
    set(motion_args -DUBER -entry fragmentMain -stage fragment)
  endif()
  if(NOT stage STREQUAL "vert")
    set(invariant)
  endif()
  add_custom_command(OUTPUT ${shader_dir}/smoke.motion.${stage}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DMOTION ${motion_args} -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smoke.motion.${stage}.spv
    ${invariant}
    DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
endforeach()
# Shadow map vertex stage: the motion vertex shader without lighting and texture coordinates (depth only, no fragment stage).
add_custom_command(OUTPUT ${shader_dir}/smoke.shadow.vert.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DMOTION -DSHADOW -entry vertexMain -stage vertex -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smoke.shadow.vert.spv
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/spirv_invariant_position.py ${shader_dir}/smoke.shadow.vert.spv
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/spirv_invariant_position.py shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
add_custom_command(OUTPUT ${shader_dir}/rcas.comp.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rcas.slang -entry main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/rcas.comp.spv
  DEPENDS shaders/rcas.slang VERBATIM)
# SMAA 1x (third_party/smaa, MIT): three compute entry points of one shader.
foreach(smaa_pass edges weights blend)
  add_custom_command(OUTPUT ${shader_dir}/smaa.${smaa_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smaa.slang -I ${CMAKE_SOURCE_DIR}/third_party/smaa -entry ${smaa_pass}Main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smaa.${smaa_pass}.spv
    DEPENDS shaders/smaa.slang ${CMAKE_SOURCE_DIR}/third_party/smaa/SMAA.hlsl VERBATIM)
endforeach()
# Ground-truth ambient occlusion: three compute entry points of one shader.
foreach(gtao_pass ao denoise contact composite)
  add_custom_command(OUTPUT ${shader_dir}/gtao.${gtao_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gtao.slang -entry ${gtao_pass}Main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/gtao.${gtao_pass}.spv
    DEPENDS shaders/gtao.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
endforeach()
# Ray tracing: the BLAS transform pass.
add_custom_command(OUTPUT ${shader_dir}/rt_transform.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rt_transform.slang -entry main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/rt_transform.spv
  DEPENDS shaders/rt_transform.slang VERBATIM)
# The own denoiser of ray-traced signals: temporal accumulation and one à-trous pass, two entry points of one shader.
foreach(denoise_pass temporal atrous)
  add_custom_command(OUTPUT ${shader_dir}/rt_denoise.${denoise_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rt_denoise.slang -entry ${denoise_pass}Main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/rt_denoise.${denoise_pass}.spv
    DEPENDS shaders/rt_denoise.slang shaders/gbuffer.slang VERBATIM)
endforeach()
# Ray-traced shadows: the visibility trace (ray query) and the composite, two entry points of one shader.
foreach(shadow_pass trace composite)
  add_custom_command(OUTPUT ${shader_dir}/rt_shadow.${shadow_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rt_shadow.slang -entry ${shadow_pass}Main -stage compute -target spirv -profile spirv_1_5 -capability spvRayQueryKHR -fvk-use-entrypoint-name -o ${shader_dir}/rt_shadow.${shadow_pass}.spv
    DEPENDS shaders/rt_shadow.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
endforeach()
# Ray-traced reflections: the trace (ray query) and the composite.
foreach(reflect_pass trace composite)
  add_custom_command(OUTPUT ${shader_dir}/rt_reflect.${reflect_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rt_reflect.slang -entry ${reflect_pass}Main -stage compute -target spirv -profile spirv_1_5 -capability spvRayQueryKHR -fvk-use-entrypoint-name -o ${shader_dir}/rt_reflect.${reflect_pass}.spv
    DEPENDS shaders/rt_reflect.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
endforeach()
# Path tracing: the albedo accumulation, the trace (ray query through the hit table) and the composite.
foreach(path_pass accumulate list trace composite)
  add_custom_command(OUTPUT ${shader_dir}/rt_path.${path_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rt_path.slang -entry ${path_pass}Main -stage compute -target spirv -profile spirv_1_5 -capability spvRayQueryKHR -fvk-use-entrypoint-name -o ${shader_dir}/rt_path.${path_pass}.spv
    DEPENDS shaders/rt_path.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
endforeach()
# The guides of DLSS Ray Reconstruction (normal + roughness, specular albedo).
add_custom_command(OUTPUT ${shader_dir}/rr_inputs.spv
  COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
  COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rr_inputs.slang -entry inputsMain -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/rr_inputs.spv
  DEPENDS shaders/rr_inputs.slang shaders/gbuffer.slang shaders/view_space.slang VERBATIM)
# DLSS 5 Neural Rendering: the bridge between the RGBA8 picture and the network's RGBA16F signal, both directions from one source.
foreach(bridge prepare resolve)
  string(TOUPPER ${bridge} bridge_define)
  add_custom_command(OUTPUT ${shader_dir}/dlss5.${bridge}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/dlss5_bridge.slang -D${bridge_define} -entry main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/dlss5.${bridge}.spv
    DEPENDS shaders/dlss5_bridge.slang VERBATIM)
endforeach()
foreach(blur_pass tile blur)
  add_custom_command(OUTPUT ${shader_dir}/motion_blur.${blur_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/motion_blur.slang -entry ${blur_pass}Main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/motion_blur.${blur_pass}.spv
    DEPENDS shaders/motion_blur.slang VERBATIM)
endforeach()
foreach(bloom_pass down_first down up composite)
  if(bloom_pass STREQUAL "down_first")
    set(bloom_entry downFirst)
  else()
    set(bloom_entry ${bloom_pass})
  endif()
  add_custom_command(OUTPUT ${shader_dir}/bloom.${bloom_pass}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/bloom.slang -entry ${bloom_entry}Main -stage compute -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/bloom.${bloom_pass}.spv
    DEPENDS shaders/bloom.slang VERBATIM)
endforeach()
# HDR10 output: the encode of the finished frame into the PQ swapchain.
foreach(stage vert frag)
  if(stage STREQUAL "vert")
    set(hdr_args -entry vertexMain -stage vertex)
  else()
    set(hdr_args -entry fragmentMain -stage fragment)
  endif()
  add_custom_command(OUTPUT ${shader_dir}/hdr.${stage}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hdr.slang ${hdr_args} -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/hdr.${stage}.spv
    DEPENDS shaders/hdr.slang VERBATIM)
endforeach()
# The headset's game shaders (STEREO): multiview, texture arrays, the per-draw eye transforms in binding 5.
foreach(stage vert frag uber)
  if(stage STREQUAL "vert")
    set(stereo_args -entry vertexMain -stage vertex)
    uc_invariant_step(invariant ${shader_dir}/smoke.stereo.${stage}.spv)
  elseif(stage STREQUAL "frag")
    set(stereo_args -entry fragmentMain -stage fragment)
  else()
    set(stereo_args -DUBER -entry fragmentMain -stage fragment)
  endif()
  if(NOT stage STREQUAL "vert")
    set(invariant)
  endif()
  add_custom_command(OUTPUT ${shader_dir}/smoke.stereo.${stage}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/smoke.slang -DSTEREO -DTEV_FAST ${stereo_args} -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/smoke.stereo.${stage}.spv
    ${invariant}
    DEPENDS shaders/smoke.slang shaders/tev.slang shaders/gbuffer.slang VERBATIM)
endforeach()
# The headset's screen pass (xr_compositor.cpp): multiview, both eyes in one pass.
foreach(stage vert frag)
  if(stage STREQUAL "vert")
    set(xr_args -entry vertexMain -stage vertex)
  else()
    set(xr_args -entry fragmentMain -stage fragment)
  endif()
  add_custom_command(OUTPUT ${shader_dir}/xr_screen.${stage}.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
    COMMAND ${UC_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/xr_screen.slang ${xr_args} -target spirv -profile spirv_1_5 -fvk-use-entrypoint-name -o ${shader_dir}/xr_screen.${stage}.spv
    DEPENDS shaders/xr_screen.slang VERBATIM)
endforeach()
# HOTD4 VR's overlay shaders (GLSL, unchanged): glslangValidator, as lindberghrecomp builds them.
find_program(UC_GLSLANG glslangValidator)
# The headset's TEV fragment modules specialized at build time for the known pipeline keys (tools/fold_tev_shaders.py): the Quest's driver
# leaves most of the module in when it specializes. On by default where the headset is built (UC_OPENXR); needs spirv-opt (SPIRV-Tools).
find_program(UC_SPIRV_OPT spirv-opt HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(UC_OPENXR AND UC_SPIRV_OPT)
  set(UC_FOLD_TEV_DEFAULT ON)
else()
  set(UC_FOLD_TEV_DEFAULT OFF)
endif()
option(UC_FOLD_TEV "Build-time specialized TEV fragment modules for the headset" ${UC_FOLD_TEV_DEFAULT})
set(UC_XR_OVERLAY_SPV)
if(UC_GLSLANG)
  foreach(program gun beam panel)
    foreach(stage vert frag)
      add_custom_command(OUTPUT ${shader_dir}/xr_overlay_${program}.${stage}.spv
        COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
        COMMAND ${UC_GLSLANG} -V --target-env vulkan1.3 -o ${shader_dir}/xr_overlay_${program}.${stage}.spv ${CMAKE_CURRENT_SOURCE_DIR}/shaders/xr_overlay_${program}.${stage}
        DEPENDS shaders/xr_overlay_${program}.${stage} VERBATIM)
      list(APPEND UC_XR_OVERLAY_SPV ${shader_dir}/xr_overlay_${program}.${stage}.spv)
    endforeach()
  endforeach()
elseif(UC_OPENXR)
  message(FATAL_ERROR "the headset build needs glslangValidator for HOTD4's overlay shaders (brew install glslang)")
endif()
set(UC_SHADER_FILES ${shader_dir}/smoke.stereo.nobank.vert.spv ${shader_dir}/smoke.stereo.xform.comp.spv ${shader_dir}/smoke.stereo.xform.vert.spv ${shader_dir}/smoke.stereo.probe.vert.spv ${shader_dir}/smoke.stereo.nobank.noinv.vert.spv ${shader_dir}/smoke.stereo.probe.frag.spv ${shader_dir}/smoke.early.frag.spv ${shader_dir}/smoke.motion.early.frag.spv ${shader_dir}/smoke.stereo.early.frag.spv ${UC_XR_OVERLAY_SPV} ${shader_dir}/smoke.stereo.vert.spv ${shader_dir}/smoke.stereo.frag.spv ${shader_dir}/smoke.stereo.uber.spv ${shader_dir}/xr_screen.vert.spv ${shader_dir}/xr_screen.frag.spv ${shader_dir}/hdr.vert.spv ${shader_dir}/hdr.frag.spv ${shader_dir}/smoke.vert.spv ${shader_dir}/smoke.frag.spv ${shader_dir}/smoke.uber.spv
  ${shader_dir}/smoke.motion.vert.spv ${shader_dir}/smoke.motion.frag.spv ${shader_dir}/smoke.motion.uber.spv ${shader_dir}/rcas.comp.spv
  ${shader_dir}/smaa.edges.spv ${shader_dir}/smaa.weights.spv ${shader_dir}/smaa.blend.spv
  ${shader_dir}/smoke.shadow.vert.spv ${shader_dir}/gtao.ao.spv ${shader_dir}/gtao.denoise.spv ${shader_dir}/gtao.contact.spv ${shader_dir}/gtao.composite.spv
  ${shader_dir}/motion_blur.tile.spv ${shader_dir}/motion_blur.blur.spv ${shader_dir}/bloom.down_first.spv ${shader_dir}/bloom.down.spv ${shader_dir}/bloom.up.spv ${shader_dir}/bloom.composite.spv ${shader_dir}/rt_transform.spv
  ${shader_dir}/rt_denoise.temporal.spv ${shader_dir}/rt_denoise.atrous.spv ${shader_dir}/rt_shadow.trace.spv ${shader_dir}/rt_shadow.composite.spv
  ${shader_dir}/rt_reflect.trace.spv ${shader_dir}/rt_reflect.composite.spv ${shader_dir}/rt_path.accumulate.spv ${shader_dir}/rt_path.list.spv ${shader_dir}/rt_path.trace.spv ${shader_dir}/rt_path.composite.spv ${shader_dir}/rr_inputs.spv ${shader_dir}/dlss5.prepare.spv ${shader_dir}/dlss5.resolve.spv CACHE INTERNAL "")
add_custom_target(uc_smoke_shaders DEPENDS ${UC_SHADER_FILES})
if(UC_FOLD_TEV)
  if(NOT UC_SPIRV_OPT)
    message(FATAL_ERROR "UC_FOLD_TEV needs spirv-opt")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(UC_FOLDED_DIR ${shader_dir}/folded CACHE INTERNAL "")
  add_custom_command(OUTPUT ${shader_dir}/folded.stamp
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/fold_tev_shaders.py --spirv-opt ${UC_SPIRV_OPT} --shaders ${shader_dir}
            --out ${shader_dir}/folded ${CMAKE_SOURCE_DIR}/project/pipeline_keys.seed ${CMAKE_SOURCE_DIR}/project/pipeline_keys.quest
    COMMAND ${CMAKE_COMMAND} -E touch ${shader_dir}/folded.stamp
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/fold_tev_shaders.py ${CMAKE_SOURCE_DIR}/project/pipeline_keys.seed ${shader_dir}/smoke.stereo.frag.spv ${shader_dir}/smoke.stereo.early.frag.spv
    VERBATIM)
  add_custom_target(uc_folded_tev DEPENDS ${shader_dir}/folded.stamp)
endif()
# DLSS 5 Neural Rendering: the support/failure/parameter logic above the NGX SDK (no SDK or GPU needed, tested with a fake backend).
add_library(uc_dlss5 STATIC ngx_dlss5.cpp)
target_include_directories(uc_dlss5 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/common)   # common: from_chars_compat.h (dlss5_settings.h)
target_link_libraries(uc_dlss5 PUBLIC Vulkan::Vulkan)
add_library(uc_prompt_icons STATIC prompt_icons.cpp pad_glyphs.cpp)
target_include_directories(uc_prompt_icons PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uc_prompt_icons PUBLIC uc_input_prompts uc_imgui)
add_library(uc_prompt_picture STATIC prompt_picture.cpp)
target_link_libraries(uc_prompt_picture PUBLIC uc_gx uc_prompt_icons)
add_library(uc_vulkan STATIC vulkan_renderer.cpp png.cpp prompt_overlay.cpp menu_overlay.cpp blit_upscaler.cpp smaa_upscaler.cpp occlusion_pass.cpp motion_blur_pass.cpp bloom_pass.cpp rt_scene.cpp rt_denoiser.cpp rt_shadow_pass.cpp rt_reflection_pass.cpp rt_path_pass.cpp rr_inputs_pass.cpp shadow_map.cpp rcas_pass.cpp hdr_pass.cpp dlss5_pass.cpp pass_timer.cpp upscaler_probe.cpp)
if(ANDROID)
  target_sources(uc_vulkan PRIVATE vulkan_android_entry.cpp)
endif()
# HOTD4 VR's camera policy (StereoScene and its projection math): no OpenXR, so the desktop stereo preview (UC_VR_PREVIEW) uses it too.
add_library(uc_stereo STATIC ${PROJECT_SOURCE_DIR}/xr/stereo_projection.c ${PROJECT_SOURCE_DIR}/xr/stereo_scene.cpp)
target_include_directories(uc_stereo PUBLIC ${PROJECT_SOURCE_DIR}/xr)
target_link_libraries(uc_vulkan PUBLIC uc_stereo)
# The headset output (HOTD4 VR's OpenXR host): with the Khronos OpenXR loader (UC_OPENXR_DIR, the unpacked AAR).
if(UC_OPENXR)
  set(OpenXR_DIR ${UC_OPENXR_DIR})   # a <Package>_DIR is used as it is: the NDK toolchain limits searches to its sysroot
  find_package(OpenXR REQUIRED CONFIG)
  add_library(uc_xr STATIC ${PROJECT_SOURCE_DIR}/xr/xr_actions.c
    ${PROJECT_SOURCE_DIR}/xr/xr_controls.c ${PROJECT_SOURCE_DIR}/xr/xr_menu.c ${PROJECT_SOURCE_DIR}/xr/xr_panel.c ${PROJECT_SOURCE_DIR}/xr/xr_settings.c
    ${PROJECT_SOURCE_DIR}/xr/xr_overlay_geometry.c ${PROJECT_SOURCE_DIR}/xr/display_cadence.c ${PROJECT_SOURCE_DIR}/xr/host_stats.c
    ${PROJECT_SOURCE_DIR}/xr/controller_input.cpp ${PROJECT_SOURCE_DIR}/xr/settings_panel.cpp xr_compositor.cpp xr_overlay.cpp)
  target_include_directories(uc_xr PUBLIC ${PROJECT_SOURCE_DIR}/xr ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(uc_xr PUBLIC OpenXR::openxr_loader Vulkan::Vulkan uc_gx uc_stereo)
  target_link_libraries(uc_vulkan PUBLIC uc_xr)
  target_compile_definitions(uc_vulkan PUBLIC UC_OPENXR=1)
endif()
target_include_directories(uc_vulkan PRIVATE ${CMAKE_SOURCE_DIR}/third_party/smaa)   # AreaTex.h, SearchTex.h
target_link_libraries(uc_vulkan PUBLIC uc_gx SDL3::SDL3 Vulkan::Vulkan uc_imgui_vulkan uc_prompt_picture uc_dlss5 PRIVATE uc_public_release)
if(UC_VENDOR_UPSCALERS)
  target_sources(uc_vulkan PRIVATE streamline.cpp dlss_upscaler.cpp dlss_frame_generation.cpp fsr_upscaler.cpp ffx_dx12.cpp fsr_frame_generation.cpp ngx_vulkan.cpp)
  target_link_libraries(uc_vulkan PRIVATE uc_streamline_sdk uc_fidelityfx_sdk uc_ngx_sdk)
  target_compile_definitions(uc_vulkan PRIVATE UC_VENDOR_UPSCALERS=1)
else()
  target_sources(uc_vulkan PRIVATE vendor_stubs.cpp)   # DLSS, FSR and NGX report "not available on this platform"
endif()
if(UC_RENDERER_DIRECTX)
  target_sources(uc_vulkan PRIVATE native_frame_generation.cpp native_dlss5.cpp)
  target_link_libraries(uc_vulkan PRIVATE d3d12 dxgi)
endif()
if(WIN32)
  target_link_libraries(uc_vulkan PUBLIC windowscodecs)
else()
  target_link_libraries(uc_vulkan PRIVATE uc_miniz)   # PNG captures (png.cpp); Windows encodes them with WIC
endif()
add_dependencies(uc_vulkan uc_smoke_shaders)
set_property(TARGET uc_vulkan APPEND PROPERTY INTERFACE_LINK_DEPENDS ${UC_SHADER_FILES})
if(WIN32)
  add_executable(renderer_smoke ${CMAKE_SOURCE_DIR}/tests/renderer_smoke.cpp)
  target_include_directories(renderer_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(renderer_smoke PRIVATE uc_vulkan)
  add_custom_command(TARGET renderer_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:renderer_smoke>/data/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UC_SHADER_FILES} $<TARGET_FILE_DIR:renderer_smoke>/data/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3-shared> $<TARGET_FILE_DIR:renderer_smoke> VERBATIM)
  add_executable(renderer_rollback_smoke ${CMAKE_SOURCE_DIR}/tests/renderer_rollback_smoke.cpp)
  target_include_directories(renderer_rollback_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(renderer_rollback_smoke PRIVATE uc_vulkan)
  add_dependencies(renderer_rollback_smoke renderer_smoke)   # shares its data/shaders and SDL copy
  add_executable(renderer_upscale_smoke ${CMAKE_SOURCE_DIR}/tests/renderer_upscale_smoke.cpp)
  target_include_directories(renderer_upscale_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(renderer_upscale_smoke PRIVATE uc_vulkan)
  add_dependencies(renderer_upscale_smoke renderer_smoke)   # shares its data/shaders and SDL copy
  add_custom_command(TARGET renderer_upscale_smoke POST_BUILD   # --dlss: Streamline and DLSS next to the test
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UC_UPSCALER_RUNTIME} $<TARGET_FILE_DIR:renderer_upscale_smoke> VERBATIM)
  add_executable(renderer_dlss5_smoke ${CMAKE_SOURCE_DIR}/tests/renderer_dlss5_smoke.cpp)
  target_include_directories(renderer_dlss5_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(renderer_dlss5_smoke PRIVATE uc_vulkan)
  add_dependencies(renderer_dlss5_smoke renderer_smoke)   # shares its data/shaders and SDL copy
  add_executable(renderer_effects_smoke ${CMAKE_SOURCE_DIR}/tests/renderer_effects_smoke.cpp)
  target_include_directories(renderer_effects_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(renderer_effects_smoke PRIVATE uc_vulkan)
  add_dependencies(renderer_effects_smoke renderer_smoke)   # shares its data/shaders and SDL copy
  install(TARGETS renderer_smoke RUNTIME DESTINATION tools)
  install(FILES $<TARGET_FILE:SDL3-shared> DESTINATION tools)
  install(FILES ${UC_SHADER_FILES} DESTINATION tools/data/shaders)
endif()
install(FILES ${sdl3_SOURCE_DIR}/LICENSE.txt DESTINATION licenses/SDL3)

if(UC_RENDERER_DIRECTX)
  add_library(uc_directx STATIC d3d11_renderer.cpp d3d12_renderer.cpp native_tev.cpp native_probe.cpp native_rt_scene.cpp native_rt_effects.cpp)
  include(${CMAKE_SOURCE_DIR}/cmake/directx_shaders.cmake)
  set(UC_DIRECTX_SHADER_FILES ${UC_DIRECTX_SHADER_FILES} PARENT_SCOPE)
  add_dependencies(uc_directx uc_directx_shaders)
  target_link_libraries(uc_directx PUBLIC uc_gx SDL3::SDL3 PRIVATE d3d11 d3d12 dxgi d3dcompiler dxguid)
  target_link_libraries(uc_directx PRIVATE uc_imgui_dx11 uc_imgui_dx12 uc_prompt_picture)
  target_link_libraries(uc_directx PRIVATE uc_vulkan)
  target_link_libraries(uc_directx PRIVATE uc_fidelityfx_sdk)
  target_include_directories(uc_directx PRIVATE ${CMAKE_SOURCE_DIR}/third_party/smaa)
  add_executable(native_renderer_smoke ${CMAKE_SOURCE_DIR}/tests/native_renderer_smoke.cpp)
  target_include_directories(native_renderer_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(native_renderer_smoke PRIVATE uc_directx uc_vulkan)
  add_dependencies(native_renderer_smoke renderer_smoke)
  add_custom_command(TARGET native_renderer_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:native_renderer_smoke>/data/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UC_DIRECTX_SHADER_FILES} $<TARGET_FILE_DIR:native_renderer_smoke>/data/shaders VERBATIM)
  set_property(TARGET native_renderer_smoke APPEND PROPERTY LINK_DEPENDS ${UC_DIRECTX_SHADER_FILES})
  add_executable(native_upscale_smoke ${CMAKE_SOURCE_DIR}/tests/native_upscale_smoke.cpp)
  target_include_directories(native_upscale_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(native_upscale_smoke PRIVATE uc_directx uc_vulkan)
  add_dependencies(native_upscale_smoke native_renderer_smoke)
  add_custom_command(TARGET native_upscale_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UC_UPSCALER_RUNTIME} $<TARGET_FILE_DIR:native_upscale_smoke> VERBATIM)
  add_executable(native_game_renderer_smoke ${CMAKE_SOURCE_DIR}/tests/native_game_renderer_smoke.cpp)
  target_include_directories(native_game_renderer_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(native_game_renderer_smoke PRIVATE uc_directx uc_vulkan)
  add_dependencies(native_game_renderer_smoke native_renderer_smoke)
  add_executable(native_frame_generation_smoke ${CMAKE_SOURCE_DIR}/tests/native_frame_generation_smoke.cpp)
  target_link_libraries(native_frame_generation_smoke PRIVATE uc_directx uc_vulkan)
  add_dependencies(native_frame_generation_smoke native_upscale_smoke)
  add_executable(native_rt_smoke ${CMAKE_SOURCE_DIR}/tests/native_rt_smoke.cpp)
  target_include_directories(native_rt_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(native_rt_smoke PRIVATE uc_directx uc_vulkan)
  add_dependencies(native_rt_smoke native_upscale_smoke)
  add_executable(native_dlss5_smoke ${CMAKE_SOURCE_DIR}/tests/native_dlss5_smoke.cpp)
  target_include_directories(native_dlss5_smoke PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(native_dlss5_smoke PRIVATE uc_vulkan d3d12 dxgi)
  add_dependencies(native_dlss5_smoke native_renderer_smoke)
  add_executable(native_dlss5_renderer_smoke ${CMAKE_SOURCE_DIR}/tests/native_dlss5_renderer_smoke.cpp)
  target_link_libraries(native_dlss5_renderer_smoke PRIVATE uc_directx uc_vulkan)
  add_dependencies(native_dlss5_renderer_smoke native_upscale_smoke)
endif()
