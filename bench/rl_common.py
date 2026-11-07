"""Helpers shared by the Python benchmark programs."""
import json
import os
import sys
import time


def now_ns():
    # Same clock as the C++ side (CLOCK_MONOTONIC) so stamps are comparable.
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def load_pb2():
    """Import the generated protobuf module (build/gen/python by default)."""
    gen = os.environ.get("ROBOLINK_GEN_PY")
    if not gen:
        here = os.path.dirname(os.path.abspath(__file__))
        gen = os.path.join(here, "..", "build", "gen", "python")
    sys.path.insert(0, gen)
    import robolink_pb2  # noqa: E402
    return robolink_pb2


def emit(obj):
    sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\n")
    sys.stdout.flush()
