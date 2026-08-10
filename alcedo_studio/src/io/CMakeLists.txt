# Image I/O target ownership (export, ultra HDR, image writer).
# Dependencies on opencv, OpenImageIO, and ImageBuffer are resolved
# before this manifest in the domain composition order.

def_library(ExportRecipe
    SOURCES
        "${ALCEDO_INCLUDE_ROOT}/io/image/export_recipe.hpp"
        "${ALCEDO_SRC_ROOT}/io/image/export_recipe.cpp"
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

if(PUERHLAB_ULTRAHDR_AVAILABLE)
    def_library(UltraHdrWriter
        SOURCES "${ALCEDO_SRC_ROOT}/io/image/ultra_hdr_writer.cpp"
        PUBLIC_DEPS OpenImageIO::OpenImageIO ImageBuffer Exiv2 puerhlab::uhdr ExportIccProfileResolver
        PRIVATE_DEPS opencv_core
    )

    if(APPLE)
        target_link_libraries(UltraHdrWriter PRIVATE "-framework CoreGraphics" "-framework CoreFoundation")
    endif()
endif()

def_library(ImageWriter
    SOURCES "${ALCEDO_SRC_ROOT}/io/image/image_writer.cpp"
    PUBLIC_DEPS OpenImageIO::OpenImageIO ImageBuffer ExportIccProfileResolver ExportRecipe
    PRIVATE_DEPS ${ALCEDO_OPENCV_IMAGE_DEPS} StrConv
)
if(PUERHLAB_ULTRAHDR_AVAILABLE)
    target_link_libraries(ImageWriter PUBLIC UltraHdrWriter)
    target_compile_definitions(ImageWriter PUBLIC ALCEDO_HAS_ULTRAHDR=1)
endif()

if(APPLE)
    target_link_libraries(ImageWriter PRIVATE "-framework CoreGraphics" "-framework CoreFoundation")
endif()
