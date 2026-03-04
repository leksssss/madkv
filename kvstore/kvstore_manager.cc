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

ABSL_FLAG(string, man_listen, "0.0.0.0:3666", "Manager listen address");
ABSL_FLAG(string, servers, "", "Comma-separated list of server addresses");

vector<string> SplitByComma(const string& s)
{
    vector<string> result;
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
        int num_servers;
        map<int, string> server_registry;
        mutex registry_mutex;
    
    public:
        ManagerServiceImpl(int num_servers) : num_servers(num_servers) {}

        Status RegisterServer(ServerContext* context, const RegisterRequest* request, RegisterResponse* response) override 
        {
            lock_guard<mutex> lock(registry_mutex);
            int id = request->server_id();
            string addr = request->address();
            if(id < 0 || id >= num_servers) 
            {
                return Status(grpc::StatusCode::INVALID_ARGUMENT, "server_id out of range");
            }
            server_registry[id] = addr;
            cout << "Server " << id << "registered at " << addr << " (" << server_registry.size() << "/" << num_servers << ")" << endl;
            
            response->set_num_servers(num_servers);
            return Status::OK;
        }

        Status GetClusterInfo(ServerContext* context, const ClusterInfoRequest* request, ClusterInfoResponse* response) override
        {
            lock_guard<mutex> lock(registry_mutex);

            int sz = (int)server_registry.size();
            if(sz < num_servers)
            {
                return Status(grpc::StatusCode::UNAVAILABLE, "All servers not registered yet");
            }

            response->set_num_servers(num_servers);
            for(auto& [id, addr] : server_registry)
            {
                ServerInfo* s = response->add_servers();
                s->set_server_id(id);
                s->set_address(addr);
            }
            return Status::OK;
        }
};

void RunManager(const string& listen_addr, int num_servers)
{
    ManagerServiceImpl service(num_servers);

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    ServerBuilder builder;
    builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    unique_ptr<Server> server(builder.BuildAndStart());
    cout << "Manager listening on " << listen_addr << " with " << num_servers << "servers" << endl;

    server->Wait();

}

int main(int argc, char** argv)
{
    absl::ParseCommandLine(argc, argv);
    absl::InitializeLog();

    string listen_addr = absl::GetFlag(FLAGS_man_listen);
    string servers_flag = absl::GetFlag(FLAGS_servers);

    if(servers_flag.empty())
    {
        cerr << "Error: --servers flag is required" << endl;
        return 1;
    }

    vector<string> server_addrs = SplitByComma(servers_flag);
    int num_servers = server_addrs.size();

    RunManager(listen_addr, num_servers);
    return 0;
}