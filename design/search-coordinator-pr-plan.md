# Search coordinator — MVP PR implementation plan

This plan breaks the MVP scope of the search-coordinator rewrite into reviewable PRs. It builds on the existing `search-coordinator/init` skeleton and lands behavior incrementally; each PR is independently testable.

## Starting point — `search-coordinator/init` branch

A skeleton already exists, mirroring the compression-coordinator: poll loop, two-phase fetch, `Semaphore(max_concurrent_jobs)` permit ownership, `QueryJobHandle` lifecycle, status-update helpers, `run_query_job_to_completion`, schema migration, config loading, and typed `QueryJobStatus`/`QueryJobType` enums. Known gaps to close:

- `fetch_new_job_rows` projects only `id` (no job-config columns for categorization).
- `QueryJobHandle::new` does no categorization — `dataset` is hardcoded `None`.
- `submit_query_job` is `todo!()` in the Spider submitter.

## PR 1.1 — Schema migration *(no behavior)*

Add `status_msg`, `update_time`, `spider_id`, and `dispatch_time` columns plus matching indices to `QUERY_JOBS_TABLE_NAME`, aligned with the `compression_jobs` schema. Ship the column constants/types. Pure DB migration; no coordinator behavior changes.

## PR 1.2 — Job handle + status-update DB helpers *(depends on 1.1)*

Introduce the `QueryJobHandle` struct and `new`, plus the DB helpers:

- `persist_spider_job_id` — transition to `Running`, set `start_time` + `num_tasks`, COALESCE `dispatch_time`.
- `update_job_status`, `mark_job_failed`, `get_job_status`, `report_failure`.

Unit-tested against a test DB, no Spider involvement.

## PR 1.3 — Submitter trait + `run_query_job_to_completion` *(depends on 1.1; independent of 1.2)*

Define the `SearchJobSubmitter` trait with `run_query_job_to_completion` (idempotent `start_job` + exponential-backoff polling until a terminal state). Leave `submit_query_job` as `todo!()`.

## PR 1.4 — Coordinator core: poll loop, two-phase fetch, dispatch, concurrency, entrypoint *(depends on 1.2, 1.3)*

Wire `SearchCoordinator::{new, run}` (`select!` on the shutdown `CancellationToken` + sleep cadence), the two-phase `fetch_new_job_rows`, `schedule_new_jobs` + `create_job_handle` (semaphore permit ownership, spawn), and `mark_jobs_dispatched`. Load config (`max_concurrent_jobs`, `job_polling_interval_millisecs`, `result_polling` backoff, `resource_group`). Add the `search_coordinator` binary entrypoint with signal-driven graceful shutdown via `CancellationToken` and a termination timeout.

The loop is real and testable; end-to-end remains blocked on categorization (PR 2) and `submit_query_job` (PR 4).

**Merge order for phase 1:** 1.1 → {1.2, 1.3 in parallel} → 1.4.

## PR 2 — Job categorization *(depends on 1.4)*

Deserialize the job config off the fetched row and categorize it (search vs. extraction; dataset extraction). Plugs into `create_job_handle` / `QueryJobHandle::new`, replacing the hardcoded `None`.

## PR 3 — clp-tdl-package search task *(parallel with PR 2)*

Add the TDL search task that runs the per-archive map. Open design point: whether archive enumeration belongs to a TDL task or to the coordinator.

## PR 4 — Bridge `submit_query_job` *(depends on 2, 3)*

Implement `submit_query_job` in the Spider submitter, bridging the coordinator to the Spider job.

## PR 5 — End-to-end MVP + results cache + job completion *(depends on 2, 3, 4)*

Complete the end-to-end plain-search path: dispatch → Spider job → results written to the results cache → job completion and DB status updates.

## PR 6 — Startup recovery *(depends on 5)*

Recover in-flight jobs across a coordinator restart, mirroring the compression-coordinator's recovery path.

## Dependency graph

```
1.1 ─┬─ 1.2 ─┐
      └─ 1.3 ─┴─ 1.4 ── 2 ──┐
                  3 ────────┤
                           └─ 4 ── 5 ── 6
```