# Single image used for the Linux / ROS 2 / netem benchmarks.
FROM ros:jazzy-ros-base
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config \
      libzmq3-dev cppzmq-dev libprotobuf-dev protobuf-compiler \
      libgrpc++-dev protobuf-compiler-grpc libgtest-dev \
      python3-zmq python3-protobuf python3-venv iproute2 stress-ng ros-jazzy-rosidl-default-generators \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
