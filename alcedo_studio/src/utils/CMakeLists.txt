# ==============================================================================
# Utils targets extracted from alcedo_studio/src/CMakeLists.txt (Phase 3D).
# ==============================================================================

def_library(TimeProvider
    SOURCES ${ALCEDO_SRC_ROOT}/utils/clock/time_provider.cpp
)

# Qt-free logging of the core libraries (utils/logging/log.hpp). On Windows it is a DLL: the CUDA
# implementation libraries are DLLs too, and every module must share one handler and one set of
# enabled levels. A static copy in each DLL would keep the default stderr handler there.
set(ALCEDO_LOG_SOURCES
    ${ALCEDO_SRC_ROOT}/utils/logging/log.cpp
    ${ALCEDO_SRC_ROOT}/include/utils/logging/log.hpp
    ${ALCEDO_SRC_ROOT}/include/utils/logging/log_categories.hpp
)
if(WIN32)
    def_library(AlcedoLog TYPE SHARED SOURCES ${ALCEDO_LOG_SOURCES})
    set_target_properties(AlcedoLog PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_options(AlcedoLog PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:/GL->
    )
    install(TARGETS AlcedoLog RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
else()
    def_library(AlcedoLog SOURCES ${ALCEDO_LOG_SOURCES})
endif()
# Compile-time floor: Debug builds compile in ALCEDO_LOG_DEBUG; other build types compile it out.
target_compile_definitions(AlcedoLog PUBLIC
    $<IF:$<CONFIG:Debug>,ALCEDO_LOG_MIN_LEVEL=0,ALCEDO_LOG_MIN_LEVEL=1>
)

if(WIN32)
    def_library(PreviewPerformance TYPE SHARED
        SOURCES
            ${ALCEDO_SRC_ROOT}/utils/diagnostics/preview_performance.cpp
            ${ALCEDO_SRC_ROOT}/utils/diagnostics/preview_performance_format.cpp
            ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/preview_performance.hpp
            ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/preview_performance_record.hpp
            ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/preview_performance_format.hpp
    )
    set_target_properties(PreviewPerformance PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_options(PreviewPerformance PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:/GL->
    )
    install(TARGETS PreviewPerformance RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
else()
    def_library(PreviewPerformance
        SOURCES
            ${ALCEDO_SRC_ROOT}/utils/diagnostics/preview_performance.cpp
            ${ALCEDO_SRC_ROOT}/utils/diagnostics/preview_performance_format.cpp
            ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/preview_performance.hpp
            ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/preview_performance_record.hpp
            ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/preview_performance_format.hpp
    )
endif()

def_library(AppDiagnostics
    SOURCES
        ${ALCEDO_SRC_ROOT}/utils/diagnostics/app_logging.cpp
        ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/app_logging.hpp
        ${ALCEDO_SRC_ROOT}/utils/diagnostics/render_e2e_timing.cpp
        ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/render_e2e_timing.hpp
    PUBLIC_DEPS Qt6::Core PreviewPerformance AlcedoLog
)

def_library(CudaDriverRequirements
    SOURCES ${ALCEDO_SRC_ROOT}/utils/cuda/cuda_driver_requirements.cpp
)

# SHA-256 digests of in-memory data (ICC profile identity for raster input).
def_library(Sha256Digest
    SOURCES ${ALCEDO_SRC_ROOT}/utils/hash/sha256.cpp
)

def_library(StrConv
    SOURCES
        ${ALCEDO_SRC_ROOT}/utils/string/converter.cpp
        ${ALCEDO_SRC_ROOT}/utils/string/search_text.cpp
    PUBLIC_DEPS utfcpp
)

# CUBE metadata comments, package inventory digests, and the local LUT library
# scan (docs/lut-package-system.md).
def_library(LutMetadata
    SOURCES
        ${ALCEDO_SRC_ROOT}/utils/lut/lut_metadata.cpp
        ${ALCEDO_SRC_ROOT}/utils/lut/lut_inventory_digest.cpp
        ${ALCEDO_SRC_ROOT}/utils/lut/lut_library_scan.cpp
        ${ALCEDO_SRC_ROOT}/include/utils/lut/lut_metadata.hpp
        ${ALCEDO_SRC_ROOT}/include/utils/lut/lut_inventory_digest.hpp
        ${ALCEDO_SRC_ROOT}/include/utils/lut/lut_library_scan.hpp
    PUBLIC_DEPS Qt6::Core
    PRIVATE_DEPS JSON ThreadPool
)

# Shared read-only utilities for source path normalization, attributes, fingerprints, directory
# listings, and volume identity.
set(ALCEDO_SOURCE_FILE_UTILS_SOURCES
    ${ALCEDO_SRC_ROOT}/utils/file/source_file.cpp
    ${ALCEDO_SRC_ROOT}/utils/file/source_file_fingerprint.cpp
    ${ALCEDO_SRC_ROOT}/utils/file/mount_directory_listing.cpp
    ${ALCEDO_SRC_ROOT}/include/utils/file/source_file_fingerprint.hpp
    ${ALCEDO_SRC_ROOT}/include/utils/file/mount_directory_listing.hpp
    ${ALCEDO_SRC_ROOT}/include/utils/file/volume_identity.hpp
)
if(WIN32)
    list(APPEND ALCEDO_SOURCE_FILE_UTILS_SOURCES
        ${ALCEDO_SRC_ROOT}/utils/file/volume_identity_windows.cpp)
elseif(APPLE)
    list(APPEND ALCEDO_SOURCE_FILE_UTILS_SOURCES
        ${ALCEDO_SRC_ROOT}/utils/file/volume_identity_macos.mm)
    set_source_files_properties(${ALCEDO_SRC_ROOT}/utils/file/volume_identity_macos.mm
        PROPERTIES LANGUAGE OBJCXX COMPILE_FLAGS "-fobjc-arc")
else()
    list(APPEND ALCEDO_SOURCE_FILE_UTILS_SOURCES
        ${ALCEDO_SRC_ROOT}/utils/file/volume_identity_linux.cpp)
endif()
def_library(SourceFileUtils
    SOURCES ${ALCEDO_SOURCE_FILE_UTILS_SOURCES}
    PUBLIC_DEPS xxHash
    PRIVATE_DEPS Qt6::Core
)
if(WIN32)
    # WNetGetConnectionW: the share of a mapped network drive.
    target_link_libraries(SourceFileUtils PRIVATE mpr)
elseif(APPLE)
    target_link_libraries(SourceFileUtils PRIVATE "-framework Foundation")
endif()
