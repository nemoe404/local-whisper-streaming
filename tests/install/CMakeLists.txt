# Out-of-tree consumer of an installed lws. This project is never added as a subdirectory of the
# main build: it is copied out of the repository and configured against an install prefix, through
# find_package(lws).
cmake_minimum_required(VERSION 3.29)
project(lws_install_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The version the consumer asks for. The package's version file accepts 0.1 and rejects 0.2.
set(LWS_WANT_VERSION 0.1 CACHE STRING "lws package version to request")
find_package(lws ${LWS_WANT_VERSION} CONFIG REQUIRED)

add_executable(consumer consumer.cpp)
# The package brings the whisper and ggml archives and fmt with it. The consumer adds no flags by hand.
target_link_libraries(consumer PRIVATE lws::lws)
