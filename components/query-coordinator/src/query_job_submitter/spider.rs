//! [`QueryJobSubmitter`] implementation for [`spider_client::SpiderClient`].

use async_trait::async_trait;
use clp_rust_utils::job_config::QueryJobId;
use clp_rust_utils::task_io::query::ClpSQueryOption;
use clp_rust_utils::task_io::query::OutputHandle;
use spider_client::SpiderClient;
use spider_core::task::DataTypeDescriptor;
use spider_core::task::ExecutionPolicy;
use spider_core::task::TaskDescriptor;
use spider_core::task::TaskGraph;
use spider_core::task::TdlContext;
use spider_core::task::ValueTypeDescriptor;
use spider_core::types::id::JobId;
use spider_core::types::id::ResourceGroupId;
use spider_core::types::io::TaskInput;

use crate::Error;
use crate::query_job_submitter::ArchiveMetadata;
use crate::query_job_submitter::QueryJobSubmitter;

/// Builds independent archive-search tasks and their positionally ordered external inputs.
///
/// # Returns
///
/// The task graph and its positionally ordered external inputs on success.
///
/// # Errors
///
/// Returns an error if:
///
/// * [`Error::NoArchivesToSearch`] if no archives are supplied.
/// * Forwards [`TaskGraph::new`]'s return values on failure.
/// * Forwards [`ValueTypeDescriptor::struct_from_name`]'s return values on failure.
/// * Forwards [`TaskGraph::insert_task`]'s return values on failure.
/// * Forwards [`rmp_serde::to_vec`]'s return values on failure.
fn build_query_task_graph(
    query_job_id: QueryJobId,
    clp_s_query_option: &ClpSQueryOption,
    output_handle: &OutputHandle,
    archives_to_search: Vec<(ArchiveMetadata, ExecutionPolicy)>,
) -> Result<(TaskGraph, Vec<TaskInput>), Error> {
    // NOTE: Keep these names and the input order in sync with the TDL package definitions.
    const CLP_TDL_PACKAGE_NAME: &str = "clp";
    const QUERY_TASK_FUNC: &str = "query::clp_s_search";

    if archives_to_search.is_empty() {
        return Err(Error::NoArchivesToSearch);
    }

    let mut graph = TaskGraph::new(None, None)?;
    let mut inputs = Vec::new();
    let query_job_id_payload = rmp_serde::to_vec(&query_job_id)?;
    let query_option_payload = rmp_serde::to_vec(clp_s_query_option)?;
    let output_handle_payload = rmp_serde::to_vec(output_handle)?;

    for (archive, execution_policy) in archives_to_search {
        graph.insert_task(TaskDescriptor {
            tdl_context: TdlContext {
                package: CLP_TDL_PACKAGE_NAME.to_owned(),
                task_func: QUERY_TASK_FUNC.to_owned(),
            },
            execution_policy: Some(execution_policy),
            inputs: vec![
                DataTypeDescriptor::Value(ValueTypeDescriptor::int32()),
                DataTypeDescriptor::Value(ValueTypeDescriptor::struct_from_name(
                    "ClpSQueryOption",
                )?),
                DataTypeDescriptor::Value(ValueTypeDescriptor::struct_from_name(
                    "Option<NonEmptyString>",
                )?),
                DataTypeDescriptor::Value(ValueTypeDescriptor::struct_from_name("NonEmptyString")?),
                DataTypeDescriptor::Value(ValueTypeDescriptor::struct_from_name("OutputHandle")?),
            ],
            outputs: vec![],
            input_sources: None,
        })?;

        // TaskContext is supplied by Spider, and archive size is coordinator-only metadata.
        inputs.push(TaskInput::ValuePayload(query_job_id_payload.clone()));
        inputs.push(TaskInput::ValuePayload(query_option_payload.clone()));
        inputs.push(TaskInput::ValuePayload(rmp_serde::to_vec(
            &archive.dataset,
        )?));
        inputs.push(TaskInput::ValuePayload(rmp_serde::to_vec(&archive.id)?));
        inputs.push(TaskInput::ValuePayload(output_handle_payload.clone()));
    }

    Ok((graph, inputs))
}

#[async_trait]
impl QueryJobSubmitter for SpiderClient {
    /// # Errors
    ///
    /// Returns an error if:
    ///
    /// * Forwards [`build_query_task_graph`]'s return values on failure.
    /// * Forwards [`SpiderClient::submit_job`]'s return values on failure.
    async fn submit_query_job(
        &self,
        query_job_id: QueryJobId,
        resource_group_id: ResourceGroupId,
        clp_s_query_option: ClpSQueryOption,
        output_handle: OutputHandle,
        archives_to_search: Vec<(ArchiveMetadata, ExecutionPolicy)>,
    ) -> Result<JobId, Error> {
        let (graph, inputs) = build_query_task_graph(
            query_job_id,
            &clp_s_query_option,
            &output_handle,
            archives_to_search,
        )?;
        // The handler must persist this ID before starting the job. Do not retry registration:
        // an uncertain response can mean Spider accepted the graph already.
        let spider_job_id = self.submit_job(resource_group_id, &graph, inputs).await?;
        tracing::info!(
            query_job_id = % query_job_id,
            spider_job_id = % spider_job_id,
            num_tasks = graph.get_num_tasks(),
            "Submitted query job to Spider.",
        );
        Ok(spider_job_id)
    }
}
