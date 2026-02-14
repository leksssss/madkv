cmake_minimum_required(VERSION 3.16)

project(KVStore C CXX)

include(common.cmake)

# Proto file
get_filename_component(kv_proto "kvstore.proto" ABSOLUTE)
get_filename_component(kv_proto_path "${kv_proto}" PATH)

# Generated sources
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
      DEPENDS "${kv_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

# kv_grpc_proto
add_library(kv_grpc_proto
  ${kv_grpc_srcs}
  ${kv_grpc_hdrs}
  ${kv_proto_srcs}
  ${kv_proto_hdrs})
target_link_libraries(kv_grpc_proto
  absl::check
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})

# Targets kvstore_[async_](client|server)
foreach(_target
  kvstore_client kvstore_server)
  add_executable(${_target} "${_target}.cc")
  target_link_libraries(${_target}
    kv_grpc_proto
    absl::check
    absl::flags
    absl::flags_parse
    absl::log
    absl::log_initialize
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF})
endforeach()
