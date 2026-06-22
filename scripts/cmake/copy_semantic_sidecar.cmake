if(NOT DEFINED ALCEDO_MIND_BINARY OR ALCEDO_MIND_BINARY STREQUAL "")
    message(FATAL_ERROR "ALCEDO_MIND_BINARY is required")
endif()

if(NOT DEFINED ALCEDO_MIND_TARGET_DIR OR ALCEDO_MIND_TARGET_DIR STREQUAL "")
    message(FATAL_ERROR "ALCEDO_MIND_TARGET_DIR is required")
endif()

if(NOT DEFINED ALCEDO_MIND_DEST_DIR OR ALCEDO_MIND_DEST_DIR STREQUAL "")
    message(FATAL_ERROR "ALCEDO_MIND_DEST_DIR is required")
endif()

if(NOT EXISTS "${ALCEDO_MIND_BINARY}")
    message(FATAL_ERROR "Semantic sidecar binary was not built: ${ALCEDO_MIND_BINARY}")
endif()

file(MAKE_DIRECTORY "${ALCEDO_MIND_DEST_DIR}")
file(COPY "${ALCEDO_MIND_BINARY}" DESTINATION "${ALCEDO_MIND_DEST_DIR}")
get_filename_component(_alcedo_mind_binary_name "${ALCEDO_MIND_BINARY}" NAME)
set(_alcedo_mind_copied_binary "${ALCEDO_MIND_DEST_DIR}/${_alcedo_mind_binary_name}")

file(GLOB _alcedo_mind_runtime_dlls "${ALCEDO_MIND_TARGET_DIR}/*.dll")
foreach(_alcedo_mind_runtime_dll IN LISTS _alcedo_mind_runtime_dlls)
    file(COPY "${_alcedo_mind_runtime_dll}" DESTINATION "${ALCEDO_MIND_DEST_DIR}")
endforeach()

file(GLOB _alcedo_mind_runtime_shared_libs
    "${ALCEDO_MIND_TARGET_DIR}/*.dylib"
    "${ALCEDO_MIND_TARGET_DIR}/*.so"
)
foreach(_alcedo_mind_runtime_shared_lib IN LISTS _alcedo_mind_runtime_shared_libs)
    file(COPY "${_alcedo_mind_runtime_shared_lib}" DESTINATION "${ALCEDO_MIND_DEST_DIR}")
endforeach()

if(APPLE)
    execute_process(COMMAND /bin/chmod u+w "${_alcedo_mind_copied_binary}" ERROR_QUIET)
    execute_process(
        COMMAND /usr/bin/otool -l "${_alcedo_mind_copied_binary}"
        RESULT_VARIABLE _alcedo_mind_otool_result
        OUTPUT_VARIABLE _alcedo_mind_otool_output
        ERROR_QUIET
    )
    if(NOT _alcedo_mind_otool_result EQUAL 0)
        message(FATAL_ERROR "otool failed for semantic sidecar: ${_alcedo_mind_copied_binary}")
    endif()
    if(NOT _alcedo_mind_otool_output MATCHES "path /usr/lib/swift \\(offset")
        execute_process(
            COMMAND /usr/bin/install_name_tool
                    -add_rpath /usr/lib/swift
                    "${_alcedo_mind_copied_binary}"
            RESULT_VARIABLE _alcedo_mind_add_swift_rpath_result
            ERROR_QUIET
        )
        if(NOT _alcedo_mind_add_swift_rpath_result EQUAL 0)
            message(FATAL_ERROR "Failed to add /usr/lib/swift rpath to semantic sidecar")
        endif()
    endif()
endif()
