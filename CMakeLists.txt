cmake_minimum_required(VERSION 3.19)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND
   (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL ""))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version supported by the built binaries." FORCE)
endif()

#set(CMAKE_C_COMPILER "clang")
#set(CMAKE_CXX_COMPILER "clang++")
# Compiler flags.

project(alcedo VERSION 0.2.5 LANGUAGES C CXX)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build alcedo libraries as static by default." FORCE)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# set(BUILD_STATIC_LIBS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    set(ALCEDO_ENABLE_CUDA_DEFAULT OFF)
    set(ALCEDO_ENABLE_METAL_DEFAULT ON)
    set(ALCEDO_ENABLE_OPENCL_DEFAULT OFF)
    set(ALCEDO_ENABLE_OPENGL_EDITOR_DEFAULT OFF)
else()
    set(ALCEDO_ENABLE_CUDA_DEFAULT ON)
    set(ALCEDO_ENABLE_METAL_DEFAULT OFF)
    set(ALCEDO_ENABLE_OPENCL_DEFAULT ON)
    set(ALCEDO_ENABLE_OPENGL_EDITOR_DEFAULT ON)
endif()

option(ALCEDO_ENABLE_CUDA "Enable CUDA-accelerated pipeline backends when CUDA is available." ${ALCEDO_ENABLE_CUDA_DEFAULT})
option(ALCEDO_ENABLE_METAL "Enable Metal-accelerated pipeline backends when building on Apple platforms." ${ALCEDO_ENABLE_METAL_DEFAULT})
option(ALCEDO_ENABLE_OPENCL "Enable OpenCL support when the OpenCL SDK/ICD loader is available." ${ALCEDO_ENABLE_OPENCL_DEFAULT})
option(ALCEDO_ENABLE_OPENGL_EDITOR "Build the OpenGL-based editor UI when the required backend is available." ${ALCEDO_ENABLE_OPENGL_EDITOR_DEFAULT})
option(ALCEDO_BUILD_TESTS "Build alcedo tests and demos." ON)
option(ALCEDO_MACOS_BUNDLE
       "Build alcedo_main as a macOS .app bundle (intended for packaging/install)."
       OFF)
option(ALCEDO_MACDEPLOYQT_STRIP
       "Allow macdeployqt to strip binaries during macOS app deployment."
       OFF)
option(ALCEDO_MACOS_VERIFY_BUNDLE_DEPS
       "Fail macOS packaging when the app bundle contains dependencies that require the build machine."
       ON)
option(ALCEDO_GENERATE_QMLTYPES
       "Generate QML type metadata for the alcedo_main QML module."
       ON)
set(ALCEDO_MACOS_CODESIGN_IDENTITY "-"
    CACHE STRING
    "Code-sign identity for macOS bundle after deployment (use '-' for ad-hoc signing, empty to skip).")
set(ALCEDO_QT_PREFIX ""
    CACHE PATH
    "Optional Qt installation prefix for macOS packaging builds (contains bin/, lib/cmake/Qt6, plugins/, and qml/).")
if(NOT DEFINED ALCEDO_MACOS_BUNDLE_NAME OR
   "${ALCEDO_MACOS_BUNDLE_NAME}" STREQUAL "" OR
   "${ALCEDO_MACOS_BUNDLE_NAME}" STREQUAL "PuerhLab")
    set(ALCEDO_MACOS_BUNDLE_NAME "AlcedoStudio"
        CACHE STRING
        "Display/executable name for the packaged macOS .app bundle."
        FORCE)
else()
    set(ALCEDO_MACOS_BUNDLE_NAME "${ALCEDO_MACOS_BUNDLE_NAME}"
        CACHE STRING
        "Display/executable name for the packaged macOS .app bundle.")
endif()
set(ALCEDO_MACOS_BUNDLE_IDENTIFIER "studio.alcedo.AlcedoStudio"
    CACHE STRING
    "Bundle identifier for the packaged macOS .app bundle.")

function(alcedo_append_existing_prefix prefix)
    if(EXISTS "${prefix}")
        list(APPEND CMAKE_PREFIX_PATH "${prefix}")
        set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)
    endif()
endfunction()

set(_alcedo_qt_prefix_hint "")
if(ALCEDO_QT_PREFIX)
    file(TO_CMAKE_PATH "${ALCEDO_QT_PREFIX}" _alcedo_qt_prefix_hint)
    if(EXISTS "${_alcedo_qt_prefix_hint}")
        list(PREPEND CMAKE_PREFIX_PATH "${_alcedo_qt_prefix_hint}")
    else()
        message(WARNING "ALCEDO_QT_PREFIX does not exist: ${_alcedo_qt_prefix_hint}")
    endif()
endif()

alcedo_append_existing_prefix("/opt/homebrew")
alcedo_append_existing_prefix("/opt/homebrew/opt/qt")
alcedo_append_existing_prefix("/opt/homebrew/opt/opencv")
alcedo_append_existing_prefix("/opt/homebrew/opt/opencolorio")
alcedo_append_existing_prefix("/opt/homebrew/opt/openimageio")
alcedo_append_existing_prefix("/opt/homebrew/opt/exiv2")
alcedo_append_existing_prefix("/opt/homebrew/opt/glib")
alcedo_append_existing_prefix("/opt/homebrew/opt/libraw")
alcedo_append_existing_prefix("/opt/homebrew/opt/xxhash")
alcedo_append_existing_prefix("/opt/homebrew/opt/duckdb")
alcedo_append_existing_prefix("/opt/homebrew/opt/little-cms2")
alcedo_append_existing_prefix("/opt/homebrew/opt/highway")
alcedo_append_existing_prefix("/opt/homebrew/opt/openblas")
alcedo_append_existing_prefix("/opt/homebrew/opt/gcc")

alcedo_append_existing_prefix("/usr/local")
alcedo_append_existing_prefix("/usr/local/opt/qt")
alcedo_append_existing_prefix("/usr/local/opt/opencv")
alcedo_append_existing_prefix("/usr/local/opt/opencolorio")
alcedo_append_existing_prefix("/usr/local/opt/openimageio")
alcedo_append_existing_prefix("/usr/local/opt/exiv2")
alcedo_append_existing_prefix("/usr/local/opt/glib")
alcedo_append_existing_prefix("/usr/local/opt/libraw")
alcedo_append_existing_prefix("/usr/local/opt/xxhash")
alcedo_append_existing_prefix("/usr/local/opt/duckdb")
alcedo_append_existing_prefix("/usr/local/opt/little-cms2")
alcedo_append_existing_prefix("/usr/local/opt/highway")
alcedo_append_existing_prefix("/usr/local/opt/openblas")
alcedo_append_existing_prefix("/usr/local/opt/gcc")

set(ALCEDO_LIBRARY_HINTS "")
set(ALCEDO_INCLUDE_HINTS "")

foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_prefix}")
        if(EXISTS "${_prefix}/lib")
            list(APPEND ALCEDO_LIBRARY_HINTS "${_prefix}/lib")
        endif()
        if(EXISTS "${_prefix}/include")
            list(APPEND ALCEDO_INCLUDE_HINTS "${_prefix}/include")
        endif()
    endif()
endforeach()

# Extra GCC runtime directories for libgfortran/libquadmath/libgcc_s.
# Homebrew gcc may place runtime dylibs in versioned subdirectories.
foreach(_gcc_prefix IN ITEMS "/opt/homebrew/opt/gcc" "/usr/local/opt/gcc")
    if(EXISTS "${_gcc_prefix}")
        if(EXISTS "${_gcc_prefix}/lib")
            list(APPEND ALCEDO_LIBRARY_HINTS "${_gcc_prefix}/lib")
        endif()

        file(GLOB _alcedo_gcc_runtime_dirs
            LIST_DIRECTORIES true
            "${_gcc_prefix}/lib/gcc/current"
            "${_gcc_prefix}/lib/gcc/*"
        )
        foreach(_dir IN LISTS _alcedo_gcc_runtime_dirs)
            if(IS_DIRECTORY "${_dir}")
                list(APPEND ALCEDO_LIBRARY_HINTS "${_dir}")
            endif()
        endforeach()
        unset(_alcedo_gcc_runtime_dirs)
    endif()
endforeach()

list(REMOVE_DUPLICATES ALCEDO_LIBRARY_HINTS)
list(REMOVE_DUPLICATES ALCEDO_INCLUDE_HINTS)



if (MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "/Zi /arch:AVX /bigobj /W4 /external:anglebrackets /external:W0 /D_DEBUG /EHsc /openmp /Zc:preprocessor")
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /arch:AVX2 /bigobj /W4 /external:anglebrackets /external:W0 /EHsc /openmp /GL /Zc:preprocessor /MP")
    # add_link_options(/DEBUG)
    set(CMAKE_VS_USE_DEBUG_LIBRARIES "$<CONFIG:Debug,Custom>")
    # FIXME: Hardcoded Qt path for Windows, consider making it configurable
    set(CMAKE_PREFIX_PATH "D:/misc/Qt/6.9.3/msvc2022_64/lib/cmake;${CMAKE_PREFIX_PATH}")
