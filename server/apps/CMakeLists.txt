add_executable(websocket_server main.cpp)
target_link_libraries(websocket_server project_lib)
target_compile_options(websocket_server PRIVATE -Wall -Wextra -O3 -DNDEBUG -march=native)

