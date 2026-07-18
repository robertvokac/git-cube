cmake_minimum_required(VERSION 3.20)
project(gitcube VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GITCUBE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(GITCUBE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(GITCUBE_ENABLE_COVERAGE "Enable GCC/Clang coverage instrumentation" OFF)
option(GITCUBE_INSTALL_SYSTEMD_USER_UNIT "Install the systemd user service template" ON)

if(GITCUBE_ENABLE_SANITIZERS AND GITCUBE_ENABLE_COVERAGE)
    message(FATAL_ERROR "Sanitizers and coverage must use separate build directories")
endif()
if((GITCUBE_ENABLE_SANITIZERS OR GITCUBE_ENABLE_COVERAGE)
   AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "Sanitizer/coverage options require GCC or Clang")
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

function(gitcube_configure_target target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    if(GITCUBE_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(GITCUBE_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
    if(GITCUBE_ENABLE_COVERAGE)
        target_compile_options(${target} PRIVATE --coverage -O0 -g)
        target_link_options(${target} PRIVATE --coverage)
    endif()
endfunction()

add_library(gitcube_core
    src/data_directory_lock.cpp
    src/util.cpp
    src/json.cpp
    src/database.cpp
    src/process.cpp
    src/git_service.cpp
    src/http_server.cpp
    src/application.cpp
    src/favicon_assets.cpp
)

target_include_directories(gitcube_core PUBLIC src)
target_link_libraries(gitcube_core PUBLIC SQLite::SQLite3 Threads::Threads)
gitcube_configure_target(gitcube_core)

add_executable(gitcube src/main.cpp)
target_link_libraries(gitcube PRIVATE gitcube_core)
gitcube_configure_target(gitcube)

include(CTest)
if(BUILD_TESTING)
    add_executable(gitcube_tests tests/test_main.cpp)
    target_link_libraries(gitcube_tests PRIVATE gitcube_core)
    gitcube_configure_target(gitcube_tests)
    add_test(NAME gitcube_tests COMMAND gitcube_tests)
    set_tests_properties(gitcube_tests PROPERTIES TIMEOUT 60)
endif()

install(TARGETS gitcube
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT Runtime
)
install(FILES README.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    COMPONENT Runtime
)

if(GITCUBE_INSTALL_SYSTEMD_USER_UNIT)
    configure_file(
        examples/gitcube.service.in
        ${CMAKE_CURRENT_BINARY_DIR}/gitcube.service
        @ONLY
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gitcube.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/systemd/user
        COMPONENT Runtime
    )
endif()

set(CPACK_PACKAGE_NAME "gitcube")
set(CPACK_PACKAGE_VENDOR "GitCube")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local public Git mirror server")
set(CPACK_PACKAGE_CONTACT "robertvokac")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ" CACHE STRING "CPack package generators")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "git, curl, zip, libsqlite3-0")
include(CPack)
