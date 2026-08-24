# Query Celery Worker Tasks

A summary of the Celery worker tasks executed from
`components/job-orchestration/job_orchestration/executor/query/` — the per-archive
query workhorses launched by the Python query scheduler.

## Celery app and config

`celery.py` creates `Celery("query")` with config from `celeryconfig.py`:

- **Imports** (i.e. the registered task modules): `fs_search_task` and `extract_stream_task`.
- **Task routes**: both `fs_search_task.search` and `extract_stream_task.extract_stream` route to the `SchedulerType.QUERY` queue (`task_create_missing_queues = True`).
- **Broker / result backend**: from `BROKER_URL` / `RESULT_BACKEND` env vars.
- **Results**: `result_persistent = True`, `result_expires = 7200`, `task_track_started = True` (distinguishes started vs queued).
- **Time limits**: `task_soft_time_limit` / `task_time_limit` from `query_worker` config.
- **Serialization**: accepts json + pickle; results serialized as json (with a TODO to revisit).
- **Logging**: `after_setup_logger` / `after_setup_task_logger` signals install CLP's JSON formatter.

## Dispatch model (one task per archive, grouped in batches)

The scheduler builds Celery **`group`** objects containing **one task per
archive**:

- `QueryJobType.SEARCH_OR_AGGREGATION` → one `search.s(...)` per archive.
- `QueryJobType.EXTRACT_JSON` / `QueryJobType.EXTRACT_IR` → one `extract_stream.s(...)` per archive.

A search job's archives are not necessarily placed in one group. The scheduler
dispatches at most `query_scheduler.num_archives_to_search_per_sub_job` archives
at a time and, if the job should continue, creates another group after the
current one finishes. An extraction job resolves to exactly one archive and is
therefore dispatched as a one-task group.

Each task is given `job_id`, `archive_id`, a per-archive `task_id`, the job
config, the CLP metadata DB connection params, the results-cache URI, and an
optional `dataset`.

## Task: `search` (`fs_search_task.search`)

Runs one `SEARCH_OR_AGGREGATION` query against **one archive**.

**Signature:** `search(job_id, task_id, job_config_blob, archive_id, clp_metadata_db_conn_params, results_cache_uri, dataset=None)` — `job_config_blob` is msgpack; decoded into `SearchJobConfig`.

**What it does:**

1. Loads worker config (`CLP_CONFIG_PATH`), decodes `SearchJobConfig`.
2. Builds the clp search command for one archive:
   - **CLP storage engine** → `clo s <archive_dir>/<archive_id>` (no S3, no `write_to_file`).
   - **CLP_S storage engine** → `clp-s s <archive_dir|s3_url> --archive-id <archive_id>` (S3 input supported via `--auth s3`).
   - Common flags: the query string, `--tge`/`--tle` (begin/end timestamp), `--ignore-case`, `--file-path` (path filter); telemetry (`--enable-telemetry`) is sampled per `query_trace_sampling_probability`.
3. Selects the **output handler** by priority:
   1. `aggregation_config` set → **`reducer`** (`--host --port --job-id`; `--count` is added when `do_count_aggregation` is non-null, including when it is explicitly `False`, and `--count-by-time <size>` is added when configured).
   2. else `network_address` set → **`network`** (`--host --port`).
   3. else `write_to_file` → **`file`** (`--path <stream_output>/<job_id>/<archive_id>`).
   4. else → **`results-cache`** (`--uri <results_cache_uri> --collection <job_id> --max-num-results <n>`, optional `--dataset`).
4. Runs the subprocess via `run_query_task` (sets `QUERY_TASKS_TABLE_NAME` `RUNNING` → `SUCCEEDED`/`FAILED` + `duration`).
5. **S3 upload**: if `stream_output.storage` is S3 **and** `write_to_file` **and** the subprocess succeeded, uploads the result file to `{job_id}/{archive_id}` relative to the configured S3 key prefix (`upload_results_to_s3`); empty files are skipped. The local file is deleted whether the upload succeeds or fails.
6. Returns a `QueryTaskResult` dump (`status`, `task_id`, `duration`).

An S3 upload failure changes the returned `QueryTaskResult` to `FAILED`, so the
scheduler will fail the job. However, `run_query_task` has already persisted
`SUCCEEDED` to the SQL task row by this point, and the upload path does not
update that row again. The returned result and `QUERY_TASKS_TABLE_NAME` can
therefore disagree.

## Task: `extract_stream` (`extract_stream_task.extract_stream`)

Runs **IR or JSON stream extraction** for one archive (`EXTRACT_IR` / `EXTRACT_JSON`).

**Signature:** `extract_stream(job_id, task_id, query_job_type, job_config, archive_id, clp_metadata_db_conn_params, results_cache_uri, dataset=None)` — `job_config` is a dict (`ExtractIrJobConfig` / `ExtractJsonJobConfig`).

> **IR vs JSON is selected by the package storage engine, not by `query_job_type`.** `query_job_type` (`EXTRACT_IR` / `EXTRACT_JSON`) is received by the Celery task but used **only for log context** — it is not passed to `extract_stream_entry_point`. The command builder (`_make_command_and_env_vars`) branches on `worker_config.package.storage_engine`: `CLP` → IR extraction (`clo i` + `ExtractIrJobConfig`); `CLP_S` → JSON extraction (`clp-s x` + `ExtractJsonJobConfig`). The implicit assumption is that `EXTRACT_IR` jobs run against CLP-engine archives and `EXTRACT_JSON` against CLP_S-engine archives.

