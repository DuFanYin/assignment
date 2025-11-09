add_executable(sender sender_main.cpp)
target_link_libraries(sender project_lib)
target_compile_options(sender PRIVATE -Wall -Wextra -O3 -DNDEBUG -march=native)

add_executable(receiver receiver_main.cpp)
target_link_libraries(receiver project_lib)
target_compile_options(receiver PRIVATE -Wall -Wextra -O3 -DNDEBUG -march=native)

