cmake_minimum_required(VERSION 3.24)

project(RifeOFX VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

if(NOT WIN32)
  message(FATAL_ERROR "RifeOFX prototype currently targets Windows only.")
endif()

include(FetchContent)

option(RIFE_ENABLE_INFERENCE
       "Enable NCNN/Vulkan inference inside the OFX render action"
       OFF)

set(RIFE_BUNDLE_MODELS_ROOT "" CACHE PATH
    "Optional external model root to copy into the release OFX bundle")

# Official OpenFX API headers. The C API is header-only for this prototype;
# the C++ support library is deliberately not linked yet.
set(OPENFX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openfx" CACHE PATH
    "Path to a checked-out AcademySoftwareFoundation/openfx tree")

if(EXISTS "${OPENFX_SOURCE_DIR}/include/ofxCore.h")
  set(OPENFX_INCLUDE_DIR "${OPENFX_SOURCE_DIR}/include")
  message(STATUS "Using local OpenFX SDK: ${OPENFX_SOURCE_DIR}")
else()
  message(STATUS "OpenFX SDK not found in third_party/openfx; fetching official tag OFX_Release_1.5.1")
  FetchContent_Declare(
    openfx
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openfx.git
    GIT_TAG        OFX_Release_1.5.1
    GIT_SHALLOW    TRUE
  )
  # Only consume the official headers. Building the OpenFX repository itself
  # would pull optional host/example dependencies (OpenGL, Expat, ...).
  FetchContent_Populate(openfx)
  set(OPENFX_INCLUDE_DIR "${openfx_SOURCE_DIR}/include")
endif()

if(RIFE_ENABLE_INFERENCE)
  find_package(Vulkan REQUIRED)
  find_package(Threads REQUIRED)

  set(RIFE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rife" CACHE PATH
      "Path to nihui/rife-ncnn-vulkan")
  if(NOT EXISTS "${RIFE_SOURCE_DIR}/src/rife.h" OR
     NOT EXISTS "${RIFE_SOURCE_DIR}/src/ncnn/CMakeLists.txt")
    message(FATAL_ERROR
      "RIFE backend not found. Clone nihui/rife-ncnn-vulkan with submodules into ${RIFE_SOURCE_DIR}.")
  endif()

  # Build only the NCNN library from the RIFE submodule. The upstream
  # executable, image codecs and CLI are not linked into the OFX plugin.
  set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
  set(NCNN_VULKAN_ONLINE_SPIRV ON CACHE BOOL "" FORCE)
  set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
  set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(NCNN_INSTALL_SDK OFF CACHE BOOL "" FORCE)
  set(NCNN_DISABLE_RTTI OFF CACHE BOOL "" FORCE)
  set(NCNN_DISABLE_EXCEPTION OFF CACHE BOOL "" FORCE)
  add_subdirectory("${RIFE_SOURCE_DIR}/src/ncnn" "${CMAKE_BINARY_DIR}/ncnn-build" EXCLUDE_FROM_ALL)

  set(RIFE_SHADER_DIR "${CMAKE_BINARY_DIR}/rife-shaders")
  set(RIFE_SHADER_NAMES
    rife_preproc rife_postproc rife_preproc_tta rife_postproc_tta
    rife_flow_tta_avg rife_v2_flow_tta_avg rife_v4_flow_tta_avg
    rife_flow_tta_temporal_avg rife_v2_flow_tta_temporal_avg
    rife_v4_flow_tta_temporal_avg rife_out_tta_temporal_avg
    rife_v4_timestep rife_v4_timestep_tta warp warp_pack4 warp_pack8)
  set(RIFE_SHADER_HEADERS)
  foreach(shader_name IN LISTS RIFE_SHADER_NAMES)
    set(shader_source "${RIFE_SOURCE_DIR}/src/${shader_name}.comp")
    set(shader_header "${RIFE_SHADER_DIR}/${shader_name}.comp.hex.h")
    add_custom_command(
      OUTPUT "${shader_header}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${RIFE_SHADER_DIR}"
      COMMAND ${CMAKE_COMMAND}
              "-DSHADER_SRC=${shader_source}"
              "-DSHADER_COMP_HEADER=${shader_header}"
              -P "${RIFE_SOURCE_DIR}/src/generate_shader_comp_header.cmake"
      DEPENDS "${shader_source}"
      VERBATIM)
    list(APPEND RIFE_SHADER_HEADERS "${shader_header}")
  endforeach()
  add_custom_target(RifeShaders DEPENDS ${RIFE_SHADER_HEADERS})
endif()

set(RIFE_PLUGIN_SOURCES
  src/CadenceMapping.h
  src/CadenceMapping.cpp
  src/TemporalMapping.h
  src/TemporalMapping.cpp
  src/DebugLog.h
  src/DebugLog.cpp
  src/PluginMain.cpp
  src/ModelRegistry.cpp
  src/TemporalFrameProvider.cpp
)

# The output-time -> source-frame mapping is pure and host independent, so it is
# covered without Resolve, Vulkan or an OFX host in the loop.
add_executable(TemporalMappingTest
  src/TemporalMappingTest.cpp
  src/TemporalMapping.cpp)
target_compile_features(TemporalMappingTest PRIVATE cxx_std_17)
target_include_directories(TemporalMappingTest PRIVATE "${OPENFX_INCLUDE_DIR}")
add_test(NAME TemporalMappingTest COMMAND TemporalMappingTest)

# Cadence ratio, phase calibration and the arithmetic mapping, including a walk
# over 60 consecutive timeline frames.
add_executable(CadenceMappingTest
  src/CadenceMappingTest.cpp
  src/CadenceMapping.cpp)
target_compile_features(CadenceMappingTest PRIVATE cxx_std_17)
add_test(NAME CadenceMappingTest COMMAND CadenceMappingTest)
if(RIFE_ENABLE_INFERENCE)
  list(APPEND RIFE_PLUGIN_SOURCES
    src/RifeEngine.cpp
    src/RifeNcnnBackend.cpp
    "${RIFE_SOURCE_DIR}/src/rife.cpp"
    "${RIFE_SOURCE_DIR}/src/warp.cpp")
endif()
add_library(RifeFrameInterpolator SHARED ${RIFE_PLUGIN_SOURCES})

if(RIFE_ENABLE_INFERENCE)
  add_executable(RifeSmokeTest
    src/RifeSmokeTest.cpp
    src/ModelRegistry.cpp
    src/DebugLog.cpp
    src/RifeEngine.cpp
    src/RifeNcnnBackend.cpp
    "${RIFE_SOURCE_DIR}/src/rife.cpp"
    "${RIFE_SOURCE_DIR}/src/warp.cpp")
  add_dependencies(RifeFrameInterpolator RifeShaders)
  add_dependencies(RifeSmokeTest RifeShaders)
endif()

target_compile_features(RifeFrameInterpolator PRIVATE cxx_std_17)
target_compile_definitions(RifeFrameInterpolator PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  RIFE_ENABLE_INFERENCE=$<BOOL:${RIFE_ENABLE_INFERENCE}>
)
target_include_directories(RifeFrameInterpolator PRIVATE
  "${OPENFX_INCLUDE_DIR}"
)
if(RIFE_ENABLE_INFERENCE)
  target_include_directories(RifeFrameInterpolator PRIVATE
    "${RIFE_SOURCE_DIR}/src"
    "${RIFE_SOURCE_DIR}/src/ncnn/src"
    "${RIFE_SHADER_DIR}")
  target_link_libraries(RifeFrameInterpolator PRIVATE ncnn Vulkan::Vulkan Threads::Threads)

  target_compile_features(RifeSmokeTest PRIVATE cxx_std_17)
  target_compile_definitions(RifeSmokeTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_include_directories(RifeSmokeTest PRIVATE
    "${OPENFX_INCLUDE_DIR}"
    "${RIFE_SOURCE_DIR}/src"
    "${RIFE_SOURCE_DIR}/src/ncnn/src"
    "${RIFE_SHADER_DIR}")
  target_link_libraries(RifeSmokeTest PRIVATE ncnn Vulkan::Vulkan Threads::Threads)
endif()

set_target_properties(RifeFrameInterpolator PROPERTIES
  OUTPUT_NAME "RifeFrameInterpolator"
  PREFIX ""
  SUFFIX ".ofx"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/RifeFrameInterpolator.ofx.bundle/Contents/Win64")
set(OFX_RESOURCE_DIR "${CMAKE_BINARY_DIR}/RifeFrameInterpolator.ofx.bundle/Contents/Resources/models")

if(RIFE_ENABLE_INFERENCE)
  add_custom_command(TARGET RifeFrameInterpolator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OFX_BUNDLE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:RifeFrameInterpolator>"
            "${OFX_BUNDLE_DIR}/RifeFrameInterpolator.ofx"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OFX_RESOURCE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/models/registry.csv"
            "${OFX_RESOURCE_DIR}/registry.csv"
    VERBATIM)

  if(RIFE_BUNDLE_MODELS_ROOT AND IS_DIRECTORY "${RIFE_BUNDLE_MODELS_ROOT}")
    set(RIFE_BUNDLE_MODEL_IDS
      rife-v4.6 rife-v4.22-lite rife-v4.25-lite rife-v4.25
      rife-v4.26 rife-v4.26-large)
    foreach(model_id IN LISTS RIFE_BUNDLE_MODEL_IDS)
      set(model_source "${RIFE_BUNDLE_MODELS_ROOT}/${model_id}")
      set(model_destination "${OFX_RESOURCE_DIR}/${model_id}")
      if(NOT IS_DIRECTORY "${model_source}")
        message(FATAL_ERROR
          "Requested bundled model is missing: ${model_source}")
      endif()
      add_custom_command(TARGET RifeFrameInterpolator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${model_destination}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${model_source}" "${model_destination}"
        VERBATIM)
    endforeach()
    message(STATUS "Bundling all configured RIFE models from: ${RIFE_BUNDLE_MODELS_ROOT}")
  else()
    set(OFX_MODEL_DIR "${OFX_RESOURCE_DIR}/rife-v4.6")
    add_custom_command(TARGET RifeFrameInterpolator POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${OFX_MODEL_DIR}"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${RIFE_SOURCE_DIR}/models/rife-v4.6"
              "${OFX_MODEL_DIR}"
      VERBATIM)
    message(STATUS "Bundling only the built-in RIFE 4.6 model")
  endif()
else()
  add_custom_command(TARGET RifeFrameInterpolator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OFX_BUNDLE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:RifeFrameInterpolator>"
            "${OFX_BUNDLE_DIR}/RifeFrameInterpolator.ofx"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OFX_RESOURCE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/models/registry.csv"
            "${OFX_RESOURCE_DIR}/registry.csv"
    VERBATIM)
endif()

install(DIRECTORY "${CMAKE_BINARY_DIR}/RifeFrameInterpolator.ofx.bundle"
        DESTINATION ".")

message(STATUS "Bundle output: ${CMAKE_BINARY_DIR}/RifeFrameInterpolator.ofx.bundle")