**What it does:**

1. Loads worker config; if `stream_output.storage` is S3, sets `enable_s3_upload = True` (this also enables `print_stream_stats`, see below). Validated configuration permits S3 stream output only with the CLP_S storage engine, so the S3-upload path is reachable for JSON extraction but not IR extraction.
2. Builds the extraction command by storage engine:
   - **CLP storage engine → IR** (`clo i`): `clo i <archive_dir>/<archive_id> <file_split_id> <stream_output_dir> <results_cache_uri> <stream_collection_name>`, plus `--target-size <n>` (from `ExtractIrJobConfig.target_uncompressed_size`) and `--print-ir-stats` (iff S3 upload enabled). **S3 input not supported** for IR/CLP; `file_split_id` is required (missing → command build fails).
   - **CLP_S storage engine → JSON** (`clp-s x`): `clp-s x <archive_dir|s3_url> <stream_output_dir>` then `--archive-id <archive_id>` (filesystem) or `--auth s3` (S3 input, with AWS credential env vars), then **always** `--ordered --mongodb-uri <results_cache_uri> --mongodb-collection <stream_collection_name>`, plus `--target-ordered-chunk-size <n>` (from `ExtractJsonJobConfig.target_chunk_size`) and `--print-ordered-chunk-stats` (iff S3 upload enabled). Stream metadata is written to the results cache via `--mongodb-collection stream_collection_name`.
3. Runs the subprocess via `run_query_task` (sets `QUERY_TASKS_TABLE_NAME` `RUNNING` → `SUCCEEDED`/`FAILED` + `duration`); returns `(QueryTaskResult, stdout_str)`.
4. **S3 upload** (iff `enable_s3_upload` **and** the subprocess succeeded): the clp process emitted one JSON stream-stats object per line on stdout (`{"path": "<local stream file>", ...}`) — that's why `--print-*-stats` was passed. The task parses each stdout line as JSON, reads its `path`, `s3_put`s that local stream file to S3 under its basename relative to the configured S3 key prefix, and `unlink`s the local copy. On the **first** parse or upload error it sets `upload_error = True`. It continues parsing later lines but makes no more upload attempts; every subsequently reported valid path is still unlinked. A malformed line or a line without `path` cannot identify a local file to remove. If `upload_error` is set at the end, the returned task status is flipped to `FAILED`.
5. Returns a `QueryTaskResult` dump.

The `--print-ir-stats` / `--print-ordered-chunk-stats` flags and the S3-upload loop are coupled: the stats lines on stdout are the only signal the task has of which stream files were produced and where they live, so the upload path is gated on S3 stream output and the stats flags are set exactly when the upload will run.

As with search-result uploads, an extraction upload failure is reflected in the
returned result and therefore the job status, but not in the SQL task row,
which was already marked `SUCCEEDED`. Local files are removed even when their
upload fails or when uploads have been suppressed by an earlier error.

## Shared runtime: `run_query_task` (`utils.py`)

Both tasks shell out through `run_query_task`, which is the actual per-archive execution + `QUERY_TASKS_TABLE_NAME` bookkeeping:

- Writes the task row to `RUNNING` with `start_time` (`update_query_task_metadata`).
- `subprocess.Popen`s the clp command in its **own process group** (`preexec_fn=os.setpgrp`), capturing stdout and redirecting stderr to a per-task log file (`<clp_logs_dir>/<job_id>/<task_id>-clo.log`). After the subprocess exits, the file's contents are replayed through the task logger.
- Registers a **SIGTERM handler** that kills the child process group (`os.killpg`) — this is how Celery revocation with termination cancels the in-flight clp process. The scheduler separately changes `PENDING` and `RUNNING` SQL task rows to `CANCELLED` when cancelling a job.
- `communicate()`s for stdout, then maps `returncode` → `SUCCEEDED` (0) / `FAILED` (nonzero); writes `duration`.
- Returns `(QueryTaskResult, stdout_str)`.

The task wrappers log and re-raise `SoftTimeLimitExceeded`, but that exception
path does not call `report_task_failure` or explicitly terminate the subprocess
group. It should not be conflated with the SIGTERM cancellation path above.

Helpers:

- `report_task_failure` — writes `FAILED` (duration 0) when worker-config loading returns `None` or command construction explicitly returns no command, and returns a `QueryTaskResult`. Exceptions such as malformed msgpack, Pydantic validation failures, missing environment variables, or subprocess-launch failures bypass this helper and are re-raised by the Celery task wrapper. Depending on where such an exception occurs, the SQL task row can remain `PENDING` or `RUNNING`; the scheduler fails the overall job when it observes the exceptional Celery result.
- `update_query_task_metadata` — the `UPDATE {QUERY_TASKS_TABLE_NAME} SET ... WHERE id = <task_id>` (built via f-string; note the `f'{k}="{v}"'` quoting).
- `get_query_hash` — SHA-256 of the query string, used only for log context.
