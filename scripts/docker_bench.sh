#!/usr/bin/env bash
# Linux/Docker benchmark run: ROS 2 (rclcpp + rclpy), ZeroMQ, gRPC transports,
# then the Python-vs-C++ control loop under CPU stress and netem delay.
# Needs a running Docker daemon (Colima). Results go to results/docker/.
set -euo pipefail
cd "$(dirname "$0")/.."
IMG=robolink-bench
docker build -q -t $IMG -f docker/Dockerfile docker >/dev/null
docker run --rm --cap-add NET_ADMIN -v "$PWD":/src -e ROBOLINK_ENV=docker $IMG bash -c '
set -e
cmake -S . -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DROBOLINK_BUILD_TESTS=OFF >/dev/null
cmake --build /tmp/build >/dev/null
source /opt/ros/jazzy/setup.bash
mkdir -p /tmp/ros_ws/src && cp -r ros2/robolink_msgs ros2/robolink_bench /tmp/ros_ws/src/
(cd /tmp/ros_ws && colcon build --event-handlers console_direct- >/dev/null)
source /tmp/ros_ws/install/setup.bash
B=/tmp/ros_ws/install/robolink_bench/lib/robolink_bench
export ROBOLINK_GEN_PY=/tmp/build/gen/python
mkdir -p results/docker
python3 bench/run_bench.py transport --bin /tmp/build/rl_bench --transports zmq grpc \
  --ros2-cpp "$B/bench_cpp" --ros2-py "python3 $B/bench_py.py" --ready 3 \
  --note "docker/Linux on Colima VM; loopback" --out results/docker/transport_all.json
python3 bench/run_bench.py control --build /tmp/build --python python3 --out results/docker/control_baseline.json
python3 bench/run_bench.py control --build /tmp/build --python python3 --stress-cpus 4 --out results/docker/control_cpu_stress.json
python3 bench/run_bench.py control --build /tmp/build --python python3 --netem "delay 2ms 1ms" --out results/docker/control_netem.json
python3 bench/run_bench.py control --build /tmp/build --python python3 --stress-cpus 4 --netem "delay 2ms 1ms" --out results/docker/control_stress_netem.json
'
