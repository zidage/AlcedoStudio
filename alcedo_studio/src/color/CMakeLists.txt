# ==============================================================================
# Color encoding catalog: gamuts, transfer functions and encodings
# (docs/roadmap/alcedo_studio/edit/lut_color_encoding_plan.md, section 5).
# ==============================================================================

def_library(ColorEncodingCatalog
    SOURCES
        ${ALCEDO_SRC_ROOT}/color/color_encoding_catalog.cpp
        ${ALCEDO_INCLUDE_ROOT}/color/color_encoding_catalog.hpp
        ${ALCEDO_INCLUDE_ROOT}/color/color_encoding_math.h
)
