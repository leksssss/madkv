# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/users/lekha/madkv/kvstore/cmake/_deps/grpc-src"
  "/users/lekha/madkv/kvstore/cmake/_deps/grpc-build"
  "/users/lekha/madkv/kvstore/cmake/_deps/grpc-subbuild/grpc-populate-prefix"
  "/users/lekha/madkv/kvstore/cmake/_deps/grpc-subbuild/grpc-populate-prefix/tmp"
  "/users/lekha/madkv/kvstore/cmake/_deps/grpc-subbuild/grpc-populate-prefix/src/grpc-populate-stamp"
  "/users/lekha/madkv/kvstore/cmake/_deps/grpc-subbuild/grpc-populate-prefix/src"
  "/users/lekha/madkv/kvstore/cmake/_deps/grpc-subbuild/grpc-populate-prefix/src/grpc-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/users/lekha/madkv/kvstore/cmake/_deps/grpc-subbuild/grpc-populate-prefix/src/grpc-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/users/lekha/madkv/kvstore/cmake/_deps/grpc-subbuild/grpc-populate-prefix/src/grpc-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
