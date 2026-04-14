set working-directory := '..'

# list project 3 just recipes
default:
    @just --list p3 --unsorted

# install extra dependencies of your KV code
deps:
    sudo apt update
    sudo apt install tree just default-jre liblog4j2-java
    sudo apt update
    sudo apt install protobuf-compiler libprotobuf-dev
    sudo apt install cmake
    sudo apt install libabsl-dev
    cargo install just
    sudo apt-get install librocksdb-dev rocksdb-tools
    sudo apt update
    sudo apt install protobuf-compiler-grpc

    sudo apt install -y \
    protobuf-compiler \
    libprotobuf-dev \
    libprotoc-dev \
    libgrpc++-dev \
    libgrpc-dev \
    pkg-config

# build your executables in release mode
build:
    mkdir -p build-kvstore
    cmake -S kvstore -B build-kvstore -DCMAKE_BUILD_TYPE=Release
    cmake --build build-kvstore -- -j"$(nproc)"

# clean the build of your executables
clean:
    rm -rf build-kvstore
    mkdir -p build-kvstore

clear_storage:
    rm -rf /tmp/backer.s0
    rm -rf /tmp/backer.s1
    rm -rf /tmp/backer.s2
    rm -rf /tmp/backer.s3
    rm -rf /tmp/backer.s4
    rm -rf ./backer.*

# run your KV store manager replica
manager man_listen="0.0.0.0:3666" \
        man_p2p_listen="0.0.0.0:3606" \
        peer_addrs="none" \
        server_rf="5" \
        server_addrs="127.0.0.1:3777,127.0.0.1:3778,127.0.0.1:3779,127.0.0.1:3780,127.0.0.1:3781" \
        backer_path="./backer.m.0":
    ./build-kvstore/kvstore_manager --man_listen {{man_listen}} --server_rf {{server_rf}} --server_addrs {{server_addrs}} --backer_path {{backer_path}}

# run your KV store server replica
server partition_id="0" \
       replica_id="0" \
       manager_addrs="127.0.0.1:3666" \
       api_port="3777" \
       p2p_port="3707" \
       peer_addrs="none" \
       backer_path="./backer.s0.0":
    ./build-kvstore/kvstore_server --partition_id {{partition_id}} --replica_id {{replica_id}} --manager_addrs {{manager_addrs}} --api_listen 0.0.0.0:{{api_port}} --p2p_listen 0.0.0.0:{{p2p_port}} --peer_addrs {{peer_addrs}} --backer_path {{backer_path}}

# run you KV store client in stdin/out interface mode
client manager_addrs="127.0.0.1:3666":
    ./build-kvstore/kvstore_client --manager_addrs {{manager_addrs}}

# start 3 replicated managers (for testing with multiple managers)
manager0:
    ./build-kvstore/kvstore_manager --man_listen 0.0.0.0:3666 --server_rf 5 --server_addrs 127.0.0.1:3777,127.0.0.1:3778,127.0.0.1:3779,127.0.0.1:3780,127.0.0.1:3781 --backer_path ./backer.m.0

manager1:
    ./build-kvstore/kvstore_manager --man_listen 0.0.0.0:3667 --server_rf 5 --server_addrs 127.0.0.1:3777,127.0.0.1:3778,127.0.0.1:3779,127.0.0.1:3780,127.0.0.1:3781 --backer_path ./backer.m.1

manager2:
    ./build-kvstore/kvstore_manager --man_listen 0.0.0.0:3668 --server_rf 5 --server_addrs 127.0.0.1:3777,127.0.0.1:3778,127.0.0.1:3779,127.0.0.1:3780,127.0.0.1:3781 --backer_path ./backer.m.2
    
managers:
    just p3::manager0 > /tmp/manager0.log 2>&1 &
    sleep 2
    just p3::manager1 > /tmp/manager1.log 2>&1 &
    sleep 2
    just p3::manager2 > /tmp/manager2.log 2>&1 &
    sleep 3
    echo "3 replicated managers started"

# start 3 replicated servers (partition 0, replicas 0-2)
server0:
    ./build-kvstore/kvstore_server --partition_id 0 --replica_id 0 --manager_addrs 127.0.0.1:3666 --api_listen 0.0.0.0:3777 --p2p_listen 0.0.0.0:3707 --peer_addrs 127.0.0.1:3708,127.0.0.1:3709,127.0.0.1:3710,127.0.0.1:3711 --backer_path ./backer.s0.0

