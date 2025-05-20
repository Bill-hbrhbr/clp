from dataclasses import dataclass
import os
from pathlib import Path
import pytest
import subprocess

@dataclass(frozen=True)
class TestParams:
    query: str
    uncompressed_logs_dir: Path
    clp_package_dir: Path
    test_output_dir: Path
    grep_search_results_path: Path

@pytest.fixture
def get_test_params() -> TestParams:
    # Get query, logs directory, and package directory from environment variables
    test_params = TestParams(
        query=os.environ.get("QUERY"),
        uncompressed_logs_dir=Path(os.environ.get("UNCOMPRESSED_LOGS_DIR")),
        clp_package_dir=Path(os.environ.get("CLP_PACKAGE_DIR")),
        test_output_dir = Path(os.environ.get("TEST_OUTPUT_DIR")),
        grep_search_results_path=Path(os.environ.get("TEST_OUTPUT_DIR")) / "grep.txt",
    )

    proc = subprocess.run(["grep", "--recursive", "--no-filename", str(test_params.query), str(test_params.uncompressed_logs_dir)], stdout=subprocess.PIPE, check=True)

    sorted_output = sorted(proc.stdout.decode().splitlines(keepends=True))
    with open(test_params.grep_search_results_path, "w") as f:
        for line in sorted_output:
            f.write(line)

    return test_params


def test_compression(get_test_params: TestParams) -> None:
    test_params = get_test_params

    try:
        proc = subprocess.run([str(test_params.clp_package_dir / "sbin" / "start-clp.sh")])
        assert proc.returncode == 0, "Failed to start CLP"

        proc = subprocess.run([str(test_params.clp_package_dir / "sbin" / "search.sh"), "--raw", test_params.query], stdout=subprocess.PIPE)
        assert proc.returncode == 0, "Failed to run search"

        sorted_output = sorted(proc.stdout.decode().splitlines(keepends=True))
        with open("clp-text-search.txt", "w") as f:
            for line in sorted_output:
                f.write(line)

        proc = subprocess.run(["diff", "grep.txt", "clp-text-search.txt"])
        assert proc.returncode == 0, "clp-text search results don't match grep results"
    finally:
        subprocess.run(["sbin/stop-clp.sh"], check=True)
