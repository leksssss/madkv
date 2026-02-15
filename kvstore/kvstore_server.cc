#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include "absl/strings/str_format.h"

#include <iostream>
#include <memory>
#include <string>
#include <mutex>
#include "absl/log/initialize.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "cmake/build/kvstore.pb.h"
#include "cmake/build/kvstore.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using kvstore::Kvstore;
using kvstore::PutRequest;
using kvstore::PutResponse;
using kvstore::SwapRequest;
using kvstore::SwapResponse;
using kvstore::GetRequest;
using kvstore::GetResponse;
using kvstore::ScanRequest;
using kvstore::ScanResponse;
using kvstore::DeleteRequest;
using kvstore::DeleteResponse;
using kvstore::KeyValuePair;
using namespace std;

ABSL_FLAG(uint16_t, port, 3777, "Server port for the service");

class KvstoreServiceImpl final : public Kvstore::Service {
  private:
    map<string, string> db;
    mutex db_mutex;

  public:
    Status Put(ServerContext* context, const PutRequest* request,
                    PutResponse* response) override {
      lock_guard<mutex> lock(db_mutex);
      auto it = db.find(request->key());
      if (it != db.end()) {
        response->set_found(true);
        it->second = request->new_value();
      } else {
        db[request->key()] = request->new_value();
      }

      return Status::OK;
    }

    Status Swap(ServerContext* context, const SwapRequest* request,
                    SwapResponse* response) override {
      lock_guard<mutex> lock(db_mutex);
      auto it = db.find(request->key());
      if (it != db.end()) {
        response->set_old_value(it->second);
        it->second = request->new_value();
      } else {
        db[request->key()] = request->new_value();
      }
      
      return Status::OK;
    }

    Status Get(ServerContext* context, const GetRequest* request,
                    GetResponse* response) override {
      lock_guard<mutex> lock(db_mutex);
      auto it = db.find(request->key());
      if(it != db.end()) {
        response->set_value(it->second);
      }

      return Status::OK;
    }

    Status Scan(ServerContext* context, const ScanRequest* request,
                    ScanResponse* response) override {
      lock_guard<mutex> lock(db_mutex);
      auto it = db.lower_bound(request->start_key());
      while (it != db.end() && it->first <= request->end_key()) {
        auto* pair = response->add_pairs();
        pair->set_key(it->first);
        pair->set_value(it->second);
        it++;
      }

      return Status::OK;
    }

    Status Delete(ServerContext* context, const DeleteRequest* request,
                    DeleteResponse* response) override {
      lock_guard<mutex> lock(db_mutex);
      size_t count = db.erase(request->key());
      response->set_found(count > 0);

      return Status::OK;
    }
};

void RunServer(uint16_t port) {
  string server_address = absl::StrFormat("0.0.0.0:%d", port);
  KvstoreServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // To keep the channels always hot to avoid Nagle's algorithm
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 20000);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
  builder.AddChannelArgument("grpc.tcp_nodelay", 1);
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  unique_ptr<Server> server(builder.BuildAndStart());
  cout << "Server listening on " << server_address << std::endl;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  RunServer(absl::GetFlag(FLAGS_port));
  return 0;
}
