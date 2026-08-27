//! The crate-level error type for the query coordinator.

/// Errors returned while constructing or submitting a query task graph.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("no archives were selected for the query task graph")]
    NoTaskInputs,

    #[error("spider request failure: {0}")]
    SpiderClient(#[from] spider_client::error::ClientError),

    #[error("failed to build the query task graph: {0}")]
    TaskGraph(#[from] spider_core::task::Error),

    #[error("failed to serialize a query task input: {0}")]
    TaskInputSerialization(#[from] rmp_serde::encode::Error),
}
