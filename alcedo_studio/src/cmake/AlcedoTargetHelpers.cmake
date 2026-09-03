# Alcedo first-party target helpers.
# Included from alcedo_studio/src/CMakeLists.txt after ALCEDO_SRC_ROOT /
# ALCEDO_INCLUDE_ROOT / ALCEDO_BINARY_ROOT are defined. Child manifests must not
# redefine those roots; they may only read them.
#
# Usage:
#   def_library(Name [TYPE STATIC|SHARED] SOURCES src1 src2
#               PUBLIC_DEPS lib1 PRIVATE_DEPS lib2)
#   def_cuda_library(Name SOURCES kernel.cu ...)

if(NOT DEFINED ALCEDO_INCLUDE_ROOT OR "${ALCEDO_INCLUDE_ROOT}" STREQUAL "")
  message(FATAL_ERROR
    "AlcedoTargetHelpers.cmake requires ALCEDO_INCLUDE_ROOT before include()")
endif()

macro(def_library target_name)
  cmake_parse_arguments(ARG "" "TYPE" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})

  if(ARG_TYPE)
    add_library(${target_name} ${ARG_TYPE} ${ARG_SOURCES})
  else()
    add_library(${target_name} ${ARG_SOURCES})
  endif()
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

# CUDA implementation libraries are DLL boundaries on Windows by default. This
# keeps their host and device-link output out of unrelated executable relinks.
# Other first-party and third-party libraries remain static.
macro(def_cuda_library target_name)
  if(WIN32 AND ALCEDO_CUDA_ENABLED AND ALCEDO_BUILD_CUDA_DLLS)
    def_library(${target_name} TYPE SHARED ${ARGN})
    set_target_properties(${target_name} PROPERTIES
      WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    # CMake 3.26 cannot extract an automatic .def from MSVC /GL objects.
    # A DLL is already an optimization boundary, so keep /O2 while emitting
    # regular COFF for the C++ translation units that the export scan reads.
    target_compile_options(${target_name} PRIVATE
      $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:/GL->
    )
    install(TARGETS ${target_name}
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
  else()
    def_library(${target_name} ${ARGN})
  endif()
endmacro()

# Copy the complete linked DLL set beside a Windows executable. The helper
# script safely handles targets whose runtime DLL set is empty.
function(alcedo_copy_linked_runtime_dlls target_name)
  if(NOT WIN32 OR NOT TARGET ${target_name})
    return()
  endif()

  set(_alcedo_runtime_search_dirs "$<TARGET_FILE_DIR:${target_name}>")
  # Submodule lensfun and first-party product DLLs must win over vcpkg/installed.
  # vcpkg lensfun 0.3.4 exports C++ names only; Operators imports the C lf_* API
  # from alcedo_studio/src/third_party/lensfun. Searching vcpkg first yields
  # STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139).
  if(TARGET puerhlab_lensfun)
    list(APPEND _alcedo_runtime_search_dirs "$<TARGET_FILE_DIR:puerhlab_lensfun>")
  endif()
  list(APPEND _alcedo_runtime_search_dirs
    "${CMAKE_BINARY_DIR}/alcedo_studio/src/decoders"
    "${CMAKE_BINARY_DIR}/alcedo_studio/src/edit"
    "${CMAKE_BINARY_DIR}/alcedo_studio/src/image"
    "${CMAKE_BINARY_DIR}/alcedo_studio/src/opencl"
    "${CMAKE_BINARY_DIR}/alcedo_studio/src/utils"
  )
  if(DEFINED CUDAToolkit_BIN_DIR AND NOT "${CUDAToolkit_BIN_DIR}" STREQUAL "")
    list(APPEND _alcedo_runtime_search_dirs "${CUDAToolkit_BIN_DIR}")
  endif()
  list(APPEND _alcedo_runtime_search_dirs
    "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows"
    "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/exiv2_x64-windows/bin"
  )
  if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND _alcedo_runtime_search_dirs
      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>bin"
    )
  endif()
  if(TARGET Qt6::Core)
    list(APPEND _alcedo_runtime_search_dirs "$<TARGET_FILE_DIR:Qt6::Core>")
  endif()

  add_custom_command(TARGET ${target_name} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      "-DALCEDO_RUNTIME_EXECUTABLE=$<TARGET_FILE:${target_name}>"
      "-DALCEDO_RUNTIME_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:${target_name}>,,>"
      "-DALCEDO_RUNTIME_DEST=$<TARGET_FILE_DIR:${target_name}>"
      "-DALCEDO_RUNTIME_SEARCH_DIRS=$<JOIN:${_alcedo_runtime_search_dirs},,>"
      -P "${CMAKE_SOURCE_DIR}/scripts/cmake/copy_linked_runtime_dlls.cmake"
    COMMENT "Alcedo: copy linked runtime DLLs for ${target_name}"
    VERBATIM
  )
endfunction()
