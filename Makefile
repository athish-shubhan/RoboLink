# Convenience wrapper around CMake. Native build (macOS or Linux).
BUILD ?= build
JOBS  ?= 4
PY    ?= .venv/bin/python
PYBIN ?= python3

.PHONY: all build test tsan asan venv demo bench bench-transport bench-control clean

all: build

build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j$(JOBS)

test: build
	ctest --test-dir $(BUILD) --output-on-failure

# Sanitizer builds (unit tests only; separate build directories).
tsan:
	cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DROBOLINK_SANITIZE=thread -DROBOLINK_BUILD_BENCH=OFF
	cmake --build build-tsan -j$(JOBS)
	ctest --test-dir build-tsan --output-on-failure

# Codec tests are excluded under ASan: Homebrew's libprotobuf is not ASan-built and its
# headers change layout under -fsanitize=address, which crashes ParseFromArray. The
# protobuf-free core (queue, watchdog, runtime, stats) is fully covered.
asan:
	cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DROBOLINK_SANITIZE=address -DROBOLINK_BUILD_BENCH=OFF
	cmake --build build-asan -j$(JOBS)
	ctest --test-dir build-asan --output-on-failure -E "^Codec\\."

# Python venv for the benchmark harness' Python controller (pyzmq + protobuf).
venv:
	$(PYBIN) -m venv .venv
	.venv/bin/pip install -q pyzmq protobuf

# Terminal dashboard: command stream, stall (HOLD then STOP), recovery. ~16 s.
demo: build
	$(BUILD)/rl_demo

OS_TAG := $(shell uname -s | tr A-Z a-z)
bench-transport: build
	python3 bench/run_bench.py transport --bin $(BUILD)/rl_bench --transports zmq grpc \
	  --out results/$(OS_TAG)/transport_zmq_grpc.json

bench-control: build venv
	$(PY) bench/run_bench.py control --build $(BUILD) --python $(PY) \
	  --out results/$(OS_TAG)/control_baseline.json
	$(PY) bench/run_bench.py control --build $(BUILD) --python $(PY) --stress-cpus 4 \
	  --out results/$(OS_TAG)/control_cpu_stress.json

bench: bench-transport bench-control

clean:
	rm -rf build build-tsan build-asan .venv
