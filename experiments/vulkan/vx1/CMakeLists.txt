# VX1 experiment (docs/roadmap/alcedo_studio/edit/vulkan_compute_backend_plan.md, section 6).
# Stand-alone: it does not join the product build. Experiment code: it does not merge.
cmake_minimum_required(VERSION 3.24)
project(alcedo_vx1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{VULKAN_SDK})
  message(FATAL_ERROR "VULKAN_SDK is not set")
endif()
file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VULKAN_SDK)
set(ALCEDO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../alcedo_studio/src" CACHE PATH "alcedo_studio/src")
set(VX1_OPENCL_ROOT "" CACHE PATH "Prefix with include/CL/cl.h and lib/OpenCL.lib")
set(SLANGC "${VULKAN_SDK}/Bin/slangc.exe")

set(VX1_SLANG_FLAGS -I "${ALCEDO_SRC}/include" -target spirv -profile spirv_1_6
                    -fvk-use-scalar-layout -fvk-use-entrypoint-name -O2)
set(VX1_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${VX1_SPV_DIR}")
set(VX1_SHARED_HEADERS
    "${ALCEDO_SRC}/include/color/color_encoding_math.h"
    "${ALCEDO_SRC}/include/edit/runtime/aces_reference_gamut_compression.h"
    "${ALCEDO_SRC}/include/edit/runtime/film/film_print_math.h")

add_custom_command(
  OUTPUT "${VX1_SPV_DIR}/primary_grade.spv"
  COMMAND "${SLANGC}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/primary_grade.slang" ${VX1_SLANG_FLAGS}
          -entry primary_grade_pointwise_rgba32f -o "${VX1_SPV_DIR}/primary_grade.spv"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/primary_grade.slang" ${VX1_SHARED_HEADERS}
  COMMENT "slangc primary_grade.slang")
add_custom_command(
  OUTPUT "${VX1_SPV_DIR}/film_print.spv"
  COMMAND "${SLANGC}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/film_print.slang" ${VX1_SLANG_FLAGS}
          -entry film_print_exposure -entry film_print_first_amounts -entry film_print_display
          -o "${VX1_SPV_DIR}/film_print.spv"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/film_print.slang" ${VX1_SHARED_HEADERS}
  COMMENT "slangc film_print.slang")
add_custom_target(vx1_shaders ALL DEPENDS "${VX1_SPV_DIR}/primary_grade.spv"
                                          "${VX1_SPV_DIR}/film_print.spv")

add_executable(vx1_runner host/vx1_runner.cpp)
add_dependencies(vx1_runner vx1_shaders)
target_include_directories(vx1_runner PRIVATE "${VULKAN_SDK}/Include" "${VX1_OPENCL_ROOT}/include"
                                              "${ALCEDO_SRC}/include")
target_link_libraries(vx1_runner PRIVATE "${VULKAN_SDK}/Lib/vulkan-1.lib"
                                         "${VX1_OPENCL_ROOT}/lib/OpenCL.lib")
target_compile_definitions(vx1_runner PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  # /fp:precise is the MSVC default: no contraction of a * b + c into an FMA.
  target_compile_options(vx1_runner PRIVATE /W4 /utf-8 /fp:precise)
endif()
