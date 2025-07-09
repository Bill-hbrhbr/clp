import shutil
import subprocess
from pathlib import Path
from typing import Callable

import pytest
from conftest import BaseTestParams
from utils import run_and_assert

pytestmark = pytest.mark.clp


@pytest.fixture(scope="module", params=["hive-24hrs"])
def name(request) -> str:
    yield request.param


@pytest.fixture(scope="module", params=["DESERIALIZE_ERRORS"])
def query(request) -> str:
    yield request.param


@pytest.fixture(scope="module")
def logs_dir(test_params: BaseTestParams, name: str) -> Path:
    yield test_params.uncompressed_logs_dir / name


@pytest.fixture(scope="module")
def grep_sorted_search_results_path(
    request, test_params: BaseTestParams, logs_dir: Path, name: str, query: str
) -> Path:
    label = f"grep-{name}-{query}"
    grep_sorted_search_results_path = test_params.test_output_dir / label
    if request.config.cache.get(label, False):
        return grep_sorted_search_results_path

    print(f"Running grep with query {query} in {logs_dir}.")
    cmd = ["grep", "--recursive", "--no-filename", query, str(logs_dir)]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, check=True)
    sorted_output = sorted(proc.stdout.decode().splitlines(keepends=True))
    with open(grep_sorted_search_results_path, "w") as f:
        for line in sorted_output:
            f.write(line)
    request.config.cache.set(label, True)
    return grep_sorted_search_results_path


@pytest.fixture(scope="module", autouse=True)
def compress(test_params: BaseTestParams, logs_dir: Path, run_clp_package) -> None:
    cmd = [str(test_params.clp_package_sbin_dir / "compress.sh"), str(logs_dir)]
    run_and_assert(cmd, stdout=subprocess.PIPE)


def test_search(
    test_params: BaseTestParams, logs_dir: Path, query: str, grep_sorted_search_results_path: Path
) -> None:
    cmd = [
        str(test_params.clp_package_sbin_dir / "search.sh"),
        "--raw",
        "--file-path",
        str(
            logs_dir
            / "logs"
            / "i-8fca0980"
            / "application_1427088391284_0097"
            / "container_1427088391284_0097_01_000007"
            / "syslog"
        ),
        query,
    ]
    proc = run_and_assert(cmd, stdout=subprocess.PIPE)
    expected_output = "2015-03-23 11:54:22,594 INFO [main] org.apache.hadoop.hive.ql.exec.MapOperator: DESERIALIZE_ERRORS:0\n"
    assert (
        expected_output == proc.stdout.decode()
    ), "clp-text search within specific file doesn't match expected output"

    # cmd = [str(package_sbin_dir / "search.sh"), "--raw", "--count", query]
    # proc = run_and_assert(cmd, stdout=subprocess.PIPE)
    # expected_output = "6945"
    # assert expected_output == proc.stdout.decode().strip(), "clp-text search returned wrong number of results"
    #
    # expected_timestamp_to_count = {
    #     1427086800000: 248,
    #     1427090400000: 1024,
    #     1427094000000: 1119,
    #     1427097600000: 1295,
    #     1427101200000: 1021,
    #     1427104800000: 1087,
    #     1427108400000: 1151,
    # }
    # cmd = [str(package_sbin_dir / "search.sh"), "--raw", "--count-by-time-bucket",
    #        str(60 * 60 * 1000), query]
    # proc = run_and_assert(cmd, stdout=subprocess.PIPE)
    # expected_output = "\n".join(
    #     [f"timestamp: {k} count: {v}" for k, v in expected_timestamp_to_count.items()])
    # assert expected_output == proc.stdout.decode().strip(), "clp-text search returned wrong count by time buckets"


@pytest.mark.parametrize(
    "query_transform, ignore_case, expect_results",
    [
        (lambda q: q, False, True),
        (lambda q: q.lower(), False, False),
        (lambda q: q[:-1] + q[-1].lower(), True, True),
    ],
)
def test_basic_search(
    test_params: BaseTestParams,
    query: str,
    query_transform: Callable[[str], str],
    ignore_case: bool,
    grep_sorted_search_results_path: Path,
    expect_results: bool,
) -> None:
    cmd = [str(test_params.clp_package_sbin_dir / "search.sh"), "--raw", query_transform(query)]
    if ignore_case:
        cmd.append("--ignore-case")
    proc = run_and_assert(cmd, stdout=subprocess.PIPE)

    if not expect_results:
        assert "" == proc.stdout.decode(), "clp-text search results don't match expected output"
    else:
        sorted_output = sorted(proc.stdout.decode().splitlines(keepends=True))
        clp_sorted_search_results_path = test_params.test_output_dir / "clp-text-search.txt"
        with open(clp_sorted_search_results_path, "w") as f:
            for line in sorted_output:
                f.write(line)

        cmd = [
            "diff",
            "--brief",
            str(grep_sorted_search_results_path),
            str(clp_sorted_search_results_path),
        ]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE)
        if 0 != proc.returncode:
            if 1 == proc.returncode:
                assert False, "clp-text search results don't match expected output"
            assert False, f"Command failed {' '.join(cmd)}"
