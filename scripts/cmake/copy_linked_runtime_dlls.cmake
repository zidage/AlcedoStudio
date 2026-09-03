if(NOT DEFINED ALCEDO_RUNTIME_DEST OR "${ALCEDO_RUNTIME_DEST}" STREQUAL "")
  message(FATAL_ERROR "ALCEDO_RUNTIME_DEST is required")
endif()

file(MAKE_DIRECTORY "${ALCEDO_RUNTIME_DEST}")
cmake_path(NORMAL_PATH ALCEDO_RUNTIME_DEST OUTPUT_VARIABLE _alcedo_runtime_dest_normalized)

# vcpkg lensfun 0.3.4 exports C++ names only. Operators imports the C lf_* API
# from the bundled submodule. Never copy a package-manager lensfun.dll.
function(alcedo_runtime_is_package_lensfun dll_path out_var)
  get_filename_component(_alcedo_dll_name "${dll_path}" NAME)
  if(NOT _alcedo_dll_name STREQUAL "lensfun.dll")
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  cmake_path(NORMAL_PATH dll_path OUTPUT_VARIABLE _alcedo_dll_normalized)
  if(_alcedo_dll_normalized MATCHES "[/\\\\]vcpkg[/\\\\](installed|packages|buildtrees)[/\\\\]")
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(alcedo_runtime_is_submodule_lensfun dll_path out_var)
  get_filename_component(_alcedo_dll_name "${dll_path}" NAME)
  if(NOT _alcedo_dll_name STREQUAL "lensfun.dll")
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  cmake_path(NORMAL_PATH dll_path OUTPUT_VARIABLE _alcedo_dll_normalized)
  if(_alcedo_dll_normalized MATCHES "third_party[/\\\\]lensfun[/\\\\]install[/\\\\]")
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(alcedo_runtime_copy_dll source_path dest_dir)
  alcedo_runtime_is_package_lensfun("${source_path}" _alcedo_skip_package_lensfun)
  if(_alcedo_skip_package_lensfun)
    return()
  endif()
  get_filename_component(_alcedo_dll_name "${source_path}" NAME)
  if(_alcedo_dll_name STREQUAL "lensfun.dll")
    alcedo_runtime_is_submodule_lensfun("${source_path}" _alcedo_is_submodule_lensfun)
    if(NOT _alcedo_is_submodule_lensfun)
      return()
    endif()
  endif()
  file(COPY_FILE
    "${source_path}"
    "${dest_dir}/${_alcedo_dll_name}"
    ONLY_IF_DIFFERENT
  )
endfunction()

function(alcedo_runtime_install_submodule_lensfun dest_dir)
  alcedo_runtime_find_build_root("${dest_dir}" _alcedo_build_root)
  if("${_alcedo_build_root}" STREQUAL "")
    return()
  endif()
  set(_alcedo_submodule_lensfun
    "${_alcedo_build_root}/third_party/lensfun/install/bin/lensfun.dll")
  if(EXISTS "${_alcedo_submodule_lensfun}")
    alcedo_runtime_copy_dll("${_alcedo_submodule_lensfun}" "${dest_dir}")
  endif()
endfunction()

function(alcedo_runtime_find_build_root start_dir out_var)
  set(_alcedo_build_probe "${start_dir}")
  foreach(_alcedo_i RANGE 0 8)
    if(EXISTS "${_alcedo_build_probe}/CMakeCache.txt")
      set(${out_var} "${_alcedo_build_probe}" PARENT_SCOPE)
      return()
    endif()
    get_filename_component(_alcedo_build_probe "${_alcedo_build_probe}" DIRECTORY)
  endforeach()
  set(${out_var} "" PARENT_SCOPE)
endfunction()

