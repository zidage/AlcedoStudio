# Storage & DB target ownership (DuckDB ORM, mappers, Storage facade).

# ORM Layer
def_library(DuckORM
    SOURCES
        "${ALCEDO_SRC_ROOT}/storage/mapper/duckorm/duckdb_orm.cpp"
        "${ALCEDO_SRC_ROOT}/storage/mapper/duckorm/duckdb_types.cpp"
        "${ALCEDO_SRC_ROOT}/storage/mapper/duckorm/duckdb_expr.cpp"
    PUBLIC_DEPS DuckDB
)

# Mappers
set(MAPPER_SRCS
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/element/file_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/element/folder_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/element/element_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/element/element_id_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/base/base_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/base/root_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/image/image_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/image/image_search_columns.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/filter/filter_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/edit_history/edit_commit_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/edit_history/version_ref_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/edit_history/image_edit_state_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/pipeline/pipeline_mapper.cpp"
)

def_library(SleeveMapper
    SOURCES ${MAPPER_SRCS}
    PUBLIC_DEPS DuckDB DuckORM Image SleeveElement ImagePool PipelineHistory EditPipeline PipelineScheduler StrConv
)

# Stores + Storage facade (no storage/service rank)
set(STORAGE_SRCS
    "${ALCEDO_SRC_ROOT}/sleeve/storage.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/store_types.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/database.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/duckdb_extension.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/image/image_store.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/semantic/semantic_model_registry.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/semantic/semantic_embedding_store.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/semantic/semantic_label_store.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/semantic/semantic_vector_search.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/semantic/semantic_label_assignment.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/semantic/semantic_score_elbow.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/semantic/semantic_tables.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/ai/ai_store.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/sleeve/element_store.cpp"
    "${ALCEDO_SRC_ROOT}/storage/store/edit_history/commit_graph_store.cpp"
)

def_library(Storage
    SOURCES ${STORAGE_SRCS}
    PUBLIC_DEPS Ai DuckDB DuckORM SleeveMapper StrConv PipelineScheduler
    PRIVATE_DEPS AppDiagnostics
)
