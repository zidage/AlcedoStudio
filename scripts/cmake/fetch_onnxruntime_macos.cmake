# ONNX Runtime with the Core ML execution provider, the C++ dependency of AI Denoise on macOS
# (docs/roadmap/alcedo_studio/raw-processing/ai_denoise/ai_denoise_integration_plan.md, DN10).
#
# Downloads the NuGet package Microsoft.ML.OnnxRuntime 1.24.4 at configure time, checks its
# SHA-256, and extracts the C and C++ headers and the osx-arm64 library (CPU, Core ML, and WebGPU
# execution providers in one dylib). 1.24.4 is the version of Windows (DN2), so both platforms
# compile the sessions against the same headers. The library is stored under its install name,
# libonnxruntime.1.24.4.dylib (@rpath/libonnxruntime.1.24.4.dylib), so it is not changed and keeps
# the signature of the package.
#
# Defines, on macOS arm64 only:
#   alcedo::onnxruntime_macos          SHARED IMPORTED GLOBAL target. Consumers link it PRIVATE:
#                                      only decoders/processor/nn includes ONNX Runtime headers.
#                                      The build tree finds the library through the build rpath
#                                      of the folder; the bundle through @executable_path/../
#                                      Frameworks.
#   ALCEDO_ONNXRUNTIME_MACOS_LIBRARY   The dylib that goes into Contents/Frameworks.

if(TARGET alcedo::onnxruntime_macos)
    return()
endif()
if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    return()
endif()

set(ALCEDO_ONNXRUNTIME_MACOS_VERSION "1.24.4")
set(_alcedo_ort_macos_root "${CMAKE_BINARY_DIR}/onnxruntime-macos-download")
set(_alcedo_ort_macos_package "microsoft.ml.onnxruntime.${ALCEDO_ONNXRUNTIME_MACOS_VERSION}.nupkg")
set(_alcedo_ort_macos_archive "${_alcedo_ort_macos_root}/${_alcedo_ort_macos_package}")
set(_alcedo_ort_macos_sha256 "4b978d5065b85e7004b6c6f60ca494bd978fbe6836cbf0a0b52d82b61ab99638")
set(_alcedo_ort_macos_extracted "${_alcedo_ort_macos_root}/microsoft.ml.onnxruntime.${ALCEDO_ONNXRUNTIME_MACOS_VERSION}")
set(_alcedo_ort_macos_lib_dir "${_alcedo_ort_macos_root}/lib")
set(_alcedo_ort_macos_dylib_name "libonnxruntime.${ALCEDO_ONNXRUNTIME_MACOS_VERSION}.dylib")

file(MAKE_DIRECTORY "${_alcedo_ort_macos_root}")
if(NOT EXISTS "${_alcedo_ort_macos_archive}")
    message(STATUS "Fetching NuGet package Microsoft.ML.OnnxRuntime ${ALCEDO_ONNXRUNTIME_MACOS_VERSION}")
    file(DOWNLOAD
        "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${ALCEDO_ONNXRUNTIME_MACOS_VERSION}/${_alcedo_ort_macos_package}"
        "${_alcedo_ort_macos_archive}.part"
        EXPECTED_HASH "SHA256=${_alcedo_ort_macos_sha256}" TLS_VERIFY ON
        STATUS _alcedo_ort_macos_status SHOW_PROGRESS)
    list(GET _alcedo_ort_macos_status 0 _alcedo_ort_macos_rc)
    if(NOT _alcedo_ort_macos_rc EQUAL 0)
        file(REMOVE "${_alcedo_ort_macos_archive}.part")
        message(FATAL_ERROR "Failed to download ${_alcedo_ort_macos_package}: ${_alcedo_ort_macos_status}")
    endif()
    file(RENAME "${_alcedo_ort_macos_archive}.part" "${_alcedo_ort_macos_archive}")
endif()
# An existing archive is checked too: an interrupted download can leave a complete name.
file(SHA256 "${_alcedo_ort_macos_archive}" _alcedo_ort_macos_actual)
if(NOT _alcedo_ort_macos_actual STREQUAL _alcedo_ort_macos_sha256)
    message(FATAL_ERROR "SHA-256 of ${_alcedo_ort_macos_archive} is ${_alcedo_ort_macos_actual}, expected ${_alcedo_ort_macos_sha256}")
endif()
if(NOT EXISTS "${_alcedo_ort_macos_extracted}/.extracted")
    file(REMOVE_RECURSE "${_alcedo_ort_macos_extracted}" "${_alcedo_ort_macos_lib_dir}")
    file(ARCHIVE_EXTRACT INPUT "${_alcedo_ort_macos_archive}"
        DESTINATION "${_alcedo_ort_macos_extracted}"
        PATTERNS "build/native/include/*" "runtimes/osx-arm64/native/libonnxruntime.dylib")
    file(MAKE_DIRECTORY "${_alcedo_ort_macos_lib_dir}")
    file(COPY_FILE "${_alcedo_ort_macos_extracted}/runtimes/osx-arm64/native/libonnxruntime.dylib"
        "${_alcedo_ort_macos_lib_dir}/${_alcedo_ort_macos_dylib_name}")
    file(TOUCH "${_alcedo_ort_macos_extracted}/.extracted")
endif()

set(_alcedo_ort_macos_include "${_alcedo_ort_macos_extracted}/build/native/include")
set(ALCEDO_ONNXRUNTIME_MACOS_LIBRARY "${_alcedo_ort_macos_lib_dir}/${_alcedo_ort_macos_dylib_name}"
    CACHE INTERNAL "ONNX Runtime library of AI Denoise on macOS.")
foreach(_alcedo_ort_macos_file IN ITEMS "${ALCEDO_ONNXRUNTIME_MACOS_LIBRARY}"
        "${_alcedo_ort_macos_include}/onnxruntime_cxx_api.h"
        "${_alcedo_ort_macos_include}/onnxruntime_session_options_config_keys.h")
    if(NOT EXISTS "${_alcedo_ort_macos_file}")
        message(FATAL_ERROR "ONNX Runtime package file is missing: ${_alcedo_ort_macos_file}")
    endif()
endforeach()

add_library(alcedo::onnxruntime_macos SHARED IMPORTED GLOBAL)
set_target_properties(alcedo::onnxruntime_macos PROPERTIES
    IMPORTED_LOCATION "${ALCEDO_ONNXRUNTIME_MACOS_LIBRARY}"
    IMPORTED_SONAME "@rpath/${_alcedo_ort_macos_dylib_name}"
    INTERFACE_INCLUDE_DIRECTORIES "${_alcedo_ort_macos_include}")
