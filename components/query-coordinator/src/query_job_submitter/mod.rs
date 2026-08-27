//! The query-job submission API for registering CLP-S query graphs with Spider.

mod spider;

use async_trait::async_trait;
use clp_rust_utils::job_config::QueryJobId;
use clp_rust_utils::task_io::query::ClpSQueryOption;
use spider_core::task::ExecutionPolicy;
use spider_core::types::id::JobId;
use spider_core::types::id::ResourceGroupId;

use crate::Error;

/// Registers CLP-S query jobs with a distributed task scheduler.
#[async_trait]
pub trait QueryJobSubmitter: Clone + Send + Sync {
    /// Builds one independent archive-query task for every `(dataset, archive_id)` pair and
    /// registers the resulting graph without starting it.
    ///
    /// Every task receives `query_job_id` and `clp_s_query_option`, but only its own scalar dataset
    /// and archive ID. The graph has no commit or cleanup task.
    ///
    /// # Parameters
    ///
    /// * `query_job_id` - The `MySQL` query-job ID and results-cache collection name.
    /// * `resource_group_id` - The Spider resource group under which to register the graph.
    /// * `clp_s_query_option` - Job-wide CLP-S query options copied into every task payload.
    /// * `archives` - The coordinator-planned `(dataset, archive_id)` pairs, one per task.
    /// * `query_task_execution_policy` - The retry, concurrency, and timeout policy attached to
    ///   every archive-query task.
    ///
    /// # Returns
    ///
    /// The Spider job ID on success.
    ///
    /// # Errors
    ///
    /// Implementations must document their error conditions.
    async fn submit_query_job(
        &self,
        query_job_id: QueryJobId,
        resource_group_id: ResourceGroupId,
        clp_s_query_option: ClpSQueryOption,
        archives: Vec<(String, String)>,
        query_task_execution_policy: ExecutionPolicy,
    ) -> Result<JobId, Error>;
}
