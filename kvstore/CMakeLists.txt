cmake_minimum_required(VERSION 3.16)
project(KVStore C CXX)

# Add bundled Abseil first, so it's available before find_package
add_subdirectory(cmake/abseil-cpp EXCLUDE_FROM_ALL)

include(common.cmake)

# ─── KV Store Proto ───────────────────────────────────────────────────────────
get_filename_component(kv_proto "kvstore.proto" ABSOLUTE)
get_filename_component(kv_proto_path "${kv_proto}" PATH)

set(kv_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/kvstore.pb.cc")
set(kv_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/kvstore.pb.h")
set(kv_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/kvstore.grpc.pb.cc")
set(kv_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/kvstore.grpc.pb.h")

add_custom_command(
    OUTPUT "${kv_proto_srcs}" "${kv_proto_hdrs}" "${kv_grpc_srcs}" "${kv_grpc_hdrs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
         -I "${kv_proto_path}"
         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
         "${kv_proto}"
    DEPENDS "${kv_proto}"
)

add_library(kv_grpc_proto
  ${kv_grpc_srcs}
  ${kv_grpc_hdrs}
  ${kv_proto_srcs}
  ${kv_proto_hdrs})
target_link_libraries(kv_grpc_proto
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})

# ─── Manager Proto ────────────────────────────────────────────────────────────
get_filename_component(manager_proto "manager.proto" ABSOLUTE)
get_filename_component(manager_proto_path "${manager_proto}" PATH)

set(manager_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/manager.pb.cc")
set(manager_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/manager.pb.h")
set(manager_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/manager.grpc.pb.cc")
set(manager_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/manager.grpc.pb.h")

add_custom_command(
    OUTPUT "${manager_proto_srcs}" "${manager_proto_hdrs}" "${manager_grpc_srcs}" "${manager_grpc_hdrs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
         -I "${manager_proto_path}"
         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
         "${manager_proto}"
    DEPENDS "${manager_proto}"
)

add_library(manager_grpc_proto
  ${manager_grpc_srcs}
  ${manager_grpc_hdrs}
  ${manager_proto_srcs}
  ${manager_proto_hdrs})
target_link_libraries(manager_grpc_proto
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})

# ─── Raft Proto ───────────────────────────────────────────────────────────────
get_filename_component(raft_proto "raft.proto" ABSOLUTE)
get_filename_component(raft_proto_path "${raft_proto}" PATH)

set(raft_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/raft.pb.cc")
set(raft_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/raft.pb.h")
set(raft_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/raft.grpc.pb.cc")
set(raft_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/raft.grpc.pb.h")

add_custom_command(
    OUTPUT "${raft_proto_srcs}" "${raft_proto_hdrs}" "${raft_grpc_srcs}" "${raft_grpc_hdrs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
         -I "${raft_proto_path}"
         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
         "${raft_proto}"
    DEPENDS "${raft_proto}"
)

add_library(raft_grpc_proto
  ${raft_grpc_srcs}
  ${raft_grpc_hdrs}
  ${raft_proto_srcs}
  ${raft_proto_hdrs})
target_link_libraries(raft_grpc_proto
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})

# ─── Shared ───────────────────────────────────────────────────────────────────
include_directories("${CMAKE_CURRENT_BINARY_DIR}")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/cmake/abseil-cpp")

find_library(ROCKSDB_LIB NAMES rocksdb)

# ─── Binaries ─────────────────────────────────────────────────────────────────

# Manager - only needs manager proto
add_executable(kvstore_manager kvstore_manager.cc)
target_link_libraries(kvstore_manager
  manager_grpc_proto
  absl::base
  absl::flags
  absl::flags_parse
  absl::log
  absl::log_initialize
  absl::strings
  absl::str_format
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})

# Server - needs kv, manager, and raft proto
add_executable(kvstore_server kvstore_server.cc)
target_link_libraries(kvstore_server
  kv_grpc_proto
  manager_grpc_proto
  raft_grpc_proto
  absl::base
  absl::flags
  absl::flags_parse
  absl::log
  absl::log_initialize
  absl::strings
  absl::str_format
  ${ROCKSDB_LIB}
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})

# Client - needs both kv proto and manager proto
add_executable(kvstore_client kvstore_client.cc)
target_link_libraries(kvstore_client
  kv_grpc_proto
  manager_grpc_proto
  absl::base
  absl::flags
  absl::flags_parse
  absl::log
  absl::log_initialize
  absl::strings
  absl::str_format
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})