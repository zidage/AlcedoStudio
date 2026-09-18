# Alcedo test registration helpers.
# Included from alcedo_studio/tests/CMakeLists.txt after ALCEDO_TEST_ROOT /
# ALCEDO_TEST_SUPPORT_ROOT are defined and category aggregate targets exist.
# Domain manifests may re-include this file; helpers are idempotent.

function(alcedo_assign_test_category category option_name category_target)
  foreach(test_target IN LISTS ARGN)
    if(TARGET ${test_target})
      if(NOT ${option_name} OR NOT ALCEDO_BUILD_TESTS_BY_DEFAULT)
        set_target_properties(${test_target} PROPERTIES
          EXCLUDE_FROM_ALL TRUE
          EXCLUDE_FROM_DEFAULT_BUILD TRUE
        )
      endif()
      if(${option_name})
        if(ALCEDO_BUILD_TESTS_BY_DEFAULT)
          set_target_properties(${test_target} PROPERTIES
            EXCLUDE_FROM_ALL FALSE
            EXCLUDE_FROM_DEFAULT_BUILD FALSE
          )
        endif()
        add_dependencies(${category_target} ${test_target})
        add_dependencies(alcedo_tests_all ${test_target})
      endif()
    endif()
  endforeach()
endfunction()

function(alcedo_copy_duckdb_extension target_name extension_path extension_file_name)
  if(TARGET ${target_name} AND NOT "${extension_path}" STREQUAL "" AND EXISTS "${extension_path}")
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "$<TARGET_FILE_DIR:${target_name}>/duckdb_extensions"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${extension_path}"
              "$<TARGET_FILE_DIR:${target_name}>/duckdb_extensions/${extension_file_name}"
      VERBATIM)
  endif()
endfunction()

function(alcedo_copy_duckdb_extensions target_name)
  alcedo_copy_duckdb_extension(${target_name}
    "${ALCEDO_DUCKDB_VSS_EXTENSION}"
    "vss.duckdb_extension")
  alcedo_copy_duckdb_extension(${target_name}
    "${ALCEDO_DUCKDB_FTS_EXTENSION}"
    "fts.duckdb_extension")
endfunction()

function(alcedo_copy_duckdb_fts_extension target_name)
  alcedo_copy_duckdb_extension(${target_name}
    "${ALCEDO_DUCKDB_FTS_EXTENSION}"
    "fts.duckdb_extension")
endfunction()

# ------------------------------------------------------------------------------
# Windows test runtime DLLs (lensfun / duckdb)
# ------------------------------------------------------------------------------
#
# Why per-target "opt-in" fixes keep failing for new tests:
#   vcpkg overrides add_executable() and always attaches applocal.ps1 as the
#   first POST_BUILD. That copies vcpkg's lensfun.dll next to the EXE.
#   Production links third_party lensfun (puerhlab_lensfun / alcedo_lensfun).
#   Loading the vcpkg ABI fails with STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139).
#   Previous fixes required each domain CMakeLists to call a helper; every new
#   test that forgot the call reproduced the crash.
#
# Policy (do not regress):
#   1. Automatic for every EXECUTABLE created in a directory that uses this
#      module (include this file, or call alcedo_register_test_target).
#   2. Fixup runs at end of the *same* CMake directory that created the target
#      (CMake requires add_custom_command(TARGET) in the creating directory).
#   3. Runs after all target_link_libraries in that directory so POST_BUILD is
#      appended after vcpkg applocal.
#   4. Per-target ${name}_runtime/ output dir so parallel Ninja builds cannot
#      re-applocal a sibling EXE's shared folder.
#   5. Force-copy submodule lensfun.dll after applocal. Never leave a
#      package-manager lensfun.dll next to a test EXE.
#
# Domain CMakeLists must NOT hand-roll lensfun/duckdb POST_BUILD copies.

function(alcedo_copy_windows_test_runtime_dlls target_name)
  if(NOT WIN32 OR NOT TARGET ${target_name})
    return()
  endif()

  get_target_property(_type ${target_name} TYPE)
  if(NOT _type STREQUAL "EXECUTABLE")
    return()
  endif()

  get_target_property(_done ${target_name} ALCEDO_WINDOWS_TEST_RUNTIME_DLLS_DONE)
  if(_done)
    return()
  endif()
  set_target_properties(${target_name} PROPERTIES
    ALCEDO_WINDOWS_TEST_RUNTIME_DLLS_DONE TRUE)

  get_target_property(_target_bin_dir ${target_name} BINARY_DIR)
  if(NOT _target_bin_dir)
    set(_target_bin_dir "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  set(_runtime_dir "${_target_bin_dir}/${target_name}_runtime")
  set_target_properties(${target_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_runtime_dir}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${_runtime_dir}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${_runtime_dir}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${_runtime_dir}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${_runtime_dir}"
  )

  alcedo_copy_linked_runtime_dlls(${target_name})

  set(_lensfun_dll "")
  if(TARGET puerhlab_lensfun)
    set(_lensfun_dll "$<TARGET_FILE:puerhlab_lensfun>")
  elseif(EXISTS "${CMAKE_BINARY_DIR}/third_party/lensfun/install/bin/lensfun.dll")
    set(_lensfun_dll "${CMAKE_BINARY_DIR}/third_party/lensfun/install/bin/lensfun.dll")
  endif()

  if(NOT _lensfun_dll STREQUAL "")
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
              "${_lensfun_dll}"
              "$<TARGET_FILE_DIR:${target_name}>/lensfun.dll"
      COMMENT "Alcedo: install submodule lensfun.dll next to ${target_name}"
      VERBATIM)
  else()
    message(WARNING
      "alcedo_copy_windows_test_runtime_dlls(${target_name}): third_party lensfun.dll "
      "missing; test launch will 0xC0000139 if the EXE imports lensfun.")
  endif()

  set(_duckdb_dll
      "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll")
  if(EXISTS "${_duckdb_dll}")
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy
              "${_duckdb_dll}"
              "$<TARGET_FILE_DIR:${target_name}>/duckdb.dll"
      COMMENT "Alcedo: install vendored duckdb.dll next to ${target_name}"
      VERBATIM)
  endif()
  alcedo_copy_duckdb_extensions(${target_name})
