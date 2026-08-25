# RFC: CLP Spider TDL package for search

Status: early draft

This RFC defines the normative Spider search TDL contract. Its primary outcome
is the final field list for each search task input and output, including every
field's Rust type, producer, validation and defaulting rules, exact use inside
the task, and consumer. The contract is forward-looking and must be complete
enough to implement the TDL functions without inferring behavior from the
Python implementation.

## MVP scope summary

The MVP covers only plain search using the results-cache output path:

- **Result-cache only.** The native search binary writes search results directly
  to MongoDB through its `results-cache` output handler. The `network` and
  `file` output handlers are outside the MVP scope.
- **No aggregation.** The MVP does not support a reducer or any
  count/count-by-time/min/max/unique aggregation. An accepted search job has no
  aggregation configuration.
- **No cancellation.** The MVP does not consume `CANCELLING` query-job rows,
  request Spider cancellation, or define cancellation behavior for an active
  search task. Cancellation is deferred to a later phase.

## 1. Context and background

The proposed search architecture separates job-level orchestration from
archive-level execution:

- The search coordinator discovers and categorizes CLP query jobs and plans
  their archive work. It creates a per-job handle with that prepared plan.
- Spider owns distributed scheduling and execution of the submitted task graph.
- The CLP TDL package supplies the search functions that Spider workers can
  execute.
- The selected native search binary writes results directly to the per-job
  results-cache collection.
- A `search::commit` termination function finalizes successful CLP query jobs in
  MySQL.

### 1.1 Component responsibilities

Although this RFC focuses on the TDL package, its contract sits inside the
following end-to-end component hierarchy:

```text
SearchCoordinator
  -> QueryJobHandle
    -> QueryJobSubmitter / SpiderClient
      -> Spider
        -> clp TDL package
```

#### 1.1.1 `SearchCoordinator`

The coordinator owns concerns spanning all query jobs:

- Poll and categorize MySQL query-job rows.
- Select the target archives and build a `SearchPlan`.
- Enforce the global concurrency limit.
- Create a `QueryJobHandle` containing the query job and its prepared plan.
- Provide coordinator-level recovery and fault tolerance.

#### 1.1.2 `QueryJobHandle`

One handle owns the durable lifecycle of one CLP query job:

- Hold the coordinator-prepared `SearchPlan` through submission.
- Ask `QueryJobSubmitter` to submit the graph definition to Spider.
- Persist `spider_id` and `RUNNING` after Spider accepts it.
- Ask the submitter to start and poll the Spider job.
- Verify committed success or report terminal failure.

The handle does not select archives and does not construct Spider's graph
representation.

#### 1.1.3 `QueryJobSubmitter` and `SpiderClient`

The submitter is the coordinator-side abstraction over the distributed
scheduler. Its `SpiderClient` implementation:

- Translates `SearchPlan` into a Spider `TaskGraph`.
- Declares TDL function names and input/output type descriptors.
- Serializes task inputs.
- Submits and starts the Spider job.
- Polls Spider and translates its terminal state into `QueryJobOutcome`.

It references the TDL wire contract but does not directly invoke the Rust TDL
functions.

#### 1.1.4 Spider

Spider owns distributed graph execution:

- Schedule graph nodes on available workers.
- Run one engine-appropriate archive-search task for each planned archive.
- Apply each node's retry, concurrency, and timeout policy.
- Run `search::commit` as the termination task.
- Expose the Spider job's state and error to the submitter.

#### 1.1.5 CLP TDL package

The TDL package defines how each graph node executes:

- `search::clo_search_to_results_cache` interprets one CLP archive input and
  launches CLO.
- `search::clp_s_search_to_results_cache` interprets one CLP-S
  dataset/archive input and launches clp-s.
- The native binary writes matches directly to MongoDB collection
  `<query_job_id>`; both archive tasks return only success or failure.
- `search::commit` marks `query_jobs` `SUCCEEDED` after Spider confirms that
  every archive task succeeded.

The package does not select archives, construct the job-specific graph, or
return search hits to the coordinator.

