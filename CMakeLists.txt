cmake_minimum_required(VERSION 3.13)
project(robots-client)
project(robots-server)
set(CMAKE_CXX_FLAGS "-std=gnu++20 -Wall -Wextra -Wconversion -Werror -O2")
set(CMAKE_CXX_STANDARD 20)
find_package(Boost COMPONENTS program_options REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

add_executable(robots-client robots-client.cpp common.h)
add_executable(robots-server robots-server.cpp common.h)

target_link_libraries(robots-client LINK_PUBLIC ${Boost_LIBRARIES} pthread)
target_link_libraries(robots-server LINK_PUBLIC ${Boost_LIBRARIES} pthread)