endfunction()

# End-of-directory: fix every EXECUTABLE created in this CMakeLists scope.
function(alcedo_finalize_current_test_directory_windows_runtime_dlls)
  if(NOT WIN32)
    return()
  endif()
  get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_t IN LISTS _targets)
    if(TARGET "${_t}")
      get_target_property(_type "${_t}" TYPE)
      if(_type STREQUAL "EXECUTABLE")
        alcedo_copy_windows_test_runtime_dlls("${_t}")
      endif()
    endif()
  endforeach()
endfunction()

# Schedule the end-of-directory pass once per CMake source directory.
function(alcedo_schedule_windows_test_runtime_dll_fixup)
  if(NOT WIN32)
    return()
  endif()
  get_directory_property(_scheduled ALCEDO_WINDOWS_TEST_DLL_FIXUP_SCHEDULED)
  if(_scheduled)
    return()
  endif()
  set_property(DIRECTORY PROPERTY ALCEDO_WINDOWS_TEST_DLL_FIXUP_SCHEDULED TRUE)
  # CMake 3.19+: runs after the rest of this directory's CMakeLists is processed,
  # so all add_executable / target_link_libraries (and vcpkg applocal attachment)
  # have already happened; our POST_BUILD steps are appended after applocal.
  cmake_language(DEFER CALL alcedo_finalize_current_test_directory_windows_runtime_dlls)
endfunction()

# Register one test executable with one or more category aggregates.
# Call after target_link_libraries. Category names match tests/CMakeLists.txt:
#   core image io raw gpu edit ui app storage demos ci_core ci_raw ci_metal
#
# Category registration only. Windows DLL fixup is scheduled for the whole
# directory (include this module or call register once).
function(alcedo_register_test_target test_target)
  if(NOT TARGET ${test_target})
    return()
  endif()
  alcedo_schedule_windows_test_runtime_dll_fixup()
  foreach(category IN LISTS ARGN)
    if(category STREQUAL "core")
      alcedo_assign_test_category(core ALCEDO_BUILD_CORE_TESTS alcedo_tests_core
        ${test_target})
    elseif(category STREQUAL "image")
      alcedo_assign_test_category(image ALCEDO_BUILD_IMAGE_TESTS alcedo_tests_image
        ${test_target})
    elseif(category STREQUAL "io")
      alcedo_assign_test_category(io ALCEDO_BUILD_IO_TESTS alcedo_tests_io
        ${test_target})
    elseif(category STREQUAL "raw")
      alcedo_assign_test_category(raw ALCEDO_BUILD_RAW_TESTS alcedo_tests_raw
        ${test_target})
    elseif(category STREQUAL "gpu")
      alcedo_assign_test_category(gpu ALCEDO_BUILD_GPU_TESTS alcedo_tests_gpu
        ${test_target})
    elseif(category STREQUAL "edit")
      alcedo_assign_test_category(edit ALCEDO_BUILD_EDIT_TESTS alcedo_tests_edit
        ${test_target})
    elseif(category STREQUAL "ui")
      alcedo_assign_test_category(ui ALCEDO_BUILD_UI_TESTS alcedo_tests_ui
        ${test_target})
    elseif(category STREQUAL "app")
      alcedo_assign_test_category(app ALCEDO_BUILD_APP_TESTS alcedo_tests_app
        ${test_target})
    elseif(category STREQUAL "storage")
      alcedo_assign_test_category(storage ALCEDO_BUILD_STORAGE_TESTS alcedo_tests_storage
        ${test_target})
    elseif(category STREQUAL "demos")
      alcedo_assign_test_category(demos ALCEDO_BUILD_DEMO_TARGETS alcedo_tests_demos
        ${test_target})
    elseif(category STREQUAL "ci_core")
      alcedo_assign_test_category(ci_core ALCEDO_BUILD_CI_TESTS alcedo_tests_ci_core
        ${test_target})
    elseif(category STREQUAL "ci_raw")
      alcedo_assign_test_category(ci_raw ALCEDO_BUILD_CI_TESTS alcedo_tests_ci_raw
        ${test_target})
    elseif(category STREQUAL "ci_metal")
      alcedo_assign_test_category(ci_metal ALCEDO_BUILD_CI_TESTS alcedo_tests_ci_metal
        ${test_target})
    else()
      message(FATAL_ERROR
        "alcedo_register_test_target: unknown category '${category}' for ${test_target}")
    endif()
  endforeach()
endfunction()

# Including this module from a test domain schedules automatic Windows DLL
# fixup for every EXECUTABLE in that directory — no per-target calls.
alcedo_schedule_windows_test_runtime_dll_fixup()