server1:
    ./build-kvstore/kvstore_server --partition_id 0 --replica_id 1 --manager_addrs 127.0.0.1:3666 --api_listen 0.0.0.0:3778 --p2p_listen 0.0.0.0:3708 --peer_addrs 127.0.0.1:3707,127.0.0.1:3709,127.0.0.1:3710,127.0.0.1:3711 --backer_path ./backer.s0.1

server2:
    ./build-kvstore/kvstore_server --partition_id 0 --replica_id 2 --manager_addrs 127.0.0.1:3666 --api_listen 0.0.0.0:3779 --p2p_listen 0.0.0.0:3709 --peer_addrs 127.0.0.1:3707,127.0.0.1:3708,127.0.0.1:3710,127.0.0.1:3711 --backer_path ./backer.s0.2

server3:
    ./build-kvstore/kvstore_server --partition_id 0 --replica_id 3 --manager_addrs 127.0.0.1:3666 --api_listen 0.0.0.0:3780 --p2p_listen 0.0.0.0:3710 --peer_addrs 127.0.0.1:3707,127.0.0.1:3708,127.0.0.1:3709,127.0.0.1:3711 --backer_path ./backer.s0.3

server4:
    ./build-kvstore/kvstore_server --partition_id 0 --replica_id 4 --manager_addrs 127.0.0.1:3666 --api_listen 0.0.0.0:3781 --p2p_listen 0.0.0.0:3711 --peer_addrs 127.0.0.1:3707,127.0.0.1:3708,127.0.0.1:3709,127.0.0.1:3710 --backer_path ./backer.s0.4
servers:
    just p3::server0 > /tmp/server0.log 2>&1 &
    sleep 2
    just p3::server1 > /tmp/server1.log 2>&1 &
    sleep 2
    just p3::server2 > /tmp/server2.log 2>&1 &
    sleep 2
    just p3::server3 > /tmp/server3.log 2>&1 &
    sleep 2
    just p3::server4 > /tmp/server4.log 2>&1 &
    sleep 3
    echo "5 replicated servers started"
    ps aux | grep kvstore_server | grep -v grep

# kill all processes of your KV store system
kill:
    -pkill -f kvstore_manager
    -pkill -f kvstore_server
    -pkill -f kvstore_client
    rm -rf backer.*

# NOTE: feel free to add more recipes as you see fit...
#       also feel free to add extra parameters to the recipes as you see fit,
#       but don't change the existing parameters

python_run := if `which uv || true` != "" { "uv run" } else { "python3" }
tmpdir_prefix := "/tmp/madkv-p3"

# launch the KV service components on node (see README for conventions)
service node="s0.0" \
        managers="127.0.0.1:3666,127.0.0.1:3667,127.0.0.1:3668" \
        manager_p2ps="127.0.0.1:3606,127.0.0.1:3607,127.0.0.1:3608" \
        server_rf="3" \
        servers="127.0.0.1:3777,127.0.0.1:3778,127.0.0.1:3779,127.0.0.1:3780,127.0.0.1:3781,127.0.0.1:3782" \
        server_p2ps="127.0.0.1:3707,127.0.0.1:3708,127.0.0.1:3709,127.0.0.1:3710,127.0.0.1:3711,127.0.0.1:3712" \
        backer_prefix="./backer":
    just p3::build
    just utils::build
    cargo run -p runner -r --bin service -- \
        --server-just-args p3::server \
            $({{python_run}} justmod/xtract.py partid "{{node}}") \
            $({{python_run}} justmod/xtract.py repid "{{node}}") \
            "{{managers}}" \
            $({{python_run}} justmod/xtract.py portof "{{servers}}" "{{node}}" "{{server_rf}}") \
            $({{python_run}} justmod/xtract.py portof "{{server_p2ps}}" "{{node}}" "{{server_rf}}") \
            $({{python_run}} justmod/xtract.py peersof "{{server_p2ps}}" "{{node}}" "{{server_rf}}") \
            "{{backer_prefix}}.{{node}}" \
        --manager-just-args p3::manager \
            $({{python_run}} justmod/xtract.py repid "{{node}}") \
            $({{python_run}} justmod/xtract.py portof "{{managers}}" "{{node}}") \
            $({{python_run}} justmod/xtract.py portof "{{manager_p2ps}}" "{{node}}") \
            $({{python_run}} justmod/xtract.py peersof "{{manager_p2ps}}" "{{node}}") \
            "{{server_rf}}" \
            "{{servers}}" \
            "{{backer_prefix}}.{{node}}" \
        --node-id "{{node}}"