### 1.2 New-job planning and lifecycle

For a new job, the coordinator owns planning and the handle owns the resulting
job lifecycle:

```text
PENDING query_jobs row
  -> SearchCoordinator creates SearchPlan
  -> QueryJobHandle asks the submitter to submit the graph to Spider
  -> QueryJobHandle persists spider_id and RUNNING
  -> QueryJobHandle starts and polls the Spider job
```

## 2. Purpose of this RFC

This RFC defines the Spider task interface required by the MVP search flow. It
will determine the required tasks and specify each task's signature, including
the complete set of input and output fields. For every input, it will describe
who produces it and exactly how it is used when constructing or executing the
Spider task graph.

For every output, the RFC will describe how it is produced and where it goes:
whether it remains internal to the task graph, is consumed by another task, is
uploaded to persistent storage, or is represented in the final Spider job
outcome. It will also state how each output or persistent side effect is used
by `QueryJobHandle` and `SearchCoordinator`. The resulting specification must
make the complete data flow traceable without assuming the number or names of
the tasks before the design is complete. Once the RFC is complete, an
implementer must be able to construct the task graph and implement every task
without consulting an existing implementation or making unstated behavioral
assumptions.

## 3. TDL design goals

Given the baseline architecture, the search TDL package must provide:

- The Spider-executable tasks required to perform and finalize an MVP search.
- A Spider-visible name and Rust function signature for every task, defined in
  the TDL package.
- Serializable input and output types under
  `clp_rust_utils::task_io::search`, shared by the coordinator-side graph
  builder and the TDL package.
- Deterministic handling and validation of every task input, with each input
  used only for the behavior assigned to the receiving task.
- Clear output behavior for every task, distinguishing values passed through
  the Spider graph from data written to MongoDB or other persistent storage.
- Sufficient completion information and persistent side effects for
  `QueryJobHandle` and `SearchCoordinator` to observe and manage the query
  job's lifecycle.
- Worker-side task implementations that remain separate from coordinator-side
  search planning and task-graph construction.
- A contract complete enough for the TDL implementation to be generated and
  reviewed from this RFC without treating the existing Python implementation
  as the source of truth.

## 4. Requirements

- Section 6 MUST give the final Spider-visible task names and Rust signatures
  and, for every input and output, its type, producer, validation/defaulting,
  exact use, side effect, and consumer.
- Every CLP-specific serialized type in a task signature MUST use Serde and
  live in or be re-exported from `clp_rust_utils::task_io::search`.
- The annotated task wrappers and implementations MUST live under
  `components/clp-tdl-package/src/task/search/` and be registered in the
  package task list in `components/clp-tdl-package/src/lib.rs`.
- The coordinator-side graph builder MUST use the same task names, type
  descriptors, argument order, and MessagePack representation specified here.
- Native search hits MUST go directly to MongoDB; only control-plane success or
  failure returns through Spider.

## 5. Constraints and assumptions

- The coordinator and TDL worker compile separately. The coordinator references
  task names and wire descriptors; it does not import or call the TDL functions
  as ordinary Rust functions.
- Component ownership is fixed by Section 1: the coordinator plans archives,
  the submitter constructs the graph, Spider schedules it, and the TDL package
  executes individual nodes.
- Deployment-wide configuration and credentials belong to worker configuration
  or secrets, not repeated task inputs.
- Persist a MySQL status or supporting column only when an external consumer
  needs it or it resolves a real restart/fault-tolerance ambiguity. Keep
  reconstructible execution phases in memory to avoid unnecessary MySQL
  updates.

## 6. Proposed design

The MVP defines three Spider-visible task functions:

```text
search::clo_search_to_results_cache
search::clp_s_search_to_results_cache
search::commit
```

`SpiderClient` attaches execution policy to the graph descriptors; execution
policy is not a TDL argument. The initial MVP policies are:

