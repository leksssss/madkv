#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include "absl/strings/str_format.h"

#include <thread>
#include <iostream>
#include <memory>
#include <string>
#include <mutex>
#include <map>
#include <atomic>

#include "absl/log/initialize.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include "kvstore.pb.h"
#include "kvstore.grpc.pb.h"
#include "manager.pb.h"
#include "manager.grpc.pb.h"
#include "raft.pb.h"
#include "raft.grpc.pb.h"

#include "raft_server.h"

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

using raft::Raft;
using raft::RequestVoteRequest;
using raft::RequestVoteResponse;
using raft::AppendEntriesRequest;
using raft::AppendEntriesResponse;

using ROCKSDB_NAMESPACE::DB;
using ROCKSDB_NAMESPACE::Options;
using ROCKSDB_NAMESPACE::WriteOptions;

using namespace std;

ABSL_FLAG(int32_t, partition_id, 0, "Partition ID");
ABSL_FLAG(int32_t, replica_id, 0, "Replica ID");
ABSL_FLAG(string, manager_addrs, "", "Manager address");
ABSL_FLAG(string, api_listen, "0.0.0.0:3777", "API listen");
ABSL_FLAG(string, p2p_listen, "0.0.0.0:3707", "P2P listen");
ABSL_FLAG(string, peer_addrs, "none", "Comma separated peers");
ABSL_FLAG(string, backer_path, "./backer", "Storage path");

class KvstoreServiceImpl final : public Kvstore::Service {
 private:
  map<string, string> db;
  mutex db_mutex;

  unique_ptr<rocksdb::DB> storage;
  Options options;
  WriteOptions write_options;

  unique_ptr<RaftServer> raft_;
  thread state_machine_thread_;
  atomic<bool> should_stop_{false};

  // Wait until specific log index applied
  bool WaitForApply(int index, int max_wait_ms = 5000) {
    int waited = 0;
    while (waited < max_wait_ms && !should_stop_) {
      if (raft_->GetLastApplied() >= index) return true;
      this_thread::sleep_for(chrono::milliseconds(1));
      waited += 1;
    }
    return false;
  }

  bool WaitForApplyFull(int max_wait_ms = 5000) {
    int waited = 0;
    while (waited < max_wait_ms && !should_stop_) {
      if (raft_->GetLastApplied() >= raft_->GetCommitIndex()) return true;
      this_thread::sleep_for(chrono::milliseconds(1));
      waited += 1;
    }
    return false;
  }

  bool WaitForQuorum(int max_wait_ms = 200) {
    int waited = 0;
    while (waited < max_wait_ms && !should_stop_) {
      if (!raft_->IsLeader()) return false;
      if (raft_->HasRecentQuorum()) return true;
      this_thread::sleep_for(chrono::milliseconds(5));
      waited += 5;
    }
    return false;
  }

  void ApplyCommittedEntries() {
    int last_applied = raft_->GetLastApplied();

    while (!should_stop_) {
      int commit_index = raft_->GetCommitIndex();

      while (last_applied < commit_index) {
        raft::LogEntry entry;
        int next = last_applied + 1;

        if (!raft_->GetLogEntry(next, entry)) break;

        {
          lock_guard<mutex> lock(db_mutex);
          const KeyValuePair& cmd = entry.command();
          string key = cmd.key();

          if (cmd.has_value() && !cmd.value().empty()) {
            db[key] = cmd.value();
            storage->Put(write_options, "kv:" + key, cmd.value());
          } else {
            db.erase(key);
            storage->Delete(write_options, "kv:" + key);
          }
        }

        last_applied = next;
        raft_->SetLastApplied(last_applied);
      }

      this_thread::sleep_for(chrono::milliseconds(1));
    }
  }

  Status NotLeaderStatus() {
    int lid = raft_->GetLeaderId();
    string msg = (lid >= 0)
        ? absl::StrFormat("not_leader:%d", lid)
        : "not_leader:-1";
    return Status(grpc::StatusCode::UNAVAILABLE, msg);
  }

