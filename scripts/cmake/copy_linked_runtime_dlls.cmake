if(NOT DEFINED ALCEDO_RUNTIME_DEST OR "${ALCEDO_RUNTIME_DEST}" STREQUAL "")
  message(FATAL_ERROR "ALCEDO_RUNTIME_DEST is required")
endif()

file(MAKE_DIRECTORY "${ALCEDO_RUNTIME_DEST}")

if(DEFINED ALCEDO_RUNTIME_DLLS AND NOT "${ALCEDO_RUNTIME_DLLS}" STREQUAL "")
  string(REPLACE "," ";" _alcedo_runtime_dlls "${ALCEDO_RUNTIME_DLLS}")
  foreach(_alcedo_runtime_dll IN LISTS _alcedo_runtime_dlls)
    if(EXISTS "${_alcedo_runtime_dll}")
      get_filename_component(_alcedo_runtime_dll_name "${_alcedo_runtime_dll}" NAME)
      file(COPY_FILE
        "${_alcedo_runtime_dll}"
        "${ALCEDO_RUNTIME_DEST}/${_alcedo_runtime_dll_name}"
        ONLY_IF_DIFFERENT
      )
    else()
      message(FATAL_ERROR "Linked runtime DLL does not exist: ${_alcedo_runtime_dll}")
    endif()
  endforeach()
endif()

