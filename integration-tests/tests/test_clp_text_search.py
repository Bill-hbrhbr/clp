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

@dataclass(frozen=True)
class TestParams(BaseTestParams):
    query: str
    grep_search_results_path: Path

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
    logs_dir = test_params.uncompressed_logs_dir / "hadoop-24hrs"
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

    try:
        proc = subprocess.run([str(test_params.clp_package_dir / "sbin" / "start-clp.sh")])
        assert proc.returncode == 0, "Failed to start CLP"

        cmd = [str(test_params.clp_package_dir / "sbin" / "compress.sh"), str(logs_dir)]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE)
        assert proc.returncode == 0, "Failed to compress logs"

        cmd = [str(test_params.clp_package_dir / "sbin" / "search.sh"), "--raw", query]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE)
        assert proc.returncode == 0, "Failed to run search"

        sorted_output = sorted(proc.stdout.decode().splitlines(keepends=True))
        clp_sorted_search_results_path = test_params.test_output_dir / "clp-text-search.txt"
        with open(clp_sorted_search_results_path, "w") as f:
            for line in sorted_output:
                f.write(line)

        cmd = ["diff", "--brief", str(grep_sorted_search_results_path), str(clp_sorted_search_results_path)]
        proc = subprocess.run(cmd)
        assert proc.returncode == 0, "clp-text search results don't match grep results"
    finally:
        subprocess.run([str(test_params.clp_package_dir / "sbin" / "stop-clp.sh")], check=True)

def _get_env_var(var_name: str) -> str:
    value = os.environ.get(var_name)
    if value is None:
        raise ValueError(f"Environment variable {var_name} is not set.")
    return value
