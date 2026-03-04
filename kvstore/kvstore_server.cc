#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include "absl/strings/str_format.h"

#include <thread>
#include <iostream>
#include <memory>
#include <string>
#include <mutex>
#include "absl/log/initialize.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "kvstore.pb.h"
#include "kvstore.grpc.pb.h"
#include "manager.pb.h"
#include "manager.grpc.pb.h"

#include "rocksdb/db.h"
#include "rocksdb/options.h"

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

using manager::Manager;
using manager::RegisterRequest;
using manager::RegisterResponse;

using ROCKSDB_NAMESPACE::DB;
using ROCKSDB_NAMESPACE::Options;
using ROCKSDB_NAMESPACE::WriteOptions;
using namespace std;

ABSL_FLAG(string, manager_addr, "", "Manager address e.g. 1.2.3.4:3666");
ABSL_FLAG(string, api_listen, "0.0.0.0:3777", "Server listen address");
ABSL_FLAG(int32_t, server_id, 0, "Unique server ID starting from 0");
ABSL_FLAG(string, backer_path, "./backer", "Path to persistent storage directory");

class KvstoreServiceImpl final : public Kvstore::Service {
  private:
    map<string, string> db;
    mutex db_mutex;
    std::unique_ptr<rocksdb::DB> storage;
    Options options;
    WriteOptions write_options;
    atomic<uint64_t> counter{0};

  void write_to_storage(KeyValuePair log_entry) {
    uint64_t id = counter.fetch_add(1);
    id = htobe64(id + 1);
    string serialized;
    log_entry.SerializeToString(&serialized);
    string key(
        reinterpret_cast<char*>(&id),
        sizeof(id)
    );
    ROCKSDB_NAMESPACE::Status s = storage->Put(write_options, key, serialized);
    assert(s.ok());
  }

  void set_counter() {
    rocksdb::Iterator* it = storage->NewIterator(rocksdb::ReadOptions());
    it->SeekToLast();
    uint64_t last_id = 0;

    if (it->Valid()) {
        memcpy(&last_id, it->key().data(), sizeof(last_id));
        last_id = be64toh(last_id);
        counter.store(last_id);
    }
    
    delete it;
  }

  void read_from_storage_on_startup() {
    rocksdb::Iterator* it = storage->NewIterator(rocksdb::ReadOptions());

    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      KeyValuePair log_entry;
      log_entry.ParseFromString(it->value().ToString());
      string key = log_entry.key();
      string value = log_entry.has_value()? log_entry.value() : "";
      cout << "Key: " << key << " | Value: " << value << endl;
      if (value == "") {
        db.erase(key);
      } else {
        db[key] = value;
      }
    }

    delete it;
  }

  public:

    KvstoreServiceImpl(const string& backer_path) {
      // Cleanup if needed
      // rocksdb::DestroyDB(kDBPath, rocksdb::Options());
      // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
      options.IncreaseParallelism();
      options.OptimizeLevelStyleCompaction();
      // Create the DB if it's not already present
      options.create_if_missing = true;
      write_options.sync = true;

      rocksdb::DB* raw_storage;
      ROCKSDB_NAMESPACE::Status s = DB::Open(options, backer_path, &raw_storage);
      assert(s.ok());
      storage.reset(raw_storage);
      // RockDB writes are atomic so the tail will be fully written.
      set_counter();
      read_from_storage_on_startup();
    }

    Status Put(ServerContext* context, const PutRequest* request,
                    PutResponse* response) override {
      lock_guard<mutex> lock(db_mutex);
      string key = request->key();
      string value = request->new_value();
      KeyValuePair log_entry;
      log_entry.set_key(key);
      log_entry.set_value(value);
      write_to_storage(log_entry);

      auto it = db.find(key);
      if (it != db.end()) {
        response->set_found(true);
        it->second = value;
      } else {
        db[key] = value;
      }

      return Status::OK;
    }

    Status Swap(ServerContext* context, const SwapRequest* request,
                    SwapResponse* response) override {
      lock_guard<mutex> lock(db_mutex);
      string key = request->key();
      string value = request->new_value();
      KeyValuePair log_entry;
      log_entry.set_key(key);
      log_entry.set_value(value);
      write_to_storage(log_entry);

      auto it = db.find(key);
      if (it != db.end()) {
        response->set_old_value(it->second);
        it->second = value;
      } else {
        db[key] = value;
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
      KeyValuePair log_entry;
      log_entry.set_key(request->key());
      write_to_storage(log_entry);

      size_t count = db.erase(request->key());
      response->set_found(count > 0);

      return Status::OK;
    }

    static void RegisterWithManager(const string& manager_addr, int server_id, const string& my_addr)
    {
      auto channel = grpc::CreateChannel(manager_addr, grpc::InsecureChannelCredentials());
      auto stub = Manager::NewStub(channel);

      RegisterRequest req;
      req.set_server_id(server_id);
      req.set_address(my_addr);

      while(true)
      {
        RegisterResponse res;
        grpc::ClientContext ctx;
        Status s = stub->RegisterServer(&ctx, req, &res);
        if(s.ok())
        {
          cout << "Registered with manager as server " << server_id << ", cluster size = " << res.num_servers() << endl;
          return;
        }
        cerr << "Failed to register with manager, retrying... (" << s.error_message() << ")" << endl;
        this_thread::sleep_for(chrono::milliseconds(500));
      }
    }
};

void RunServer(const string& listen_addr, const string& manager_addr, int server_id, const string& backer_path) 
{
  // Register with manager first, before accepting any client requests
  KvstoreServiceImpl::RegisterWithManager(manager_addr, server_id, listen_addr);
  KvstoreServiceImpl service(backer_path);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
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
  cout << "Server " << server_id << " listening on " << listen_addr << std::endl;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

int main(int argc, char** argv) 
{
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  string listen_addr = absl::GetFlag(FLAGS_api_listen);
  string manager_addr = absl::GetFlag(FLAGS_manager_addr);
  int server_id = absl::GetFlag(FLAGS_server_id);
  string backer_path = absl::GetFlag(FLAGS_backer_path);

  if (manager_addr.empty()) {
    cerr << "Error: --manager_addr is required" << endl;
    return 1;
  }

  RunServer(listen_addr, manager_addr, server_id, backer_path);
  return 0;
}
