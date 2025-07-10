import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

pytestmark = pytest.mark.clp_s

from conftest import EnvParams
from utils import run_and_assert


@dataclass(frozen=True)
class DatasetSearchParams:
    name: str
    logs_dir: Path
    query: str
    dataset: str


@pytest.fixture(scope="module", params=[("mongodb", "mongo::RecoveryUnit::onRollback", "pytest")])
def test_params(request, env_params: EnvParams) -> DatasetSearchParams:
    name, query, dataset = request.param
    logs_dir = env_params.uncompressed_logs_dir / name

    return DatasetSearchParams(
        name=name,
        logs_dir=logs_dir,
        query=query,
        dataset=dataset,
    )


@pytest.fixture(scope="module", autouse=True)
def compress(env_params: EnvParams, test_params: DatasetSearchParams, run_clp_package) -> None:
    cmd = [
        str(env_params.clp_package_sbin_dir / "compress.sh"),
        "--dataset",
        test_params.dataset,
        str(test_params.logs_dir),
    ]
    run_and_assert(cmd, stdout=subprocess.PIPE)


def test_count(env_params: EnvParams, test_params: DatasetSearchParams) -> None:
    cmd = [
        str(env_params.clp_package_sbin_dir / "search.sh"),
        "--dataset",
        test_params.dataset,
        test_params.query,
    ]
    proc = run_and_assert(cmd, stdout=subprocess.PIPE, check=True)

    output_lines = proc.stdout.decode().splitlines()
    print(f"Output line count: {len(output_lines)}")
