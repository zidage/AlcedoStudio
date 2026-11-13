# Link-line checks for the legacy pipeline removal (GPU DAG Phase G10).
#
# Usage: cmake -DNINJA_FILE=<build.ninja> -DTARGET_NAME=<executable target> -DCHECK=<name>
#              -P target_link_checks.cmake
#
# The check reads the link statement that CMake generated for TARGET_NAME, so it tests the
# libraries that the linker actually receives, including transitive ones.
#
# CHECK=FramePresenterLinksScopeWithoutEditPipeline
#   Fails unless the link libraries of TARGET_NAME contain EditScope and contain none of
#   EditPipeline, PipelineScheduler, or Operators. G10.6 moved the scope analyzer out of
#   EditPipeline, so a DAG frame presenter test must link without the legacy pipeline.

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED NINJA_FILE OR NOT EXISTS "${NINJA_FILE}")
  message(FATAL_ERROR "NINJA_FILE must name the generated build.ninja file")
endif()
if(NOT DEFINED TARGET_NAME OR TARGET_NAME STREQUAL "")
  message(FATAL_ERROR "TARGET_NAME must name an executable target")
endif()
if(NOT DEFINED CHECK)
  message(FATAL_ERROR "CHECK must name the link check to run")
endif()

# Return the LINK_LIBRARIES value of the link statement whose output is TARGET_NAME.
function(_alcedo_read_link_libraries out_var)
  file(STRINGS "${NINJA_FILE}" _lines)
  set(_in_statement FALSE)
  set(_found_statement FALSE)
  set(_libraries "")
  foreach(_line IN LISTS _lines)
    if(_line MATCHES "^build ")
      set(_in_statement FALSE)
      if(_line MATCHES "[/\\\\]${TARGET_NAME}(\\.exe)?: [A-Za-z_]*LINKER__")
        set(_in_statement TRUE)
        set(_found_statement TRUE)
      endif()
    elseif(_in_statement AND _line MATCHES "^[ \t]+LINK_LIBRARIES = (.*)$")
      set(_libraries "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  if(NOT _found_statement)
    message(FATAL_ERROR "No link statement for ${TARGET_NAME} in ${NINJA_FILE}")
  endif()
  set(${out_var} "${_libraries}" PARENT_SCOPE)
endfunction()

if(CHECK STREQUAL "FramePresenterLinksScopeWithoutEditPipeline")
  _alcedo_read_link_libraries(_libraries)
  set(_problems "")
  # Library files end in .lib, .a, .so, or .dylib; match the name as a whole path component.
  set(_library_suffix "(\\.lib|\\.a|\\.so|\\.dylib)")
  if(NOT _libraries MATCHES "(^|[ /\\\\])(lib)?EditScope${_library_suffix}")
    list(APPEND _problems "does not link EditScope")
  endif()
  foreach(_legacy IN ITEMS EditPipeline PipelineScheduler Operators)
    if(_libraries MATCHES "(^|[ /\\\\])(lib)?${_legacy}${_library_suffix}")
      list(APPEND _problems "links legacy library ${_legacy}")
    endif()
  endforeach()
  if(_problems)
    list(JOIN _problems "; " _report)
    message(FATAL_ERROR "${TARGET_NAME} ${_report}")
  endif()
  message(STATUS "${TARGET_NAME} links EditScope and no legacy pipeline library.")
else()
  message(FATAL_ERROR "Unknown link check: ${CHECK}")
endif()
