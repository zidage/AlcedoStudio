# VX3 experiment (docs/roadmap/alcedo_studio/edit/vulkan_compute_backend_plan.md, section 6).
# Long dispatch on the shared physical device: a 45 megapixel render or a timeout kernel on the
# pipeline VkDevice while the VX2 viewport runs on the display VkDevice.
# Stand-alone: it does not join the product build. Experiment code: it does not merge.
cmake_minimum_required(VERSION 3.24)
project(alcedo_vx3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT DEFINED ENV{VULKAN_SDK})
  message(FATAL_ERROR "VULKAN_SDK is not set")
endif()
file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" VULKAN_SDK)
set(ALCEDO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../alcedo_studio/src" CACHE PATH "alcedo_studio/src")
set(VX2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vx2")
set(VX1_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vx1")
set(VX3_LCMS_ROOT "" CACHE PATH "Prefix with include/lcms2.h and lib/lcms2.lib")
set(SLANGC "${VULKAN_SDK}/Bin/slangc.exe")
set(SPIRV_VAL "${VULKAN_SDK}/Bin/spirv-val.exe")

find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Quick ShaderTools)

# Slang modules (plan section 4.3 arguments), validated with spirv-val.
set(VX3_SLANG_FLAGS -I "${ALCEDO_SRC}/include" -target spirv -profile spirv_1_6
                    -fvk-use-scalar-layout -fvk-use-entrypoint-name -O2)
set(VX3_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${VX3_SPV_DIR}")
set(VX3_SPV_FILES)
function(vx3_slang name source)
  set(entries)
  foreach(entry IN LISTS ARGN)
    list(APPEND entries -entry ${entry})
  endforeach()
  add_custom_command(
    OUTPUT "${VX3_SPV_DIR}/${name}.spv"
    COMMAND "${SLANGC}" "${source}" ${VX3_SLANG_FLAGS} ${entries} -o "${VX3_SPV_DIR}/${name}.spv"
    COMMAND "${SPIRV_VAL}" --target-env vulkan1.3 --scalar-block-layout "${VX3_SPV_DIR}/${name}.spv"
    DEPENDS "${source}" "${ALCEDO_SRC}/include/color/color_encoding_math.h"
            "${ALCEDO_SRC}/include/edit/runtime/film/film_print_math.h"
    COMMENT "slangc ${name}")
  set(VX3_SPV_FILES ${VX3_SPV_FILES} "${VX3_SPV_DIR}/${name}.spv" PARENT_SCOPE)
endfunction()
vx3_slang(vx3_llf "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vx3_llf.slang"
          vx3_init_scene llf_extract llf_pyr_down llf_remap llf_select llf_collapse llf_apply)
vx3_slang(vx3_blur "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vx3_blur.slang"
          film_blur_horizontal film_blur_vertical)
vx3_slang(vx3_spin "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vx3_spin.slang" vx3_spin)
vx3_slang(film_print "${VX1_DIR}/shaders/film_print.slang"
          film_print_exposure film_print_first_amounts film_print_display)
add_custom_target(vx3_shaders ALL DEPENDS ${VX3_SPV_FILES})

add_executable(vx3_load
  host/vx3_main.cpp
  host/vx3_load.cpp
  host/vx3_load.hpp
  host/vx3_film_tables.cpp
  host/vx3_film_tables.hpp
  "${VX2_DIR}/host/vx2_vulkan.cpp"
  "${VX2_DIR}/host/vx2_vulkan.hpp"
  "${VX2_DIR}/host/vx2_item.cpp"
  "${VX2_DIR}/host/vx2_item.hpp"
  "${VX2_DIR}/host/vx2_display_lut.cpp"
  "${VX2_DIR}/host/vx2_common.hpp")
add_dependencies(vx3_load vx3_shaders)
target_include_directories(vx3_load PRIVATE "${VULKAN_SDK}/Include" "${VX3_LCMS_ROOT}/include"
                                            "${VX2_DIR}/host" "${ALCEDO_SRC}/include")
target_link_libraries(vx3_load PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Quick
                                       "${VULKAN_SDK}/Lib/vulkan-1.lib"
                                       "${VX3_LCMS_ROOT}/lib/lcms2.lib" dxgi user32 gdi32)
target_compile_definitions(vx3_load PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS VK_USE_PLATFORM_WIN32_KHR)
if(MSVC)
  target_compile_options(vx3_load PRIVATE /W4 /utf-8 /permissive-)
endif()

qt_add_shaders(vx3_load "vx3_viewport_shaders"
  PREFIX "/vx2"
  BASE "${ALCEDO_SRC}/ui/editor_rhi/shaders"
  NOGLSL NOMSL
  FILES
    "${ALCEDO_SRC}/ui/editor_rhi/shaders/editor_viewport.vert"
    "${ALCEDO_SRC}/ui/editor_rhi/shaders/editor_viewport.frag")
qt_add_shaders(vx3_load "vx3_compute_shaders"
  PREFIX "/vx2"
  BASE "${VX2_DIR}/shaders"
  NOGLSL NOMSL
  FILES "${VX2_DIR}/shaders/vx2_fill.comp")
