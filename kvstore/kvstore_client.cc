#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>
#include <fstream>
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

ABSL_FLAG(string, manager_addr, "localhost:3666", "Manager address");

shared_ptr<Channel> MakeChannel(const string& addr)
{
  ChannelArguments args;
  args.SetInt("grpc.tcp_nodelay", 1);
  args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 40000);
  args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
  return grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);
}

class KvstoreClient {
 public:
  KvstoreClient(const string& manager_addr)
  {
    auto channel = MakeChannel(manager_addr);
    auto manager_stub = Manager::NewStub(channel);

    // Retry until all servers are registered
    while(true)
    {
      ClusterInfoRequest req;
      ClusterInfoResponse res;
      ClientContext ctx;
      Status s = manager_stub->GetClusterInfo(&ctx, req, &res);
      if(s.ok())
      {
        num_servers = res.num_servers();
        for(const auto& s: res.servers())
        {
          stubs_[s.server_id()] = Kvstore::NewStub(MakeChannel(s.address()));
        }
        cout << "Connected to cluster: " << num_servers << " servers" << endl;
        return;
      }
      cerr << "Waiting for cluster to be ready... " << s.error_message() << endl;
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

    auto status = CallWithRetry(key, [&](Kvstore::Stub* stub)
    {
      ClientContext context;
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
      cout << status.error_code() << ": " << status.error_message() << endl;
    }
  }

  void Swap(const string& key, const string& new_value) {
    SwapRequest request;
    request.set_key(key);
    request.set_new_value(new_value);

    SwapResponse response;
 
    auto status = CallWithRetry(key, [&](Kvstore::Stub* stub)
    {
      ClientContext context;
      return stub->Swap(&context, request, &response);
    });
    
    if(status.ok()) {
      if(response.has_old_value()) {
        cout << "SWAP " << key <<  " " << response.old_value() << endl; 
      } else {
       cout << "SWAP " << key <<  " null\n";  
      }
    } else {
      cout << status.error_code() << ":" << status.error_message() << endl;
    }
  }

  void Get(const string& key) {
    GetRequest request;
    request.set_key(key);
    
    GetResponse response;

    auto status = CallWithRetry(key, [&](Kvstore::Stub* stub)
    {
      ClientContext context;
      return stub->Get(&context, request, &response);
    });

    if(status.ok()) {
      if(response.has_value()) {
        cout << "GET " << key <<  " " << response.value() << endl; 
      } else {
        cout << "GET " << key <<  " null\n"; 
      }    
    } else {
      cout << status.error_code() << ":" << status.error_message() << endl;
    }
  }

  void Scan(const string& start_key, const string& end_key) {
    ScanRequest request;
    request.set_start_key(start_key);
    request.set_end_key(end_key);

    ScanResponse response;

    // Fan out to all servers and merge results into sorted map
    map<string, string> merged;
    for(auto& [id, stub] : stubs_)
    {
      ScanResponse response;
      while(true)
      {
        ClientContext context;
        Status status = stub->Scan(&context, request, &response);
        if(status.ok())
        {
          for(const auto& pair : response.pairs())
          {
            merged[pair.key()] = pair.value();
          }
          break;
        }

        cerr << "Scan to server " << id << " failed, retrying ..." << endl;
        this_thread::sleep_for(chrono::milliseconds(500));
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
    auto status = CallWithRetry(key, [&](Kvstore::Stub* stub) 
    {  
      ClientContext ctx;
      return stub->Delete(&ctx, request, &response);
    });

    if(status.ok()) {
      if (response.found()) {
        cout << "DELETE " << key << " found\n";
      } else {
        cout << "DELETE " << key << " not_found\n";
      }
    } else {
      cout << status.error_code() << ": " << status.error_message() << endl;
    }
  }

 private:
  int num_servers;
  map<int, unique_ptr<Kvstore::Stub>> stubs_;

  // Hash function to get server id
  int GetServerId(const string& key)
  {
    return hash<string>{}(key) % num_servers;
  }

  // Route to correct server, retry on failure
  template <typename F> Status CallWithRetry(const string& key, F rpc_call)
  {
    int server_id = GetServerId(key);
    while(true)
    {
      Status s = rpc_call(stubs_[server_id].get());
      if(s.ok())
      {
        return s;
      }
      cerr << "RPC failed, retrying ... (" << s.error_message() << ")" << endl;
      this_thread::sleep_for(chrono::milliseconds(500));
    }
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
  string manager_addr = absl::GetFlag(FLAGS_manager_addr);

  KvstoreClient kvstore(manager_addr);
  
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