else()
    #set(CMAKE_CXX_FLAGS_INIT "-Wall -Wextra -Wpedantic -D_DEBUG -fopenmp -O2 -fsanitize=address -g")

    add_compile_options(-Wall -Wextra -Wpedantic)

    add_compile_options(
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Debug>:-O2>
        $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-DNDEBUG>
    )

    if(APPLE)
        add_compile_definitions(__MACOSX_CORE__)
    endif()
endif()

# Qt6
set(ALCEDO_QT_COMPONENTS
    Core
    LinguistTools
    Svg
    Widgets
    Quick
    Qml
    QuickControls2
    QuickDialogs2
    QuickEffects
    Network
)
if(ALCEDO_BUILD_TESTS)
    list(APPEND ALCEDO_QT_COMPONENTS Test)
endif()
find_package(Qt6 REQUIRED COMPONENTS ${ALCEDO_QT_COMPONENTS})

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# CUDA
set(ALCEDO_CUDA_ENABLED OFF)
if(ALCEDO_ENABLE_CUDA)
    find_package(CUDAToolkit QUIET)
endif()
if (ALCEDO_ENABLE_CUDA AND CUDAToolkit_FOUND)
    enable_language(CUDA)
    add_definitions(-DHAVE_CUDA)
    set(CMAKE_CUDA_RUNTIME_LIBRARY Shared)
    set(CMAKE_CUDA_ARCHITECTURES 60-real 61-real 75-real 86-real 86-virtual 89-real 90-real)
    include_directories(SYSTEM ${CUDAToolkit_INCLUDE_DIRS})
    add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:-O3>)
    add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:--use_fast_math>)
    add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:--extra-device-vectorization>)
    add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Debug>>:-G>)
    if(MSVC)
        list(APPEND CMAKE_CUDA_HOST_COMPILER_OPTIONS /W4 /external:anglebrackets /external:W0 /D_DEBUG /EHsc /arch:AVX2 /openmp)
    endif()
    set(ALCEDO_CUDA_ENABLED ON)
elseif(ALCEDO_ENABLE_CUDA)
    message(STATUS "CUDA toolkit not found; building without CUDA backends.")
endif()

# Metal
set(ALCEDO_METAL_ENABLED OFF)
if(ALCEDO_ENABLE_METAL AND APPLE)
    add_compile_definitions(HAVE_METAL)
    set(ALCEDO_METAL_ENABLED ON)
elseif(ALCEDO_ENABLE_METAL)
    message(STATUS "Metal support is only available on Apple platforms; building without Metal backends.")
endif()

# OpenCL
set(ALCEDO_OPENCL_ENABLED OFF)
if(ALCEDO_ENABLE_OPENCL AND WIN32)
    find_package(OpenCL QUIET)
endif()
if(ALCEDO_ENABLE_OPENCL AND WIN32 AND OpenCL_FOUND)
    add_compile_definitions(HAVE_OPENCL)
    set(ALCEDO_OPENCL_ENABLED ON)
elseif(ALCEDO_ENABLE_OPENCL AND NOT WIN32)
    message(STATUS "OpenCL support is only available on Windows in Alcedo; building without OpenCL support.")
elseif(ALCEDO_ENABLE_OPENCL)
    message(STATUS "OpenCL SDK/ICD loader not found; building without OpenCL support.")
endif()

