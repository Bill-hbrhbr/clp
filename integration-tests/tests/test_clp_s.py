import shutil
import subprocess
from dataclasses import dataclass
import os
from pathlib import Path
import pytest
import time

@dataclass(frozen=True)
class BaseTestParams:
    clp_bins_dir: Path
    test_output_dir: Path
    uncompressed_logs_dir: Path

@pytest.fixture
def get_base_test_params() -> BaseTestParams:
    return BaseTestParams(
        clp_bins_dir=Path(_get_env_var("CLP_BINS_DIR")),
        test_output_dir=Path(_get_env_var("TEST_OUTPUT_DIR")),
        uncompressed_logs_dir=Path(_get_env_var("UNCOMPRESSED_LOGS_DIR")),
    )

def slow_fn():
    time.sleep(1)

def compress_dataset(clp_s_bin: Path, dataset_dir: Path, output_dir: Path) -> int:
    cmd = [
        str(clp_s_bin),
        "c",
        str(output_dir),
        str(dataset_dir),
    ]
    proc = subprocess.run(cmd)
    return proc.returncode

def test_compression_speed(benchmark, get_base_test_params: BaseTestParams) -> None:
    test_params = get_base_test_params
    test_params.test_output_dir.mkdir(parents=True, exist_ok=True)

    dataset_dir = test_params.uncompressed_logs_dir / "postgresql"
    output_dir = test_params.test_output_dir / "postgresql"
    shutil.rmtree(output_dir)
    benchmark(compress_dataset, )
    # Compress a dataset
    # Decompress it
    # Check that the decompressed dataset is the same as the original one

def _get_env_var(var_name: str) -> str:
    value = os.environ.get(var_name)
    if value is None:
        raise ValueError(f"Environment variable {var_name} is not set.")
    return value
