cmake_minimum_required(VERSION 3.16)
project(ReceiverMicroservice)

# Enable compile_commands.json for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
include_directories(include)
include_directories(src)
include_directories(databento-cpp/include)
include_directories(databento-cpp/build/include)
include_directories(databento-cpp/build/_deps/date_src-src/include)
include_directories(databento-cpp/build/_deps/httplib-src)
include_directories(databento-cpp/build/_deps/json-src/include)

# Source files
set(COMMON_SOURCES
    src/utils.cpp
    src/tcp_receiver.cpp
)

# Receiver Microservice executable
add_executable(receiver_microservice
    receiver_microservice.cpp
    ${COMMON_SOURCES}
)

# Link libraries
target_link_libraries(receiver_microservice 
    ${CMAKE_CURRENT_SOURCE_DIR}/databento-cpp/build/lib/Debug/libdatabento.a
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    CURL::libcurl
    ${ZSTD_LIBRARY}
    pthread
)

# Cross-platform compiler flags
target_compile_options(receiver_microservice PRIVATE 
    -Wall 
    -Wextra 
    -O2
)

# Set output directory
set_target_properties(receiver_microservice PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
