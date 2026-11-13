# CPack post-build script: archive the verified macOS .app as the update ZIP.
#
# Code signatures of non-Mach-O files under Contents/MacOS (ICC profiles, LUTs, lens data) are
# stored in com.apple.cs.* extended attributes. The CPack ZIP generator drops extended
# attributes, so its ZIP extracts to a bundle whose signature does not validate. ditto stores
# them as AppleDouble entries, which `ditto -x -k --sequesterRsrc` (the in-app updater) and
# Archive Utility restore.

if(NOT CPACK_GENERATOR STREQUAL "DragNDrop")
    return()
endif()
if(NOT CPACK_ALCEDO_MACOS_APP OR NOT IS_DIRECTORY "${CPACK_ALCEDO_MACOS_APP}")
    message(FATAL_ERROR "macOS app bundle to archive not found: '${CPACK_ALCEDO_MACOS_APP}'")
endif()

set(_alcedo_update_zip "${CPACK_PACKAGE_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}.zip")
file(REMOVE "${_alcedo_update_zip}")
execute_process(
    COMMAND /usr/bin/ditto -c -k --sequesterRsrc --keepParent
            "${CPACK_ALCEDO_MACOS_APP}" "${_alcedo_update_zip}"
    RESULT_VARIABLE _alcedo_ditto_result
)
if(NOT _alcedo_ditto_result EQUAL 0)
    message(FATAL_ERROR "ditto failed to create ${_alcedo_update_zip}")
endif()
message(STATUS "CPack: - package: ${_alcedo_update_zip} generated.")