# ensure a subdir under 'tmp/' exists
tmpdir subdir:
    mkdir -p "{{tmpdir_prefix}}/{{subdir}}"

# run a fuzz testing scenario
fuzz server_rf="5" crashing="no" \
     managers="127.0.0.1:3666,127.0.0.1:3667,127.0.0.1:3668": (tmpdir "fuzz")
    just p3::build
    just utils::build
    cargo run -p runner -r --bin fuzzer -- \
        --num-clis 5 \
        --conflict \
        --client-just-args p3::client "{{managers}}" \
        | tee "{{tmpdir_prefix}}/fuzz/fuzz-{{server_rf}}-{{crashing}}.log"
    just p3::kill

# run a YCSB benchmark workload
bench nclis wload server_rf="3" \
      managers="127.0.0.1:3666": (tmpdir "bench")
    just p3::build
    just utils::build
    just utils::ycsb
    cargo run -p runner -r --bin bencher -- \
        --num-clis "{{nclis}}" \
        --workload "{{wload}}" \
        --client-just-args p3::client "{{managers}}" \
        | tee "{{tmpdir_prefix}}/bench/bench-{{nclis}}-{{wload}}-{{server_rf}}.log"
    # just p3::kill

# comprehensive build and test: compile, start cluster, and run tests
build_and_test:
    #!/bin/bash
    set -e
    
    echo "=== Building project ==="
    just p3::build
    
    echo ""
    echo "=== Killing any existing processes ==="
    pkill -f kvstore_manager || true
    pkill -f kvstore_server || true
    pkill -f kvstore_client || true
    sleep 1
    
    echo ""
    echo "=== Clearing old storage ==="
    rm -rf ./backer.m.0 ./backer.s0.* 2>/dev/null || true
    
    echo ""
    echo "=== Starting manager ==="
    just p3::manager > /tmp/manager.log 2>&1 &
    sleep 2
    
    echo ""
    echo "=== Starting 3-server cluster ==="
    just p3::server0 > /tmp/server0.log 2>&1 &
    sleep 2
    just p3::server1 > /tmp/server1.log 2>&1 &
    sleep 2
    just p3::server2 > /tmp/server2.log 2>&1 &
    sleep 3
    
    echo ""
    echo "=== Checking server logs for leader election ==="
    sleep 5
    echo "Server 0 logs (tail):"
    tail -5 /tmp/server0.log
    echo "Server 1 logs (tail):"
    tail -5 /tmp/server1.log
    echo "Server 2 logs (tail):"
    tail -5 /tmp/server2.log
    
    echo ""
    echo "=== Running KV tests ==="
    sleep 2
    python3 /tmp/test_kv.py 2>&1 || true
    
    echo ""
    echo "=== Test completed ==="
    echo "Manager log: /tmp/manager.log"
    echo "Server 0 log: /tmp/server0.log"
    echo "Server 1 log: /tmp/server1.log"
    echo "Server 2 log: /tmp/server2.log"

# generate .md report template from existing results (wip)
report:
    {{python_run}} sumgen/proj3.py

# ==================== BENCHMARK RECIPES ====================

# Workloads A-F with 10 clients, varying replication factor (1, 3, 5)
bench_all_workloads_10cli rf="3" managers="127.0.0.1:3666,127.0.0.1:3667,127.0.0.1:3668": (tmpdir "bench")
    #!/bin/bash
    set -e
    
    echo "=== Clearing old storage ==="
    pkill -f kvstore_manager || true
    pkill -f kvstore_server || true
    sleep 1
    rm -rf ./backer.m.* ./backer.s0.* 2>/dev/null || true
    
    echo "=== Starting 3 managers ==="
    just p3::manager0 > /tmp/manager0.log 2>&1 &
    sleep 2
    just p3::manager1 > /tmp/manager1.log 2>&1 &
    sleep 2
    just p3::manager2 > /tmp/manager2.log 2>&1 &
    sleep 3
    
    echo "=== Starting {{rf}} servers ==="
    if [ "{{rf}}" -ge 1 ]; then just p3::server0 > /tmp/server0.log 2>&1 &; sleep 2; fi
    if [ "{{rf}}" -ge 2 ]; then just p3::server1 > /tmp/server1.log 2>&1 &; sleep 2; fi
    if [ "{{rf}}" -ge 3 ]; then just p3::server2 > /tmp/server2.log 2>&1 &; sleep 2; fi
    if [ "{{rf}}" -ge 4 ]; then just p3::server3 > /tmp/server3.log 2>&1 &; sleep 2; fi
    if [ "{{rf}}" -ge 5 ]; then just p3::server4 > /tmp/server4.log 2>&1 &; sleep 2; fi
    sleep 3
    
    echo "=== Benchmarking all workloads A-F with 10 clients, RF={{rf}} ==="
    for wload in a b c d e f; do
        echo ""
        echo "--- Running Workload $wload with RF={{rf}} ---"
        just p3::bench 10 "$wload" {{rf}} {{managers}}
    done
    echo ""
    echo "=== All workload benchmarks completed ==="