# Dependencies
# GTest
if(ALCEDO_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# OpenMP
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    find_program(ALCEDO_BREW_EXECUTABLE brew)
    if(ALCEDO_BREW_EXECUTABLE)
        execute_process(
            COMMAND "${ALCEDO_BREW_EXECUTABLE}" --prefix libomp
            OUTPUT_VARIABLE ALCEDO_LIBOMP_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(ALCEDO_LIBOMP_PREFIX AND EXISTS "${ALCEDO_LIBOMP_PREFIX}")
            list(PREPEND CMAKE_PREFIX_PATH "${ALCEDO_LIBOMP_PREFIX}")
            # Keep stale CMake caches from linking a second libomp from /usr/local.
            set(OpenMP_libomp_LIBRARY "${ALCEDO_LIBOMP_PREFIX}/lib/libomp.dylib"
                CACHE FILEPATH "Homebrew libomp runtime library" FORCE)
            set(OpenMP_CXX_INCLUDE_DIR "${ALCEDO_LIBOMP_PREFIX}/include"
                CACHE PATH "Homebrew libomp include directory" FORCE)
        endif()
    endif()
endif()
find_package(OpenMP COMPONENTS CXX REQUIRED)

# Headers-only libraries
# UUID
add_library(stduuid INTERFACE)
target_include_directories(stduuid INTERFACE "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/stduuid")

# UUID
add_library(uuid_v4 INTERFACE)
target_include_directories(uuid_v4 INTERFACE "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/uuid_v4")

# utfcpp 
add_library(utfcpp INTERFACE)
target_include_directories(utfcpp INTERFACE "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/utfcpp")

# MurmurHash3
add_library(MurmurHash3 STATIC "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/murmurhash3/MurmurHash3.cpp")
set_target_properties(MurmurHash3 PROPERTIES 
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/murmurhash3"
)

# xxHash
find_package(xxHash CONFIG REQUIRED)
# Create a unified alias target 'xxHash' so the rest of the project can link to it.
if (NOT TARGET xxHash)
    add_library(xxHash INTERFACE)
    if (TARGET xxHash::xxhash)
        target_link_libraries(xxHash INTERFACE xxHash::xxhash)
    elseif (TARGET xxhash)
        target_link_libraries(xxHash INTERFACE xxhash)
    else()
        message(FATAL_ERROR "xxHash package found but no known CMake target (xxHash::xxhash or xxhash) was defined.")
    endif()
endif()

# JSON  
add_library(JSON INTERFACE)
target_include_directories(JSON INTERFACE "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/nlohmann_json")


# Other libraries
# OpenCV
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs calib3d highgui)

# Eigen3
find_package(Eigen3 CONFIG REQUIRED)

set(ALCEDO_REAL_WIDGET_EDITOR OFF)
set(ALCEDO_REAL_OPENGL_EDITOR OFF)
set(ALCEDO_RHI_WIDGET_EDITOR OFF)
if(ALCEDO_ENABLE_OPENGL_EDITOR AND ALCEDO_CUDA_ENABLED)
    find_package(Qt6 REQUIRED COMPONENTS OpenGL OpenGLWidgets)
    find_package(OpenGL REQUIRED)
    set(ALCEDO_REAL_OPENGL_EDITOR ON)
    set(ALCEDO_REAL_WIDGET_EDITOR ON)
    add_compile_definitions(ALCEDO_HAS_LEGACY_GL_VIEWER)
elseif(ALCEDO_ENABLE_OPENGL_EDITOR)
    message(STATUS "OpenGL editor requested, but no accelerated backend is available. Building editor stub only.")
endif()

if(WIN32 AND ALCEDO_OPENCL_ENABLED)
    find_package(OpenGL REQUIRED)
endif()

if((WIN32 AND (ALCEDO_CUDA_ENABLED OR ALCEDO_OPENCL_ENABLED)) OR (APPLE AND ALCEDO_METAL_ENABLED))
    find_package(Qt6 REQUIRED COMPONENTS ShaderTools GuiPrivate)
    set(ALCEDO_RHI_WIDGET_EDITOR ON)
    set(ALCEDO_REAL_WIDGET_EDITOR ON)
    add_compile_definitions(ALCEDO_HAS_RHI_VIEWER)
endif()

# Profiler/hwy/lcms2
# if (WIN32)
# find_package(easy_profiler REQUIRED)
# add_compile_definitions(HAS_EASY_PROFILER)
# endif()
find_package(hwy CONFIG REQUIRED)

if (WIN32)
find_package(lcms2 CONFIG REQUIRED)
elseif(APPLE)
    find_library(LCMS2_LIB NAMES lcms2 PATHS ${ALCEDO_LIBRARY_HINTS})
    find_path(LCMS2_INC NAMES lcms2.h PATHS ${ALCEDO_INCLUDE_HINTS})
    if(LCMS2_LIB AND LCMS2_INC)
        add_library(lcms2::lcms2 SHARED IMPORTED)
        set_target_properties(lcms2::lcms2 PROPERTIES IMPORTED_LOCATION "${LCMS2_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${LCMS2_INC}")
    else()
        message(FATAL_ERROR "lcms2 not found. Install via Homebrew (little-cms2) or set CMAKE_PREFIX_PATH to its prefix.")
    endif()
endif()
if(UNIX AND NOT APPLE)
    find_package(lcms2 REQUIRED)
endif()

# OpenColorIO
find_package(OpenColorIO CONFIG REQUIRED)
if (TARGET OpenColorIO::OpenColorIO)
    add_library(OpenColorIO ALIAS OpenColorIO::OpenColorIO)
else()
    find_library(OCIO_LIB NAMES OpenColorIO PATHS ${ALCEDO_LIBRARY_HINTS})
    find_path(OCIO_INC NAMES OpenColorIO/OpenColorIO.h PATHS ${ALCEDO_INCLUDE_HINTS})
    if(OCIO_LIB AND OCIO_INC)
        add_library(OpenColorIO SHARED IMPORTED)
        set_target_properties(OpenColorIO PROPERTIES IMPORTED_LOCATION "${OCIO_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${OCIO_INC}")
    else()
        message(FATAL_ERROR "OpenColorIO not found. Install via Homebrew (opencolorio) or set CMAKE_PREFIX_PATH to its prefix.")
    endif()
endif()
   
# OpenImageIO
find_package(OpenImageIO CONFIG QUIET)
if (NOT TARGET OpenImageIO::OpenImageIO)
    if (TARGET OpenImageIO)
        add_library(OpenImageIO::OpenImageIO ALIAS OpenImageIO)
    else()
        find_library(OPENIMAGEIO_LIB NAMES OpenImageIO PATHS ${ALCEDO_LIBRARY_HINTS})
        find_path(OPENIMAGEIO_INC NAMES OpenImageIO/imageio.h PATHS ${ALCEDO_INCLUDE_HINTS})
        if(OPENIMAGEIO_LIB AND OPENIMAGEIO_INC)
            add_library(OpenImageIO::OpenImageIO SHARED IMPORTED)
            set_target_properties(OpenImageIO::OpenImageIO PROPERTIES
                IMPORTED_LOCATION "${OPENIMAGEIO_LIB}"
                INTERFACE_INCLUDE_DIRECTORIES "${OPENIMAGEIO_INC}"
            )
        else()
            message(FATAL_ERROR "OpenImageIO not found. Install via Homebrew (openimageio) or set CMAKE_PREFIX_PATH to its prefix.")
        endif()
    endif()
endif()

# Exiv2
find_package(Exiv2 QUIET CONFIG)
if (TARGET Exiv2::Exiv2 AND NOT TARGET Exiv2)
    add_library(Exiv2 ALIAS Exiv2::Exiv2)
elseif(WIN32)
    add_library(Exiv2 SHARED IMPORTED)
    set_target_properties(Exiv2 PROPERTIES
        IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/exiv2_x64-windows/bin/exiv2.dll"
        IMPORTED_IMPLIB "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/exiv2_x64-windows/lib/exiv2.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/exiv2_x64-windows/include"
    )
else()
  find_library(EXIV2_LIB NAMES exiv2 PATHS ${ALCEDO_LIBRARY_HINTS})
  find_path(EXIV2_INC NAMES exiv2/exiv2.hpp PATHS ${ALCEDO_INCLUDE_HINTS})
  if(EXIV2_LIB AND EXIV2_INC)
    add_library(Exiv2 SHARED IMPORTED)
    set_target_properties(Exiv2 PROPERTIES IMPORTED_LOCATION "${EXIV2_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${EXIV2_INC}")
  else()
    message(FATAL_ERROR "Exiv2 not found. Install via Homebrew (exiv2) or set CMAKE_PREFIX_PATH to its prefix.")
  endif()
endif()

# DuckDB API
if (WIN32)
    find_path(DUCKDB_INC NAMES duckdb.h PATHS ${ALCEDO_INCLUDE_HINTS})
    find_library(DUCKDB_LIB NAMES duckdb PATHS ${ALCEDO_LIBRARY_HINTS})
    if(DUCKDB_INC AND DUCKDB_LIB)
        add_library(DuckDB UNKNOWN IMPORTED)
        set_target_properties(DuckDB PROPERTIES
            IMPORTED_LOCATION "${DUCKDB_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${DUCKDB_INC}")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.lib")
        add_library(DuckDB SHARED IMPORTED)
        set_target_properties(DuckDB PROPERTIES
            IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll"
            IMPORTED_IMPLIB "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows")
    else()
        message(FATAL_ERROR "DuckDB not found. Install vcpkg package `duckdb` or provide alcedo_studio/third_party/libduckdb-windows.")
    endif()
else()        
  find_path(DUCKDB_INC NAMES duckdb.h PATHS ${ALCEDO_INCLUDE_HINTS})
  find_library(DUCKDB_LIB NAMES duckdb PATHS ${ALCEDO_LIBRARY_HINTS})
  if(DUCKDB_INC AND DUCKDB_LIB)
    add_library(DuckDB SHARED IMPORTED)
    set_target_properties(DuckDB PROPERTIES IMPORTED_LOCATION "${DUCKDB_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${DUCKDB_INC}")
  else()
    message(FATAL_ERROR "DuckDB not found. Install via Homebrew (duckdb) or set CMAKE_PREFIX_PATH to its prefix.")
  endif()
endif()

if(WIN32)
    set(ALCEDO_DUCKDB_VSS_EXTENSION
        "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/extensions/vss.duckdb_extension"
        CACHE FILEPATH "DuckDB VSS extension matching the bundled Windows DuckDB runtime.")
    if(NOT EXISTS "${ALCEDO_DUCKDB_VSS_EXTENSION}")
        message(FATAL_ERROR
            "DuckDB VSS extension is required for semantic search. "
            "Expected: ${ALCEDO_DUCKDB_VSS_EXTENSION}")
    endif()
endif()

# Modules
set(ALCEDO_DEFAULT_TEST_RES_DIR "${CMAKE_SOURCE_DIR}/alcedo_studio/tests/resources")
set(TEST_RES_DIR "${ALCEDO_DEFAULT_TEST_RES_DIR}" CACHE PATH "Path to test resources")
if(NOT EXISTS "${TEST_RES_DIR}")
    message(WARNING
        "Configured TEST_RES_DIR does not exist: ${TEST_RES_DIR}. "
        "Falling back to ${ALCEDO_DEFAULT_TEST_RES_DIR}.")
    set(TEST_RES_DIR "${ALCEDO_DEFAULT_TEST_RES_DIR}" CACHE PATH "Path to test resources" FORCE)
endif()
file(TO_CMAKE_PATH "${TEST_RES_DIR}" TEST_RES_DIR)
add_compile_definitions(
    TEST_DB_PATH=\"${TEST_RES_DIR}/temp_folder/test.db\"
    TEST_META_PATH=\"${TEST_RES_DIR}/temp_folder/meta.json\"
    TEST_IMG_PATH=\"${TEST_RES_DIR}/sample_images\"
    TEST_PROFILER_OUTPUT_PATH=\"${TEST_RES_DIR}/temp_folder/test_profile.prof\"
    CONFIG_PATH=\"${CMAKE_SOURCE_DIR}/alcedo_studio/src/config/\"
)

include(GNUInstallDirs)

function(alcedo_collect_packaged_luts out_var config_dir)
    set(_lut_dir "${config_dir}/LUTs")
    set(_packaged_luts "")
    if(EXISTS "${_lut_dir}")
        # Ship the curated film-stock LUT set only. This intentionally skips
        # spektrafilm and the older generic LUT samples in the source tree.
        file(GLOB _packaged_luts CONFIGURE_DEPENDS
            "${_lut_dir}/Agfa_*.cube"
            "${_lut_dir}/Fuji_*.cube"
            "${_lut_dir}/Kodak_*.cube"
        )
        list(SORT _packaged_luts)
    endif()
    set(${out_var} ${_packaged_luts} PARENT_SCOPE)
endfunction()

function(alcedo_install_packaged_luts config_dir destination)
    alcedo_collect_packaged_luts(_alcedo_packaged_luts "${config_dir}")
    install(CODE
        "file(REMOVE_RECURSE \"\${CMAKE_INSTALL_PREFIX}/${destination}\")"
    )
    if(_alcedo_packaged_luts)
        install(FILES ${_alcedo_packaged_luts} DESTINATION "${destination}")
    endif()
endfunction()

add_subdirectory(${CMAKE_SOURCE_DIR}/alcedo_studio/src/third_party)
if(DEFINED ALCEDO_LENSFUN_LIBRARY_DIR AND NOT "${ALCEDO_LENSFUN_LIBRARY_DIR}" STREQUAL "")
    list(APPEND CMAKE_BUILD_RPATH "${ALCEDO_LENSFUN_LIBRARY_DIR}")
endif()
add_subdirectory(${CMAKE_SOURCE_DIR}/alcedo_studio/src)
if(ALCEDO_BUILD_TESTS)
    add_subdirectory(${CMAKE_SOURCE_DIR}/alcedo_studio/tests)
endif()

set(ALCEDO_MAIN_BIN alcedo_main)
if(TARGET ${ALCEDO_MAIN_BIN})
    if(APPLE AND ALCEDO_MACOS_BUNDLE)
        # Match the app bundle name to the QML module URI (Alcedo.Main)
        # so that Qt's QML deploy puts files in the correct bundle.
        set_target_properties(${ALCEDO_MAIN_BIN} PROPERTIES OUTPUT_NAME "${ALCEDO_MACOS_BUNDLE_NAME}")
        set_property(TARGET ${ALCEDO_MAIN_BIN} APPEND PROPERTY INSTALL_RPATH "@executable_path/../Frameworks")

        # macOS: install the .app bundle into the prefix root so that
        # CPack picks up the app bundle directly.
        install(TARGETS ${ALCEDO_MAIN_BIN}
            BUNDLE  DESTINATION .
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )

        # Copy asset folders into Contents/MacOS/ (next to the executable)
        set(_config_dir "${CMAKE_SOURCE_DIR}/alcedo_studio/src/config")
        set(_alcedo_macos_app_bundle "${ALCEDO_MACOS_BUNDLE_NAME}.app")
        set(_bundle_macos_dest "${_alcedo_macos_app_bundle}/Contents/MacOS")
        set(_bundle_resources_dest "${_alcedo_macos_app_bundle}/Contents/Resources")
        set(_bundle_frameworks_dest "${_alcedo_macos_app_bundle}/Contents/Frameworks")
        install(DIRECTORY "${_config_dir}/fonts/" DESTINATION "${_bundle_macos_dest}/fonts")
        install(DIRECTORY "${_config_dir}/icc/" DESTINATION "${_bundle_macos_dest}/config/icc")
        install(DIRECTORY "${_config_dir}/lens_calib/" DESTINATION "${_bundle_macos_dest}/config/lens_calib")
        install(DIRECTORY "${_config_dir}/nikon_lens/" DESTINATION "${_bundle_macos_dest}/config/nikon_lens")
        alcedo_install_packaged_luts("${_config_dir}" "${_bundle_macos_dest}/LUTs")
        if(ALCEDO_OPENCL_ENABLED)
            install(DIRECTORY "${CMAKE_SOURCE_DIR}/alcedo_studio/src/decoders/processor/operators/gpu/opencl_shader/"
                DESTINATION "${_bundle_macos_dest}/opencl/decoders/processor/operators/gpu/opencl_shader")
            install(DIRECTORY "${CMAKE_SOURCE_DIR}/alcedo_studio/src/edit/pipeline/opencl_shader/"
                DESTINATION "${_bundle_macos_dest}/opencl/edit/pipeline/opencl_shader")
            install(DIRECTORY "${CMAKE_SOURCE_DIR}/alcedo_studio/src/edit/operators/geometry/opencl_shader/"
                DESTINATION "${_bundle_macos_dest}/opencl/edit/operators/geometry/opencl_shader")
            install(DIRECTORY "${CMAKE_SOURCE_DIR}/alcedo_studio/src/edit/scope/opencl_shader/"
                DESTINATION "${_bundle_macos_dest}/opencl/edit/scope/opencl_shader")
            install(DIRECTORY "${CMAKE_SOURCE_DIR}/alcedo_studio/src/opencl/opencl_shader/"
                DESTINATION "${_bundle_macos_dest}/opencl/opencl/opencl_shader")
        endif()
        if(ALCEDO_METAL_ENABLED AND ALCEDO_METAL_RUNTIME_LIBS)
            install(FILES ${ALCEDO_METAL_RUNTIME_LIBS}
                DESTINATION "${_bundle_resources_dest}/metallib"
            )
        endif()
        if(DEFINED ALCEDO_LENSFUN_LIBRARY_DIR AND NOT "${ALCEDO_LENSFUN_LIBRARY_DIR}" STREQUAL "")
            set(_lensfun_soname_path "${ALCEDO_LENSFUN_LIBRARY_DIR}/liblensfun.2.dylib")
            file(REAL_PATH "${_lensfun_soname_path}" _lensfun_real_path)
            if(EXISTS "${_lensfun_real_path}")
                install(FILES "${_lensfun_real_path}"
                    DESTINATION "${_bundle_frameworks_dest}"
                    OPTIONAL
                )
                get_filename_component(_lensfun_real_name "${_lensfun_real_path}" NAME)
                if(NOT "${_lensfun_real_name}" STREQUAL "liblensfun.2.dylib")
                    install(CODE
                        "set(_frameworks_dir \"\${CMAKE_INSTALL_PREFIX}/${_bundle_frameworks_dest}\")
                         file(MAKE_DIRECTORY \"\${_frameworks_dir}\")
                         file(REMOVE \"\${_frameworks_dir}/liblensfun.2.dylib\")
                         execute_process(
                             COMMAND \"${CMAKE_COMMAND}\" -E create_symlink \"${_lensfun_real_name}\" \"\${_frameworks_dir}/liblensfun.2.dylib\"
                             RESULT_VARIABLE _alcedo_lensfun_symlink_result
                         )
                         if(NOT _alcedo_lensfun_symlink_result EQUAL 0)
                             message(FATAL_ERROR \"Failed to create lensfun soname symlink in \${_frameworks_dir}\")
                         endif()
                        "
                    )
                endif()
            else()
                install(FILES "${_lensfun_soname_path}"
                    DESTINATION "${_bundle_frameworks_dest}"
                    OPTIONAL
                )
            endif()
        endif()

        # Ensure JPEG XL runtime deps are present for OpenImageIO/Homebrew builds.
        set(_jxl_cms_soname_path "")
        foreach(_lib_dir IN LISTS ALCEDO_LIBRARY_HINTS)
            if(EXISTS "${_lib_dir}/libjxl_cms.0.11.dylib")
                set(_jxl_cms_soname_path "${_lib_dir}/libjxl_cms.0.11.dylib")
                break()
            endif()
        endforeach()
        if(_jxl_cms_soname_path)
            file(REAL_PATH "${_jxl_cms_soname_path}" _jxl_cms_real_path)
            if(EXISTS "${_jxl_cms_real_path}")
                install(FILES "${_jxl_cms_real_path}"
                    DESTINATION "${_bundle_frameworks_dest}"
                    OPTIONAL
                )
                get_filename_component(_jxl_cms_real_name "${_jxl_cms_real_path}" NAME)
                if(NOT "${_jxl_cms_real_name}" STREQUAL "libjxl_cms.0.11.dylib")
                    install(CODE
                        "set(_frameworks_dir \"\${CMAKE_INSTALL_PREFIX}/${_bundle_frameworks_dest}\")
                         file(MAKE_DIRECTORY \"\${_frameworks_dir}\")
                         file(REMOVE \"\${_frameworks_dir}/libjxl_cms.0.11.dylib\")
                         execute_process(
                             COMMAND \"${CMAKE_COMMAND}\" -E create_symlink \"${_jxl_cms_real_name}\" \"\${_frameworks_dir}/libjxl_cms.0.11.dylib\"
                             RESULT_VARIABLE _alcedo_jxl_cms_symlink_result
                         )
                         if(NOT _alcedo_jxl_cms_symlink_result EQUAL 0)
                             message(FATAL_ERROR \"Failed to create libjxl_cms soname symlink in \${_frameworks_dir}\")
                         endif()
                        "
                    )
                endif()
            else()
                install(FILES "${_jxl_cms_soname_path}"
                    DESTINATION "${_bundle_frameworks_dest}"
                    OPTIONAL
                )
            endif()
        else()
            message(WARNING "libjxl_cms.0.11.dylib was not found in ALCEDO_LIBRARY_HINTS; app may fail to launch on systems without JPEG XL runtime installed.")
        endif()
    else()
        install(TARGETS ${ALCEDO_MAIN_BIN}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )

        set(_config_dir "${CMAKE_SOURCE_DIR}/alcedo_studio/src/config")
        install(DIRECTORY "${_config_dir}/fonts/" DESTINATION "${CMAKE_INSTALL_BINDIR}/fonts")
        install(DIRECTORY "${_config_dir}/icc/" DESTINATION "${CMAKE_INSTALL_BINDIR}/config/icc")
        install(DIRECTORY "${_config_dir}/lens_calib/" DESTINATION "${CMAKE_INSTALL_BINDIR}/config/lens_calib")
        install(DIRECTORY "${_config_dir}/nikon_lens/" DESTINATION "${CMAKE_INSTALL_BINDIR}/config/nikon_lens")

        alcedo_install_packaged_luts("${_config_dir}" "${CMAKE_INSTALL_BINDIR}/LUTs")
        if(ALCEDO_OPENCL_ENABLED)
            install(DIRECTORY "${CMAKE_SOURCE_DIR}/alcedo_studio/src/decoders/processor/operators/gpu/opencl_shader/"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/opencl/decoders/processor/operators/gpu/opencl_shader")
            install(DIRECTORY "${CMAKE_SOURCE_DIR}/alcedo_studio/src/edit/pipeline/opencl_shader/"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/opencl/edit/pipeline/opencl_shader")
            install(DIRECTORY "${CMAKE_SOURCE_DIR}/alcedo_studio/src/edit/operators/geometry/opencl_shader/"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/opencl/edit/operators/geometry/opencl_shader")
            install(DIRECTORY "${CMAKE_SOURCE_DIR}/alcedo_studio/src/edit/scope/opencl_shader/"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/opencl/edit/scope/opencl_shader")
            install(DIRECTORY "${CMAKE_SOURCE_DIR}/alcedo_studio/src/opencl/opencl_shader/"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/opencl/opencl/opencl_shader")
        endif()
        if(WIN32 AND ALCEDO_DUCKDB_VSS_EXTENSION)
            install(FILES "${ALCEDO_DUCKDB_VSS_EXTENSION}"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/duckdb_extensions"
            )
        endif()

        if(ALCEDO_BUILD_SEMANTIC_SIDECAR AND ALCEDO_MIND_BINARY)
            install(CODE
                "set(ALCEDO_MIND_BINARY \"${ALCEDO_MIND_BINARY}\")
                 set(ALCEDO_MIND_TARGET_DIR \"${ALCEDO_MIND_TARGET_DIR}\")
                 set(ALCEDO_MIND_DEST_DIR \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
                 include(\"${CMAKE_SOURCE_DIR}/scripts/cmake/copy_semantic_sidecar.cmake\")
                "
            )
        endif()

        if(ALCEDO_ARIA2C_BINARY)
            install(FILES "${ALCEDO_ARIA2C_BINARY}"
                DESTINATION "${CMAKE_INSTALL_BINDIR}"
                PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                            GROUP_READ GROUP_EXECUTE
                            WORLD_READ WORLD_EXECUTE
            )
        endif()

        if(APPLE AND ALCEDO_METAL_ENABLED AND ALCEDO_METAL_RUNTIME_LIBS)
            install(FILES ${ALCEDO_METAL_RUNTIME_LIBS}
                DESTINATION "${CMAKE_INSTALL_BINDIR}/metallib"
            )
        endif()
    endif()
endif()

if(WIN32 AND TARGET ${ALCEDO_MAIN_BIN})
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    include(InstallRequiredSystemLibraries)

    set(ALCEDO_RUNTIME_DEP_DIRS "" CACHE STRING "Extra directories to search for runtime DLL dependencies during install.")
    set(ALCEDO_RUNTIME_EXES "$<TARGET_FILE:${ALCEDO_MAIN_BIN}>")
    set(ALCEDO_RUNTIME_SEARCH_DIRS "$<TARGET_FILE_DIR:${ALCEDO_MAIN_BIN}>")
    if(ALCEDO_MIND_BINARY)
        list(APPEND ALCEDO_RUNTIME_EXES "${ALCEDO_MIND_BINARY}")
    endif()
    if(ALCEDO_MIND_TARGET_DIR)
        list(APPEND ALCEDO_RUNTIME_SEARCH_DIRS "${ALCEDO_MIND_TARGET_DIR}")
    endif()

    set(_third_party_exiv2_bin "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/exiv2_x64-windows/bin")
    set(_third_party_duckdb_bin "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows")

    set(_vcpkg_bin_dir "")
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(_vcpkg_bin_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()

    set(_qt6_bin_dir "")
    get_target_property(_qt6_core_loc Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _qt6_core_loc)
        get_target_property(_qt6_core_loc Qt6::Core IMPORTED_LOCATION)
    endif()
    if(_qt6_core_loc)
        get_filename_component(_qt6_bin_dir "${_qt6_core_loc}" DIRECTORY)
    endif()

    set(_cuda_bin_dir "")
    if(ALCEDO_CUDA_ENABLED AND CUDAToolkit_BIN_DIR)
        set(_cuda_bin_dir "${CUDAToolkit_BIN_DIR}")
    endif()

    install(CODE
        "message(STATUS \"[alcedo] Installing runtime DLL dependencies...\")
         set(_bindir \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
         file(MAKE_DIRECTORY \"\${_bindir}\")
         set(_search_dirs
             \"\${_bindir}\"
             ${ALCEDO_RUNTIME_SEARCH_DIRS}
             \"${_third_party_exiv2_bin}\"
             \"${_third_party_duckdb_bin}\"
             \"${_vcpkg_bin_dir}\"
             \"${_qt6_bin_dir}\"
             \"${_cuda_bin_dir}\"
             \${ALCEDO_RUNTIME_DEP_DIRS}
         )
         if(NOT \"${_third_party_dawn_bin}\" STREQUAL \"\")
             list(APPEND _search_dirs \"${_third_party_dawn_bin}\")
         endif()
         file(GET_RUNTIME_DEPENDENCIES
             EXECUTABLES ${ALCEDO_RUNTIME_EXES}
             DIRECTORIES \${_search_dirs}
             RESOLVED_DEPENDENCIES_VAR _resolved
             UNRESOLVED_DEPENDENCIES_VAR _unresolved
             PRE_EXCLUDE_REGEXES
               \"api-ms-win-.*\"
               \"ext-ms-.*\"
             POST_EXCLUDE_REGEXES
               \".*[\\\\\\\\/][Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\\\\\\\/].*\"
               \".*[\\\\\\\\/][Ss][Yy][Ss][Tt][Ee][Mm]32[\\\\\\\\/].*\"
               \".*[\\\\\\\\/][Ss][Yy][Ss][Ww][Oo][Ww]64[\\\\\\\\/].*\"
               \"azureattest.*\\\\.dll\"
               \"hvsifiletrust\\\\.dll\"
               \"pdmutilities\\\\.dll\"
               \"wpaxholder\\\\.dll\"
               \"opencv_cudaarithm.*\\\\.dll\"
               \"opencv_cudafilters.*\\\\.dll\"
               \"opencv_cudaimgproc.*\\\\.dll\"
               \"opencv_cudawarping.*\\\\.dll\"
               \"cublasLt64_.*\\\\.dll\"
         )
         list(REMOVE_DUPLICATES _resolved)
         foreach(_dep IN LISTS _resolved)
             file(INSTALL DESTINATION \"\${_bindir}\" TYPE SHARED_LIBRARY FILES \"\${_dep}\")
         endforeach()
         if(NOT \"${_cuda_bin_dir}\" STREQUAL \"\")
             file(GLOB _cuda_runtime_dlls \"${_cuda_bin_dir}/cudart64_*.dll\")
             foreach(_cuda_runtime_dll IN LISTS _cuda_runtime_dlls)
                 file(INSTALL DESTINATION \"\${_bindir}\" TYPE SHARED_LIBRARY FILES \"\${_cuda_runtime_dll}\")
             endforeach()
             if(NOT _cuda_runtime_dlls)
                 message(WARNING \"[alcedo] cudart64_*.dll not found in: ${_cuda_bin_dir}\")
             endif()
         endif()
         if(_unresolved)
             message(WARNING \"[alcedo] Unresolved runtime deps: \${_unresolved}\")
         endif()
         if(NOT \"${_third_party_dawn_bin}\" STREQUAL \"\")
             # Explicitly copy bundled Dawn DLLs even if they are not direct linker dependencies
             # (Dawn may load them dynamically at runtime).
             file(GLOB _dawn_dlls \"${_third_party_dawn_bin}/*.dll\")
             set(_alcedo_dawn_runtime_dlls \"${ALCEDO_DAWN_RUNTIME_DLLS}\")
             if(_alcedo_dawn_runtime_dlls)
                 list(APPEND _dawn_dlls \${_alcedo_dawn_runtime_dlls})
             endif()
             foreach(_dawn_dll IN LISTS _dawn_dlls)
                 file(INSTALL DESTINATION \"\${_bindir}\" TYPE SHARED_LIBRARY FILES \"\${_dawn_dll}\")
             endforeach()
         endif()
        "
    )

    if(NOT COMMAND qt_generate_deploy_qml_app_script)
        message(FATAL_ERROR "qt_generate_deploy_qml_app_script is unavailable. Use Qt 6.3+ with deployment tooling.")
    endif()

    option(ALCEDO_DEPLOY_SOFTWARE_OPENGL "Bundle opengl32sw.dll for software OpenGL fallback." OFF)

    set(_alcedo_qt_deploy_tool_options_default
        --no-compiler-runtime
        --no-system-d3d-compiler
        --no-system-dxc-compiler
    )
    set(_alcedo_qt_deploy_tool_options_legacy_default
        --no-compiler-runtime
        --no-translations
        --no-system-d3d-compiler
        --no-system-dxc-compiler
    )

    if(NOT DEFINED ALCEDO_QT_DEPLOY_TOOL_OPTIONS OR
       "${ALCEDO_QT_DEPLOY_TOOL_OPTIONS}" STREQUAL "${_alcedo_qt_deploy_tool_options_legacy_default}")
        set(ALCEDO_QT_DEPLOY_TOOL_OPTIONS
            ${_alcedo_qt_deploy_tool_options_default}
            CACHE STRING
            "Semicolon-separated options forwarded to Qt deploy tool during install."
            FORCE
        )
    else()
        set(ALCEDO_QT_DEPLOY_TOOL_OPTIONS
            "${ALCEDO_QT_DEPLOY_TOOL_OPTIONS}"
            CACHE STRING
            "Semicolon-separated options forwarded to Qt deploy tool during install."
        )
    endif()

    set(_deploy_tool_options ${ALCEDO_QT_DEPLOY_TOOL_OPTIONS})
    if(NOT ALCEDO_DEPLOY_SOFTWARE_OPENGL)
        list(APPEND _deploy_tool_options --no-opengl-sw)
    endif()

    qt_generate_deploy_qml_app_script(
        TARGET ${ALCEDO_MAIN_BIN}
        OUTPUT_SCRIPT _alcedo_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
        DEPLOY_TOOL_OPTIONS ${_deploy_tool_options}
    )
    install(SCRIPT ${_alcedo_deploy_script})
endif()

# ── macOS deployment ────────────────────────────────────────────────────────
# Bundles external dylibs + Qt frameworks/plugins/QML modules into
# Alcedo.app so the packaged ZIP/DMG is self-contained.
#
# Build & package workflow:
#   cmake --preset macos_release
#   cmake --build --preset macos_release
#   cmake --build --preset macos_package         # install + create the DMG + ZIP
#
# End-user can run the app from the terminal after mounting/extracting:
#   ./AlcedoStudio.app/Contents/MacOS/AlcedoStudio
# ────────────────────────────────────────────────────────────────────────────
if(APPLE AND ALCEDO_MACOS_BUNDLE AND TARGET ${ALCEDO_MAIN_BIN})

    # Use Qt's QML deploy API (macdeployqt underneath) and pass Homebrew/3rd-party
    # library search roots via -libpath.
    if(NOT COMMAND qt_generate_deploy_qml_app_script)
        message(FATAL_ERROR
            "qt_generate_deploy_qml_app_script is unavailable. "
            "Use Qt 6.3+ with deployment tooling.")
    endif()

    set(_alcedo_macos_deploy_tool_options "")
    if(NOT ALCEDO_MACDEPLOYQT_STRIP)
        list(APPEND _alcedo_macos_deploy_tool_options -no-strip)
    endif()

    set(_alcedo_macos_libpaths "")
    foreach(_lib_dir IN LISTS ALCEDO_LIBRARY_HINTS)
        if(EXISTS "${_lib_dir}")
            list(APPEND _alcedo_macos_libpaths "-libpath=${_lib_dir}")
        endif()
    endforeach()

    if(DEFINED ALCEDO_LENSFUN_LIBRARY_DIR AND NOT "${ALCEDO_LENSFUN_LIBRARY_DIR}" STREQUAL "")
        list(APPEND _alcedo_macos_libpaths "-libpath=${ALCEDO_LENSFUN_LIBRARY_DIR}")
    endif()

    list(REMOVE_DUPLICATES _alcedo_macos_libpaths)
    list(APPEND _alcedo_macos_deploy_tool_options ${_alcedo_macos_libpaths})

    set(_alcedo_macos_deploy_args NO_UNSUPPORTED_PLATFORM_ERROR)
    if(_alcedo_macos_deploy_tool_options)
        list(APPEND _alcedo_macos_deploy_args
            DEPLOY_TOOL_OPTIONS
            ${_alcedo_macos_deploy_tool_options}
        )
    endif()

    qt_generate_deploy_qml_app_script(
        TARGET ${ALCEDO_MAIN_BIN}
        OUTPUT_SCRIPT _alcedo_macos_deploy_script
        ${_alcedo_macos_deploy_args}
    )
    install(SCRIPT ${_alcedo_macos_deploy_script})

    # Build a search-dir list for manual GCC/OpenBLAS runtime fixups.
    set(_alcedo_runtime_dirs ${ALCEDO_LIBRARY_HINTS})
    if(DEFINED ALCEDO_LENSFUN_LIBRARY_DIR AND NOT "${ALCEDO_LENSFUN_LIBRARY_DIR}" STREQUAL "")
        list(APPEND _alcedo_runtime_dirs "${ALCEDO_LENSFUN_LIBRARY_DIR}")
    endif()
    list(REMOVE_DUPLICATES _alcedo_runtime_dirs)
    list(JOIN _alcedo_runtime_dirs ";" _alcedo_runtime_dirs_joined)

    # Qt/macdeployqt already does the main deployment pass.
    # Here we only patch the OpenBLAS -> libgfortran -> libquadmath/libgcc_s chain.
    install(CODE
        "
        set(_alcedo_app_path \"\${CMAKE_INSTALL_PREFIX}/${ALCEDO_MACOS_BUNDLE_NAME}.app\")
        set(_alcedo_fw_dir   \"\${_alcedo_app_path}/Contents/Frameworks\")
        set(_alcedo_search_dirs \"${_alcedo_runtime_dirs_joined}\")

        if(NOT EXISTS \"\${_alcedo_app_path}\")
            message(FATAL_ERROR \"macOS bundle not found: \${_alcedo_app_path}\")
        endif()

        file(MAKE_DIRECTORY \"\${_alcedo_fw_dir}\")

        function(_alcedo_find_lib _out _name)
            set(_found \"\")
            foreach(_dir IN LISTS _alcedo_search_dirs)
                if(EXISTS \"\${_dir}/\${_name}\")
                    set(_found \"\${_dir}/\${_name}\")
                    break()
                endif()
            endforeach()
            set(\${_out} \"\${_found}\" PARENT_SCOPE)
        endfunction()

        function(_alcedo_copy_lib_if_missing _name)
            set(_dst \"\${_alcedo_fw_dir}/\${_name}\")
            if(NOT EXISTS \"\${_dst}\")
                _alcedo_find_lib(_src \"\${_name}\")
                if(\"\${_src}\" STREQUAL \"\")
                    message(FATAL_ERROR \"Required runtime library not found: \${_name}\")
                endif()

                execute_process(
                    COMMAND /bin/cp -fL \"\${_src}\" \"\${_dst}\"
                    RESULT_VARIABLE _copy_result
                )
                if(NOT _copy_result EQUAL 0)
                    message(FATAL_ERROR \"Failed to copy \${_src} -> \${_dst}\")
                endif()
            endif()

            execute_process(COMMAND /bin/chmod u+w \"\${_dst}\" ERROR_QUIET)
        endfunction()

        # Ensure the GCC runtime chain exists in the bundle.
        _alcedo_copy_lib_if_missing(\"libgfortran.5.dylib\")
        _alcedo_copy_lib_if_missing(\"libquadmath.0.dylib\")
        _alcedo_copy_lib_if_missing(\"libgcc_s.1.1.dylib\")

        # Patch libgfortran to use sibling dylibs in Frameworks.
        execute_process(
            COMMAND /usr/bin/install_name_tool
                    -change @rpath/libquadmath.0.dylib @loader_path/libquadmath.0.dylib
                    \"\${_alcedo_fw_dir}/libgfortran.5.dylib\"
            RESULT_VARIABLE _ignore_quadmath
            ERROR_QUIET
        )
        execute_process(
            COMMAND /usr/bin/install_name_tool
                    -change @rpath/libgcc_s.1.1.dylib @loader_path/libgcc_s.1.1.dylib
                    \"\${_alcedo_fw_dir}/libgfortran.5.dylib\"
            RESULT_VARIABLE _ignore_gcc
            ERROR_QUIET
        )

        # Make libopenblas prefer the bundled libgfortran when applicable.
        if(EXISTS \"\${_alcedo_fw_dir}/libopenblas.0.dylib\")
            execute_process(COMMAND /bin/chmod u+w \"\${_alcedo_fw_dir}/libopenblas.0.dylib\" ERROR_QUIET)

            execute_process(
                COMMAND /usr/bin/install_name_tool
                        -change @rpath/libgfortran.5.dylib @loader_path/libgfortran.5.dylib
                        \"\${_alcedo_fw_dir}/libopenblas.0.dylib\"
                RESULT_VARIABLE _ignore_openblas_rpath
                ERROR_QUIET
            )
            execute_process(
                COMMAND /usr/bin/install_name_tool
                        -change @executable_path/../Frameworks/libgfortran.5.dylib @loader_path/libgfortran.5.dylib
                        \"\${_alcedo_fw_dir}/libopenblas.0.dylib\"
                RESULT_VARIABLE _ignore_openblas_exec
                ERROR_QUIET
            )
        endif()

        set(_alcedo_dylib_ids
            \"libopencv_core.412.dylib=@rpath/libopencv_core.412.dylib\"
            \"libopencv_imgproc.412.dylib=@rpath/libopencv_imgproc.412.dylib\"
            \"libopencv_imgcodecs.412.dylib=@rpath/libopencv_imgcodecs.412.dylib\"
            \"libopencv_videoio.412.dylib=@rpath/libopencv_videoio.412.dylib\"
            \"libopencv_features2d.412.dylib=@rpath/libopencv_features2d.412.dylib\"
            \"libopencv_flann.412.dylib=@rpath/libopencv_flann.412.dylib\"
            \"libgcc_s.1.1.dylib=@rpath/libgcc_s.1.1.dylib\"
            \"libjxl_cms.0.11.2.dylib=@rpath/libjxl_cms.0.11.dylib\"
        )
        foreach(_alcedo_dylib_id IN LISTS _alcedo_dylib_ids)
            string(REPLACE \"=\" \";\" _alcedo_dylib_id_parts \"\${_alcedo_dylib_id}\")
            list(GET _alcedo_dylib_id_parts 0 _alcedo_dylib_name)
            list(GET _alcedo_dylib_id_parts 1 _alcedo_dylib_install_name)
            set(_alcedo_dylib_path \"\${_alcedo_fw_dir}/\${_alcedo_dylib_name}\")
            if(EXISTS \"\${_alcedo_dylib_path}\")
                execute_process(COMMAND /bin/chmod u+w \"\${_alcedo_dylib_path}\" ERROR_QUIET)
                execute_process(
                    COMMAND /usr/bin/install_name_tool -id \"\${_alcedo_dylib_install_name}\" \"\${_alcedo_dylib_path}\"
                    RESULT_VARIABLE _alcedo_install_id_result
                )
                if(NOT _alcedo_install_id_result EQUAL 0)
                    message(FATAL_ERROR \"Failed to set install name for \${_alcedo_dylib_path}\")
                endif()
            endif()
        endforeach()

        # Optional sanity check for the patched libgfortran.
        execute_process(
            COMMAND /usr/bin/otool -L \"\${_alcedo_fw_dir}/libgfortran.5.dylib\"
            RESULT_VARIABLE _gfortran_otool_result
            OUTPUT_VARIABLE _gfortran_otool_output
            ERROR_VARIABLE  _gfortran_otool_error
        )
        if(NOT _gfortran_otool_result EQUAL 0)
            message(FATAL_ERROR
                \"otool failed for libgfortran.5.dylib: \${_gfortran_otool_error}\")
        endif()

        message(STATUS \"Patched GCC runtime chain for macOS bundle\")
        "
    )

    if(ALCEDO_MACOS_VERIFY_BUNDLE_DEPS)
        install(CODE
            "set(_alcedo_app_path \"\${CMAKE_INSTALL_PREFIX}/${ALCEDO_MACOS_BUNDLE_NAME}.app\")
             set(_alcedo_fw_dir \"\${_alcedo_app_path}/Contents/Frameworks\")
             set(_alcedo_main_bin \"\${_alcedo_app_path}/Contents/MacOS/${ALCEDO_MACOS_BUNDLE_NAME}\")

             if(NOT EXISTS \"\${_alcedo_main_bin}\")
                 message(FATAL_ERROR \"macOS bundle executable not found: \${_alcedo_main_bin}\")
             endif()

             set(_alcedo_macho_files \"\${_alcedo_main_bin}\")
             file(GLOB_RECURSE _alcedo_bundle_dylibs
                 \"\${_alcedo_app_path}/Contents/Frameworks/*.dylib\"
                 \"\${_alcedo_app_path}/Contents/PlugIns/*.dylib\"
                 \"\${_alcedo_app_path}/Contents/Resources/qml/*.dylib\")
             file(GLOB _alcedo_bundle_framework_dirs
                 LIST_DIRECTORIES true
                 \"\${_alcedo_app_path}/Contents/Frameworks/*.framework\")
             set(_alcedo_bundle_framework_bins \"\")
             foreach(_alcedo_framework_dir IN LISTS _alcedo_bundle_framework_dirs)
                 get_filename_component(_alcedo_framework_name \"\${_alcedo_framework_dir}\" NAME_WE)
                 set(_alcedo_framework_candidates
                     \"\${_alcedo_framework_dir}/\${_alcedo_framework_name}\"
                     \"\${_alcedo_framework_dir}/Versions/Current/\${_alcedo_framework_name}\")
                 file(GLOB _alcedo_versioned_framework_bins
                     \"\${_alcedo_framework_dir}/Versions/*/\${_alcedo_framework_name}\")
                 list(APPEND _alcedo_framework_candidates
                     \${_alcedo_versioned_framework_bins})
                 foreach(_alcedo_framework_candidate IN LISTS _alcedo_framework_candidates)
                     if(EXISTS \"\${_alcedo_framework_candidate}\" AND
                        NOT IS_DIRECTORY \"\${_alcedo_framework_candidate}\")
                         list(APPEND _alcedo_bundle_framework_bins
                             \"\${_alcedo_framework_candidate}\")
                     endif()
                 endforeach()
             endforeach()
             list(APPEND _alcedo_macho_files
                 \${_alcedo_bundle_dylibs}
                 \${_alcedo_bundle_framework_bins})
             set(_alcedo_resolved_macho_files \"\")
             foreach(_alcedo_macho_file IN LISTS _alcedo_macho_files)
                 if(EXISTS \"\${_alcedo_macho_file}\")
                     get_filename_component(_alcedo_resolved_macho_file
                         \"\${_alcedo_macho_file}\" REALPATH)
                     list(APPEND _alcedo_resolved_macho_files
                         \"\${_alcedo_resolved_macho_file}\")
                 endif()
             endforeach()
             set(_alcedo_macho_files \${_alcedo_resolved_macho_files})
             list(REMOVE_DUPLICATES _alcedo_macho_files)

             set(_alcedo_bad_deps \"\")
             set(_alcedo_missing_rpath_deps \"\")
             set(_alcedo_missing_relative_deps \"\")

             function(_alcedo_verify_dependency _owner _dep)
                 if(\"\${_dep}\" MATCHES \"^/System/Library/\" OR
                    \"\${_dep}\" MATCHES \"^/usr/lib/\")
                     return()
                 endif()

                 if(\"\${_dep}\" MATCHES \"^@rpath/(.+)$\")
                     set(_rel \"\${CMAKE_MATCH_1}\")
                     if(EXISTS \"\${_alcedo_fw_dir}/\${_rel}\")
                         return()
                     endif()
                     list(APPEND _alcedo_missing_rpath_deps
                         \"\${_owner} -> \${_dep}\")
                     set(_alcedo_missing_rpath_deps \"\${_alcedo_missing_rpath_deps}\" PARENT_SCOPE)
                     return()
                 endif()

                 if(\"\${_dep}\" MATCHES \"^@executable_path/(.+)$\")
                     set(_rel \"\${CMAKE_MATCH_1}\")
                     if(EXISTS \"\${_alcedo_app_path}/Contents/MacOS/\${_rel}\")
                         return()
                     endif()
                     list(APPEND _alcedo_missing_relative_deps
                         \"\${_owner} -> \${_dep}\")
                     set(_alcedo_missing_relative_deps \"\${_alcedo_missing_relative_deps}\" PARENT_SCOPE)
                     return()
                 endif()

                 if(\"\${_dep}\" MATCHES \"^@loader_path/(.+)$\")
                     set(_rel \"\${CMAKE_MATCH_1}\")
                     get_filename_component(_owner_dir \"\${_owner}\" DIRECTORY)
                     if(EXISTS \"\${_owner_dir}/\${_rel}\")
                         return()
                     endif()
                     list(APPEND _alcedo_missing_relative_deps
                         \"\${_owner} -> \${_dep}\")
                     set(_alcedo_missing_relative_deps \"\${_alcedo_missing_relative_deps}\" PARENT_SCOPE)
                     return()
                 endif()

                 if(\"\${_dep}\" MATCHES \"^/\")
                     list(APPEND _alcedo_bad_deps \"\${_owner} -> \${_dep}\")
                     set(_alcedo_bad_deps \"\${_alcedo_bad_deps}\" PARENT_SCOPE)
                 endif()
             endfunction()

             foreach(_alcedo_macho IN LISTS _alcedo_macho_files)
                 if(IS_DIRECTORY \"\${_alcedo_macho}\")
                     continue()
                 endif()
                 execute_process(
                     COMMAND /usr/bin/otool -L \"\${_alcedo_macho}\"
                     RESULT_VARIABLE _alcedo_otool_result
                     OUTPUT_VARIABLE _alcedo_otool_output
                     ERROR_QUIET)
                 if(NOT _alcedo_otool_result EQUAL 0)
                     continue()
                 endif()

                 string(REPLACE \"\\n\" \";\" _alcedo_otool_lines \"\${_alcedo_otool_output}\")
                 foreach(_alcedo_otool_line IN LISTS _alcedo_otool_lines)
                     string(STRIP \"\${_alcedo_otool_line}\" _alcedo_otool_line)
                     if(\"\${_alcedo_otool_line}\" STREQUAL \"\" OR
                        \"\${_alcedo_otool_line}\" MATCHES \":$\")
                         continue()
                     endif()
                     string(REGEX MATCH \"^[^ ]+\" _alcedo_dep \"\${_alcedo_otool_line}\")
                     _alcedo_verify_dependency(\"\${_alcedo_macho}\" \"\${_alcedo_dep}\")
                 endforeach()
             endforeach()

             if(_alcedo_bad_deps)
                 list(JOIN _alcedo_bad_deps \"\\n  \" _alcedo_bad_deps_text)
                 message(FATAL_ERROR
                     \"macOS bundle contains dependencies outside the app/system roots:\\n  \${_alcedo_bad_deps_text}\")
             endif()

             if(_alcedo_missing_rpath_deps)
                 list(JOIN _alcedo_missing_rpath_deps \"\\n  \" _alcedo_missing_rpath_deps_text)
                 message(FATAL_ERROR
                     \"macOS bundle contains @rpath dependencies that are not present in Contents/Frameworks:\\n  \${_alcedo_missing_rpath_deps_text}\")
             endif()

             if(_alcedo_missing_relative_deps)
                 list(JOIN _alcedo_missing_relative_deps \"\\n  \" _alcedo_missing_relative_deps_text)
                 message(FATAL_ERROR
                     \"macOS bundle contains @loader_path/@executable_path dependencies that cannot be resolved:\\n  \${_alcedo_missing_relative_deps_text}\")
             endif()

             message(STATUS \"Verified macOS bundle runtime dependencies\")
            "
        )
    endif()

    # Sign only after all bundle mutations are complete.
    if(NOT "${ALCEDO_MACOS_CODESIGN_IDENTITY}" STREQUAL "")
        install(CODE
            "set(_alcedo_app_path \"\${CMAKE_INSTALL_PREFIX}/${ALCEDO_MACOS_BUNDLE_NAME}.app\")
             if(EXISTS \"\${_alcedo_app_path}\")
                 if(EXISTS \"/usr/bin/codesign\")
                     execute_process(
                         COMMAND /usr/bin/codesign
                                 --force
                                 --deep
                                 --sign \"${ALCEDO_MACOS_CODESIGN_IDENTITY}\"
                                 --timestamp=none
                                 \"\${_alcedo_app_path}\"
                         RESULT_VARIABLE _alcedo_codesign_result
                     )
                     if(NOT _alcedo_codesign_result EQUAL 0)
                         message(FATAL_ERROR
                             \"codesign failed for \${_alcedo_app_path} (exit=\${_alcedo_codesign_result})\")
                     endif()
                 else()
                     message(WARNING \"/usr/bin/codesign not found; skipping macOS bundle re-sign\")
                 endif()
             else()
                 message(WARNING \"macOS bundle not found for re-sign: \${_alcedo_app_path}\")
             endif()
            "
        )
    endif()
endif()


file(GLOB_RECURSE ALL_SOURCE_FILES
    "${CMAKE_SOURCE_DIR}/alcedo_studio/src/*.c"
    "${CMAKE_SOURCE_DIR}/alcedo_studio/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/alcedo_studio/src/*.cu"
    "${CMAKE_SOURCE_DIR}/alcedo_studio/src/*.h"
    "${CMAKE_SOURCE_DIR}/alcedo_studio/src/*.hpp"
    "${CMAKE_SOURCE_DIR}/alcedo_studio/src/*.cuh")

# Files compiled by clang-tidy; keep to TUs so compile_commands entries are used.
file(GLOB_RECURSE TIDY_SOURCE_FILES
    "${CMAKE_SOURCE_DIR}/alcedo_studio/src/*.c"
    "${CMAKE_SOURCE_DIR}/alcedo_studio/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/alcedo_studio/src/*.cu"
    "${CMAKE_SOURCE_DIR}/alcedo_studio/tests/*.cpp")

# Header filter: allow both '/' and '\\' so Windows paths match.
set(MY_HEADER_FILTER ".*alcedo[\\\\/](src|tests)[\\\\/].*")

add_custom_target(format
    COMMAND clang-format
    -i
    -style=file
    ${ALL_SOURCE_FILES}
    COMMENT "Running clang-format on source files"
)

add_custom_target(tidy
    COMMAND clang-tidy
    "\"-p=${CMAKE_BINARY_DIR}\""
    "\"--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy\""
    "\"--header-filter=${MY_HEADER_FILTER}\""
    "\"--extra-arg=--driver-mode=cl\""
    "\"--extra-arg=-fms-compatibility\""
    "\"--extra-arg=-fms-extensions\""
    "-fix"
    "-fix-errors"
    ${TIDY_SOURCE_FILES}
    COMMENT "Running clang-tidy on source files"
)

set(CPACK_PACKAGE_NAME "alcedo")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND ALCEDO_MACOS_BUNDLE)
    set(CPACK_GENERATOR "DragNDrop;ZIP")
    set(CPACK_PACKAGE_NAME "${ALCEDO_MACOS_BUNDLE_NAME}")
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_DMG_VOLUME_NAME "${ALCEDO_MACOS_BUNDLE_NAME}")
    set(CPACK_DMG_FORMAT "UDBZ")
    # Include the .app bundle at the top level of the ZIP so the user
    # extracts it and finds the app bundle directly.
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
elseif(WIN32 AND TARGET ${ALCEDO_MAIN_BIN})
    # ------------------------------------------------------------------
    # Windows installer packaging (WIX and/or NSIS)
    # ------------------------------------------------------------------
    set(CPACK_PACKAGE_NAME "Alcedo Studio")
    set(CPACK_PACKAGE_VENDOR "Alcedo Studio Team")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Alcedo Studio")
    set(CPACK_PACKAGE_FILE_NAME "AlcedoStudio-${PROJECT_VERSION}-Windows-${CMAKE_SYSTEM_PROCESSOR}")

    set(_pkg_res_dir "${CMAKE_SOURCE_DIR}/packaging/windows")
    if(EXISTS "${_pkg_res_dir}/EULA_RTF.rtf")
        set(CPACK_RESOURCE_FILE_LICENSE "${_pkg_res_dir}/EULA_RTF.rtf")
    endif()
    if(EXISTS "${_pkg_res_dir}/alcedo.ico")
        set(CPACK_NSIS_MUI_ICON "${_pkg_res_dir}/alcedo.ico")
        set(CPACK_NSIS_MUI_UNIICON "${_pkg_res_dir}/alcedo.ico")
        set(CPACK_WIX_PRODUCT_ICON "${_pkg_res_dir}/alcedo.ico")
    endif()

    # --- NSIS settings ------------------------------------------------
    set(CPACK_NSIS_DISPLAY_NAME "Alcedo Studio v${PROJECT_VERSION}")
    set(CPACK_NSIS_PACKAGE_NAME "Alcedo Studio v${PROJECT_VERSION}")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\alcedo_main.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)

    # Start-menu shortcut
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut \\\"$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Alcedo Studio.lnk\\\" \\\"$INSTDIR\\\\bin\\\\alcedo_main.exe\\\""
    )
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete \\\"$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Alcedo Studio.lnk\\\""
    )
    # Desktop shortcut
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
        "CreateShortCut \\\"$DESKTOP\\\\Alcedo Studio.lnk\\\" \\\"$INSTDIR\\\\bin\\\\alcedo_main.exe\\\""
    )
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
        "Delete \\\"$DESKTOP\\\\Alcedo Studio.lnk\\\""
    )

    # --- WiX settings -------------------------------------------------
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "Alcedo Studio")
    set(CPACK_WIX_CULTURES "zh-CN;en-US")
    set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "https://alcedo.studio")
    # Fixed upgrade GUID so subsequent installers replace the same product.
    set(CPACK_WIX_UPGRADE_GUID "0EF54FAE-B541-4077-BB2A-7EDB85133B8E")

    # Auto-detect available generators
    set(_alcedo_win_generators "")
    find_program(_ALCEDO_WIX_CANDLE candle.exe
        PATHS
            "$ENV{WIX}bin"
            "C:/Program Files (x86)/WiX Toolset v3.11/bin"
            "C:/Program Files/WiX Toolset v3.11/bin"
            "C:/Program Files (x86)/WiX Toolset v3.14/bin"
            "C:/Program Files/WiX Toolset v3.14/bin"
    )
    find_program(_ALCEDO_WIX_LIGHT light.exe
        PATHS
            "$ENV{WIX}bin"
            "C:/Program Files (x86)/WiX Toolset v3.11/bin"
            "C:/Program Files/WiX Toolset v3.11/bin"
            "C:/Program Files (x86)/WiX Toolset v3.14/bin"
            "C:/Program Files/WiX Toolset v3.14/bin"
    )
    if(_ALCEDO_WIX_CANDLE AND _ALCEDO_WIX_LIGHT)
        list(APPEND _alcedo_win_generators "WIX")
    endif()
    find_program(_ALCEDO_NSIS_MAKENSIS makensis.exe)
    if(_ALCEDO_NSIS_MAKENSIS)
        list(APPEND _alcedo_win_generators "NSIS")
    endif()
    # Always provide ZIP as a fallback
    list(APPEND _alcedo_win_generators "ZIP")

    set(CPACK_GENERATOR "${_alcedo_win_generators}")
    message(STATUS "[alcedo] Windows package generators: ${CPACK_GENERATOR}")
else()
    set(CPACK_GENERATOR "ZIP")
endif()
include(CPack)

if(APPLE AND ALCEDO_MACOS_BUNDLE AND TARGET ${ALCEDO_MAIN_BIN})
    add_custom_target(macos_package
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
        COMMAND "${CMAKE_CPACK_COMMAND}" --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake" -B "${CMAKE_INSTALL_PREFIX}"
        DEPENDS ${ALCEDO_MAIN_BIN}
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Installing ${ALCEDO_MACOS_BUNDLE_NAME}.app and creating macOS packages"
        USES_TERMINAL
    )
endif()
