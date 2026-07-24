# Alcedo first-party target helpers.
# Included from alcedo_studio/src/CMakeLists.txt after ALCEDO_SRC_ROOT /
# ALCEDO_INCLUDE_ROOT / ALCEDO_BINARY_ROOT are defined. Child manifests must not
# redefine those roots; they may only read them.
#
# Usage: def_library(Name SOURCES src1 src2 PUBLIC_DEPS lib1 PRIVATE_DEPS lib2)

if(NOT DEFINED ALCEDO_INCLUDE_ROOT OR "${ALCEDO_INCLUDE_ROOT}" STREQUAL "")
  message(FATAL_ERROR
    "AlcedoTargetHelpers.cmake requires ALCEDO_INCLUDE_ROOT before include()")
endif()

macro(def_library target_name)
  cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})

  add_library(${target_name} ${ARG_SOURCES})
  # Stable public include root: never a directory-relative "include" that would
  # change meaning when the declaring CMakeLists lives under a child domain.
  target_include_directories(${target_name} PUBLIC "${ALCEDO_INCLUDE_ROOT}")

  if(ARG_PUBLIC_DEPS)
    target_link_libraries(${target_name} PUBLIC ${ARG_PUBLIC_DEPS})
  endif()

  if(ARG_PRIVATE_DEPS)
    target_link_libraries(${target_name} PRIVATE ${ARG_PRIVATE_DEPS})
  endif()
endmacro()
