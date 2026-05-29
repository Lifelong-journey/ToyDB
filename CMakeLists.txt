cmake_minimum_required(VERSION 3.15)

project(ToyDB
  VERSION 1.0
  DESCRIPTION "A toy database"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

set(THIRD_PARTY_INCLUDE_DIR
  ${PROJECT_SOURCE_DIR}/third_party/libpg_query/include
)

include_directories(${THIRD_PARTY_INCLUDE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/src)

add_subdirectory(third_party)
add_subdirectory(src)
