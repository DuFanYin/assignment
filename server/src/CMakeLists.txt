# Core library CMakeLists.txt
# Find required system libraries
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(PostgreSQL REQUIRED)

# Find zstd library
find_library(ZSTD_LIBRARY zstd)
if(NOT ZSTD_LIBRARY)
    message(FATAL_ERROR "zstd library not found")
endif()

# Determine databento path relative to project root (now in thirdparty)
set(DATABENTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/databento-cpp)

# uWebSockets path
set(UWEBSOCKETS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/uWebSockets)
set(USOCKETS_ROOT ${UWEBSOCKETS_ROOT}/uSockets)

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
    ${UWEBSOCKETS_ROOT}/src
    ${USOCKETS_ROOT}/src
    ${PostgreSQL_INCLUDE_DIRS}
)

# Build database library
add_library(database_lib
    database/postgres_connection.cpp
    database/database_writer.cpp
    database/json_generator.cpp
)

target_include_directories(database_lib PUBLIC
    ../include
    ${PostgreSQL_INCLUDE_DIRS}
    ${DATABENTO_ROOT}/include
    ${DATABENTO_ROOT}/build/include
    ${DATABENTO_ROOT}/build/_deps/date_src-src/include
    ${DATABENTO_ROOT}/build/_deps/httplib-src
    ${DATABENTO_ROOT}/build/_deps/json-src/include
)

# OpenSSL includes are provided by OpenSSL::SSL and OpenSSL::Crypto targets

target_link_libraries(database_lib
    ${PostgreSQL_LIBRARIES}
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