function(alcedo_runtime_find_first_party_dll build_root dll_name out_var)
  # leftover lensfun.dll copies sit next to Operators in src/decoders. That file
  # is not a first-party product; the bundled submodule lives under
  # third_party/lensfun/install.
  if(dll_name STREQUAL "lensfun.dll")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  foreach(_alcedo_product_dir IN ITEMS decoders edit image opencl utils)
    set(_alcedo_candidate "${build_root}/alcedo_studio/src/${_alcedo_product_dir}/${dll_name}")
    if(EXISTS "${_alcedo_candidate}")
      set(${out_var} "${_alcedo_candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out_var} "" PARENT_SCOPE)
endfunction()

function(alcedo_runtime_source_is_dest_local source_path dest_dir out_var)
  cmake_path(NORMAL_PATH source_path OUTPUT_VARIABLE _alcedo_source_normalized)
  cmake_path(NORMAL_PATH dest_dir OUTPUT_VARIABLE _alcedo_dest_normalized)
  cmake_path(IS_PREFIX _alcedo_dest_normalized _alcedo_source_normalized NORMALIZE _alcedo_is_local)
  set(${out_var} "${_alcedo_is_local}" PARENT_SCOPE)
endfunction()

# GET_RUNTIME_DEPENDENCIES still searches the executable directory even when
# that directory is omitted from DIRECTORIES. A leftover first-party DLL there
# (RawProcessorOp, Operators, ...) is then copied onto itself with
# ONLY_IF_DIFFERENT, leaving STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139).
function(alcedo_runtime_refresh_first_party_dlls dest_dir)
  alcedo_runtime_find_build_root("${dest_dir}" _alcedo_build_root)
  if("${_alcedo_build_root}" STREQUAL "")
    return()
  endif()
  foreach(_alcedo_product_dir IN ITEMS decoders edit image opencl utils)
    file(GLOB _alcedo_product_dlls
      LIST_DIRECTORIES false
      "${_alcedo_build_root}/alcedo_studio/src/${_alcedo_product_dir}/*.dll"
    )
    foreach(_alcedo_product_dll IN LISTS _alcedo_product_dlls)
      get_filename_component(_alcedo_dll_name "${_alcedo_product_dll}" NAME)
      if(_alcedo_dll_name STREQUAL "lensfun.dll")
        continue()
      endif()
      if(EXISTS "${dest_dir}/${_alcedo_dll_name}")
        alcedo_runtime_copy_dll("${_alcedo_product_dll}" "${dest_dir}")
      endif()
    endforeach()
  endforeach()
endfunction()

if(DEFINED ALCEDO_RUNTIME_DLLS AND NOT "${ALCEDO_RUNTIME_DLLS}" STREQUAL "")
  string(REPLACE "," ";" _alcedo_runtime_dlls "${ALCEDO_RUNTIME_DLLS}")
  foreach(_alcedo_runtime_dll IN LISTS _alcedo_runtime_dlls)
    if(EXISTS "${_alcedo_runtime_dll}")
      alcedo_runtime_copy_dll("${_alcedo_runtime_dll}" "${ALCEDO_RUNTIME_DEST}")
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
    alcedo_runtime_source_is_dest_local(
      "${_alcedo_runtime_dll}"
      "${ALCEDO_RUNTIME_DEST}"
      _alcedo_resolved_is_local
    )
    if(_alcedo_resolved_is_local)
      continue()
    endif()
    alcedo_runtime_copy_dll("${_alcedo_runtime_dll}" "${ALCEDO_RUNTIME_DEST}")
  endforeach()

  if(_alcedo_unresolved_runtime_dlls)
    # Thin unit tests may import first-party CUDA product DLLs without those
    # output dirs being in DIRECTORIES. Resolve remaining names from product
    # output directories, never leftover copies under tests/ or ui/.
    get_filename_component(_alcedo_exe_dir "${ALCEDO_RUNTIME_EXECUTABLE}" DIRECTORY)
    alcedo_runtime_find_build_root("${_alcedo_exe_dir}" _alcedo_build_root)

    set(_alcedo_still_unresolved "")
    set(_alcedo_resolved_any FALSE)
    foreach(_alcedo_missing IN LISTS _alcedo_unresolved_runtime_dlls)
      get_filename_component(_alcedo_missing_name "${_alcedo_missing}" NAME)
      set(_alcedo_found "")
      if(NOT "${_alcedo_build_root}" STREQUAL "")
        alcedo_runtime_find_first_party_dll(
          "${_alcedo_build_root}"
          "${_alcedo_missing_name}"
          _alcedo_found
        )
      endif()
      if(NOT "${_alcedo_found}" STREQUAL "" AND EXISTS "${_alcedo_found}")
        alcedo_runtime_copy_dll("${_alcedo_found}" "${ALCEDO_RUNTIME_DEST}")
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
          alcedo_runtime_find_first_party_dll(
            "${_alcedo_build_root}"
            "${_alcedo_extra}"
            _alcedo_extra_found
          )
          if(NOT "${_alcedo_extra_found}" STREQUAL "" AND EXISTS "${_alcedo_extra_found}")
            alcedo_runtime_copy_dll("${_alcedo_extra_found}" "${ALCEDO_RUNTIME_DEST}")
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

alcedo_runtime_refresh_first_party_dlls("${ALCEDO_RUNTIME_DEST}")
alcedo_runtime_install_submodule_lensfun("${ALCEDO_RUNTIME_DEST}")
