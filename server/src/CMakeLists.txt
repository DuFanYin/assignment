# Core library CMakeLists.txt
# Find required system libraries
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)

# ClickHouse library path
set(CLICKHOUSE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/clickhouse-cpp)

# Find zstd library
find_library(ZSTD_LIBRARY zstd)
if(NOT ZSTD_LIBRARY)
    message(FATAL_ERROR "zstd library not found")
endif()

# Find system libraries required by clickhouse-cpp
find_library(LZ4_LIB lz4)
if(NOT LZ4_LIB)
    message(FATAL_ERROR "lz4 library not found. Please install it (e.g., macOS: brew install lz4).")
endif()
find_library(CITYHASH_LIB cityhash)
if(NOT CITYHASH_LIB)
    message(FATAL_ERROR "cityhash library not found. Please install it (e.g., macOS: brew install cityhash).")
endif()

# Determine databento path relative to project root (now in thirdparty)
set(DATABENTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/databento-cpp)

# uWebSockets path
set(UWEBSOCKETS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/uWebSockets)
set(USOCKETS_ROOT ${UWEBSOCKETS_ROOT}/uSockets)

# ClickHouse library path (already set above)

# Build core library
add_library(core_lib
    core/config.cpp
    core/utils.cpp
    core/server.cpp
)

target_include_directories(core_lib PUBLIC
    ../include
    ${DATABENTO_ROOT}/include
    ${DATABENTO_ROOT}/build/include
    ${DATABENTO_ROOT}/build/_deps/date_src-src/include
    ${DATABENTO_ROOT}/build/_deps/httplib-src
    ${DATABENTO_ROOT}/build/_deps/json-src/include
    ${UWEBSOCKETS_ROOT}
    ${UWEBSOCKETS_ROOT}/src
    ${USOCKETS_ROOT}/src
    ${CLICKHOUSE_ROOT}
    ${CLICKHOUSE_ROOT}/contrib/absl
)

# Build database library (ClickHouse instead of PostgreSQL)
add_library(database_lib
    database/clickhouse_connection.cpp
    database/database_writer.cpp
    database/json_generator.cpp
)

target_include_directories(database_lib PUBLIC
    ../include
    ${CLICKHOUSE_ROOT}
    ${CLICKHOUSE_ROOT}/contrib/absl
    ${DATABENTO_ROOT}/include
    ${DATABENTO_ROOT}/build/include
    ${DATABENTO_ROOT}/build/_deps/date_src-src/include
    ${DATABENTO_ROOT}/build/_deps/httplib-src
    ${DATABENTO_ROOT}/build/_deps/json-src/include
)

# OpenSSL includes are provided by OpenSSL::SSL and OpenSSL::Crypto targets

target_link_libraries(database_lib
    ${CLICKHOUSE_ROOT}/build/clickhouse/libclickhouse-cpp-lib.a
    ${LZ4_LIB}
    ${CITYHASH_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Link core library with all dependencies
target_link_libraries(core_lib
    database_lib
    ${DATABENTO_ROOT}/build/lib/Debug/libdatabento.a
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    CURL::libcurl
    ${ZSTD_LIBRARY}
)

# Link uSockets library if it exists
# uSockets builds as uSockets.a (not libusockets.a)
if(EXISTS ${USOCKETS_ROOT}/uSockets.a)
    target_link_libraries(core_lib ${USOCKETS_ROOT}/uSockets.a)
    message(STATUS "Found uSockets static library: ${USOCKETS_ROOT}/uSockets.a")
elseif(EXISTS ${USOCKETS_ROOT}/libusockets.a)
    target_link_libraries(core_lib ${USOCKETS_ROOT}/libusockets.a)
    message(STATUS "Found uSockets static library: ${USOCKETS_ROOT}/libusockets.a")
elseif(EXISTS ${USOCKETS_ROOT}/libusockets.so)
    target_link_libraries(core_lib ${USOCKETS_ROOT}/libusockets.so)
    message(STATUS "Found uSockets shared library: ${USOCKETS_ROOT}/libusockets.so")
else()
    message(WARNING "uSockets library not found at ${USOCKETS_ROOT}. Please build uSockets first.")
endif()

# Compiler flags
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
target_compile_options(core_lib PRIVATE -Wall -Wextra -O3 -DNDEBUG -march=native)
target_compile_options(database_lib PRIVATE -Wall -Wextra -O3)

# For backward compatibility, create an alias
add_library(project_lib ALIAS core_lib)
