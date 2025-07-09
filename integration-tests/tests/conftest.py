import os
from dataclasses import dataclass
from pathlib import Path

import pytest
from utils import (
    clean_package_data,
    get_env_var,
    run_and_assert,
)


@dataclass(frozen=True)
class EnvParams:
    clp_bins_dir: Path
    clp_package_dir: Path
    clp_package_sbin_dir: Path
    test_output_dir: Path
    uncompressed_logs_dir: Path


@pytest.fixture(scope="session")
def env_params() -> EnvParams:
    return EnvParams(
        clp_bins_dir=Path(get_env_var("CLP_BINS_DIR")),
        clp_package_dir=Path(get_env_var("CLP_PACKAGE_DIR")),
        clp_package_sbin_dir=Path(get_env_var("CLP_PACKAGE_SBIN_DIR")),
        test_output_dir=Path(get_env_var("TEST_OUTPUT_DIR")),
        uncompressed_logs_dir=Path(get_env_var("UNCOMPRESSED_LOGS_DIR")),
    )


@pytest.fixture(scope="session", autouse=True)
def create_test_output_dir(env_params: EnvParams) -> None:
    env_params.test_output_dir.mkdir(parents=True, exist_ok=True)


@pytest.fixture(scope="module", autouse=True)
def run_clp_package(env_params: EnvParams) -> None:
    clean_package_data(env_params.clp_package_dir)
    try:
        cmd = [str(env_params.clp_package_sbin_dir / "start-clp.sh")]
        run_and_assert(cmd)
        yield
    finally:
        cmd = [str(env_params.clp_package_sbin_dir / "stop-clp.sh")]
        run_and_assert(cmd, check=True)
        clean_package_data(env_params.clp_package_dir)
