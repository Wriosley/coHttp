cmake_minimum_required(VERSION 3.10)
project(coHttp)

# 使用 C++17（fmt 需要 C++11+）
set(CMAKE_CXX_STANDARD 17)

# 查找 fmt 库（你之前已经 sudo make install 到 /usr/local 了）
find_package(fmt REQUIRED)

# 定义可执行文件 server，由 server.cpp 编译
add_executable(server server.cpp)

# 链接 fmt
target_link_libraries(server PRIVATE fmt::fmt)
