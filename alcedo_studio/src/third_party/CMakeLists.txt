set(PUERHLAB_LIBRAW_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/libraw"
    CACHE PATH
    "Path to the bundled LibRaw source checkout.")
option(PUERHLAB_USE_BUNDLED_LIBRAW
    "Build the bundled LibRaw checkout instead of a system LibRaw package."
    ON)

set(PUERHLAB_LIBRAW_AVAILABLE OFF)
if(PUERHLAB_USE_BUNDLED_LIBRAW
   AND EXISTS "${PUERHLAB_LIBRAW_SOURCE_DIR}/libraw/libraw.h"
   AND EXISTS "${PUERHLAB_LIBRAW_SOURCE_DIR}/src/libraw_c_api.cpp")
    find_package(Threads REQUIRED)

    file(GLOB_RECURSE PUERHLAB_LIBRAW_SOURCES CONFIGURE_DEPENDS
        "${PUERHLAB_LIBRAW_SOURCE_DIR}/src/*.cpp")
    list(FILTER PUERHLAB_LIBRAW_SOURCES EXCLUDE REGEX
        ".*/src/(preprocessing/preprocessing_ph|postprocessing/postprocessing_ph|write/write_ph)\\.cpp$")

    add_library(puerhlab_libraw STATIC ${PUERHLAB_LIBRAW_SOURCES})
    set_target_properties(puerhlab_libraw PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF)
    target_include_directories(puerhlab_libraw
        SYSTEM
        PUBLIC
            "${PUERHLAB_LIBRAW_SOURCE_DIR}")
    target_link_libraries(puerhlab_libraw PUBLIC Threads::Threads)
    target_compile_options(puerhlab_libraw PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/w>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)
    if(WIN32)
        target_compile_definitions(puerhlab_libraw PUBLIC LIBRAW_NODLL)
    endif()

    if(NOT TARGET libraw::raw_r)
        add_library(libraw::raw_r ALIAS puerhlab_libraw)
    endif()
    set(PUERHLAB_LIBRAW_AVAILABLE ON)
else()
    if(PUERHLAB_USE_BUNDLED_LIBRAW)
        message(WARNING
            "Bundled LibRaw source was not found at ${PUERHLAB_LIBRAW_SOURCE_DIR}. "
            "Run `git submodule update --init --recursive alcedo_studio/src/third_party/libraw` "
            "and re-configure. Falling back to a system LibRaw package.")
    endif()

    if(WIN32)
        find_package(libraw CONFIG REQUIRED)
    else()
        find_path(LIBRAW_INC NAMES libraw/libraw.h PATHS ${ALCEDO_INCLUDE_HINTS})
        find_library(LIBRAW_LIB NAMES raw_r raw libraw PATHS ${ALCEDO_LIBRARY_HINTS})
        if(LIBRAW_INC AND LIBRAW_LIB)
            add_library(libraw::raw_r SHARED IMPORTED)
            set_target_properties(libraw::raw_r PROPERTIES
                IMPORTED_LOCATION "${LIBRAW_LIB}"
                INTERFACE_INCLUDE_DIRECTORIES "${LIBRAW_INC}"
            )
        else()
            message(FATAL_ERROR
                "LibRaw not found. Initialize the bundled LibRaw submodule, install LibRaw, "
                "or set CMAKE_PREFIX_PATH to its prefix.")
        endif()
    endif()
endif()

if(TARGET libraw::raw AND NOT TARGET libraw::raw_r)
    get_target_property(_puerhlab_libraw_raw_alias_target libraw::raw ALIASED_TARGET)
    if(_puerhlab_libraw_raw_alias_target)
        add_library(libraw::raw_r ALIAS "${_puerhlab_libraw_raw_alias_target}")
    else()
        add_library(libraw::raw_r ALIAS libraw::raw)
    endif()
elseif(TARGET libraw::raw_r AND NOT TARGET libraw::raw)
    get_target_property(_puerhlab_libraw_raw_r_alias_target libraw::raw_r ALIASED_TARGET)
    if(_puerhlab_libraw_raw_r_alias_target)
        add_library(libraw::raw ALIAS "${_puerhlab_libraw_raw_r_alias_target}")
    else()
        add_library(libraw::raw ALIAS libraw::raw_r)
    endif()
