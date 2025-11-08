cmake_minimum_required(VERSION 3.16)
project(TCPStreamer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required system libraries
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)

# Find zstd library
find_library(ZSTD_LIBRARY zstd)
if(NOT ZSTD_LIBRARY)
    message(FATAL_ERROR "zstd library not found")
endif()

# Include directories
include_directories(.)
include_directories(../src/include)
include_directories(../databento-cpp/include)
include_directories(../databento-cpp/build/include)
include_directories(../databento-cpp/build/_deps/date_src-src/include)
include_directories(../databento-cpp/build/_deps/httplib-src)
include_directories(../databento-cpp/build/_deps/json-src/include)

# TCP Sender executable
add_executable(tcp_sender
    tcp_sender.cpp
    sender_main.cpp
)

target_link_libraries(tcp_sender 
    ${CMAKE_CURRENT_SOURCE_DIR}/../databento-cpp/build/lib/Debug/libdatabento.a
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    CURL::libcurl
    ${ZSTD_LIBRARY}
)

# TCP Receiver executable
add_executable(tcp_receiver
    tcp_receiver.cpp
    receiver_main.cpp
)

target_link_libraries(tcp_receiver 
    # No additional libraries needed for basic TCP communication
)

# Compiler flags
target_compile_options(tcp_sender PRIVATE -Wall -Wextra -O2)
target_compile_options(tcp_receiver PRIVATE -Wall -Wextra -O2)
