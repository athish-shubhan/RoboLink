cmake_minimum_required(VERSION 3.16)
project(RoboLink VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ROBOLINK_BUILD_TESTS "Build unit tests" ON)
option(ROBOLINK_BUILD_BENCH "Build ZeroMQ/gRPC benchmark + demo binaries" ON)
set(ROBOLINK_SANITIZE "" CACHE STRING "Sanitizer to enable: address, thread or empty")

find_package(Threads REQUIRED)

# -Wall -Wextra -Werror applies to first-party targets only (not generated code).
add_library(robolink_warnings INTERFACE)
target_compile_options(robolink_warnings INTERFACE -Wall -Wextra -Werror)
if(ROBOLINK_SANITIZE)
  target_compile_options(robolink_warnings INTERFACE -fsanitize=${ROBOLINK_SANITIZE} -fno-omit-frame-pointer -g)
  target_link_options(robolink_warnings INTERFACE -fsanitize=${ROBOLINK_SANITIZE})
endif()

# ---- core library (no third-party deps) ------------------------------------
add_library(robolink_core src/watchdog.cpp src/stats.cpp src/runtime.cpp)
target_include_directories(robolink_core PUBLIC include)
target_link_libraries(robolink_core PUBLIC Threads::Threads robolink_warnings)

# ---- protobuf schema + codec -------------------------------------------------
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()
find_package(gRPC CONFIG QUIET)

set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR} ${GEN_DIR}/python)
set(PROTO_FILE ${CMAKE_SOURCE_DIR}/proto/robolink.proto)

if(TARGET protobuf::protoc)
  set(PROTOC_EXE $<TARGET_FILE:protobuf::protoc>)
else()
  set(PROTOC_EXE ${Protobuf_PROTOC_EXECUTABLE})
endif()

set(PROTO_SRCS ${GEN_DIR}/robolink.pb.cc)
set(PROTO_OUTS ${GEN_DIR}/robolink.pb.cc ${GEN_DIR}/robolink.pb.h ${GEN_DIR}/python/robolink_pb2.py)
set(PROTOC_CMD ${PROTOC_EXE} -I ${CMAKE_SOURCE_DIR}/proto --cpp_out=${GEN_DIR} --python_out=${GEN_DIR}/python)
if(gRPC_FOUND)
  if(TARGET gRPC::grpc_cpp_plugin)
    set(GRPC_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
  else()
    find_program(GRPC_PLUGIN grpc_cpp_plugin REQUIRED)
  endif()
  list(APPEND PROTOC_CMD --grpc_out=${GEN_DIR} --plugin=protoc-gen-grpc=${GRPC_PLUGIN})
  list(APPEND PROTO_SRCS ${GEN_DIR}/robolink.grpc.pb.cc)
  list(APPEND PROTO_OUTS ${GEN_DIR}/robolink.grpc.pb.cc ${GEN_DIR}/robolink.grpc.pb.h)
endif()
add_custom_command(OUTPUT ${PROTO_OUTS}
  COMMAND ${PROTOC_CMD} ${PROTO_FILE}
  DEPENDS ${PROTO_FILE} VERBATIM)

add_library(robolink_proto ${PROTO_SRCS} src/codec.cpp)
target_include_directories(robolink_proto PUBLIC ${GEN_DIR})
target_link_libraries(robolink_proto PUBLIC robolink_core protobuf::libprotobuf)
if(gRPC_FOUND)
  target_link_libraries(robolink_proto PUBLIC gRPC::grpc++)
endif()
# Generated sources are not held to -Werror.
set_source_files_properties(${PROTO_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
# gRPC's headers want C++17 or newer; C++20 is fine.

if(ROBOLINK_BUILD_BENCH)
  find_package(cppzmq QUIET)
  if(NOT cppzmq_FOUND)
    find_path(CPPZMQ_INCLUDE zmq.hpp REQUIRED)
    find_library(ZMQ_LIB zmq REQUIRED)
    add_library(cppzmq INTERFACE)
    target_include_directories(cppzmq INTERFACE ${CPPZMQ_INCLUDE})
    target_link_libraries(cppzmq INTERFACE ${ZMQ_LIB})
  endif()
  add_executable(rl_demo bench/demo.cpp)
  include_directories(bench)
  target_link_libraries(rl_demo PRIVATE robolink_proto cppzmq)
  add_executable(rl_plant bench/plant.cpp)
  target_link_libraries(rl_plant PRIVATE robolink_proto cppzmq)
  add_executable(rl_ctrl bench/ctrl.cpp)
  target_link_libraries(rl_ctrl PRIVATE robolink_proto cppzmq)
  if(gRPC_FOUND)
    add_executable(rl_bench bench/bench_main.cpp)
    target_link_libraries(rl_bench PRIVATE robolink_proto cppzmq)
  else()
    message(WARNING "gRPC not found: rl_bench (ZeroMQ+gRPC) is not built")
  endif()
endif()

if(ROBOLINK_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(robolink_tests
    tests/test_queue.cpp tests/test_packet.cpp tests/test_watchdog.cpp
    tests/test_runtime.cpp tests/test_stats.cpp)
  target_link_libraries(robolink_tests PRIVATE robolink_proto GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(robolink_tests DISCOVERY_TIMEOUT 30)
endif()