if(DEFINED ALCEDO_RUNTIME_EXECUTABLE AND EXISTS "${ALCEDO_RUNTIME_EXECUTABLE}")
  string(REPLACE "," ";" _alcedo_runtime_search_dirs "${ALCEDO_RUNTIME_SEARCH_DIRS}")
  # The executable directory already contains files copied by applocal. Passing it back to
  # GET_RUNTIME_DEPENDENCIES together with the package bin directory makes identical DLL names
  # appear as conflicting candidates on CMake 4.x.
  cmake_path(NORMAL_PATH ALCEDO_RUNTIME_DEST OUTPUT_VARIABLE _alcedo_runtime_dest_normalized)
  set(_alcedo_filtered_runtime_search_dirs "")
  foreach(_alcedo_runtime_search_dir IN LISTS _alcedo_runtime_search_dirs)
    cmake_path(NORMAL_PATH _alcedo_runtime_search_dir OUTPUT_VARIABLE _alcedo_runtime_search_dir_normalized)
    if(NOT _alcedo_runtime_search_dir_normalized STREQUAL _alcedo_runtime_dest_normalized)
      list(APPEND _alcedo_filtered_runtime_search_dirs "${_alcedo_runtime_search_dir}")
    endif()
  endforeach()
  set(_alcedo_runtime_search_dirs "${_alcedo_filtered_runtime_search_dirs}")
  file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${ALCEDO_RUNTIME_EXECUTABLE}"
    DIRECTORIES ${_alcedo_runtime_search_dirs}
    RESOLVED_DEPENDENCIES_VAR _alcedo_resolved_runtime_dlls
    UNRESOLVED_DEPENDENCIES_VAR _alcedo_unresolved_runtime_dlls
    # Applocal can place a dependency beside the executable before this validation pass. CMake
    # 4.x otherwise treats that valid local copy plus the package-bin source as a fatal conflict.
    CONFLICTING_DEPENDENCIES_PREFIX _alcedo_runtime_conflicts
    PRE_EXCLUDE_REGEXES
      "api-ms-win-.*"
      "ext-ms-.*"
    POST_EXCLUDE_REGEXES
      ".*[\\\\/][Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\\\/].*"
      ".*[\\\\/][Ss][Yy][Ss][Tt][Ee][Mm]32[\\\\/].*"
      ".*[\\\\/][Ss][Yy][Ss][Ww][Oo][Ww]64[\\\\/].*"
  )

  foreach(_alcedo_runtime_dll IN LISTS _alcedo_resolved_runtime_dlls)
    get_filename_component(_alcedo_runtime_dll_name "${_alcedo_runtime_dll}" NAME)
    file(COPY_FILE
      "${_alcedo_runtime_dll}"
      "${ALCEDO_RUNTIME_DEST}/${_alcedo_runtime_dll_name}"
      ONLY_IF_DIFFERENT
    )
  endforeach()

  if(_alcedo_unresolved_runtime_dlls)
    # Thin unit tests may import first-party CUDA product DLLs without those
    # output dirs being in DIRECTORIES (adding them can conflict on opencl.dll).
    # Resolve only the remaining names under the build tree, then re-check.
    get_filename_component(_alcedo_exe_dir "${ALCEDO_RUNTIME_EXECUTABLE}" DIRECTORY)
    # Climb toward the CMake build root: .../tests/<domain>/<name>_runtime -> build root
    set(_alcedo_build_probe "${_alcedo_exe_dir}")
    set(_alcedo_build_root "")
    foreach(_alcedo_i RANGE 0 8)
      if(EXISTS "${_alcedo_build_probe}/CMakeCache.txt")
        set(_alcedo_build_root "${_alcedo_build_probe}")
        break()
      endif()
      get_filename_component(_alcedo_build_probe "${_alcedo_build_probe}" DIRECTORY)
    endforeach()

    set(_alcedo_still_unresolved "")
    set(_alcedo_resolved_any FALSE)
    foreach(_alcedo_missing IN LISTS _alcedo_unresolved_runtime_dlls)
      get_filename_component(_alcedo_missing_name "${_alcedo_missing}" NAME)
      set(_alcedo_found "")
      if(NOT "${_alcedo_build_root}" STREQUAL "")
        file(GLOB_RECURSE _alcedo_candidates
          LIST_DIRECTORIES false
          "${_alcedo_build_root}/alcedo_studio/src/*/${_alcedo_missing_name}"
        )
        if(_alcedo_candidates)
          list(GET _alcedo_candidates 0 _alcedo_found)
        endif()
      endif()
      if(NOT "${_alcedo_found}" STREQUAL "" AND EXISTS "${_alcedo_found}")
        file(COPY_FILE
          "${_alcedo_found}"
          "${ALCEDO_RUNTIME_DEST}/${_alcedo_missing_name}"
          ONLY_IF_DIFFERENT
        )
        set(_alcedo_resolved_any TRUE)
      else()
        list(APPEND _alcedo_still_unresolved "${_alcedo_missing}")
      endif()
    endforeach()

    # Product CUDA DLLs (DemosaicNet, CudaDemosaicNetEntry) import CudaUtils but
    # thin tests do not always list it as a direct PE dependency of the EXE.
    if(_alcedo_resolved_any AND NOT "${_alcedo_build_root}" STREQUAL "")
      foreach(_alcedo_extra IN ITEMS CudaUtils.dll)
        if(NOT EXISTS "${ALCEDO_RUNTIME_DEST}/${_alcedo_extra}")
          file(GLOB_RECURSE _alcedo_extra_candidates
            LIST_DIRECTORIES false
            "${_alcedo_build_root}/alcedo_studio/src/*/${_alcedo_extra}"
          )
          if(_alcedo_extra_candidates)
            list(GET _alcedo_extra_candidates 0 _alcedo_extra_found)
            if(EXISTS "${_alcedo_extra_found}")
              file(COPY_FILE
                "${_alcedo_extra_found}"
                "${ALCEDO_RUNTIME_DEST}/${_alcedo_extra}"
                ONLY_IF_DIFFERENT
              )
            endif()
          endif()
        endif()
      endforeach()
    endif()

    if(_alcedo_still_unresolved)
      list(JOIN _alcedo_still_unresolved "\n  " _alcedo_unresolved_text)
      message(FATAL_ERROR "Unresolved linked runtime DLLs:\n  ${_alcedo_unresolved_text}")
    endif()
  endif()
endif()
