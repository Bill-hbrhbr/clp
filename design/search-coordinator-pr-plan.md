# Search coordinator — MVP PR implementation plan

This plan splits the search-coordinator MVP into reviewable PRs. It uses the
`search-coordinator/init` branch as a prototype, but does not assume that the
prototype's commits are the right merge boundaries. Each PR below should leave
the repository buildable and include tests for the behavior it introduces.

The MVP handles plain `SEARCH_OR_AGGREGATION` jobs whose
`aggregation_config` is absent. Aggregation, cancellation, and extraction remain
on the legacy path until their later phases.

## Starting point: `search-coordinator/init`

The prototype already contains:

- A `search-coordinator` workspace crate, binary entry point, configuration
  loading, database connection setup, signal handling, and image packaging.
- Matching Python and Rust `SearchCoordinator` configuration models and a
  validator requiring Spider when the coordinator is enabled.
- Query-job schema additions (`status_msg`, `update_time`, `spider_id`, and
  `dispatch_time`) and typed Rust query-job IDs, statuses, types, and search
  configuration.
- A coordinator poll loop, two-phase pending-job fetch, deferred
  `dispatch_time` updates, a `Semaphore(max_concurrent_jobs)`, resource-group
  setup, and initial recovery of running Spider jobs.
- `QueryJobHandle` lifecycle and SQL helpers.
- A `QueryJobSubmitter` abstraction and an implemented
  `run_query_job_to_completion` path that idempotently starts a Spider job and
  polls it with exponential backoff.
- Helm defaults and ConfigMap rendering for `search_coordinator`.

These pieces are scaffolding, not a working search path. The important known
gaps are:

- `fetch_new_job_rows` selects only `id`; it cannot categorize or plan a job.
- `QueryJobHandle::new` does not read the job row or deserialize its plain
  MessagePack `job_config`; `dataset` is hardcoded to `None`.
- `SpiderClient::submit_query_job` is still `todo!()`.
- `num_tasks` is hardcoded to one. No per-archive `query_tasks` rows are created,
  and `num_tasks_completed` and task durations/statuses are not maintained.
- No search termination/commit task exists. The handle copies the compression
  coordinator's assumption that a successful Spider graph has already committed
  `SUCCEEDED` transactionally, so today a hypothetical successful search graph
  would leave the SQL query job `RUNNING`.
- Terminal status writes do not use a previous-status compare-and-set guard, so
  they can overwrite a concurrent transition.
- `schedule_new_jobs` builds its `dispatched_job_ids` list before handle
  construction. A rejected/unsupported row can therefore receive a
  `dispatch_time` even though this coordinator did not dispatch it.
- Recovery does not consume semaphore permits, and spawned job handles are
  detached rather than tracked for orderly shutdown.
- The schema edit affects newly created tables only. `CREATE TABLE IF NOT
  EXISTS` does not add the new columns to an existing deployment.
- The crate has no coordinator, job-handle, or submitter tests.
- Deployment is incomplete: Helm renders the configuration but has no
  search-coordinator Deployment or scheduling/resource settings; Compose and
  the package config template do not expose or start the service.

## Phase 1: land the reusable coordinator skeleton

### PR 1.1 — Query schema and shared Rust types

Add the query-job columns and indices required for Spider coordination, plus the
typed `QueryJobId`, `QueryJobStatus`, `QueryJobType`, `SearchJobConfig`, and
`AggregationConfig` definitions.

This PR must explicitly choose and test an upgrade strategy:

- Add an idempotent migration for existing `query_jobs` tables; or
- State that the MVP supports fresh databases only and add a follow-up issue
  before any upgrade-capable release.

Updating only the `CREATE TABLE IF NOT EXISTS` body is not a migration.

### PR 1.2 — Crate, configuration, and binary scaffold

Add the workspace crate and binary, Python/Rust `SearchCoordinator` config
models, Spider-required validation, database credential loading, and
signal-driven startup/shutdown. Include the binary in the package image.

Test configuration defaults, Python/Rust field parity, invalid zero values, and
the requirement that `search_coordinator` cannot be enabled without `spider`.

Keep task-execution-policy wiring provisional until PR 3 defines the task graph.
The prototype currently exposes `commit_task_*` fields but incorrectly builds a
`search_task_execution_policy` from the commit timeouts, while
`commit_task_max_retry` is unused. Following the compression pattern, the
coordinator should carry `search_task_max_retry` plus a distinct
`commit_task_execution_policy`; search-task timeouts should be separately named
or derived per archive.

### PR 1.3 — Job handle and SQL lifecycle helpers

Introduce `QueryJobHandle` and the database helpers:

- `persist_spider_job_id` — transition an owned job to `RUNNING`, set
  `start_time`, record the real task count, and fill `dispatch_time` only if it
  is absent.
