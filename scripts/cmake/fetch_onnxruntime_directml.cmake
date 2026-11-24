# ONNX Runtime with the DirectML execution provider, the C++ dependency of AI Denoise on Windows
# (docs/roadmap/alcedo_studio/raw-processing/ai_denoise/ai_denoise_integration_plan.md, DN2).
#
# Downloads the NuGet packages Microsoft.ML.OnnxRuntime.DirectML 1.24.4 (onnxruntime.dll,
# onnxruntime_providers_shared.dll, onnxruntime.lib, the C and C++ headers) and
# Microsoft.AI.DirectML 1.15.4 (DirectML.dll) at configure time, checks their SHA-256, and
# extracts the win-x64 files into the build tree. 1.24.4 is the last DirectML release of ONNX
# Runtime. DirectML.dll 1.15.4 is the same file that the AI Mask sidecar build copies
# (copy_onnxruntime_directml_runtime.cmake checks it).
#
# Defines, on Windows x64 only:
#   alcedo::onnxruntime_directml      SHARED IMPORTED GLOBAL target. Consumers link it PRIVATE:
#                                     only decoders/processor/nn includes ONNX Runtime headers.
#                                     onnxruntime.dll is delay-loaded, so a process without it
#                                     starts and the session reports the load error.
#   ALCEDO_ONNXRUNTIME_DIRECTML_RUNTIME_DLLS
#                                     The three DLLs that go beside an executable that runs it.
#   alcedo_copy_onnxruntime_directml_runtime(<target>)
#                                     POST_BUILD copy of those DLLs beside <target>.

if(TARGET alcedo::onnxruntime_directml)
    return()
endif()
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64")
    return()
endif()

set(ALCEDO_ONNXRUNTIME_DIRECTML_VERSION "1.24.4")
set(ALCEDO_DIRECTML_VERSION "1.15.4")
set(_alcedo_ort_dml_root "${CMAKE_BINARY_DIR}/onnxruntime-directml-download")

# Downloads <name>.<version>.nupkg from nuget.org, checks its SHA-256, and extracts the archive
# members that match the patterns after <out_dir_var> (each pattern must match a member).
function(_alcedo_fetch_nuget_package name version sha256 out_dir_var)
    set(_package "${name}.${version}.nupkg")
    set(_archive "${_alcedo_ort_dml_root}/${_package}")
    set(_extracted "${_alcedo_ort_dml_root}/${name}.${version}")
    file(MAKE_DIRECTORY "${_alcedo_ort_dml_root}")
    if(NOT EXISTS "${_archive}")
        message(STATUS "Fetching NuGet package ${name} ${version}")
        file(DOWNLOAD "https://api.nuget.org/v3-flatcontainer/${name}/${version}/${_package}"
            "${_archive}.part"
            EXPECTED_HASH "SHA256=${sha256}" TLS_VERIFY ON
            STATUS _status SHOW_PROGRESS)
        list(GET _status 0 _rc)
        if(NOT _rc EQUAL 0)
            file(REMOVE "${_archive}.part")
            message(FATAL_ERROR "Failed to download ${_package}: ${_status}")
        endif()
        file(RENAME "${_archive}.part" "${_archive}")
    endif()
    # An existing archive is checked too: an interrupted download can leave a complete name.
    file(SHA256 "${_archive}" _actual)
    if(NOT _actual STREQUAL sha256)
        message(FATAL_ERROR "SHA-256 of ${_archive} is ${_actual}, expected ${sha256}")
    endif()
    if(NOT EXISTS "${_extracted}/.extracted")
        file(REMOVE_RECURSE "${_extracted}")
        file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_extracted}" PATTERNS ${ARGN})
        file(TOUCH "${_extracted}/.extracted")
    endif()
    set(${out_dir_var} "${_extracted}" PARENT_SCOPE)
endfunction()

