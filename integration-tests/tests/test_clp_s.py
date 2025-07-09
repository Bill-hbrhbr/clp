import shutil
import subprocess
import time
from pathlib import Path

import pytest

pytestmark = pytest.mark.skip

from conftest import EnvParams


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


def test_compression_speed(benchmark, env_params: EnvParams) -> None:
    dataset_dir = env_params.uncompressed_logs_dir / "postgresql"
    shutil.rmtree(output_dir)
    benchmark(
        compress_dataset,
    )
    # Compress a dataset
    # Decompress it
    # Check that the decompressed dataset is the same as the original one
