"""Run using the pinned SDK checkout with this bundle's patch applied."""
import argparse
from dataclasses import replace
import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sdk", type=Path, required=True)
args = parser.parse_args()
sdk = args.sdk.resolve()
base = Path(__file__).resolve().parent
src = base.parent / "hocket"
sys.path.insert(0, str(sdk))
from tooling import adapters, machines

machine = machines.assign_numbers([machines.machine_at(src)])[0][0]
with TemporaryDirectory() as tmp:
    out = Path(tmp)
    adapters.generate_player_object(out, replace(machine, display="stock"))
    (out / "stock.hpp").write_text("""#include <opxy/types.hpp>
const void* stock_address(opxy::u32);
namespace opxy { namespace platform {
u32 word(u32);
template<class F> F* stock(u32 a) { return reinterpret_cast<F*>(const_cast<void*>(stock_address(a))); }
}}
""")
    include = [out, src, sdk / "sdk/include", sdk / "sdk/platform/opxy-1.2.1"]
    subprocess.run([os.environ.get("CXX", "c++"), "-O2", "-std=gnu++11", "-Wall", "-Werror",
                    *["-I" + str(path) for path in include], base / "clocked_playback.cpp",
                    "-o", out / "check"], check=True)
    subprocess.run([out / "check"], check=True)
