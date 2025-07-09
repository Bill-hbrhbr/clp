import os
import shutil
import subprocess
from pathlib import Path


def clean_package_data(clp_package_dir: Path) -> None:
    package_data_dir = clp_package_dir / "var" / "data"
    shutil.rmtree(package_data_dir, ignore_errors=True)
    package_logs_dir = clp_package_dir / "var" / "log"
    shutil.rmtree(package_logs_dir, ignore_errors=True)


def get_env_var(var_name: str) -> str:
    value = os.environ.get(var_name)
    if value is None:
        raise ValueError(f"Environment variable {var_name} is not set.")
    return value


def run_and_assert(cmd, **kwargs):
    proc = subprocess.run(cmd, **kwargs)
    assert proc.returncode == 0, f"Command failed: {' '.join(cmd)}"
    return proc
