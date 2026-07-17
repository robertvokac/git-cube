cmake_minimum_required(VERSION 3.20)
project(gitcube VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

add_library(gitcube_core
    src/util.cpp
    src/json.cpp
    src/database.cpp
    src/process.cpp
    src/git_service.cpp
    src/http_server.cpp
    src/application.cpp
)

target_include_directories(gitcube_core PUBLIC src)
target_link_libraries(gitcube_core PUBLIC SQLite::SQLite3 Threads::Threads)
target_compile_options(gitcube_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(gitcube src/main.cpp)
target_link_libraries(gitcube PRIVATE gitcube_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(gitcube_tests tests/test_main.cpp)
    target_link_libraries(gitcube_tests PRIVATE gitcube_core)
    add_test(NAME gitcube_tests COMMAND gitcube_tests)
endif()

install(TARGETS gitcube RUNTIME DESTINATION bin)