| Task | `max_num_instances` | `max_num_retry` | Soft / hard timeout | Rationale |
| --- | ---: | ---: | ---: | --- |
| `search::clo_search_to_results_cache` | 1 | 0 | 600 s / 1,200 s | Preserves the legacy search time limits. Automatic retry is disabled because another invocation can insert duplicate MongoDB documents with new `_id` values. |
| `search::clp_s_search_to_results_cache` | 1 | 0 | 600 s / 1,200 s | Same result-writing and retry semantics as the CLO task. |
| `search::commit` | 1 | 1 | 45 s / 60 s | Matches the compression commit policy. One retry is safe because the MySQL transaction is idempotent for an already-`SUCCEEDED` row. |

The timeout and retry values MUST be coordinator configuration rendered into
the deployment configuration rather than constants in the TDL functions.
`max_num_instances = 1` applies to each graph node; different archive nodes may
still run concurrently subject to the Spider resource group.

The archive-search functions return no value payload. Their search results are
written directly to MongoDB by the native binary. Returning `Ok(())` tells
Spider only that the native process completed successfully; returning
`Err(TdlError)` fails that graph node. `search::commit` also returns no value
payload. It publishes successful job completion through a transactional MySQL
update.

**Archive and dataset preprocessing.** This happens before any TDL function is
invoked:

1. `SearchCoordinator` deserializes and validates `SearchJobConfig` and selects
   the configured storage engine.
2. For the CLP engine, the job has no dataset list. The coordinator queries the
   non-dataset archive table and produces one `CloSearchTaskInput` per matching
   archive.
3. For the CLP-S engine, the coordinator resolves a missing dataset selection
   to `default`, deduplicates and validates the selected datasets, and queries
   their archive-metadata tables. When more than one dataset is selected, it
   combines the per-dataset `SELECT` statements with `UNION ALL`, includes the
   dataset name with every selected row, and globally orders the rows by
   `end_timestamp DESC`.
4. The coordinator applies the query time range and archive-retention cutoff
   while selecting archives. The resulting in-memory mapping has one
   `(dataset, archive_id)` entry per CLP-S archive; the CLP mapping contains only
   `archive_id`.
5. `SearchCoordinator` gives the prepared inputs to `QueryJobHandle`, which
   asks `QueryJobSubmitter` to submit them. The `SpiderClient` implementation
   creates one graph node per input and serializes that input as the node's
   MessagePack payload. The vector itself is not sent to a TDL function.
6. All archive nodes for the query job are registered in one Spider graph; the
   coordinator does not divide them into sequential dispatch batches. A graph
   uses either the CLO task or the CLP-S task, never both. A CLP-S graph may
   contain archive tasks for different datasets.
7. The submitter installs `search::commit` as the graph's termination task, so
   Spider invokes it only after every archive-search node succeeds.

For example, a CLP-S query over two datasets becomes:

```text
SearchCoordinator output
  [(dataset-a, archive-1),
   (dataset-a, archive-2),
   (dataset-b, archive-3)]

Spider graph
  clp_s_search(dataset-a, archive-1) --+
  clp_s_search(dataset-a, archive-2) --+--> search::commit
  clp_s_search(dataset-b, archive-3) --+
```

**Shared task input types.** The signatures in Sections 6.1 and 6.2 use the
following MessagePack-serialized types from
`clp_rust_utils::task_io::search`:

```rust
use std::num::NonZeroU32;

use serde::Deserialize;
use serde::Serialize;

pub type QueryJobId = i32;

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct SearchQuery {
    pub query_string: String,
    pub max_num_results: NonZeroU32,
    pub begin_timestamp: Option<i64>,
    pub end_timestamp: Option<i64>,
    pub ignore_case: bool,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct CloSearchTaskInput {
    pub query_job_id: QueryJobId,
    pub archive_id: String,
    pub query: SearchQuery,
    pub path_filter: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ClpSSearchTaskInput {
    pub query_job_id: QueryJobId,
    pub dataset: String,
    pub archive_id: String,
    pub query: SearchQuery,
}
```

