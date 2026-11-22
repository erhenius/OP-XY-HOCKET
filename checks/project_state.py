"""Exercise Hocket's project companion using the SDK's stock I/O test harness."""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk', type=Path, required=True)
sdk = parser.parse_args().sdk.resolve()
sys.path[:0] = [str(sdk), str(sdk / 'tests')]
from test_projects import host_writer

fixture = Path(__file__).with_suffix('.cpp').resolve()
with tempfile.TemporaryDirectory() as directory:
    folder = Path(directory)
    binary = host_writer(folder, fixture)
    subprocess.run([binary], cwd=folder, check=True)