endif()

if(TARGET libraw::raw_r AND NOT TARGET LibRaw)
    get_target_property(_puerhlab_libraw_raw_r_alias_target libraw::raw_r ALIASED_TARGET)
    if(_puerhlab_libraw_raw_r_alias_target)
        add_library(LibRaw ALIAS "${_puerhlab_libraw_raw_r_alias_target}")
    else()
        add_library(LibRaw ALIAS libraw::raw_r)
    endif()
endif()

set(PUERHLAB_LIBRAW_AVAILABLE "${PUERHLAB_LIBRAW_AVAILABLE}" CACHE INTERNAL
    "Whether the bundled LibRaw checkout is being built.")

set(PUERHLAB_LENSFUN_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/lensfun"
    CACHE PATH
    "Path to the bundled Lensfun source checkout.")
set(PUERHLAB_LENSFUN_BINARY_DIR
    "${CMAKE_BINARY_DIR}/third_party/lensfun"
    CACHE PATH
    "Out-of-source build directory for the bundled Lensfun build.")
set(PUERHLAB_LENSFUN_INSTALL_DIR
    "${PUERHLAB_LENSFUN_BINARY_DIR}/install"
    CACHE PATH
    "Install prefix for the bundled Lensfun build.")

file(MAKE_DIRECTORY
    "${PUERHLAB_LENSFUN_INSTALL_DIR}/bin"
    "${PUERHLAB_LENSFUN_INSTALL_DIR}/include"
    "${PUERHLAB_LENSFUN_INSTALL_DIR}/lib")

if(NOT EXISTS "${PUERHLAB_LENSFUN_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Bundled Lensfun source was not found at ${PUERHLAB_LENSFUN_SOURCE_DIR}. "
        "Run `git submodule update --init --recursive pu-erh_lab/src/third_party/lensfun` and re-configure.")
endif()

set(_puerhlab_lensfun_cmake_args
    "-DCMAKE_INSTALL_PREFIX:PATH=${PUERHLAB_LENSFUN_INSTALL_DIR}"
    "-DCMAKE_INSTALL_DATAROOTDIR:PATH=share"
    "-DCMAKE_MODULE_PATH:STRING=${CMAKE_CURRENT_SOURCE_DIR}/cmake"
    "-DBUILD_STATIC:BOOL=OFF"
    "-DBUILD_TESTS:BOOL=OFF"
    "-DBUILD_LENSTOOL:BOOL=OFF"
    "-DBUILD_DOC:BOOL=OFF"
    "-DINSTALL_PYTHON_MODULE:BOOL=OFF"
    "-DINSTALL_HELPER_SCRIPTS:BOOL=OFF"
)

if(CMAKE_BUILD_TYPE)
    list(APPEND _puerhlab_lensfun_cmake_args
        "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}")
endif()

if(CMAKE_PREFIX_PATH)
    set(_puerhlab_lensfun_prefix_path "${CMAKE_PREFIX_PATH}")
    string(REPLACE ";" "\\;" _puerhlab_lensfun_prefix_path "${_puerhlab_lensfun_prefix_path}")
    list(APPEND _puerhlab_lensfun_cmake_args
        "-DCMAKE_PREFIX_PATH:STRING=${_puerhlab_lensfun_prefix_path}")
endif()

foreach(_tool_var IN ITEMS CMAKE_TOOLCHAIN_FILE CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_MAKE_PROGRAM)
    if(DEFINED ${_tool_var} AND NOT "${${_tool_var}}" STREQUAL "")
        list(APPEND _puerhlab_lensfun_cmake_args
            "-D${_tool_var}:FILEPATH=${${_tool_var}}")
    endif()
endforeach()