`QueryJobId` mirrors the signed MySQL `INT` type of `query_jobs.id`.
`max_num_results` is non-zero because both native result-cache handlers reject
zero; the coordinator resolves the API's zero-as-default convention before it
constructs these inputs. Timestamps are Unix epoch milliseconds.

The relationship to the existing compression wire types is:

| Search type | Role | Compression-side analogue |
| --- | --- | --- |
| `QueryJobId` | Identifies the durable CLP query job and its MongoDB collection. | `CompressionJobId` identifies the durable compression job. |
| `SearchQuery` | Job-wide native search options copied into every archive-task payload. | `ClpSCompressionOption` contains job-wide native compression options copied into every compression-task payload. |
| `CloSearchTaskInput` | Complete input for one CLO archive-search invocation. | No single direct analogue; it combines the job identity, native options, and one archive identity. |
| `ClpSSearchTaskInput` | Complete input for one clp-s archive-search invocation, including its single dataset/archive pair. | Together, the compression task's `ClpSCompressionOption`, `dataset`, and `S3InputSource` arguments form the corresponding complete per-task input. |

Search intentionally has no `SearchTaskOutput` equivalent to
`CompressionTaskOutput`. Compression must return newly created archive
metadata for `compression::commit` to publish. Search hits are already written
to MongoDB, and `search::commit` needs no archive-task values.

### 6.1 `search::clo_search_to_results_cache`

#### 6.1.1 Signature

```rust
#[task(name = "search::clo_search_to_results_cache")]
pub(crate) fn clo_search_to_results_cache_task(
    ctx: TaskContext,
    input: CloSearchTaskInput,
) -> Result<(), TdlError>;
```

The task executes exactly one CLO search against exactly one filesystem CLP
archive. It MUST fail if the worker's archive-output storage is S3 because the
legacy CLO search path does not support S3 archives.

#### 6.1.2 Inputs and exact uses

| Input | Producer | Exact use |
| --- | --- | --- |
| `ctx` | Spider | Supplies Spider job, task, and task-instance identities for tracing and error context. `ctx.job_id` is the Spider job ID and MUST NOT be used as the MongoDB collection name. |
| `input.query_job_id` | `SearchCoordinator`, copied into every archive input for the query job | Converted to its decimal string and passed as `results-cache --collection <query_job_id>`. This preserves the existing per-query result collection and numeric collection-name contract. |
| `input.archive_id` | `SearchCoordinator`, from the selected CLP archive-metadata row | Appended to the configured filesystem archive root to form the positional archive path passed to CLO. It also identifies the archive in task logs. |
| `input.query.query_string` | Query-job configuration | Passed as CLO's positional search query without reinterpretation by the TDL task. |
| `input.query.max_num_results` | Query-job configuration after zero-default normalization | Passed as `results-cache --max-num-results <n>`. The limit applies to this archive invocation, not globally across the query job. |
| `input.query.begin_timestamp` | Query-job configuration | When present, passed as `--tge <milliseconds>`; omitted otherwise. |
| `input.query.end_timestamp` | Query-job configuration | When present, passed as `--tle <milliseconds>`; omitted otherwise. |
| `input.query.ignore_case` | Query-job configuration | Adds `--ignore-case` when true; adds no argument when false. |
| `input.path_filter` | Query-job configuration | When present, passed as `--file-path <filter>` before the query string; omitted otherwise. This option exists only on the CLO task. |

The results-cache URI and archive root are deployment configuration, not task
inputs. The task obtains `CLP_HOME`, `archive_output`, and `results_cache` from
the TDL worker's process-global configuration. `results_cache` therefore must
be added to `SpiderTaskExecutorConfig`. The task constructs the URI from its
configured host, port, and database name.

The resulting command is:

```text
<CLP_HOME>/bin/clo s <archive-root>/<archive-id>
    [--file-path <path-filter>]
    <query-string>
    [--tge <begin-ms>]
    [--tle <end-ms>]
    [--ignore-case]
    results-cache
    --uri <results-cache-uri>
    --collection <query-job-id>
    --max-num-results <n>
```

