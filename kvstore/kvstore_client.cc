#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include <thread>
#include <chrono>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "kvstore.pb.h"
#include "kvstore.grpc.pb.h"
#include "manager.pb.h"
#include "manager.grpc.pb.h"

using grpc::Channel;
using grpc::ChannelArguments;
using grpc::ClientContext;
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
using manager::ClusterInfoRequest;
using manager::ClusterInfoResponse;
using namespace std;

ABSL_FLAG(string, manager_addrs, "localhost:3666", "Manager addresses list");

vector<string> SplitByComma(const string& s) {
    vector<string> result;
    if (s.empty()) return result;
    stringstream ss(s);
    string token;
    while (getline(ss, token, ','))
        if (!token.empty()) result.push_back(token);
    return result;
}

shared_ptr<Channel> MakeChannel(const string& addr)
{
  ChannelArguments args;
  args.SetInt("grpc.tcp_nodelay", 1);
  args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 40000);
  args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
  return grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);
}

// Server encodes "not_leader:N" in the error message where N is the leader's
// replica index within the partition.
int ParseLeaderHint(const string& msg)
{
  auto pos = msg.find("not_leader:");
  if(pos == string::npos)
  {
    return -1;
  }
  try
  {
    return stoi(msg.substr(pos+11));
  }
  catch (...)
  {
    return -1;
  }
}

class KvstoreClient {
 public:
  KvstoreClient(const vector<string>& manager_addrs)
  {
    // Try each manager address in a loop until we get cluster info.
    // Handles both single manager and replicated manager.
    while(true)
    {
      for(const auto& addr: manager_addrs)
      {
        auto manager_stub = Manager::NewStub(MakeChannel(addr));
        ClusterInfoRequest req;
        ClusterInfoResponse res;
        ClientContext ctx;
        Status s = manager_stub->GetClusterInfo(&ctx, req, &res);
        if(s.ok())
        {
          num_partitions = res.num_servers();
          // Compute server_rf from the number of servers: total_servers / num_partitions
          server_rf = res.servers_size() / num_partitions;
          stubs_.clear();
          stubs_.resize(num_partitions);
          for (int i = 0; i < num_partitions; i++) {
            stubs_[i].resize(server_rf);
          }
          for(const auto& s: res.servers())
          {
            int p = s.server_id() / server_rf;
            int r = s.server_id() % server_rf;
            stubs_[p][r] = Kvstore::NewStub(MakeChannel(s.address()));
          }
          current_leader.assign(num_partitions, 0);
          // cout << "Connected: " << num_partitions << " partitions, rf=" << server_rf << "\n";
          return;
        }
      }
      this_thread::sleep_for(chrono::milliseconds(500));
    }
  }

  // Assembles the client's payload, sends it and presents the response back
  // from the server.
  void Put(const string& key, const string& value) {
    // Data we are sending to the server.
    PutRequest request;
    request.set_key(key);
    request.set_new_value(value);

    // Container for the data we expect from the server.
    PutResponse response;

    auto status = CallLeader(key, [&](Kvstore::Stub* stub)
    {
      ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
      return stub->Put(&context, request, &response);
    });

    // Act upon its status.
    if (status.ok()) {
      if (response.found()) {
        cout << "PUT " << key << " found\n";
      } else {
        cout << "PUT " << key << " not_found\n";
      }
    } else {
      cerr << status.error_code() << ": " << status.error_message() << endl;
    }
  }

  void Swap(const string& key, const string& new_value) {
    SwapRequest request;
    request.set_key(key);
    request.set_new_value(new_value);

    SwapResponse response;
 
    auto status = CallLeader(key, [&](Kvstore::Stub* stub)
    {
      ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
      return stub->Swap(&context, request, &response);
    });
    
    if(status.ok()) {
      if(response.has_old_value()) {
        cout << "SWAP " << key <<  " " << response.old_value() << endl;
      } else {
       cout << "SWAP " << key <<  " null\n";
      }
    } else {
      cerr << status.error_code() << ":" << status.error_message() << endl;
    }
  }

  void Get(const string& key) {
    GetRequest request;
    request.set_key(key);
    
    GetResponse response;

    auto status = CallLeader(key, [&](Kvstore::Stub* stub)
    {
      ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
      return stub->Get(&context, request, &response);
    });

    if(status.ok()) {
      if(response.has_value()) {
        cout << "GET " << key <<  " " << response.value() << endl;
      } else {
        cout << "GET " << key <<  " null\n";
      }
    } else {
      cerr << status.error_code() << ":" << status.error_message() << endl;
    }
  }

