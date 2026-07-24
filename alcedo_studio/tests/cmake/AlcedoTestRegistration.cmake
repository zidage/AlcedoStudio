# Alcedo test registration helpers.
# Included from alcedo_studio/tests/CMakeLists.txt after ALCEDO_TEST_ROOT /
# ALCEDO_TEST_SUPPORT_ROOT are defined and category aggregate targets exist.
# The root test manifest must not redefine these functions after include.

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

# Windows runtime DLLs that must sit next to test executables.
#
# Order matters: vcpkg's toolchain attaches applocal.ps1 as a POST_BUILD step that
# copies every DLL from vcpkg/installed/.../bin, including vcpkg's lensfun.dll.
# Production code links the in-tree third_party lensfun (alcedo_lensfun /
# puerhlab_lensfun). The two ABIs are not interchangeable — loading the vcpkg
# DLL yields STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139) for lf_db_create and friends.
#
# This helper must be called AFTER target_link_libraries so its POST_BUILD steps
# run after applocal and overwrite lensfun.dll with the correct third_party build.
# Also copies the vendored duckdb.dll and optional DuckDB extensions.
function(alcedo_copy_windows_test_runtime_dlls target_name)
  if(NOT WIN32 OR NOT TARGET ${target_name})
    return()
  endif()
  if(TARGET alcedo_lensfun)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:alcedo_lensfun>"
              "$<TARGET_FILE_DIR:${target_name}>/lensfun.dll"
      VERBATIM)
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll")
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll"
              "$<TARGET_FILE_DIR:${target_name}>/duckdb.dll"
      VERBATIM)
  endif()
  alcedo_copy_duckdb_extensions(${target_name})
endfunction()

# Register one test executable with one or more category aggregates.
# Call adjacent to the test declaration (Phase 3C+). Category names match the
# trailing alcedo_assign_test_category groups in tests/CMakeLists.txt:
#   core image io raw gpu edit ui app storage demos ci_core ci_raw ci_metal
function(alcedo_register_test_target test_target)
  if(NOT TARGET ${test_target})
    return()
  endif()
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
