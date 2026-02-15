#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>
#include <fstream>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "cmake/build/kvstore.pb.h"
#include "cmake/build/kvstore.grpc.pb.h"

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
using namespace std;

ABSL_FLAG(string, target, "localhost:3777", "Server address");

class KvstoreClient {
 public:
  KvstoreClient(shared_ptr<Channel> channel)
      : stub_(Kvstore::NewStub(channel)) {}

  // Assembles the client's payload, sends it and presents the response back
  // from the server.
  void Put(const string& key, const string& value) {
    // Data we are sending to the server.
    PutRequest request;
    request.set_key(key);
    request.set_new_value(value);

    // Container for the data we expect from the server.
    PutResponse response;

    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    ClientContext context;

    // The actual RPC.
    Status status = stub_->Put(&context, request, &response);

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
    ClientContext context;

    Status status = stub_->Swap(&context, request, &response);
    
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
    ClientContext context;

    Status status = stub_->Get(&context, request, &response);

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
    ClientContext context;

    Status status = stub_->Scan(&context, request, &response);

    if (status.ok()) {
      cout << "SCAN " << start_key << " " << end_key << " BEGIN\n";
      for (const auto& pair : response.pairs()) {
        cout << "\t" << pair.key() << " " << pair.value() << "\n";
      }
      cout << "SCAN END\n";
    } else {
      cout << status.error_code() << ": " << status.error_message() << endl;
    }
  }

  void Delete(const string& key) {
    DeleteRequest request;
    request.set_key(key);

    DeleteResponse response;
    ClientContext context;

    Status status = stub_->Delete(&context, request, &response);

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
  unique_ptr<Kvstore::Stub> stub_;
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

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  // Instantiate the client. It requires a channel, out of which the actual RPCs
  // are created. This channel models a connection to an endpoint specified by
  // the argument "--target=" which is the only expected argument.
  string target_str = absl::GetFlag(FLAGS_target);
  // Setting channel args
  ChannelArguments channel_args;
  channel_args.SetInt("grpc.tcp_nodelay", 1);
  channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 40000);
  channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
  // We indicate that the channel isn't authenticated (use of
  // InsecureChannelCredentials()).
  KvstoreClient kvstore(
      grpc::CreateCustomChannel(target_str, grpc::InsecureChannelCredentials(), channel_args));

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