# VX2 experiment (docs/roadmap/alcedo_studio/edit/vulkan_compute_backend_plan.md, section 6).
# Qt Quick on Vulkan with a VkDevice that the program creates, against Qt Quick on Direct3D 11.
# Stand-alone: it does not join the product build. Experiment code: it does not merge.
cmake_minimum_required(VERSION 3.24)
project(alcedo_vx2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT DEFINED ENV{VULKAN_SDK})
  message(FATAL_ERROR "VULKAN_SDK is not set")
endif()
file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VULKAN_SDK)
set(ALCEDO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../alcedo_studio/src" CACHE PATH "alcedo_studio/src")
set(VX2_LCMS_ROOT "" CACHE PATH "Prefix with include/lcms2.h and lib/lcms2.lib")

find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Quick ShaderTools)

add_executable(vx2_viewport
  host/vx2_main.cpp
  host/vx2_vulkan.cpp
  host/vx2_vulkan.hpp
  host/vx2_item.cpp
  host/vx2_item.hpp
  host/vx2_display_lut.cpp
  host/vx2_common.hpp)
target_include_directories(vx2_viewport PRIVATE "${VULKAN_SDK}/Include" "${VX2_LCMS_ROOT}/include")
target_link_libraries(vx2_viewport PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Quick
                                           "${VULKAN_SDK}/Lib/vulkan-1.lib"
                                           "${VX2_LCMS_ROOT}/lib/lcms2.lib" dxgi user32 gdi32)
target_compile_definitions(vx2_viewport PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS
                                                VK_USE_PLATFORM_WIN32_KHR)
if(MSVC)
  target_compile_options(vx2_viewport PRIVATE /W4 /utf-8 /permissive-)
endif()

# The composite shaders of the product viewport, unchanged (ViewportCompositePass).
qt_add_shaders(vx2_viewport "vx2_viewport_shaders"
  PREFIX "/vx2"
  BASE "${ALCEDO_SRC}/ui/editor_rhi/shaders"
  NOGLSL NOMSL
  FILES
    "${ALCEDO_SRC}/ui/editor_rhi/shaders/editor_viewport.vert"
    "${ALCEDO_SRC}/ui/editor_rhi/shaders/editor_viewport.frag")
# The compute shader that writes the photo. One source: the SPIR-V runs on the Vulkan queue of
# the program, the HLSL runs in a QRhi compute pass on Direct3D 11.
qt_add_shaders(vx2_viewport "vx2_compute_shaders"
  PREFIX "/vx2"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
  NOGLSL NOMSL
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vx2_fill.comp")