 public:
  KvstoreServiceImpl(const string& backer_path,
                     int partition_id,
                     int replica_id,
                     const vector<string>& peer_addrs) {
    options.create_if_missing = true;
    write_options.sync = true;

    rocksdb::DB* raw;
    auto s = DB::Open(options, backer_path, &raw);
    assert(s.ok());
    storage.reset(raw);

    RaftServer::Config cfg;
    cfg.replica_id = replica_id;
    cfg.peer_addrs = peer_addrs;

    raft_ = make_unique<RaftServer>(cfg, storage.get());
    raft_->StartBackgroundThreads();

    state_machine_thread_ =
        thread([this] { ApplyCommittedEntries(); });

    // Restore db from RocksDB
    auto it = storage->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      if (it->key().starts_with("kv:")) {
        db[it->key().ToString().substr(3)] =
            it->value().ToString();
      }
    }
    delete it;
  }

  ~KvstoreServiceImpl() {
    should_stop_ = true;
    raft_->StopBackgroundThreads();
    if (state_machine_thread_.joinable())
      state_machine_thread_.join();
  }

  // ================= WRITES =================

  Status Put(ServerContext*, const PutRequest* req,
             PutResponse* res) override {
    if (!raft_->IsLeader()) return NotLeaderStatus();
    if (!WaitForApplyFull()) return Status(grpc::StatusCode::DEADLINE_EXCEEDED, "Not caught up");

    bool existed;
    {
      lock_guard<mutex> lock(db_mutex);
      existed = db.count(req->key());
    }

    KeyValuePair cmd;
    cmd.set_key(req->key());
    cmd.set_value(req->new_value());

    int index = raft_->AppendCommand(cmd);
    if (index < 0) return NotLeaderStatus();

    if (!WaitForApply(index))
      return Status(grpc::StatusCode::DEADLINE_EXCEEDED, "Apply timeout");

    res->set_found(existed);
    return Status::OK;
  }

  Status Swap(ServerContext*, const SwapRequest* req,
              SwapResponse* res) override {
    if (!raft_->IsLeader()) return NotLeaderStatus();
    if (!WaitForApplyFull()) return Status(grpc::StatusCode::DEADLINE_EXCEEDED, "Not caught up");

    string old;
    bool had_old;
    {
      lock_guard<mutex> lock(db_mutex);
      auto it = db.find(req->key());
      had_old = (it != db.end());
      if (had_old) old = it->second;
    }

    KeyValuePair cmd;
    cmd.set_key(req->key());
    cmd.set_value(req->new_value());

    int index = raft_->AppendCommand(cmd);
    if (index < 0) return NotLeaderStatus();

    if (!WaitForApply(index))
      return Status(grpc::StatusCode::DEADLINE_EXCEEDED, "Apply timeout");

    if (had_old) res->set_old_value(old);
    return Status::OK;
  }

  Status Delete(ServerContext*, const DeleteRequest* req,
                DeleteResponse* res) override {
    if (!raft_->IsLeader()) return NotLeaderStatus();
    if (!WaitForApplyFull()) return Status(grpc::StatusCode::DEADLINE_EXCEEDED, "Not caught up");

    bool existed;
    {
      lock_guard<mutex> lock(db_mutex);
      existed = db.count(req->key());
    }

    KeyValuePair cmd;
    cmd.set_key(req->key());

    int index = raft_->AppendCommand(cmd);
    if (index < 0) return NotLeaderStatus();

    if (!WaitForApply(index))
      return Status(grpc::StatusCode::DEADLINE_EXCEEDED, "Apply timeout");

    res->set_found(existed);
    return Status::OK;
  }

  // ================= READS =================

  Status Get(ServerContext*, const GetRequest* req,
             GetResponse* res) override {
    if (!raft_->IsLeader()) return NotLeaderStatus();
    if (!WaitForQuorum()) return NotLeaderStatus();

    lock_guard<mutex> lock(db_mutex);
    auto it = db.find(req->key());
    if (it != db.end())
      res->set_value(it->second);
    return Status::OK;
  }

  Status Scan(ServerContext*, const ScanRequest* req,
              ScanResponse* res) override {
    if (!raft_->IsLeader()) return NotLeaderStatus();
    if (!WaitForQuorum()) return NotLeaderStatus();

    lock_guard<mutex> lock(db_mutex);
    auto it = db.lower_bound(req->start_key());
    while (it != db.end() && it->first <= req->end_key()) {
      auto* p = res->add_pairs();
      p->set_key(it->first);
      p->set_value(it->second);
      ++it;
    }
    return Status::OK;
  }

      static void RegisterWithManager(const string& manager_addrs, int partition_id,
                                    int replica_id, const string& my_addr)
    {
      auto channel = grpc::CreateChannel(manager_addrs, grpc::InsecureChannelCredentials());
      auto stub = Manager::NewStub(channel);

      RegisterRequest req;
      int server_id = partition_id * 100 + replica_id;
      req.set_server_id(server_id);
      req.set_address(my_addr);

      while (true) {
        RegisterResponse res;
        grpc::ClientContext ctx;
        Status s = stub->RegisterServer(&ctx, req, &res);
        if (s.ok()) {
          cout << "Registered with manager as server " << server_id
               << " (partition " << partition_id << " replica " << replica_id << ")" << endl;
          return;
        }
        cerr << "Failed to register with manager, retrying... (" << s.error_message() << ")" << endl;
        this_thread::sleep_for(chrono::milliseconds(500));
      }
    }

  RaftServer* GetRaft() { return raft_.get(); }
};