# Workload a with varying client counts (1-30), RF=1
bench_workload_a_rf1: (tmpdir "bench")
    #!/bin/bash
    set -e
    echo "=== Benchmarking Workload A with RF=1, scaling clients 1->30 ==="
    for ncli in 1 5 10 15 20 25 30; do
        echo ""
        echo "--- Running Workload A with $ncli clients, RF=1 ---"
        just p3::bench "$ncli" A 1 127.0.0.1:3666
    done
    echo ""
    echo "=== Workload A RF=1 scaling completed ==="

# Workload A with varying client counts (1-30), RF=5
bench_workload_a_rf5: (tmpdir "bench")
    #!/bin/bash
    set -e
    echo "=== Benchmarking Workload A with RF=5, scaling clients 1->30 ==="
    for ncli in 10; do
        echo ""
        echo "--- Running Workload A with $ncli clients, RF=5 ---"
        just p3::kill; \
        sleep 2; \
        just p3::manager & \
        sleep 2; \
        just p3::servers & \
        sleep 10; \
        just p3::bench "$ncli" f 5 127.0.0.1:3666
    done
    echo ""
    echo "=== Workload A RF=5 scaling completed ==="

# Comprehensive benchmark: workloads A-F at RF 1,3,5 with 10 clients each
bench_comprehensive: (tmpdir "bench")
    #!/bin/bash
    set -e
    echo "=== Comprehensive Benchmark: Workloads A-F, RF 1/3/5, 10 clients ==="
    for rf in 1 3 5; do
        echo ""
        echo "========== REPLICATION FACTOR: $rf =========="
        for wload in A B C D E F; do
            echo ""
            echo "--- Workload $wload (RF=$rf, 10 clients) ---"
            just p3::bench 10 "$wload" "$rf" 127.0.0.1:3666,127.0.0.1:3667,127.0.0.1:3668
            sleep 2
        done
    done
    echo ""
    echo "=== Comprehensive benchmark completed ==="


manager_rf1:
    ./build-kvstore/kvstore_manager --man_listen 0.0.0.0:3666 --server_rf 1 \
        --server_addrs 127.0.0.1:3777 --backer_path ./backer.m.0

manager_rf3:
    ./build-kvstore/kvstore_manager --man_listen 0.0.0.0:3666 --server_rf 3 \
        --server_addrs 127.0.0.1:3777,127.0.0.1:3778,127.0.0.1:3779 \
        --backer_path ./backer.m.0

bench_rf1_rf3: (tmpdir "bench")
    #!/bin/bash
    set -e
    for rf in 1 3; do
        just p3::kill; \
        sleep 2; \
        just p3::manager_rf${rf} & \
        sleep 2; \
        just p3::servers & \
        sleep 25; \
        for wload in a b c d e f; do
            echo "--- Workload $wload RF=$rf ---"
            just p3::bench 10 "$wload" "$rf" 127.0.0.1:3666
            sleep 2
        done
    done

bench_all_workloads_10: (tmpdir "bench")
    #!/bin/bash
    set -e
    for rf in 1 3; do
        echo "========== RF=$rf =========="
        just p3::kill; \
        sleep 2; \
        just p3::manager server_rf="$rf" > /tmp/manager.log 2>&1 & \
        sleep 2; \
        just p3::servers > /tmp/servers.log 2>&1 & \
        sleep 15; \
        for wload in a b c d e f; do
            echo "--- Workload $wload RF=$rf ---"
            just p3::bench 10 "$wload" "$rf" 127.0.0.1:3666
            sleep 2
        done
    done
    just p3::kill
    echo "=== done ==="
