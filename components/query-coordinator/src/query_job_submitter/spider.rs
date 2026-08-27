//! [`QueryJobSubmitter`] implementation for [`spider_client::SpiderClient`].

use async_trait::async_trait;
use clp_rust_utils::job_config::QueryJobId;
use clp_rust_utils::task_io::query::ClpSQueryOption;
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
use crate::query_job_submitter::QueryJobSubmitter;

const CLP_TDL_PACKAGE_NAME: &str = "clp";
const QUERY_TASK_FUNC: &str = "query::clp_s_query_to_results_cache";
const QUERY_TASK_NUM_INPUTS: usize = 4;

/// Builds the query task graph and its flattened external inputs.
///
/// Spider associates graph inputs with tasks positionally, so the returned input vector repeats
/// the TDL signature order for each task:
///
/// 1. `query_job_id`
/// 2. `clp_s_query_option`
/// 3. `dataset`
/// 4. `archive_id`
fn build_query_task_graph(
    query_job_id: QueryJobId,
    clp_s_query_option: &ClpSQueryOption,
    archives: Vec<(String, String)>,
    query_task_execution_policy: &ExecutionPolicy,
) -> Result<(TaskGraph, Vec<TaskInput>), Error> {
    if archives.is_empty() {
        return Err(Error::NoTaskInputs);
    }

    let mut graph = TaskGraph::new(None, None)?;
    let mut inputs = Vec::with_capacity(archives.len() * QUERY_TASK_NUM_INPUTS);
    let query_job_id_payload = rmp_serde::to_vec(&query_job_id)?;
    let clp_s_query_option_payload = rmp_serde::to_vec(clp_s_query_option)?;

    for (dataset, archive_id) in archives {
        graph.insert_task(TaskDescriptor {
            tdl_context: TdlContext {
                package: CLP_TDL_PACKAGE_NAME.to_owned(),
                task_func: QUERY_TASK_FUNC.to_owned(),
            },
            execution_policy: Some(query_task_execution_policy.clone()),
            inputs: vec![
                DataTypeDescriptor::Value(ValueTypeDescriptor::int32()),
                DataTypeDescriptor::Value(ValueTypeDescriptor::struct_from_name(
                    "ClpSQueryOption",
                )?),
                DataTypeDescriptor::Value(ValueTypeDescriptor::struct_from_name("String")?),
                DataTypeDescriptor::Value(ValueTypeDescriptor::struct_from_name("String")?),
            ],
            outputs: vec![DataTypeDescriptor::Value(
                ValueTypeDescriptor::struct_from_name("QueryTaskOutput")?,
            )],
            input_sources: None,
        })?;

        inputs.push(TaskInput::ValuePayload(query_job_id_payload.clone()));
        inputs.push(TaskInput::ValuePayload(clp_s_query_option_payload.clone()));
        inputs.push(TaskInput::ValuePayload(rmp_serde::to_vec(&dataset)?));
        inputs.push(TaskInput::ValuePayload(rmp_serde::to_vec(&archive_id)?));
    }

    Ok((graph, inputs))
}

#[async_trait]
impl QueryJobSubmitter for SpiderClient {
    /// # Errors
    ///
    /// Returns an error if:
    ///
    /// * No archives were supplied.
    /// * Graph construction or execution-policy validation fails.
    /// * A task input cannot be serialized as `MessagePack`.
    /// * Spider rejects the job submission request.
    async fn submit_query_job(
        &self,
        query_job_id: QueryJobId,
        resource_group_id: ResourceGroupId,
        clp_s_query_option: ClpSQueryOption,
        archives: Vec<(String, String)>,
        query_task_execution_policy: ExecutionPolicy,
    ) -> Result<JobId, Error> {
        let (graph, inputs) = build_query_task_graph(
            query_job_id,
            &clp_s_query_option,
            archives,
            &query_task_execution_policy,
        )?;
        let job_id = self.submit_job(resource_group_id, &graph, inputs).await?;

        tracing::info!(
            query_job_id,
            spider_job_id = %job_id,
            num_tasks = graph.get_num_tasks(),
            "Submitted query job to Spider."
        );

        Ok(job_id)
    }
}

#[cfg(test)]
mod tests {
    use std::num::NonZeroU32;

    use clp_rust_utils::task_io::query::ClpSQueryOption;
    use spider_core::task::DataTypeDescriptor;
    use spider_core::task::ExecutionPolicy;
    use spider_core::task::TimeoutPolicy;
    use spider_core::task::ValueTypeDescriptor;
    use spider_core::types::io::TaskInput;

    use super::CLP_TDL_PACKAGE_NAME;
    use super::QUERY_TASK_FUNC;
    use super::build_query_task_graph;
    use crate::Error;

    fn query_options() -> ClpSQueryOption {
        ClpSQueryOption {
            query_string: "level:error".to_owned(),
            max_num_results: NonZeroU32::new(1_000).expect("1,000 is nonzero"),
            begin_timestamp: Some(1_700_000_000_000_001),
            end_timestamp: Some(1_700_000_000_000_999),
            ignore_case: true,
        }
    }