The implementation MUST construct the argument vector without a shell, wait
for the child process, and drain its standard streams. Exit code zero returns
`Ok(())`; a configuration error, spawn/wait error, or non-zero exit returns
`TdlError::ExecutionError`. Zero matching log events is successful.

#### 6.1.3 Outputs and their consumers

**Returned task output:** `()`. No search hits, count, duration, archive ID, or
timestamp range is returned through Spider. `search::commit` depends on
Spider's prerequisite-success guarantee and does not consume an output value
from this task.

**Persistent output:** CLO writes one MongoDB document per retained match into
collection `<query_job_id>`. MongoDB supplies `_id`; CLO supplies
`orig_file_id`, `orig_file_path`, `log_event_ix`, `timestamp`, and `message`.
The webui, API server, and other results-cache readers consume these documents.
Neither `QueryJobHandle`, `SearchCoordinator`, nor `search::commit` reads or
rewrites them.

**Coordinator-visible outcome:** Spider exposes node failure through the graph
state. On success, no task value is returned to `QueryJobHandle`; the handle
observes the terminal graph outcome after `search::commit` runs.

### 6.2 `search::clp_s_search_to_results_cache`

#### 6.2.1 Signature

```rust
#[task(name = "search::clp_s_search_to_results_cache")]
pub(crate) fn clp_s_search_to_results_cache_task(
    ctx: TaskContext,
    input: ClpSSearchTaskInput,
) -> Result<(), TdlError>;
```

The task executes exactly one clp-s search against exactly one archive in one
resolved dataset. Different invocations in the same graph may use different
datasets.

#### 6.2.2 Inputs and exact uses

| Input | Producer | Exact use |
| --- | --- | --- |
| `ctx` | Spider | Supplies Spider job, task, and task-instance identities for tracing and error context. `ctx.job_id` MUST NOT replace the CLP query-job ID. |
| `input.query_job_id` | `SearchCoordinator`, copied into every archive input for the query job | Converted to its decimal string and passed as `results-cache --collection <query_job_id>`. Every archive task in the graph therefore writes to the same per-query MongoDB collection. |
| `input.dataset` | `SearchCoordinator`, from the archive-selection row after default resolution and validation | For filesystem storage, selects `<archive-root>/<dataset>` and is also passed as `results-cache --dataset <dataset>`. For S3 storage, forms the object key `<key-prefix><dataset>/<archive-id>` and is also passed through `--dataset` so each MongoDB result records its dataset. |
| `input.archive_id` | `SearchCoordinator`, from the selected dataset's archive-metadata row | For filesystem storage, passed as `--archive-id <archive-id>`. For S3 storage, forms the final component of the archive object key. It also identifies the archive in task logs and in clp-s result documents. |
| `input.query.query_string` | Query-job configuration | Passed as clp-s's positional query without reinterpretation by the TDL task. |
| `input.query.max_num_results` | Query-job configuration after zero-default normalization | Passed as `results-cache --max-num-results <n>`. The limit applies independently to this archive invocation. |
| `input.query.begin_timestamp` | Query-job configuration | When present, passed as `--tge <milliseconds>`; omitted otherwise. |
| `input.query.end_timestamp` | Query-job configuration | When present, passed as `--tle <milliseconds>`; omitted otherwise. |
| `input.query.ignore_case` | Query-job configuration | Adds `--ignore-case` when true; adds no argument when false. |

As in Section 6.1, the task obtains `CLP_HOME`, `archive_output`, and
`results_cache` from process-global worker configuration. For S3 archive
storage it also obtains the endpoint, region, bucket, key prefix, and AWS
authentication configuration from `archive_output`, resolves the credentials,
and injects them into the clp-s child environment. These deployment-wide
values are not serialized into every task.

For filesystem archives, the resulting command is:

```text
<CLP_HOME>/bin/clp-s s <archive-root>/<dataset>
    --archive-id <archive-id>
    <query-string>
    [--tge <begin-ms>]
    [--tle <end-ms>]
    [--ignore-case]
    results-cache
    --uri <results-cache-uri>
    --collection <query-job-id>
    --max-num-results <n>
    --dataset <dataset>
```