- `get_job_status`, `update_job_status`, `mark_job_failed`, and
  `report_failure`.

Status transitions should be typed and guarded by their expected previous
status. Tests should cover successful transitions, lost compare-and-set races,
oversized task counts, and best-effort failure reporting against a test DB.

Do not mark a successful job from the handle: PR 3's commit task owns the
transactional `RUNNING` → `SUCCEEDED` transition. As in the compression handle,
the failed/cancelled Spider branches must first check whether the commit task
already recorded `SUCCEEDED` and avoid overwriting it if Spider failed only
while reporting the commit result.

### PR 1.4 — Submitter abstraction and Spider completion polling

Add `QueryJobSubmitter`, `QueryJobOutcome`, and
`run_query_job_to_completion`. Keep task-graph construction out of this PR.

Unit-test:

- Starting a new job.
- Treating `InvalidJobState` as an idempotent already-started case.
- Exponential backoff and its maximum.
- Successful, failed, and cancelled terminal states.
- Failure to fetch Spider state or the Spider error message.

### PR 1.5 — Coordinator core and entry point

Wire `SearchCoordinator::{new, run}`, resource-group setup, two-phase fetching,
semaphore permit ownership, deferred dispatch marking, handle spawning, and the
binary entry point.

The prototype behavior needs two corrections in this PR:

- Add a job to `dispatched_job_ids` only after this coordinator has accepted it
  and acquired permission to dispatch it.
- Track spawned handles so graceful shutdown has defined behavior instead of
  dropping detached tasks when the Tokio runtime exits.

Use an injected/mock submitter and a test database to exercise fetch limits,
first-fetch recovery rows, dispatch marking, permit release, rejected jobs, and
shutdown. End-to-end execution remains blocked on categorization and the real
Spider task graph.

**Phase 1 merge order:** `1.1` and `1.2` can begin in parallel;
`1.3` depends on `1.1`; `1.4` is independent; `1.5` combines them.

## Phase 2: build the plain-search data path

### PR 2 — Job categorization and archive planning

Widen the pending-row projection to include `type`, `job_config`, and
`creation_time`. Decode query configs as **plain MessagePack** and categorize
them before claiming the row:

- Accept `SEARCH_OR_AGGREGATION` with no `aggregation_config` for the MVP.
- Leave aggregation, `EXTRACT_IR`, and `EXTRACT_JSON` for the legacy scheduler.
- Mark malformed configs and invalid supported jobs `FAILED` with a useful
  status message.

Port archive selection, dataset handling, time-range filtering, retention
cutoff, newest-first ordering, and archive batching. Produce an explicit
per-archive task plan and insert the corresponding `query_tasks` rows so the
real task count and task IDs are known before Spider submission.

The coordinator and legacy scheduler must have non-overlapping ownership rules.
Unsupported rows must not be marked dispatched by the coordinator, while
accepted plain searches must not also be consumed by the legacy scheduler.
Cover mixed job types and malformed configs in tests.

### PR 3 — `clp-tdl-package` per-archive search task

Follow the existing compression-task organization rather than introducing a
second package/runtime pattern:

- Add serializable search protocol types under
  `clp-rust-utils::task_io::search`, including the per-archive input/options and
  a `SearchTaskOutput` carrying the query-task identity and completion metadata
  needed by the commit task.
- Add `task/search/{mod.rs,search.rs,commit.rs}`. Keep the `#[task]` wrappers in
  `mod.rs` thin and put testable implementations in `search.rs` and `commit.rs`.
- Register the search and search-commit functions in `clp-tdl-package/src/lib.rs`
  and document them in the package README.
- Reuse `common.rs` for the process-global Tokio runtime, executor config,
  `CLP_HOME`, and JSON stderr tracing.

The map task runs one clp-s archive search and uses the MVP results-cache output
path. It must build arguments without a shell, drain stderr while consuming any
stdout protocol, terminate/reap clp-s on protocol or callback errors, and clean
temporary files. It updates its `query_tasks` row to `RUNNING` and ensures a
failed invocation does not strand that row in `RUNNING`.

Add a Spider termination task analogous to `compression::commit`. It reads and
deserializes all map-task outputs through `TaskContext::get_task_graph_outputs`,
looks up the CLP query job by `spider_id`, locks it, and in one transaction:

- Treats an already-`SUCCEEDED` job as an idempotent no-op.
- Refuses to commit a job not in `RUNNING`.
- Finalizes the per-archive `query_tasks` rows.
- CAS-transitions the query job from `RUNNING` to `SUCCEEDED`.
- Records `num_tasks_completed` and a DB-clock-derived duration.

Define error behavior for an empty output set and mismatched/duplicate task IDs.
Model tests on the existing compression TDL tests: argument construction,
stdout parsing, malformed output, subprocess failure, cleanup, idempotent
commit, and transaction rollback.

