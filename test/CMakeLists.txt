cmake_minimum_required(VERSION 3.24)

project(databento_test)

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

# Create the executable
add_executable(inspect_data inspect_data.cpp)

# Set include directories
target_include_directories(inspect_data PRIVATE
    ../databento-cpp/include
    ../databento-cpp/build/include
    ../databento-cpp/build/_deps/date_src-src/include
    ../databento-cpp/build/_deps/httplib-src
    ../databento-cpp/build/_deps/json-src/include
)

# Link libraries manually
target_link_libraries(inspect_data 
    ${CMAKE_CURRENT_SOURCE_DIR}/../databento-cpp/build/lib/Debug/libdatabento.a
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    CURL::libcurl
    ${ZSTD_LIBRARY}
)
