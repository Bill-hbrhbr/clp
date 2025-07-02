import shutil
from dataclasses import dataclass
import os
from pathlib import Path
import pytest
import subprocess


@dataclass(frozen=True)
class BaseTestParams:
    clp_package_dir: Path
    test_output_dir: Path
    uncompressed_logs_dir: Path


@pytest.fixture
def get_base_test_params() -> BaseTestParams:
    return BaseTestParams(
        clp_package_dir=Path(_get_env_var("CLP_PACKAGE_DIR")),
        test_output_dir=Path(_get_env_var("TEST_OUTPUT_DIR")),
        uncompressed_logs_dir=Path(_get_env_var("UNCOMPRESSED_LOGS_DIR")),
    )


def test_search(get_base_test_params: BaseTestParams) -> None:
    test_params = get_base_test_params
    test_params.test_output_dir.mkdir(parents=True, exist_ok=True)

    query = "DESERIALIZE_ERRORS"
    logs_dir = test_params.uncompressed_logs_dir / "hive-24hrs"

    # TODO cache with pytest cache
    cmd = [
        "grep",
        "--recursive",
        "--no-filename",
        query,
        str(logs_dir)
    ]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, check=True)

    sorted_output = sorted(proc.stdout.decode().splitlines(keepends=True))
    grep_sorted_search_results_path = test_params.test_output_dir / "grep.txt"
    with open(grep_sorted_search_results_path, "w") as f:
        for line in sorted_output:
            f.write(line)

    _clean_package_data(test_params.clp_package_dir)

    try:
        package_sbin_dir = test_params.clp_package_dir / "sbin"

        cmd = [str(package_sbin_dir / "start-clp.sh")]
        _run_and_assert(cmd)

        cmd = [str(package_sbin_dir / "compress.sh"), str(logs_dir)]
        _run_and_assert(cmd, stdout=subprocess.PIPE)

        _test_basic_search(package_sbin_dir, query, False, grep_sorted_search_results_path,
                           test_params.test_output_dir)
        _test_basic_search(package_sbin_dir, query.lower(), False, None,
                           test_params.test_output_dir)
        _test_basic_search(package_sbin_dir, query[:-1] + query[-1].lower(), True,
                           grep_sorted_search_results_path, test_params.test_output_dir)

        cmd = [str(package_sbin_dir / "search.sh"), "--raw", "--file-path",
               str(logs_dir / "logs" / "i-8fca0980" / "application_1427088391284_0097" / "container_1427088391284_0097_01_000007" / "syslog"),
               query]
        proc = _run_and_assert(cmd, stdout=subprocess.PIPE)
        expected_output = "2015-03-23 11:54:22,594 INFO [main] org.apache.hadoop.hive.ql.exec.MapOperator: DESERIALIZE_ERRORS:0\n"
        assert expected_output == proc.stdout.decode(), "clp-text search within specific file doesn't match expected output"

        cmd = [str(package_sbin_dir / "search.sh"), "--raw", "--count", query]
        proc = _run_and_assert(cmd, stdout=subprocess.PIPE)
        expected_output = "6945"
        assert expected_output == proc.stdout.decode().strip(), "clp-text search returned wrong number of results"

        expected_timestamp_to_count = {
            1427086800000: 248,
            1427090400000: 1024,
            1427094000000: 1119,
            1427097600000: 1295,
            1427101200000: 1021,
            1427104800000: 1087,
            1427108400000: 1151,
        }
        cmd = [str(package_sbin_dir / "search.sh"), "--raw", "--count-by-time-bucket",
               str(60 * 60 * 1000), query]
        proc = _run_and_assert(cmd, stdout=subprocess.PIPE)
        expected_output = "\n".join(
            [f"timestamp: {k} count: {v}" for k, v in expected_timestamp_to_count.items()])
        assert expected_output == proc.stdout.decode().strip(), "clp-text search returned wrong count by time buckets"

        _clean_package_data(test_params.clp_package_dir)
    finally:
        subprocess.run([str(test_params.clp_package_dir / "sbin" / "stop-clp.sh")], check=True)


def _clean_package_data(clp_package_dir: Path):
    package_data_dir = clp_package_dir / "var" / "data"
    shutil.rmtree(package_data_dir, ignore_errors=True)
    package_logs_dir = clp_package_dir / "var" / "log"
    shutil.rmtree(package_logs_dir, ignore_errors=True)


def _get_env_var(var_name: str) -> str:
    value = os.environ.get(var_name)
    if value is None:
        raise ValueError(f"Environment variable {var_name} is not set.")
    return value


def _run_and_assert(cmd, **kwargs):
    proc = subprocess.run(cmd, **kwargs)
    assert proc.returncode == 0, f"Command failed: {' '.join(cmd)}"
    return proc


def _test_basic_search(package_sbin_dir: Path, query: str, ignore_case: bool,
                       expected_results_path: Path | None, test_output_dir: Path) -> None:
    cmd = [str(package_sbin_dir / "search.sh"), "--raw", query]
    if ignore_case:
        cmd.append("--ignore-case")
    proc = _run_and_assert(cmd, stdout=subprocess.PIPE)

    if expected_results_path is None:
        assert "" == proc.stdout.decode(), "clp-text search results don't match expected output"
    else:
        sorted_output = sorted(proc.stdout.decode().splitlines(keepends=True))
        clp_sorted_search_results_path = test_output_dir / "clp-text-search.txt"
        with open(clp_sorted_search_results_path, "w") as f:
            for line in sorted_output:
                f.write(line)

        cmd = ["diff", "--brief", str(expected_results_path),
               str(clp_sorted_search_results_path)]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE)
        if 0 != proc.returncode:
            if 1 == proc.returncode:
                assert False, "clp-text search results don't match expected output"
            assert False, f"Command failed {' '.join(cmd)}"
