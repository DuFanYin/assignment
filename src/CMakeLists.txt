cmake_minimum_required(VERSION 3.16)
project(MarketDataProcessor)

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
include_directories(../databento-cpp/include)
include_directories(../databento-cpp/build/include)
include_directories(../databento-cpp/build/_deps/date_src-src/include)
include_directories(../databento-cpp/build/_deps/httplib-src)
include_directories(../databento-cpp/build/_deps/json-src/include)

# Source files
set(SOURCES
    src/main.cpp
    src/streamer.cpp
    src/utils.cpp
)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Link libraries manually
target_link_libraries(${PROJECT_NAME} 
    ${CMAKE_CURRENT_SOURCE_DIR}/../databento-cpp/build/lib/Debug/libdatabento.a
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    CURL::libcurl
    ${ZSTD_LIBRARY}
)

# Compiler flags
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -O2)
