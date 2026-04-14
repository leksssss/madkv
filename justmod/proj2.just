set working-directory := '..'
set shell := ["bash", "-cu"]

# list project 2 just recipes
default:
    @just --list p2 --unsorted

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

# run your KV store manager
manager man_port="3666" servers="127.0.0.1:3777,127.0.0.1:3778,127.0.0.1:3779":
    ./build-kvstore/kvstore_manager --man_listen 0.0.0.0:{{man_port}} --servers {{servers}}

manager5 man_port="3666" servers="127.0.0.1:3775,127.0.0.1:3776,127.0.0.1:3777,127.0.0.1:3778,127.0.0.1:3779":
    ./build-kvstore/kvstore_manager --man_listen 0.0.0.0:{{man_port}} --servers {{servers}}

# run a single KV store server
server id="0" manager="127.0.0.1:3666" api_port="3777" backer_path="/tmp/backer.s0":
    ./build-kvstore/kvstore_server --manager_addr {{manager}} --api_listen 0.0.0.0:{{api_port}} --server_id {{id}} --backer_path {{backer_path}}

# launch all 3 servers at once in the background
servers manager="127.0.0.1:3666":
    ./build-kvstore/kvstore_server --manager_addr {{manager}} --api_listen 0.0.0.0:3777 --server_id 0 --backer_path /tmp/backer.s0 &
    ./build-kvstore/kvstore_server --manager_addr {{manager}} --api_listen 0.0.0.0:3778 --server_id 1 --backer_path /tmp/backer.s1 &
    ./build-kvstore/kvstore_server --manager_addr {{manager}} --api_listen 0.0.0.0:3779 --server_id 2 --backer_path /tmp/backer.s2 &

servers5 manager="127.0.0.1:3666":
    ./build-kvstore/kvstore_server --manager_addr {{manager}} --api_listen 0.0.0.0:3775 --server_id 3 --backer_path /tmp/backer.s3 &
    ./build-kvstore/kvstore_server --manager_addr {{manager}} --api_listen 0.0.0.0:3776 --server_id 4 --backer_path /tmp/backer.s4 &
    ./build-kvstore/kvstore_server --manager_addr {{manager}} --api_listen 0.0.0.0:3777 --server_id 0 --backer_path /tmp/backer.s0 &
    ./build-kvstore/kvstore_server --manager_addr {{manager}} --api_listen 0.0.0.0:3778 --server_id 1 --backer_path /tmp/backer.s1 &
    ./build-kvstore/kvstore_server --manager_addr {{manager}} --api_listen 0.0.0.0:3779 --server_id 2 --backer_path /tmp/backer.s2 &

# run your KV store client in stdin/out interface mode
client manager="127.0.0.1:3666":
    ./build-kvstore/kvstore_client --manager_addr {{manager}}

# kill all processes of your KV store system
kill:
    -pkill -f kvstore_manager
    -pkill -f kvstore_server
    -pkill -f kvstore_client
    
# NOTE: feel free to add more recipes as you see fit...
#       also feel free to add extra parameters to the recipes as you see fit,
#       but don't change the existing parameters

python_run := if `which uv || true` != "" { "uv run" } else { "python3" }
tmpdir_prefix := "/tmp/madkv-p2"

# launch the KV service components on node (see README for conventions)
service node="s0" manager="127.0.0.1:3666" servers="127.0.0.1:3777,127.0.0.1:3778,127.0.0.1:3779" backer_prefix="./backer":
    just p2::build
    just utils::build
    cargo run -p runner -r --bin service -- \
        --server-just-args p2::server \
            $({{python_run}} justmod/xtract.py partid "{{node}}") \
            "{{manager}}" \
            $({{python_run}} justmod/xtract.py portof "{{servers}}" "{{node}}") \
            "{{backer_prefix}}.{{node}}" \
        --manager-just-args p2::manager \
            $({{python_run}} justmod/xtract.py portof "{{manager}}" "{{node}}") \
            "{{servers}}" \
        --node-id "{{node}}"

# ensure a subdir under 'tmp/' exists
tmpdir subdir:
    mkdir -p "{{tmpdir_prefix}}/{{subdir}}"

# run a fuzz testing scenario
fuzz nservers="3" crashing="no" manager="127.0.0.1:3666": (tmpdir "fuzz")
    just p2::build
    just utils::build
    cargo run -p runner -r --bin fuzzer -- \
        --num-clis 5 \
        --conflict \
        --client-just-args p2::client "{{manager}}" \
        | tee "{{tmpdir_prefix}}/fuzz/fuzz-{{nservers}}-{{crashing}}.log"
    just p2::kill

# run a YCSB benchmark workload
bench nclis wload nservers="1" manager="127.0.0.1:3666": (tmpdir "bench")
    just p2::build
    just utils::build
    just utils::ycsb
    cargo run -p runner -r --bin bencher -- \
        --num-clis "{{nclis}}" \
        --workload "{{wload}}" \
        --client-just-args p2::client "{{manager}}" \
        | tee "{{tmpdir_prefix}}/bench/bench-{{nclis}}-{{wload}}-{{nservers}}.log"
    just p2::kill

# generate .md report template from existing results (wip)
report:
    {{python_run}} sumgen/proj2.py

manager1 man_port="3666" servers="127.0.0.1:3777":
    ./build-kvstore/kvstore_manager --man_listen 0.0.0.0:{{man_port}} --servers {{servers}}

manager3 man_port="3666":
    ./build-kvstore/kvstore_manager --man_listen 0.0.0.0:{{man_port}} --servers 127.0.0.1:3777,127.0.0.1:3778,127.0.0.1:3779

servers1 manager="127.0.0.1:3666":
    ./build-kvstore/kvstore_server --manager_addr {{manager}} --api_listen 0.0.0.0:3777 --server_id 0 --backer_path /tmp/backer.s0

# runs all bench tests necessary for report
bench-set1 manager="127.0.0.1:3666":
    #!/bin/bash
    for s in 1 3 5; do \
        for w in a b c d e f; do \
            just p2::kill; \
            sleep 2; \
            just p2::clear_storage; \
            if [ $s -eq 1 ]; then \
                just p2::manager1 & \
                sleep 1; \
                just p2::servers1 & \
            elif [ $s -eq 3 ]; then \
                just p2::manager3 & \
                sleep 1; \
                just p2::servers & \
            else \
                just p2::manager5 & \
                sleep 1; \
                just p2::servers5 & \
            fi; \
            sleep 2; \
            just p2::bench 10 $w $s {{manager}}; \
            just p2::kill; \
            sleep 2; \
        done; \
    done

bench-set2 manager="127.0.0.1:3666":
    #!/bin/bash
    for s in 1 5; do \
        for n in 1 20 30; do \
            just p2::kill; \
            sleep 2; \
            just p2::clear_storage; \
            if [ $s -eq 1 ]; then \
                just p2::manager1 & \
                sleep 1; \
                just p2::servers1 & \
            else \
                just p2::manager5 & \
                sleep 1; \
                just p2::servers5 & \
            fi; \
            sleep 2; \
            just p2::bench $n a $s {{manager}}; \
            just p2::kill; \
            sleep 2; \
        done; \
    done

run_bench manager="127.0.0.1:3666":
    just p2::kill
    sleep 2
    just p2::clear_storage
    just p2::manager3  & \
    sleep 2
    just p2::servers  & \
    sleep 2
    just p2::bench 10 f 3 {{manager}}
    just p2::kill