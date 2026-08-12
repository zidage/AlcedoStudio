cmake_minimum_required(VERSION 3.21)

foreach(_required IN ITEMS ALCEDO_BUILD_NUMBER_MODE ALCEDO_REPO_ROOT
                           ALCEDO_BUILD_DIR ALCEDO_BUILD_PLATFORM)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

if(NOT ALCEDO_BUILD_NUMBER_MODE STREQUAL "resolve" AND
   NOT ALCEDO_BUILD_NUMBER_MODE STREQUAL "commit")
    message(FATAL_ERROR
        "ALCEDO_BUILD_NUMBER_MODE must be 'resolve' or 'commit', got '${ALCEDO_BUILD_NUMBER_MODE}'")
endif()
if(NOT ALCEDO_BUILD_PLATFORM MATCHES "^[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "ALCEDO_BUILD_PLATFORM contains unsupported characters")
endif()

set(_state_dir "${ALCEDO_REPO_ROOT}/build/tmp/update-build-number")
set(_state_file "${_state_dir}/${ALCEDO_BUILD_PLATFORM}.txt")
set(_pending_file "${_state_dir}/${ALCEDO_BUILD_PLATFORM}.pending.txt")
file(MAKE_DIRECTORY "${_state_dir}")

function(alcedo_read_positive_integer path output)
    if(NOT EXISTS "${path}")
        set(${output} "" PARENT_SCOPE)
        return()
    endif()
    file(READ "${path}" _value LIMIT 128)
    string(STRIP "${_value}" _value)
    if(NOT _value MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "Invalid build number in ${path}: '${_value}'")
    endif()
    set(${output} "${_value}" PARENT_SCOPE)
endfunction()

function(alcedo_write_integer path value)
    set(_temporary "${path}.new")
    file(WRITE "${_temporary}" "${value}\n")
    file(RENAME "${_temporary}" "${path}")
endfunction()

if(ALCEDO_BUILD_NUMBER_MODE STREQUAL "commit")
    alcedo_read_positive_integer("${_pending_file}" _pending)
    if(_pending STREQUAL "")
        message(FATAL_ERROR "No pending package build number exists for ${ALCEDO_BUILD_PLATFORM}")
    endif()
    alcedo_read_positive_integer("${_state_file}" _previous)
    if(NOT _previous STREQUAL "" AND _previous GREATER _pending)
        set(_committed "${_previous}")
    else()
        set(_committed "${_pending}")
    endif()
    alcedo_write_integer("${_state_file}" "${_committed}")
    file(REMOVE "${_pending_file}")
    message(STATUS
        "Recorded successful ${ALCEDO_BUILD_PLATFORM} package build ${_pending}")
    return()
endif()

file(READ "${ALCEDO_REPO_ROOT}/CMakeLists.txt" _root_cmake)
string(REGEX MATCH
    "project\\([ \t\r\n]*alcedo[ \t\r\n]+VERSION[ \t\r\n]+([0-9]+)\\.([0-9]+)\\.([0-9]+)"
    _version_match "${_root_cmake}")
if(NOT _version_match)
    message(FATAL_ERROR "Cannot read the Alcedo project version from CMakeLists.txt")
endif()
math(EXPR _default_build
    "${CMAKE_MATCH_1} * 1000000 + ${CMAKE_MATCH_2} * 1000 + ${CMAKE_MATCH_3}")

set(_selected "")
set(_source "")
if(DEFINED ALCEDO_BUILD_NUMBER_OVERRIDE AND
   NOT "${ALCEDO_BUILD_NUMBER_OVERRIDE}" STREQUAL "" AND
   NOT "${ALCEDO_BUILD_NUMBER_OVERRIDE}" STREQUAL "0")
    if(NOT ALCEDO_BUILD_NUMBER_OVERRIDE MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "ALCEDO_BUILD_NUMBER_OVERRIDE must be a positive integer, got "
            "'${ALCEDO_BUILD_NUMBER_OVERRIDE}'")
    endif()
    set(_selected "${ALCEDO_BUILD_NUMBER_OVERRIDE}")
    set(_source "explicit override")
else()
    alcedo_read_positive_integer("${_pending_file}" _pending)
    alcedo_read_positive_integer("${_state_file}" _previous)
    if(NOT _pending STREQUAL "")
        set(_selected "${_pending}")
        set(_source "pending retry")
    elseif(NOT _previous STREQUAL "")
        math(EXPR _selected "${_previous} + 1")
        set(_source "last successful package ${_previous}")
    else()
        set(_cache_file "${ALCEDO_BUILD_DIR}/CMakeCache.txt")
        if(EXISTS "${_cache_file}")
            file(READ "${_cache_file}" _cache LIMIT 4194304)
            string(REGEX MATCH
                "(^|\n)ALCEDO_BUILD_NUMBER:[^=\n]*=([0-9]+)"
                _cache_match "${_cache}")
            if(_cache_match AND CMAKE_MATCH_2 GREATER 0)
                math(EXPR _cache_next "${CMAKE_MATCH_2} + 1")
                set(_selected "${_cache_next}")
                set(_source "existing CMake cache ${CMAKE_MATCH_2}")
            endif()
        endif()
        if(_selected STREQUAL "")
            set(_selected "${_default_build}")
            set(_source "project version default")
        endif()
    endif()

    if(_selected LESS _default_build)
        set(_selected "${_default_build}")
        set(_source "project version default")
    endif()
endif()

alcedo_write_integer("${_pending_file}" "${_selected}")
message(STATUS
    "Selected ${ALCEDO_BUILD_PLATFORM} package build ${_selected} (${_source})")