class RaftServiceImpl final : public Raft::Service {
 private:
  RaftServer* raft_;
 public:
  RaftServiceImpl(RaftServer* r) : raft_(r) {}

  Status RequestVote(ServerContext*, const RequestVoteRequest* req,
                     RequestVoteResponse* res) override {
    raft_->HandleRequestVote(*req, *res);
    return Status::OK;
  }

  Status AppendEntries(ServerContext*, const AppendEntriesRequest* req,
                       AppendEntriesResponse* res) override {
    raft_->HandleAppendEntries(*req, *res);
    return Status::OK;
  }
};


void RunServer(int partition_id, int replica_id, const string& manager_addrs,
               const string& api_listen, const string& p2p_listen,
               const std::vector<string>& peer_addrs, const string& backer_path)
{
  KvstoreServiceImpl::RegisterWithManager(manager_addrs, partition_id, replica_id, api_listen);
  KvstoreServiceImpl kvstore_service(backer_path, partition_id, replica_id, peer_addrs);
  RaftServiceImpl raft_service(kvstore_service.GetRaft());

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  builder.AddListeningPort(api_listen, grpc::InsecureServerCredentials());
  builder.AddListeningPort(p2p_listen, grpc::InsecureServerCredentials());
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 20000);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
  builder.AddChannelArgument("grpc.tcp_nodelay", 1);
  builder.RegisterService(&kvstore_service);
  builder.RegisterService(&raft_service);
  unique_ptr<Server> server(builder.BuildAndStart());
  cout << "Server partition " << partition_id << " replica " << replica_id
       << " listening on API " << api_listen << " and P2P " << p2p_listen << std::endl;

  server->Wait();
}

int main(int argc, char** argv)
{
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  int partition_id      = absl::GetFlag(FLAGS_partition_id);
  int replica_id        = absl::GetFlag(FLAGS_replica_id);
  string manager_addrs  = absl::GetFlag(FLAGS_manager_addrs);
  string api_listen     = absl::GetFlag(FLAGS_api_listen);
  string p2p_listen     = absl::GetFlag(FLAGS_p2p_listen);
  string peer_addrs_str = absl::GetFlag(FLAGS_peer_addrs);
  string backer_path    = absl::GetFlag(FLAGS_backer_path);

  if (manager_addrs.empty()) {
    cerr << "Error: --manager_addrs is required" << endl;
    return 1;
  }

  std::vector<string> peer_addrs;
  if (peer_addrs_str != "none") {
    size_t start = 0;
    size_t comma_pos = 0;
    while ((comma_pos = peer_addrs_str.find(',', start)) != string::npos) {
      peer_addrs.push_back(peer_addrs_str.substr(start, comma_pos - start));
      start = comma_pos + 1;
    }
    peer_addrs.push_back(peer_addrs_str.substr(start));
  }

  RunServer(partition_id, replica_id, manager_addrs, api_listen, p2p_listen,
            peer_addrs, backer_path);
  return 0;
}