For S3 archives, the archive locator is instead:

```text
<CLP_HOME>/bin/clp-s s <s3-url-for-key-prefix/dataset/archive-id> --auth s3
```

The query and result-cache arguments following that locator are unchanged. The
implementation MUST construct the argument vector without a shell, wait for
the child process, and drain its standard streams. Exit code zero returns
`Ok(())`; a configuration, credential, URL-construction, spawn/wait, or
non-zero-exit failure returns `TdlError::ExecutionError`. Zero matching log
events is successful.

#### 6.2.3 Outputs and their consumers

**Returned task output:** `()`. The task returns no hits or result statistics
through Spider. `search::commit` consumes no archive-task value.

**Persistent output:** clp-s writes one MongoDB document per retained match
into collection `<query_job_id>`. MongoDB supplies `_id`; clp-s supplies
`orig_file_path`, `message`, `timestamp`, `archive_id`, `log_event_ix`, and
`dataset`. The current clp-s results-cache handler writes an empty
`orig_file_path`. Results-cache readers use `dataset` and `archive_id` to
identify the result source. Neither `QueryJobHandle`, `SearchCoordinator`, nor
`search::commit` reads or rewrites these documents.

**Coordinator-visible outcome:** as with CLO, only Spider's graph state crosses
back to the job handle. Search hits remain in MongoDB.

### 6.3 `search::commit`

#### 6.3.1 Signature

```rust
#[task(name = "search::commit")]
pub(crate) fn commit_task(ctx: TaskContext) -> Result<(), TdlError>;
```

The submitter registers this function as the graph's termination task. It has
no serialized task inputs and does not call `get_task_graph_outputs()`, because
the archive-search tasks deliberately return no value payload. Spider's
termination-task ordering is the proof that every archive-search task returned
successfully.

#### 6.3.2 Input and exact use

| Input | Producer | Exact use |
| --- | --- | --- |
| `ctx` | Spider | `ctx.job_id` is the Spider job ID persisted earlier in `query_jobs.spider_id`. The task uses it to reverse-look up and lock the corresponding CLP query-job row. It also supplies tracing context. |

The task obtains the MySQL host, port, and database name from
`SpiderTaskExecutorConfig.database` and reads the username and password from
`CLP_DB_USER` and `CLP_DB_PASS`. Database credentials are worker secrets and
MUST NOT be task inputs.

The task performs one transaction:

1. Select `id` and `status` from `query_jobs` by `spider_id` using
   `SELECT ... FOR UPDATE`.
2. Fail if no row exists.
3. Return `Ok(())` without another update when the row is already `SUCCEEDED`;
   this makes a retried commit idempotent.
4. Fail if the row is in any state other than `RUNNING` or `SUCCEEDED`.
5. CAS-update the `RUNNING` row to `SUCCEEDED` and set `duration` from the
   database clock using the elapsed time since `start_time`.
6. Require exactly one affected row and commit the transaction; otherwise roll
   it back and return `TdlError::ExecutionError`.

The task does not access MongoDB, inspect result documents, update
`query_tasks`, or distinguish CLO from CLP-S. It can therefore terminate either
archive-search graph and can commit a CLP-S graph containing multiple
datasets.

#### 6.3.3 Outputs and their consumers

**Returned task output:** `()`. Returning `Ok(())` makes the Spider graph
successful. A returned `TdlError` makes the graph fail.

**Persistent output:** the MySQL `query_jobs` row contains `status = SUCCEEDED`
and the completed duration. Existing webui, API, CLI, and MCP consumers observe
job completion through this row.

**Coordinator-visible outcome:** `QueryJobHandle` observes Spider's terminal
success and verifies that the MySQL row is already `SUCCEEDED`; it performs no
second success update. If an archive task or the commit task fails, Spider
reports graph failure and the coordinator records the query job's failure
without overwriting an already committed success.
