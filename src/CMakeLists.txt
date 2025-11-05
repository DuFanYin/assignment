cmake_minimum_required(VERSION 3.16)
project(OrderBookProcessor)

set(CMAKE_CXX_STANDARD 20)
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
include_directories(include)
include_directories(../databento-cpp/include)
include_directories(../databento-cpp/build/include)
include_directories(../databento-cpp/build/_deps/date_src-src/include)
include_directories(../databento-cpp/build/_deps/httplib-src)
include_directories(../databento-cpp/build/_deps/json-src/include)

# Common source files
set(COMMON_SOURCES
    src/streamer.cpp
    src/utils.cpp
    src/config.cpp
)

# TCP sender executable
set(SENDER_SOURCES
    src/sender_main.cpp
    src/tcp_sender.cpp
    ${COMMON_SOURCES}
)

# TCP receiver executable (simplified without queue processing)
set(RECEIVER_SOURCES
    src/receiver_main.cpp
    src/tcp_receiver.cpp
    ${COMMON_SOURCES}
)

# Create executables
add_executable(TCP_Sender ${SENDER_SOURCES})
add_executable(TCP_Receiver ${RECEIVER_SOURCES})

# Link libraries for executables
target_link_libraries(TCP_Sender 
    ${CMAKE_CURRENT_SOURCE_DIR}/../databento-cpp/build/lib/Debug/libdatabento.a
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    CURL::libcurl
    ${ZSTD_LIBRARY}
)

target_link_libraries(TCP_Receiver 
    ${CMAKE_CURRENT_SOURCE_DIR}/../databento-cpp/build/lib/Debug/libdatabento.a
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    CURL::libcurl
    ${ZSTD_LIBRARY}
)

# Compiler flags
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
target_compile_options(TCP_Sender PRIVATE -Wall -Wextra -O3 -DNDEBUG -march=native)
target_compile_options(TCP_Receiver PRIVATE -Wall -Wextra -O3 -DNDEBUG -march=native)
