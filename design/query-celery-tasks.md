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

## Dispatch model (one task per archive)

The scheduler (`query_scheduler.py:get_task_group_for_job`) builds a Celery
**`group`** with **one task per archive** for a job, then dispatches it:

- `QueryJobType.SEARCH_OR_AGGREGATION` → one `search.s(...)` per archive.
- `QueryJobType.EXTRACT_JSON` / `QueryJobType.EXTRACT_IR` → one `extract_stream.s(...)` per archive.

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
   1. `aggregation_config` set → **`reducer`** (`--host --port --job-id`; plus `--count` and/or `--count-by-time <size>`).
   2. else `network_address` set → **`network`** (`--host --port`).
   3. else `write_to_file` → **`file`** (`--path <stream_output>/<job_id>/<archive_id>`).
   4. else → **`results-cache`** (`--uri <results_cache_uri> --collection <job_id> --max-num-results <n>`, optional `--dataset`).
4. Runs the subprocess via `run_query_task` (sets `QUERY_TASKS_TABLE_NAME` `RUNNING` → `SUCCEEDED`/`FAILED` + `duration`).
5. **S3 upload**: if `stream_output.storage` is S3 **and** `write_to_file` **and** succeeded, uploads the result file to `{job_id}/{archive_id}` (`upload_results_to_s3`); empty files are skipped.
6. Returns a `QueryTaskResult` dump (`status`, `task_id`, `duration`).

## Task: `extract_stream` (`extract_stream_task.extract_stream`)

Runs **IR or JSON stream extraction** for one archive (`EXTRACT_IR` / `EXTRACT_JSON`).

**Signature:** `extract_stream(job_id, task_id, query_job_type, job_config, archive_id, clp_metadata_db_conn_params, results_cache_uri, dataset=None)` — `query_job_type` selects IR vs JSON; `job_config` is a dict (`ExtractIrJobConfig` / `ExtractJsonJobConfig`).

**What it does:**

1. Loads worker config; detects S3 stream output (enables stream-stats printing).
2. Builds the extraction command by `query_job_type`:
   - **`EXTRACT_IR`** (CLP storage engine) → `clo i <archive> <file_split_id> <stream_output_dir> <results_cache_uri> <stream_collection_name>` (`--target-size`, `--print-ir-stats`). **S3 input not supported** for IR/CLP; requires `file_split_id`.
   - **`EXTRACT_JSON`** (CLP_S storage engine) → `clp-s x <archive_dir|s3_url> <stream_output_dir> --ordered --mongodb-uri <results_cache_uri> --mongodb-collection <stream_collection_name>` (`--archive-id`, `--auth s3`, `--target-ordered-chunk-size`, `--print-ordered-chunk-stats`). Stream metadata is written to the results cache via `--mongodb-collection stream_collection_name`.
3. Runs the subprocess via `run_query_task`.
4. **S3 upload**: if S3 stream output **and** succeeded, parses each stdout line as JSON stream-stats (`{"path": ...}`), `s3_put`s each stream file, and unlinks it locally; any upload error marks the task `FAILED`.
5. Returns a `QueryTaskResult` dump.

## Shared runtime: `run_query_task` (`utils.py`)

Both tasks shell out through `run_query_task`, which is the actual per-archive execution + `QUERY_TASKS_TABLE_NAME` bookkeeping:

- Writes the task row to `RUNNING` with `start_time` (`update_query_task_metadata`).
- `subprocess.Popen`s the clp command in its **own process group** (`preexec_fn=os.setpgrp`), capturing stdout and teeing stderr to a per-task log file (`<clp_logs_dir>/<job_id>/<task_id>-clo.log`).
- Registers a **SIGTERM handler** that kills the child process group (`os.killpg`) — this is how task cancellation terminates the in-flight clp process.
- `communicate()`s for stdout, then maps `returncode` → `SUCCEEDED` (0) / `FAILED` (nonzero); writes `duration`.
- Returns `(QueryTaskResult, stdout_str)`.

Helpers:

- `report_task_failure` — writes `FAILED` (duration 0) for early-failure paths (bad config, command-build failure) and returns a `QueryTaskResult`.
- `update_query_task_metadata` — the `UPDATE {QUERY_TASKS_TABLE_NAME} SET ... WHERE id = <task_id>` (built via f-string; note the `f'{k}="{v}"'` quoting).
- `get_query_hash` — SHA-256 of the query string, used only for log context.