# Find required system libraries
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)

# Find zstd library
find_library(ZSTD_LIBRARY zstd)
if(NOT ZSTD_LIBRARY)
    message(FATAL_ERROR "zstd library not found")
endif()

# Determine databento path relative to project root
set(DATABENTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../databento-cpp)

add_library(project_lib
    config.cpp
    tcp_receiver.cpp
    tcp_sender.cpp
    utils.cpp
)

target_include_directories(project_lib PUBLIC
    ../include
    ${DATABENTO_ROOT}/include
    ${DATABENTO_ROOT}/build/include
    ${DATABENTO_ROOT}/build/_deps/date_src-src/include
    ${DATABENTO_ROOT}/build/_deps/httplib-src
    ${DATABENTO_ROOT}/build/_deps/json-src/include
)

target_link_libraries(project_lib
    ${DATABENTO_ROOT}/build/lib/Debug/libdatabento.a
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    CURL::libcurl
    ${ZSTD_LIBRARY}
)

# Compiler flags
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
target_compile_options(project_lib PRIVATE -Wall -Wextra -O3 -DNDEBUG -march=native)