  void Scan(const string& start_key, const string& end_key) {
    ScanRequest request;
    request.set_start_key(start_key);
    request.set_end_key(end_key);

    ScanResponse response;

    // Fan out to all partitions and merge results into sorted map
    // TODO: need to implement this!!!
    map<string, string> merged;
    for(int p = 0; p < num_partitions; p++)
    {
      ScanResponse partition_response;
      auto status = CallLeaderPartition(p, [&](Kvstore::Stub* stub) 
      {
        ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        return stub->Scan(&ctx, request, &partition_response);
      });
      if (!status.ok()) {
        cerr << status.error_code() << ":" << status.error_message() << endl;
        return;
      }
      for(const auto& pair : partition_response.pairs())
      {
        merged[pair.key()] = pair.value();
      }
    }

    
    cout << "SCAN " << start_key << " " << end_key << " BEGIN\n";
    for (const auto& [k, v] : merged) {
      cout << "\t" << k << " " << v << "\n";
    }
    cout << "SCAN END\n";
   
  }

  void Delete(const string& key) {
    DeleteRequest request;
    request.set_key(key);

    DeleteResponse response;
    auto status = CallLeader(key, [&](Kvstore::Stub* stub) 
    {  
      ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
      return stub->Delete(&ctx, request, &response);
    });

    if(status.ok()) {
      if (response.found()) {
        cout << "DELETE " << key << " found\n";
      } else {
        cout << "DELETE " << key << " not_found\n";
      }
    } else {
      cerr << status.error_code() << ": " << status.error_message() << endl;
    }
  }

 private:
  int num_partitions = 0;
  int server_rf = 1;

  // stubs_[partition][replica]
  vector<vector<unique_ptr<Kvstore::Stub>>> stubs_;
  vector<int> current_leader;

  int PartitionFor(const string& key)
  {
    return (int)(hash<string>{}(key) % num_partitions);
  }

  // Route to key's partition and find the leader within it
  template <typename F> Status CallLeader(const string& key, F rpc_call)
  {
    return CallLeaderPartition(PartitionFor(key), rpc_call);
  }

  // Main loop: need to check if okay!?
    //   1. Send RPC to current_leader[partition].
    //   2. On UNAVAILABLE with "not_leader:N" hint -> update leader to N, retry.
    //   3. On connection/timeout error -> round-robin to next replica, retry.
    //   4. On success -> return.
  template<typename F>
  Status CallLeaderPartition(int partition, F rpc)
  {
    const int max_attempts = 200;
    for(int attempt=0; attempt < max_attempts; attempt++)
    {
      int r = current_leader[partition];
      Status s = rpc(stubs_[partition][r].get());

      if(s.ok())
      {
        return s;
      }
      if(s.error_code() == grpc::StatusCode::UNAVAILABLE)
      {
        int hint = ParseLeaderHint(s.error_message());
        if(hint >= 0 && hint < server_rf)
        {
          current_leader[partition] = hint;
        }
        else
        {
          current_leader[partition] = (r+1) % server_rf;
        }
      }
      else
      {
        current_leader[partition] = (r+1) % server_rf;
      }
        
      this_thread::sleep_for(chrono::milliseconds(10));
    }
    return Status(grpc::StatusCode::UNAVAILABLE, "no leader found after retries");
  }
};

vector<string> split(const string& str, const string& delimiter = " ") {
    vector<string> tokens;
    size_t pos = 0;
    size_t lastPos = 0;

    while ((pos = str.find(delimiter, lastPos)) != string::npos) {
        tokens.push_back(str.substr(lastPos, pos - lastPos));
        lastPos = pos + delimiter.length();
    }

    tokens.push_back(str.substr(lastPos));

    return tokens;
}

int main(int argc, char** argv) 
{
  absl::ParseCommandLine(argc, argv);
  string managers_addrs = absl::GetFlag(FLAGS_manager_addrs);

  vector<string> manager_list = SplitByComma(managers_addrs);
  KvstoreClient kvstore(manager_list);
  
  while (true) {
    string input;
    getline(cin, input);

    if (input == "STOP") { // stop reading stdin, exit
      cout << "STOP\n";
      break;
    }

    vector<string> query = split(input);
    if (query[0] == "PUT") {
      kvstore.Put(query[1], query[2]);
    } else if (query[0] == "SWAP"){
      kvstore.Swap(query[1], query[2]);
    } else if (query[0] == "GET"){
      kvstore.Get(query[1]);
    } else if (query[0] == "SCAN"){
      kvstore.Scan(query[1], query[2]);
    } else if (query[0] == "DELETE"){
      kvstore.Delete(query[1]);
    } else {
      cout << "Invalid input: " << input << endl;
      return 1;
    }
  }

  return 0;
}