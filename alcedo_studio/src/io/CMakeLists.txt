# Image I/O target ownership (export, ultra HDR, image writer).
# Dependencies on opencv, OpenImageIO, and ImageBuffer are resolved
# before this manifest in the domain composition order.

def_library(ExportRecipe
    SOURCES
        "${ALCEDO_INCLUDE_ROOT}/io/image/export_recipe.hpp"
        "${ALCEDO_SRC_ROOT}/io/image/export_recipe.cpp"
    PUBLIC_DEPS ExportIccProfileResolver
)

def_library(ExportIccProfileResolver
    SOURCES
        "${ALCEDO_INCLUDE_ROOT}/io/image/export_color_profile_config.hpp"
        "${ALCEDO_INCLUDE_ROOT}/io/image/export_icc_profile_resolver.hpp"
        "${ALCEDO_SRC_ROOT}/io/image/export_icc_profile_resolver.cpp"
    PUBLIC_DEPS
        opencv_core
)

if(APPLE)
    target_link_libraries(ExportIccProfileResolver PRIVATE "-framework CoreGraphics" "-framework CoreFoundation")
endif()

# Shared app name/version for EXIF Software (0x0131) and UI. Defaults live in
# alcedo_version.hpp; CMake overrides them from PROJECT_VERSION.
add_library(AlcedoVersion INTERFACE)
target_include_directories(AlcedoVersion INTERFACE "${ALCEDO_INCLUDE_ROOT}")
target_compile_definitions(AlcedoVersion INTERFACE
    ALCEDO_APP_NAME="Alcedo Studio"
    ALCEDO_APP_VERSION="${PROJECT_VERSION}"
)

def_library(JpegExifApp1
    SOURCES
        "${ALCEDO_INCLUDE_ROOT}/io/image/jpeg_exif_app1.hpp"
        "${ALCEDO_SRC_ROOT}/io/image/jpeg_exif_app1.cpp"
    PUBLIC_DEPS AlcedoVersion JSON
)

if(PUERHLAB_ULTRAHDR_AVAILABLE)
    def_library(UltraHdrWriter
        SOURCES "${ALCEDO_SRC_ROOT}/io/image/ultra_hdr_writer.cpp"
        PUBLIC_DEPS OpenImageIO::OpenImageIO ImageBuffer puerhlab::uhdr ExportIccProfileResolver JpegExifApp1
        PRIVATE_DEPS opencv_core
    )

    if(APPLE)
        target_link_libraries(UltraHdrWriter PRIVATE "-framework CoreGraphics" "-framework CoreFoundation")
    endif()
endif()

def_library(ImageWriter
    SOURCES "${ALCEDO_SRC_ROOT}/io/image/image_writer.cpp"
    PUBLIC_DEPS OpenImageIO::OpenImageIO ImageBuffer ExportIccProfileResolver ExportRecipe JpegExifApp1
    PRIVATE_DEPS ${ALCEDO_OPENCV_IMAGE_DEPS} StrConv
)
if(PUERHLAB_ULTRAHDR_AVAILABLE)
    target_link_libraries(ImageWriter PUBLIC UltraHdrWriter)
    target_compile_definitions(ImageWriter PUBLIC ALCEDO_HAS_ULTRAHDR=1)
endif()

if(APPLE)
    target_link_libraries(ImageWriter PRIVATE "-framework CoreGraphics" "-framework CoreFoundation")
endif()
