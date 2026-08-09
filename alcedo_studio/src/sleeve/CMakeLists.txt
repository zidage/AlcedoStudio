# Sleeve target ownership (filters, elements, AI domain objects, filesystem, manager).

set(FILTER_SRCS
    "${ALCEDO_SRC_ROOT}/sleeve/sleeve_filter/filter_sql.cpp"
    "${ALCEDO_SRC_ROOT}/sleeve/sleeve_filter/filter_factory.cpp"
)

def_library(SleeveFilter
    SOURCES ${FILTER_SRCS}
    PUBLIC_DEPS DuckORM StrConv
    PRIVATE_DEPS Image EditHistory JSON
)

set(ELEMENT_SRCS
    "${ALCEDO_SRC_ROOT}/sleeve/sleeve_element/sleeve_element_factory.cpp"
    "${ALCEDO_SRC_ROOT}/sleeve/sleeve_element/sleeve_element.cpp"
    "${ALCEDO_SRC_ROOT}/sleeve/sleeve_element/sleeve_file.cpp"
    "${ALCEDO_SRC_ROOT}/sleeve/sleeve_element/sleeve_folder.cpp"
)

def_library(SleeveElement
    SOURCES ${ELEMENT_SRCS}
    PUBLIC_DEPS Image ImageLoader SleeveFilter EditHistory ImagePool
)

# AI domain objects (Phase 5f): standalone image understanding/rating objects.
def_library(Ai
    SOURCES "${ALCEDO_SRC_ROOT}/ai/ai_description.cpp" "${ALCEDO_SRC_ROOT}/ai/ai_rating.cpp"
    PUBLIC_DEPS JSON
)

set(FS_SRCS
    "${ALCEDO_SRC_ROOT}/sleeve/sleeve_base.cpp"
    "${ALCEDO_SRC_ROOT}/sleeve/path_resolver.cpp"
    "${ALCEDO_SRC_ROOT}/sleeve/dentry_cache_manager.cpp"
    "${ALCEDO_SRC_ROOT}/sleeve/sleeve_filesystem.cpp"
)

def_library(SleeveFS
    SOURCES ${FS_SRCS}
    PUBLIC_DEPS SleeveElement Storage
)

def_library(SleeveManager
    SOURCES "${ALCEDO_SRC_ROOT}/sleeve/sleeve_manager.cpp"
    PUBLIC_DEPS SleeveFS PipelineScheduler
)
