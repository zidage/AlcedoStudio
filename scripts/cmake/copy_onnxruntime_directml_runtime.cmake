# Copies the ONNX Runtime DirectML DLLs of AI Denoise (fetch_onnxruntime_directml.cmake) into a
# destination folder, after a check that the folder gets one DirectML.dll of one version.
#
# Arguments (-D):
#   ALCEDO_ORT_DLLS          The DLL paths, separated by "|".
#   ALCEDO_ORT_DEST_DIR      The destination folder (the folder of the executable).
#   ALCEDO_SIDECAR_DIRECTML  Optional: the DirectML.dll that the AI Mask sidecar build copies into
#                            the same folder. When the file exists and its SHA-256 differs from the
#                            DirectML.dll of the package, the build fails: the two copies would
#                            overwrite each other, and the last copy would decide the version that
#                            the application and the sidecar load.

foreach(_required IN ITEMS ALCEDO_ORT_DLLS ALCEDO_ORT_DEST_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

string(REPLACE "|" ";" _dlls "${ALCEDO_ORT_DLLS}")
file(MAKE_DIRECTORY "${ALCEDO_ORT_DEST_DIR}")
foreach(_dll IN LISTS _dlls)
    if(NOT EXISTS "${_dll}")
        message(FATAL_ERROR "ONNX Runtime DirectML DLL is missing: ${_dll}")
    endif()
    get_filename_component(_name "${_dll}" NAME)
    if(_name STREQUAL "DirectML.dll" AND DEFINED ALCEDO_SIDECAR_DIRECTML
       AND NOT "${ALCEDO_SIDECAR_DIRECTML}" STREQUAL "" AND EXISTS "${ALCEDO_SIDECAR_DIRECTML}")
        file(SHA256 "${_dll}" _package_sha256)
        file(SHA256 "${ALCEDO_SIDECAR_DIRECTML}" _sidecar_sha256)
        if(NOT _package_sha256 STREQUAL _sidecar_sha256)
            message(FATAL_ERROR
                "DirectML.dll of the AI Mask sidecar differs from DirectML.dll 1.15.4 of AI Denoise:\n"
                "  sidecar: ${ALCEDO_SIDECAR_DIRECTML} (${_sidecar_sha256})\n"
                "  package: ${_dll} (${_package_sha256})\n"
                "The application folder holds one DirectML.dll; both must use the same version.")
        endif()
    endif()
    file(COPY_FILE "${_dll}" "${ALCEDO_ORT_DEST_DIR}/${_name}" ONLY_IF_DIFFERENT)
endforeach()
