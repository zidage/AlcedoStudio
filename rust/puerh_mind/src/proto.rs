pub mod common {
    tonic::include_proto!("common");
}

pub mod semantic {
    tonic::include_proto!("semantic");
}

// The `alcedo.ai` package is shared by ai_common.proto and segmentation.proto.
// A single tonic::include_proto!("alcedo.ai") brings in every type from both
// files (tonic_prost_build emits one module per package). The dotted package
// name maps to nested modules `alcedo::ai`, which we provide here so callers
// reach types as `crate::proto::alcedo::ai::AiRequestHeader`.
pub mod alcedo {
    pub mod ai {
        tonic::include_proto!("alcedo.ai");
    }
}