Decide whether archive batching is represented by one Spider graph or by
repeated submissions from the job handle; do not leave this as an implicit
coordinator/TDL responsibility split.

Finalize execution policies here: use `search_task_max_retry` for map tasks and
apply `commit_task_max_retry` plus the commit soft/hard timeouts only to the
termination task. Add separately named search-task timeouts or document a
per-archive derivation; do not reuse commit timeouts for map tasks.

### PR 4 — Implement `submit_query_job`

Replace the Spider submitter's `todo!()` with task-graph construction and job
registration. Mirror `submit_s3_compression_job`: use constants kept in sync
with the TDL package names, declare typed inputs/outputs, serialize value inputs
with MessagePack, insert one map task per planned archive, and attach the search
commit task as the graph's termination task. Feed it the per-archive plan from
PR 2 and return the registered Spider job ID.

Persist the Spider ID and transition the query job to `RUNNING` only after
registration succeeds. Define retry/idempotency behavior for the failure window
between Spider registration and SQL persistence so a retry cannot silently
create duplicate work.

### PR 5 — End-to-end MVP lifecycle

Complete and test the plain-search path:

```text
pending SQL job
  → categorize and enumerate archives
  → insert per-archive SQL task rows
  → register and start the Spider map + termination-task graph
  → clp-s workers write results to MongoDB collection <job_id>
  → the termination task finalizes task rows and atomically commits SUCCEEDED
  → the handle records FAILED/KILLED only when no successful commit won the race
```

Port max-results short-circuiting and archive-batch continuation if they remain
part of the MVP contract. Exercise zero-archive searches, partial task failure,
Spider failure/cancellation, SQL-write failure, and duplicate/retried submission
in integration tests.

Keep the MVP scope explicit: the results-cache-backed plain-search path is
required. File, network, aggregation, and extraction output paths should either
be supported deliberately or remain owned by the legacy scheduler; they must
not be accidentally accepted and then mishandled.

### PR 6 — Package and deployment wiring *(later step)*

Make the working coordinator deployable without changing the chart version on
the MVP branch.

- Keep the search-coordinator defaults in
  `tools/deployment/package-helm/values.yaml`.
- Render the complete `search_coordinator` block from
  `tools/deployment/package-helm/templates/configmap.yaml` when Spider is
  enabled. The prototype already contains these two Helm changes.
- Add a Helm search-coordinator Deployment modeled on the compression
  coordinator, including database credentials, Spider/db readiness,
  `RUST_LOG`, termination grace, scheduling, and resource settings.
- Add `searchCoordinator` entries under Helm `scheduling` and `resources`, plus
  a Helm-only logging-level value if the Deployment reads one.
- Add the service to the Spider Compose deployment and the package controller,
  and expose the config in the package config template.
- Verify the release build produces the binary copied by the package Dockerfile.
- Define the cutover from the legacy query scheduler so two schedulers cannot
  race for the same plain-search rows.

Validate with `helm lint`, a rendered-config assertion for every coordinator
field, Compose config validation, and a container startup smoke test.

### PR 7 — Recovery and shutdown hardening

Turn the prototype recovery path into tested MVP behavior:

- Recover `RUNNING` rows with a persisted Spider ID without resubmitting them.
- Re-dispatch accepted `PENDING` rows with `dispatch_time` set but no Spider ID.
- Decide whether recovered running jobs count against `max_concurrent_jobs` and
  enforce the documented choice.
- Make result writes and finalization safe when a recovered task may have
  partially completed before the restart.
- Preserve a successful commit if Spider reports the graph failed or cancelled
  after the termination task committed but before its result reached Spider.
- Track in-flight handles and define graceful-stop versus forced-abort behavior
  within `termination_timeout_secs`.

## Dependency graph

```text
1.1 ── 1.3 ──┐
1.2 ──────────┼── 1.5 ── 2 ──┐
1.4 ──────────┘              ├── 4 ── 5 ── 6 ── 7
1.2 ── 3 ────────────────────┘
```

PR 3 can proceed in parallel with PR 2 once the per-archive task contract is
agreed; PR 4 is the bridge that requires both.

## Deferred phases

- **MVP+1 — cancellation:** poll `CANCELLING`, maintain per-job cancellation
  tokens, cancel Spider work, and transition query/task rows with guarded SQL
  updates.
- **MVP+2 — timeline aggregation:** retain clp-s per-archive bucketing but move
  the cross-archive reduction into the coordinator; do not port the reducer.
- **MVP+3 — extraction:** design Spider tasks and resource-group isolation for
  `EXTRACT_IR` and `EXTRACT_JSON`.
- **MVP+N — other aggregations:** depends on the future Spider API and remains
  outside this plan.
