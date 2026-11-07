# Static source check for the removal of Advanced Content Analysis (issue #710).
#
# Usage: cmake -DARCHIVE_ROOT=<archive directory> -DSCAN_ROOTS=<dir;dir> -P
#        advanced_content_analysis_removal_checks.cmake
#
# CHECK=NoCMakeFileNamesArchivedAdvancedContentAnalysisFile
#   Reads the file names under ARCHIVE_ROOT (the advanced_content_analysis archive of issue #710),
#   except README.md, and fails when a CMakeLists.txt or *.cmake file under one of SCAN_ROOTS
#   (alcedo_studio/src and alcedo_studio/tests; third_party excluded) names one of them. Each
#   report line gives the CMake file and the archived file name that it contains.

if(NOT CHECK STREQUAL "NoCMakeFileNamesArchivedAdvancedContentAnalysisFile")
  message(FATAL_ERROR "Unknown CHECK: ${CHECK}")
endif()
if(NOT DEFINED ARCHIVE_ROOT OR NOT IS_DIRECTORY "${ARCHIVE_ROOT}")
  message(FATAL_ERROR "ARCHIVE_ROOT must name the Advanced Content Analysis archive")
endif()
if(NOT DEFINED SCAN_ROOTS OR SCAN_ROOTS STREQUAL "")
  message(FATAL_ERROR "SCAN_ROOTS must list the directories to scan")
endif()

file(GLOB_RECURSE _archived_files RELATIVE "${ARCHIVE_ROOT}" "${ARCHIVE_ROOT}/*")
set(_archived_names "")
foreach(_archived_file IN LISTS _archived_files)
  get_filename_component(_name "${_archived_file}" NAME)
  if(NOT _name STREQUAL "README.md")
    list(APPEND _archived_names "${_name}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _archived_names)
if(NOT _archived_names)
  message(FATAL_ERROR "No archived file found under ${ARCHIVE_ROOT}")
endif()

set(_violations "")
set(_scanned 0)
foreach(_root IN LISTS SCAN_ROOTS)
  if(NOT IS_DIRECTORY "${_root}")
    message(FATAL_ERROR "Scan root is not a directory: ${_root}")
  endif()
  # Directories first, so third_party trees (gRPC is large) are not read.
  file(GLOB _children LIST_DIRECTORIES true RELATIVE "${_root}" "${_root}/*")
  set(_cmake_files "")
  foreach(_child IN LISTS _children)
    if(_child STREQUAL "third_party")
      continue()
    endif()
    if(IS_DIRECTORY "${_root}/${_child}")
      file(GLOB_RECURSE _found RELATIVE "${_root}"
        "${_root}/${_child}/CMakeLists.txt" "${_root}/${_child}/*.cmake")
      list(APPEND _cmake_files ${_found})
    elseif(_child STREQUAL "CMakeLists.txt" OR _child MATCHES "\\.cmake$")
      list(APPEND _cmake_files "${_child}")
    endif()
  endforeach()
  foreach(_relative_path IN LISTS _cmake_files)
    if(_relative_path MATCHES "(^|/)third_party/")
      continue()
    endif()
    math(EXPR _scanned "${_scanned} + 1")
    file(READ "${_root}/${_relative_path}" _text)
    foreach(_name IN LISTS _archived_names)
      string(FIND "${_text}" "${_name}" _position)
      if(_position GREATER_EQUAL 0)
        list(APPEND _violations "${_root}/${_relative_path}: ${_name}")
      endif()
    endforeach()
  endforeach()
endforeach()

if(_scanned EQUAL 0)
  message(FATAL_ERROR "No CMake files found under ${SCAN_ROOTS}")
endif()
if(_violations)
  list(JOIN _violations "\n  " _report)
  message(FATAL_ERROR "A CMake file names an archived Advanced Content Analysis file:\n  ${_report}")
endif()
list(LENGTH _archived_names _archived_count)
message(STATUS
  "Scanned ${_scanned} CMake files; none names one of ${_archived_count} archived files.")