_alcedo_fetch_nuget_package(microsoft.ml.onnxruntime.directml
    "${ALCEDO_ONNXRUNTIME_DIRECTML_VERSION}"
    "57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681"
    _alcedo_ort_dir
    "build/native/include/*" "runtimes/win-x64/native/*")
_alcedo_fetch_nuget_package(microsoft.ai.directml
    "${ALCEDO_DIRECTML_VERSION}"
    "4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9"
    _alcedo_dml_dir
    "bin/x64-win/DirectML.dll")

set(_alcedo_ort_bin "${_alcedo_ort_dir}/runtimes/win-x64/native")
set(ALCEDO_ONNXRUNTIME_DIRECTML_RUNTIME_DLLS
    "${_alcedo_ort_bin}/onnxruntime.dll"
    "${_alcedo_ort_bin}/onnxruntime_providers_shared.dll"
    "${_alcedo_dml_dir}/bin/x64-win/DirectML.dll"
    CACHE INTERNAL "ONNX Runtime DirectML runtime DLLs of AI Denoise.")
foreach(_alcedo_ort_file IN LISTS ALCEDO_ONNXRUNTIME_DIRECTML_RUNTIME_DLLS
        "${_alcedo_ort_bin}/onnxruntime.lib" "${_alcedo_ort_dir}/build/native/include/onnxruntime_cxx_api.h")
    if(NOT EXISTS "${_alcedo_ort_file}")
        message(FATAL_ERROR "ONNX Runtime DirectML package file is missing: ${_alcedo_ort_file}")
    endif()
endforeach()

add_library(alcedo::onnxruntime_directml SHARED IMPORTED GLOBAL)
set_target_properties(alcedo::onnxruntime_directml PROPERTIES
    IMPORTED_LOCATION "${_alcedo_ort_bin}/onnxruntime.dll"
    IMPORTED_IMPLIB "${_alcedo_ort_bin}/onnxruntime.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${_alcedo_ort_dir}/build/native/include"
    # The session calls Ort::InitApi itself (ORT_API_MANUAL_INIT), so the DLL loads at the first
    # session and not at process start.
    INTERFACE_COMPILE_DEFINITIONS "ORT_API_MANUAL_INIT"
    # The host link only: a CUDA library on the link chain passes its link libraries and options
    # to the device link of nvcc. delayimp.lib comes from `#pragma comment(lib)` of the session.
    INTERFACE_LINK_OPTIONS "$<HOST_LINK:/DELAYLOAD:onnxruntime.dll>"
)

set(ALCEDO_ONNXRUNTIME_DIRECTML_COPY_SCRIPT
    "${CMAKE_SOURCE_DIR}/scripts/cmake/copy_onnxruntime_directml_runtime.cmake"
    CACHE INTERNAL "Copies and checks the ONNX Runtime DirectML runtime DLLs.")

function(alcedo_copy_onnxruntime_directml_runtime target_name)
    set(_sidecar_directml "")
    if(DEFINED ALCEDO_MIND_TARGET_DIR AND NOT "${ALCEDO_MIND_TARGET_DIR}" STREQUAL "")
        set(_sidecar_directml "${ALCEDO_MIND_TARGET_DIR}/DirectML.dll")
    endif()
    string(REPLACE ";" "|" _dlls "${ALCEDO_ONNXRUNTIME_DIRECTML_RUNTIME_DLLS}")
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DALCEDO_ORT_DLLS=${_dlls}"
            "-DALCEDO_ORT_DEST_DIR=$<TARGET_FILE_DIR:${target_name}>"
            "-DALCEDO_SIDECAR_DIRECTML=${_sidecar_directml}"
            -P "${ALCEDO_ONNXRUNTIME_DIRECTML_COPY_SCRIPT}"
        COMMENT "Copying ONNX Runtime DirectML DLLs beside ${target_name}"
        VERBATIM)
endfunction()

message(STATUS "ONNX Runtime DirectML ${ALCEDO_ONNXRUNTIME_DIRECTML_VERSION}: ${_alcedo_ort_dir}")
