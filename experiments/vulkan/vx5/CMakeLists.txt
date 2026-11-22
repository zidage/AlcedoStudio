# VX5 experiment (docs/roadmap/alcedo_studio/edit/vulkan_compute_backend_plan.md, section 6).
# Video memory budget and device loss on the pipeline VkDevice and the display VkDevice of VX2.
# Stand-alone: it does not join the product build. Experiment code: it does not merge.
cmake_minimum_required(VERSION 3.24)
project(alcedo_vx5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT DEFINED ENV{VULKAN_SDK})
  message(FATAL_ERROR "VULKAN_SDK is not set")
endif()
file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VULKAN_SDK)
set(ALCEDO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../alcedo_studio/src" CACHE PATH "alcedo_studio/src")
set(VX2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vx2")
set(VX5_LCMS_ROOT "" CACHE PATH "Prefix with include/lcms2.h and lib/lcms2.lib")
set(SLANGC "${VULKAN_SDK}/Bin/slangc.exe")
set(SPIRV_VAL "${VULKAN_SDK}/Bin/spirv-val.exe")

find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Quick ShaderTools)

# Slang module (plan section 4.3 arguments), validated with spirv-val.
set(VX5_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${VX5_SPV_DIR}")
add_custom_command(
  OUTPUT "${VX5_SPV_DIR}/vx5_mem.spv"
  COMMAND "${SLANGC}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vx5_mem.slang" -target spirv
          -profile spirv_1_6 -fvk-use-scalar-layout -fvk-use-entrypoint-name -O2
          -entry vx5_touch -entry vx5_fault -o "${VX5_SPV_DIR}/vx5_mem.spv"
  COMMAND "${SPIRV_VAL}" --target-env vulkan1.3 --scalar-block-layout "${VX5_SPV_DIR}/vx5_mem.spv"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vx5_mem.slang"
  COMMENT "slangc vx5_mem")
add_custom_target(vx5_shaders ALL DEPENDS "${VX5_SPV_DIR}/vx5_mem.spv")

add_executable(vx5_memory
  host/vx5_main.cpp
  host/vx5_budget.cpp
  host/vx5_budget.hpp
  host/vx5_compute.cpp
  host/vx5_compute.hpp
  "${VX2_DIR}/host/vx2_vulkan.cpp"
  "${VX2_DIR}/host/vx2_vulkan.hpp"
  "${VX2_DIR}/host/vx2_item.cpp"
  "${VX2_DIR}/host/vx2_item.hpp"
  "${VX2_DIR}/host/vx2_display_lut.cpp"
  "${VX2_DIR}/host/vx2_common.hpp")
add_dependencies(vx5_memory vx5_shaders)
target_include_directories(vx5_memory PRIVATE "${VULKAN_SDK}/Include" "${VX5_LCMS_ROOT}/include"
                                              "${VX2_DIR}/host")
target_link_libraries(vx5_memory PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Quick
                                         "${VULKAN_SDK}/Lib/vulkan-1.lib"
                                         "${VX5_LCMS_ROOT}/lib/lcms2.lib" dxgi user32 gdi32)
target_compile_definitions(vx5_memory PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS VK_USE_PLATFORM_WIN32_KHR)
if(MSVC)
  target_compile_options(vx5_memory PRIVATE /W4 /utf-8 /permissive-)
endif()

qt_add_shaders(vx5_memory "vx5_viewport_shaders"
  PREFIX "/vx2"
  BASE "${ALCEDO_SRC}/ui/editor_rhi/shaders"
  NOGLSL NOMSL
  FILES
    "${ALCEDO_SRC}/ui/editor_rhi/shaders/editor_viewport.vert"
    "${ALCEDO_SRC}/ui/editor_rhi/shaders/editor_viewport.frag")
qt_add_shaders(vx5_memory "vx5_compute_shaders"
  PREFIX "/vx2"
  BASE "${VX2_DIR}/shaders"
  NOGLSL NOMSL
  FILES "${VX2_DIR}/shaders/vx2_fill.comp")