if(DEFINED CMAKE_OSX_ARCHITECTURES AND NOT "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
    set(_puerhlab_lensfun_osx_arches "${CMAKE_OSX_ARCHITECTURES}")
    string(REPLACE ";" "\\;" _puerhlab_lensfun_osx_arches "${_puerhlab_lensfun_osx_arches}")
    list(APPEND _puerhlab_lensfun_cmake_args
        "-DCMAKE_OSX_ARCHITECTURES:STRING=${_puerhlab_lensfun_osx_arches}")
endif()

if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
    list(APPEND _puerhlab_lensfun_cmake_args
        "-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

if(WIN32)
    set(_puerhlab_lensfun_legacy_glib2_base
        "${CMAKE_SOURCE_DIR}/pu-erh_lab/third_party/glib-2.28.1")
    set(_puerhlab_lensfun_default_glib2_base
        "${_puerhlab_lensfun_legacy_glib2_base}")
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(_puerhlab_lensfun_vcpkg_glib2_base
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
        if(EXISTS "${_puerhlab_lensfun_vcpkg_glib2_base}/include/glib-2.0/glib.h")
            set(_puerhlab_lensfun_default_glib2_base
                "${_puerhlab_lensfun_vcpkg_glib2_base}")
        endif()
    endif()

    set(_puerhlab_lensfun_configured_glib2_base "${PUERHLAB_LENSFUN_GLIB2_BASE_DIR}")
    if(NOT DEFINED PUERHLAB_LENSFUN_GLIB2_BASE_DIR
       OR "${_puerhlab_lensfun_configured_glib2_base}" STREQUAL ""
       OR ("${_puerhlab_lensfun_configured_glib2_base}" STREQUAL "${_puerhlab_lensfun_legacy_glib2_base}"
           AND NOT EXISTS "${_puerhlab_lensfun_configured_glib2_base}"))
        set(_puerhlab_lensfun_configured_glib2_base
            "${_puerhlab_lensfun_default_glib2_base}")
        set(_puerhlab_lensfun_force_glib2_base FORCE)
    else()
        set(_puerhlab_lensfun_force_glib2_base)
    endif()

    set(PUERHLAB_LENSFUN_GLIB2_BASE_DIR
        "${_puerhlab_lensfun_configured_glib2_base}"
        CACHE PATH
        "Base path of the GLib2 package used by the bundled Lensfun build on Windows."
        ${_puerhlab_lensfun_force_glib2_base})
    if(EXISTS "${PUERHLAB_LENSFUN_GLIB2_BASE_DIR}")
        list(APPEND _puerhlab_lensfun_cmake_args
            "-DGLIB2_BASE_DIR:PATH=${PUERHLAB_LENSFUN_GLIB2_BASE_DIR}")
    else()
        message(STATUS
            "PUERHLAB_LENSFUN_GLIB2_BASE_DIR does not exist. "
            "Install vcpkg package `glib` or set it explicitly if the bundled Lensfun configure step cannot find GLib2 on Windows.")
    endif()
endif()

if(WIN32)
    set(PUERHLAB_LENSFUN_IMPORTED_LOCATION "${PUERHLAB_LENSFUN_INSTALL_DIR}/bin/lensfun.dll")
    set(PUERHLAB_LENSFUN_IMPORTED_IMPLIB "${PUERHLAB_LENSFUN_INSTALL_DIR}/lib/lensfun.lib")
elseif(APPLE)
    set(PUERHLAB_LENSFUN_IMPORTED_LOCATION "${PUERHLAB_LENSFUN_INSTALL_DIR}/lib/liblensfun.dylib")
else()
    set(PUERHLAB_LENSFUN_IMPORTED_LOCATION "${PUERHLAB_LENSFUN_INSTALL_DIR}/lib/liblensfun.so")
endif()

set(_puerhlab_lensfun_byproducts
    "${PUERHLAB_LENSFUN_IMPORTED_LOCATION}"
    "${PUERHLAB_LENSFUN_INSTALL_DIR}/include/lensfun/lensfun.h"
)
if(WIN32)
    list(APPEND _puerhlab_lensfun_byproducts "${PUERHLAB_LENSFUN_IMPORTED_IMPLIB}")
endif()

set(_puerhlab_lensfun_args_manifest "${CMAKE_CURRENT_BINARY_DIR}/puerhlab_lensfun_args.txt")
string(REPLACE ";" "\n" _puerhlab_lensfun_args_manifest_contents "${_puerhlab_lensfun_cmake_args}")
file(GENERATE OUTPUT "${_puerhlab_lensfun_args_manifest}"
    CONTENT "${_puerhlab_lensfun_args_manifest_contents}\n")

set(_puerhlab_lensfun_configure_stamp "${PUERHLAB_LENSFUN_BINARY_DIR}/.puerhlab-lensfun-configured")

add_custom_command(
    OUTPUT "${_puerhlab_lensfun_configure_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PUERHLAB_LENSFUN_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND}
        -S "${PUERHLAB_LENSFUN_SOURCE_DIR}"
        -B "${PUERHLAB_LENSFUN_BINARY_DIR}"
        -G "${CMAKE_GENERATOR}"
        ${_puerhlab_lensfun_cmake_args}
    COMMAND ${CMAKE_COMMAND} -E touch "${_puerhlab_lensfun_configure_stamp}"
    DEPENDS
        "${PUERHLAB_LENSFUN_SOURCE_DIR}/CMakeLists.txt"
        "${_puerhlab_lensfun_args_manifest}"
    VERBATIM
)

add_custom_command(
    OUTPUT ${_puerhlab_lensfun_byproducts}
    COMMAND ${CMAKE_COMMAND} --build "${PUERHLAB_LENSFUN_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} --install "${PUERHLAB_LENSFUN_BINARY_DIR}" --prefix "${PUERHLAB_LENSFUN_INSTALL_DIR}"
    DEPENDS "${_puerhlab_lensfun_configure_stamp}"
    VERBATIM
)

add_custom_target(puerhlab_lensfun_build DEPENDS ${_puerhlab_lensfun_byproducts})

add_library(puerhlab_lensfun SHARED IMPORTED GLOBAL)
set_target_properties(puerhlab_lensfun PROPERTIES
    IMPORTED_LOCATION "${PUERHLAB_LENSFUN_IMPORTED_LOCATION}"
    INTERFACE_INCLUDE_DIRECTORIES "${PUERHLAB_LENSFUN_INSTALL_DIR}/include"
)
if(WIN32)
    set_target_properties(puerhlab_lensfun PROPERTIES
        IMPORTED_IMPLIB "${PUERHLAB_LENSFUN_IMPORTED_IMPLIB}")
endif()
add_dependencies(puerhlab_lensfun puerhlab_lensfun_build)

if(NOT TARGET alcedo_lensfun)
    add_library(alcedo_lensfun ALIAS puerhlab_lensfun)
endif()

if(NOT TARGET lensfun::lensfun)
    add_library(lensfun::lensfun ALIAS puerhlab_lensfun)
endif()

set(PUERHLAB_LENSFUN_INCLUDE_DIR "${PUERHLAB_LENSFUN_INSTALL_DIR}/include" CACHE INTERNAL
    "Header directory for the bundled Lensfun build.")
set(PUERHLAB_LENSFUN_LIBRARY_DIR "${PUERHLAB_LENSFUN_INSTALL_DIR}/lib" CACHE INTERNAL
    "Runtime library directory for the bundled Lensfun build.")
set(ALCEDO_LENSFUN_INCLUDE_DIR "${PUERHLAB_LENSFUN_INCLUDE_DIR}" CACHE INTERNAL
    "Header directory for the bundled Lensfun build.")
set(ALCEDO_LENSFUN_LIBRARY_DIR "${PUERHLAB_LENSFUN_LIBRARY_DIR}" CACHE INTERNAL
    "Runtime library directory for the bundled Lensfun build.")

set(PUERHLAB_ULTRAHDR_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/libultrahdr"
    CACHE PATH
    "Path to the libultrahdr submodule checkout.")

if(NOT EXISTS "${PUERHLAB_ULTRAHDR_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "libultrahdr source was not found at ${PUERHLAB_ULTRAHDR_SOURCE_DIR}. "
        "Run `git submodule update --init --recursive alcedo_studio/src/third_party/libultrahdr` "
        "and re-configure.")
endif()

set(UHDR_BUILD_EXAMPLES OFF CACHE BOOL "Disable libultrahdr examples" FORCE)
set(UHDR_BUILD_TESTS OFF CACHE BOOL "Disable libultrahdr tests" FORCE)
set(UHDR_BUILD_BENCHMARK OFF CACHE BOOL "Disable libultrahdr benchmarks" FORCE)
set(UHDR_BUILD_FUZZERS OFF CACHE BOOL "Disable libultrahdr fuzzers" FORCE)
set(UHDR_BUILD_DEPS OFF CACHE BOOL "Use system libjpeg when building libultrahdr" FORCE)
set(UHDR_BUILD_JAVA OFF CACHE BOOL "Disable libultrahdr JNI bindings" FORCE)
set(UHDR_BUILD_PACKAGING OFF CACHE BOOL "Disable libultrahdr packaging" FORCE)
set(UHDR_ENABLE_INSTALL OFF CACHE BOOL "Disable libultrahdr install target" FORCE)
set(UHDR_ENABLE_GLES OFF CACHE BOOL "Disable libultrahdr GLES path" FORCE)
set(UHDR_ENABLE_WERROR OFF CACHE BOOL "Disable libultrahdr -Werror" FORCE)
set(UHDR_WRITE_XMP ON CACHE BOOL "Enable XMP gainmap metadata packets" FORCE)
set(UHDR_WRITE_ISO ON CACHE BOOL "Enable ISO 21496-1 gainmap metadata packets" FORCE)

set(PUERHLAB_ULTRAHDR_IMAGE_IO_HEADER
    "${PUERHLAB_ULTRAHDR_SOURCE_DIR}/third_party/image_io/includes/image_io/base/data_segment_data_source.h")
set(PUERHLAB_ULTRAHDR_AVAILABLE OFF)
if(EXISTS "${PUERHLAB_ULTRAHDR_IMAGE_IO_HEADER}")
    add_subdirectory(
        "${PUERHLAB_ULTRAHDR_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/third_party/libultrahdr"
    )

    if(TARGET uhdr)
        add_library(puerhlab_ultrahdr INTERFACE)
        target_link_libraries(puerhlab_ultrahdr INTERFACE uhdr)
    elseif(TARGET uhdr-static)
        add_library(puerhlab_ultrahdr INTERFACE)
        target_link_libraries(puerhlab_ultrahdr INTERFACE uhdr-static)
    else()
        message(FATAL_ERROR "Bundled libultrahdr was configured, but no usable target was exported.")
    endif()

    target_include_directories(puerhlab_ultrahdr
        INTERFACE
            "${PUERHLAB_ULTRAHDR_SOURCE_DIR}"
            "${PUERHLAB_ULTRAHDR_SOURCE_DIR}/lib/include")

    if(NOT TARGET puerhlab::uhdr)
        add_library(puerhlab::uhdr ALIAS puerhlab_ultrahdr)
    endif()

    set(PUERHLAB_ULTRAHDR_AVAILABLE ON)
else()
    message(WARNING
        "libultrahdr checkout is incomplete: missing ${PUERHLAB_ULTRAHDR_IMAGE_IO_HEADER}. "
        "Run `git submodule update --init --recursive alcedo_studio/src/third_party/libultrahdr` "
        "and re-configure. Ultra HDR support will be disabled for this build.")
endif()

set(PUERHLAB_ULTRAHDR_AVAILABLE "${PUERHLAB_ULTRAHDR_AVAILABLE}" CACHE INTERNAL
    "Whether the libultrahdr submodule checkout is complete enough to build.")

set(PUERHLAB_PROTOBUF_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/protobuf"
    CACHE PATH
    "Path to the protobuf submodule checkout.")
set(PUERHLAB_GRPC_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/grpc"
    CACHE PATH
    "Path to the gRPC submodule checkout.")

set(PUERHLAB_USE_SYSTEM_GRPC_PROTOBUF_DEFAULT OFF)
if(APPLE AND (NOT EXISTS "${PUERHLAB_PROTOBUF_SOURCE_DIR}/CMakeLists.txt" OR
             NOT EXISTS "${PUERHLAB_GRPC_SOURCE_DIR}/CMakeLists.txt"))
    set(PUERHLAB_USE_SYSTEM_GRPC_PROTOBUF_DEFAULT ON)
endif()
option(PUERHLAB_USE_SYSTEM_GRPC_PROTOBUF
    "Use system-installed gRPC/protobuf packages instead of the bundled source checkouts."
    "${PUERHLAB_USE_SYSTEM_GRPC_PROTOBUF_DEFAULT}")

if(PUERHLAB_USE_SYSTEM_GRPC_PROTOBUF)
    find_package(Protobuf CONFIG REQUIRED)
    find_package(gRPC CONFIG REQUIRED)

    if(TARGET gRPC::grpc++)
        set(_puerhlab_grpcpp_target gRPC::grpc++)
    elseif(TARGET grpc++)
        set(_puerhlab_grpcpp_target grpc++)
    else()
        message(FATAL_ERROR
            "System gRPC was found, but no grpc++ target was exported. "
            "Install gRPC with CMake package support, for example `brew install grpc`.")
    endif()

    if(TARGET protobuf::libprotobuf)
        set(_puerhlab_protobuf_target protobuf::libprotobuf)
    elseif(TARGET libprotobuf)
        set(_puerhlab_protobuf_target libprotobuf)
    else()
        message(FATAL_ERROR
            "System protobuf was found, but no libprotobuf target was exported. "
            "Install protobuf with CMake package support, for example `brew install protobuf`.")
    endif()

    if(TARGET protobuf::protoc)
        get_target_property(_puerhlab_protoc_executable protobuf::protoc IMPORTED_LOCATION_RELEASE)
        if(NOT _puerhlab_protoc_executable)
            get_target_property(_puerhlab_protoc_executable protobuf::protoc IMPORTED_LOCATION)
        endif()
    endif()
    if(NOT _puerhlab_protoc_executable)
        find_program(_puerhlab_protoc_executable NAMES protoc REQUIRED)
    endif()

    if(TARGET gRPC::grpc_cpp_plugin)
        get_target_property(_puerhlab_grpc_cpp_plugin_executable
            gRPC::grpc_cpp_plugin IMPORTED_LOCATION_RELEASE)
        if(NOT _puerhlab_grpc_cpp_plugin_executable)
            get_target_property(_puerhlab_grpc_cpp_plugin_executable
                gRPC::grpc_cpp_plugin IMPORTED_LOCATION)
        endif()
    endif()
    if(NOT _puerhlab_grpc_cpp_plugin_executable)
        find_program(_puerhlab_grpc_cpp_plugin_executable NAMES grpc_cpp_plugin REQUIRED)
    endif()
else()
    if(NOT EXISTS "${PUERHLAB_PROTOBUF_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "protobuf source was not found at ${PUERHLAB_PROTOBUF_SOURCE_DIR}. "
            "Run `git submodule update --init --recursive alcedo_studio/src/third_party/protobuf` "
            "and re-configure, or set PUERHLAB_USE_SYSTEM_GRPC_PROTOBUF=ON.")
    endif()

    if(NOT EXISTS "${PUERHLAB_GRPC_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "gRPC source was not found at ${PUERHLAB_GRPC_SOURCE_DIR}. "
            "Run `git submodule update --init --recursive alcedo_studio/src/third_party/grpc` "
            "and re-configure, or set PUERHLAB_USE_SYSTEM_GRPC_PROTOBUF=ON.")
    endif()

    set(_puerhlab_grpc_required_paths
        "third_party/abseil-cpp/CMakeLists.txt"
        "third_party/boringssl-with-bazel/CMakeLists.txt"
        "third_party/cares/cares/CMakeLists.txt"
        "third_party/re2/CMakeLists.txt"
        "third_party/zlib/CMakeLists.txt"
    )
    foreach(_puerhlab_grpc_required_path IN LISTS _puerhlab_grpc_required_paths)
        if(NOT EXISTS "${PUERHLAB_GRPC_SOURCE_DIR}/${_puerhlab_grpc_required_path}")
            message(FATAL_ERROR
                "gRPC submodule dependency is missing: "
                "${PUERHLAB_GRPC_SOURCE_DIR}/${_puerhlab_grpc_required_path}. "
                "Run `git submodule update --init --recursive alcedo_studio/src/third_party/grpc` "
                "and re-configure.")
        endif()
    endforeach()

    set(protobuf_BUILD_TESTS OFF CACHE BOOL "Disable protobuf tests" FORCE)
    set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "Disable protobuf conformance tests" FORCE)
    set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "Disable protobuf examples" FORCE)
    set(protobuf_INSTALL OFF CACHE BOOL "Disable protobuf install rules" FORCE)
    set(protobuf_WITH_ZLIB OFF CACHE BOOL "Build protobuf without zlib" FORCE)
    set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "Use the project MSVC runtime for protobuf" FORCE)

    set(gRPC_BUILD_TESTS OFF CACHE BOOL "Disable gRPC tests" FORCE)
    set(gRPC_BUILD_CODEGEN ON CACHE BOOL "Build gRPC code generation tools" FORCE)
    set(gRPC_DOWNLOAD_ARCHIVES OFF CACHE BOOL "Do not download missing gRPC third-party archives" FORCE)
    set(gRPC_INSTALL OFF CACHE BOOL "Disable gRPC install rules" FORCE)
    set(gRPC_PROTOBUF_PROVIDER "module" CACHE STRING "Use bundled protobuf for gRPC" FORCE)
    set(PROTOBUF_ROOT_DIR "${PUERHLAB_PROTOBUF_SOURCE_DIR}" CACHE PATH
        "Use the sibling protobuf submodule for the bundled gRPC build." FORCE)
    set(gRPC_ABSL_PROVIDER "module" CACHE STRING "Use bundled Abseil for gRPC" FORCE)
    set(gRPC_CARES_PROVIDER "module" CACHE STRING "Use bundled c-ares for gRPC" FORCE)
    set(gRPC_RE2_PROVIDER "module" CACHE STRING "Use bundled RE2 for gRPC" FORCE)
    set(gRPC_SSL_PROVIDER "module" CACHE STRING "Use bundled BoringSSL for gRPC" FORCE)
    set(gRPC_ZLIB_PROVIDER "module" CACHE STRING "Use bundled zlib for gRPC" FORCE)
    set(gRPC_BUILD_GRPCPP_OTEL_PLUGIN OFF CACHE BOOL "Disable gRPC OpenTelemetry plugin" FORCE)
    if(MSVC)
        set(gRPC_BUILD_MSVC_MP_COUNT 0 CACHE STRING "Inherit project-level parallelism" FORCE)
    endif()

    if(MSVC)
        set(_puerhlab_saved_c_flags_release "${CMAKE_C_FLAGS_RELEASE}")
        set(_puerhlab_saved_cxx_flags_release "${CMAKE_CXX_FLAGS_RELEASE}")
        string(REPLACE "/GL" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
        string(REPLACE "/GL" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    endif()

    add_subdirectory(
        "${PUERHLAB_GRPC_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/third_party/grpc"
    )

    if(MSVC)
        set(CMAKE_C_FLAGS_RELEASE "${_puerhlab_saved_c_flags_release}")
        set(CMAKE_CXX_FLAGS_RELEASE "${_puerhlab_saved_cxx_flags_release}")
        unset(_puerhlab_saved_c_flags_release)
        unset(_puerhlab_saved_cxx_flags_release)
    endif()

    if(MSVC)
        foreach(_puerhlab_grpc_warning_target
                grpc
                grpc++
                grpc_unsecure
                grpc++_unsecure
                libprotobuf
                libprotoc
                protoc
                grpc_cpp_plugin)
            if(TARGET ${_puerhlab_grpc_warning_target})
                target_compile_options(${_puerhlab_grpc_warning_target} PRIVATE /wd4789)
            endif()
        endforeach()
        unset(_puerhlab_grpc_warning_target)
    endif()

    if(NOT TARGET grpc++)
        message(FATAL_ERROR "Bundled gRPC was configured, but target grpc++ is unavailable.")
    endif()
    if(NOT TARGET protobuf::libprotobuf)
        message(FATAL_ERROR "Bundled protobuf was configured, but target protobuf::libprotobuf is unavailable.")
    endif()

    set(_puerhlab_grpcpp_target grpc++)
    set(_puerhlab_protobuf_target protobuf::libprotobuf)
    set(_puerhlab_protoc_executable "$<TARGET_FILE:protobuf::protoc>")
    set(_puerhlab_grpc_cpp_plugin_executable "$<TARGET_FILE:grpc_cpp_plugin>")
endif()

add_library(puerhlab_grpcpp INTERFACE)
target_link_libraries(puerhlab_grpcpp INTERFACE ${_puerhlab_grpcpp_target})
add_library(puerhlab::grpc++ ALIAS puerhlab_grpcpp)

add_library(puerhlab_protobuf INTERFACE)
target_link_libraries(puerhlab_protobuf INTERFACE ${_puerhlab_protobuf_target})
add_library(puerhlab::protobuf ALIAS puerhlab_protobuf)

set(PUERHLAB_PROTOC_EXECUTABLE "${_puerhlab_protoc_executable}" CACHE INTERNAL
    "protoc executable used to generate Alcedo semantic protobuf sources.")
set(PUERHLAB_GRPC_CPP_PLUGIN_EXECUTABLE "${_puerhlab_grpc_cpp_plugin_executable}" CACHE INTERNAL
    "grpc_cpp_plugin executable used to generate Alcedo semantic gRPC sources.")

set(PUERHLAB_METALCPP_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/metal-cpp"
    CACHE PATH
    "Path to the bundled metal-cpp source checkout.")
set(PUERHLAB_METALCPP_HELPER_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    CACHE INTERNAL
    "Header directory for project-local metal-cpp wrapper headers.")

if(NOT EXISTS "${PUERHLAB_METALCPP_SOURCE_DIR}/Metal/Metal.hpp")
    message(FATAL_ERROR
        "Bundled metal-cpp source was not found at ${PUERHLAB_METALCPP_SOURCE_DIR}. "
        "Run `git submodule update --init --recursive pu-erh_lab/src/third_party/metal-cpp` and re-configure.")
endif()

add_library(puerhlab_metal_cpp INTERFACE)
target_include_directories(puerhlab_metal_cpp
    INTERFACE
        "${PUERHLAB_METALCPP_HELPER_INCLUDE_DIR}"
)
target_include_directories(puerhlab_metal_cpp
    SYSTEM INTERFACE
        "${PUERHLAB_METALCPP_SOURCE_DIR}"
)
target_compile_features(puerhlab_metal_cpp INTERFACE cxx_std_20)

if(APPLE)
    find_library(PUERHLAB_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(PUERHLAB_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(PUERHLAB_METAL_FRAMEWORK Metal REQUIRED)

    add_library(puerhlab_metal_cpp_runtime STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/puerhlab_metal_cpp_implementation.cpp"
    )
    set_target_properties(puerhlab_metal_cpp_runtime PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_link_libraries(puerhlab_metal_cpp_runtime
        PUBLIC
            "${PUERHLAB_FOUNDATION_FRAMEWORK}"
            "${PUERHLAB_QUARTZCORE_FRAMEWORK}"
            "${PUERHLAB_METAL_FRAMEWORK}"
    )
    target_include_directories(puerhlab_metal_cpp_runtime
        PRIVATE
            "${PUERHLAB_METALCPP_HELPER_INCLUDE_DIR}"
    )
    target_include_directories(puerhlab_metal_cpp_runtime
        SYSTEM PRIVATE
            "${PUERHLAB_METALCPP_SOURCE_DIR}"
    )

    target_link_libraries(puerhlab_metal_cpp
        INTERFACE
            puerhlab_metal_cpp_runtime
            "${PUERHLAB_FOUNDATION_FRAMEWORK}"
            "${PUERHLAB_QUARTZCORE_FRAMEWORK}"
            "${PUERHLAB_METAL_FRAMEWORK}"
    )
endif()

if(NOT TARGET puerhlab::metal_cpp)
    add_library(puerhlab::metal_cpp ALIAS puerhlab_metal_cpp)
endif()
if(NOT TARGET alcedo::metal_cpp)
    add_library(alcedo::metal_cpp ALIAS puerhlab_metal_cpp)
endif()

set(PUERHLAB_METALCPP_INCLUDE_DIR "${PUERHLAB_METALCPP_SOURCE_DIR}" CACHE INTERNAL
    "Header directory for the bundled metal-cpp checkout.")
