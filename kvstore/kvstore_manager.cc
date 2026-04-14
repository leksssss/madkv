#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <iostream>
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <sstream>

#include "absl/log/initialize.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"

#include "manager.pb.h"
#include "manager.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using manager::Manager;
using manager::RegisterRequest;
using manager::RegisterResponse;
using manager::ClusterInfoRequest;
using manager::ClusterInfoResponse;
using manager::ServerInfo;
using namespace std;

ABSL_FLAG(string,  man_listen,    "0.0.0.0:3666", "Listen address for client/server requests");
ABSL_FLAG(int32_t, server_rf,     1,              "Replication factor for partition servers");
ABSL_FLAG(string,  server_addrs,  "",             "All server API addresses ordered by partition then replica");
ABSL_FLAG(string,  backer_path,   "./backer.m",   "Durable storage path (bonus)");

vector<string> SplitByComma(const string& s)
{
    vector<string> result;
    if (s == "none" || s.empty()) return result;
    stringstream ss(s);
    string token;
    while(getline(ss, token, ','))
    {
        if(!token.empty())
        {
            result.push_back(token);
        }
    }
    return result;
}

class ManagerServiceImpl final : public Manager::Service
{
    private:
        // replication factor for each partition's server group
        int server_rf;
        // server_addrs[partition][replica] = api_address
        vector<vector<string>> server_addrs;
        // num of partitions = total server count / server_rf
        int num_partitions;
        map<int, string> server_registry;
        mutex registry_mutex;
    
    public:
        ManagerServiceImpl(int server_rf, const vector<string>& addrs) : server_rf(server_rf) {
            num_partitions = addrs.size() / server_rf;
            // Reshape list into server_addrs[partition][replica]
            server_addrs.resize(num_partitions);
            for (int p = 0; p < num_partitions; p++) {
                server_addrs[p].resize(server_rf);
                for (int r = 0; r < server_rf; r++) {
                    server_addrs[p][r] = addrs[p * server_rf + r];
                }
            }
            cout << "Manager: " << num_partitions << " partitions, rf=" << server_rf << "\n";
        }

        Status RegisterServer(ServerContext* context, const RegisterRequest* request, RegisterResponse* response) override 
        {
            lock_guard<mutex> lock(registry_mutex);
            int id = request->server_id();
            string addr = request->address();
   
            server_registry[id] = addr;
            cout << "Server " << id << "registered at " << addr << " (" << server_registry.size() << "/" << num_partitions * server_rf << ")\n" << endl;
            
            response->set_num_servers(num_partitions * server_rf);
            return Status::OK;
        }

        // Returns all partition replica addresses so the client can:
        //   1. Hash a key to a partition
        //   2. Try each replica in the partition to find the Raft leader
        Status GetClusterInfo(ServerContext* context, const ClusterInfoRequest* request, ClusterInfoResponse* response) override
        {
            response->set_num_servers(num_partitions);
            
            for (int p = 0; p < num_partitions; p++) {
                for (int r = 0; r < server_rf; r++) {
                    ServerInfo* info = response->add_servers();
                
                    info->set_server_id(p * server_rf + r);
                    info->set_address(server_addrs[p][r]);
                }
            }
            return Status::OK;
        }
};

void RunManager(const string& listen_addr, int server_rf, const vector<string>& addrs)
{
    ManagerServiceImpl service(server_rf, addrs);

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    ServerBuilder builder;
    builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    unique_ptr<Server> server(builder.BuildAndStart());
    cout << "Manager listening on " << listen_addr << endl;

    server->Wait();

}

int main(int argc, char** argv)
{
    absl::ParseCommandLine(argc, argv);
    absl::InitializeLog();

    string listen_addr = absl::GetFlag(FLAGS_man_listen);
    int server_rf  = absl::GetFlag(FLAGS_server_rf);
    string servers_flag = absl::GetFlag(FLAGS_server_addrs);

    if (servers_flag.empty()) {
        cerr << "Error: --server_addrs is required\n";
        return 1;
    }

    vector<string> server_addrs = SplitByComma(servers_flag);

    // Sanity check: total addresses must be divisible by replication factor
    if ((int)server_addrs.size() % server_rf != 0) {
        cerr << "Error: " << server_addrs.size() << " server addresses"
             << " not divisible by server_rf=" << server_rf << "\n";
        return 1;
    }

    RunManager(listen_addr, server_rf, server_addrs);
    return 0;
}