    fn execution_policy() -> ExecutionPolicy {
        ExecutionPolicy {
            max_num_retry: 1,
            max_num_instances: 2,
            timeout_policy: TimeoutPolicy {
                soft_timeout_ms: 600_000,
                hard_timeout_ms: 1_200_000,
            },
        }
    }

    fn value_payload(input: &TaskInput) -> &[u8] {
        let TaskInput::ValuePayload(payload) = input;
        payload
    }

    #[test]
    fn creates_one_independent_task_per_archive_without_termination_tasks() {
        let options = query_options();
        let policy = execution_policy();
        let archives = vec![
            ("dataset-a".to_owned(), "archive-1".to_owned()),
            ("dataset-b".to_owned(), "archive-2".to_owned()),
        ];

        let (graph, inputs) = build_query_task_graph(42, &options, archives, &policy)
            .expect("valid archive inputs should produce a graph");

        assert_eq!(2, graph.get_num_tasks());
        assert!(graph.get_commit_task_descriptor().is_none());
        assert!(graph.get_cleanup_task_descriptor().is_none());
        assert_eq!(8, inputs.len());

        let expected_input_types = vec![
            DataTypeDescriptor::Value(ValueTypeDescriptor::int32()),
            DataTypeDescriptor::Value(
                ValueTypeDescriptor::struct_from_name("ClpSQueryOption")
                    .expect("type name should be valid"),
            ),
            DataTypeDescriptor::Value(
                ValueTypeDescriptor::struct_from_name("String").expect("type name should be valid"),
            ),
            DataTypeDescriptor::Value(
                ValueTypeDescriptor::struct_from_name("String").expect("type name should be valid"),
            ),
        ];
        let expected_output_types = vec![DataTypeDescriptor::Value(
            ValueTypeDescriptor::struct_from_name("QueryTaskOutput")
                .expect("type name should be valid"),
        )];

        for task_idx in 0..2 {
            let task = graph.get_task(task_idx).expect("task should exist");
            assert_eq!(CLP_TDL_PACKAGE_NAME, task.get_tdl_context().package);
            assert_eq!(QUERY_TASK_FUNC, task.get_tdl_context().task_func);
            assert_eq!(&policy, task.get_execution_policy());
            assert_eq!(0, task.get_num_parents());
            assert_eq!(0, task.get_num_children());

            let actual_input_types = (0..4)
                .map(|position| {
                    graph
                        .get_task_input(spider_core::task::TaskInputOutputIndex {
                            task_idx,
                            position,
                        })
                        .expect("task input should exist")
                        .get_type_descriptor()
                        .clone()
                })
                .collect::<Vec<_>>();
            let actual_output_types = (0..1)
                .map(|position| {
                    graph
                        .get_task_output(spider_core::task::TaskInputOutputIndex {
                            task_idx,
                            position,
                        })
                        .expect("task output should exist")
                        .get_type_descriptor()
                        .clone()
                })
                .collect::<Vec<_>>();

            assert_eq!(expected_input_types, actual_input_types);
            assert_eq!(expected_output_types, actual_output_types);
        }
    }

    #[test]
    fn serializes_each_task_inputs_in_tdl_signature_order() {
        let options = query_options();
        let archives = vec![
            ("dataset-a".to_owned(), "archive-1".to_owned()),
            ("dataset-b".to_owned(), "archive-2".to_owned()),
        ];

        let (_, inputs) = build_query_task_graph(42, &options, archives, &execution_policy())
            .expect("valid archive inputs should produce graph inputs");

        for (task_idx, (expected_dataset, expected_archive_id)) in
            [("dataset-a", "archive-1"), ("dataset-b", "archive-2")]
                .into_iter()
                .enumerate()
        {
            let offset = task_idx * 4;
            assert_eq!(
                42,
                rmp_serde::from_slice::<i32>(value_payload(&inputs[offset]))
                    .expect("query job ID should deserialize")
            );
            assert_eq!(
                options,
                rmp_serde::from_slice::<ClpSQueryOption>(value_payload(&inputs[offset + 1]))
                    .expect("query options should deserialize")
            );
            assert_eq!(
                expected_dataset,
                rmp_serde::from_slice::<String>(value_payload(&inputs[offset + 2]))
                    .expect("dataset should deserialize")
            );
            assert_eq!(
                expected_archive_id,
                rmp_serde::from_slice::<String>(value_payload(&inputs[offset + 3]))
                    .expect("archive ID should deserialize")
            );
        }
    }

    #[test]
    fn rejects_an_empty_archive_list() {
        let error = build_query_task_graph(42, &query_options(), Vec::new(), &execution_policy())
            .expect_err("an empty archive list should be rejected");

        assert!(matches!(error, Error::NoTaskInputs));
    }

    #[test]
    fn rejects_an_invalid_execution_policy() {
        let invalid_policy = ExecutionPolicy {
            max_num_retry: 1,
            max_num_instances: 0,
            timeout_policy: TimeoutPolicy::default(),
        };

        let error = build_query_task_graph(
            42,
            &query_options(),
            vec![("dataset".to_owned(), "archive".to_owned())],
            &invalid_policy,
        )
        .expect_err("an invalid execution policy should be rejected");

        assert!(matches!(error, Error::TaskGraph(_)));
    }
}
