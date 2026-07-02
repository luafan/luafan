cmake_minimum_required(VERSION 3.15)

project(fan)

SET (MODULE_NAME   "luafan")
SET (OUTPUT_NAME   "fan")

file(GLOB MODULE_SOURCE
    "src/*.h"
    "src/*.c"
    "src/mariadb/*.h"
    "src/mariadb/*.c"
)

# Exclude mariadb if mysql headers are not available
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h
    PATHS /usr/local/mysql/include /opt/homebrew/include /usr/include
)
if(NOT MYSQL_INCLUDE_DIR)
    list(FILTER MODULE_SOURCE EXCLUDE REGEX ".*/mariadb/.*")
    list(FILTER MODULE_SOURCE EXCLUDE REGEX "luamariadb\\.c$")
    message(STATUS "mysql headers not found, excluding mariadb sources")
endif()

add_library(${MODULE_NAME} MODULE ${MODULE_SOURCE})

# Platform-specific compile definitions (target-scoped)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(${MODULE_NAME} PRIVATE LUA_WIN)
    SET( PRMAN_COMPILE_FLAGS "/c /nologo /MT /TP" )
    SET( PRMAN_LINK_FLAGS "/nologo /dll /LIBPATH:\"%RMANTREE%\\lib\" libprman.lib" )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(${MODULE_NAME} PRIVATE LUA_USE_MACOSX)
    SET( PRMAN_COMPILE_FLAGS "-c" )
    SET( PRMAN_LINK_FLAGS "-undefined dynamic_lookup" )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(${MODULE_NAME} PRIVATE LUA_USE_LINUX _GNU_SOURCE)
    SET( PRMAN_COMPILE_FLAGS "-c -fPIC" )
    SET( PRMAN_LINK_FLAGS "-shared" )
endif()

# Include directories (target-scoped)
target_include_directories(${MODULE_NAME} PRIVATE
    "/opt/homebrew/include/lua5.5"
    "/usr/include/lua5.3"
    "/usr/local/include/lua5.3"
    "/usr/local/include/luajit-2.0"
    "/usr/include/luajit-2.0"
    "/opt/homebrew/opt/luajit/include/luajit-2.1"
    "/usr/local/mysql/include"
    "/usr/local/Cellar/mariadb/5.5.30/include/mysql"
    "/usr/local/include"
    "/opt/homebrew/include"
    "/opt/homebrew/opt/libevent/include"
)

# Link directories (target-scoped)
target_link_directories(${MODULE_NAME} PRIVATE
    "/usr/local/mysql/lib"
    "/usr/local/lib"
    "/opt/homebrew/lib"
    "/opt/homebrew/opt/libevent/lib"
)

target_link_libraries(${MODULE_NAME} event)
target_link_libraries(${MODULE_NAME} event_openssl)
target_link_libraries(${MODULE_NAME} ssl)
target_link_libraries(${MODULE_NAME} crypto)
target_link_libraries(${MODULE_NAME} pthread)

target_link_libraries(${MODULE_NAME} curl)
target_link_libraries(${MODULE_NAME} resolv)
if(MYSQL_INCLUDE_DIR)
    target_link_libraries(${MODULE_NAME} mysqlclient)
endif()

SET_TARGET_PROPERTIES(
    ${MODULE_NAME}
    PROPERTIES
    COMPILE_FLAGS ${PRMAN_COMPILE_FLAGS}
    LINK_FLAGS ${PRMAN_LINK_FLAGS}
    PREFIX ""
    OUTPUT_NAME ${OUTPUT_NAME}
)

INSTALL(TARGETS ${MODULE_NAME}
    RUNTIME DESTINATION sbin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib/static)

# Testing integration
enable_testing()

# Build C test executable
file(GLOB C_TEST_SOURCES
    "tests/c/framework/*.c"
    "tests/c/unit/*.c"
)
add_executable(run_c_tests ${C_TEST_SOURCES} ${MODULE_SOURCE})
target_include_directories(run_c_tests PRIVATE
    "tests/c/framework"
    "src"
    "/usr/include/lua5.3"
    "/usr/local/include/lua5.3"
    "/usr/local/include/luajit-2.0"
    "/usr/include/luajit-2.0"
    "/opt/homebrew/opt/luajit/include/luajit-2.1"
    "/usr/local/mysql/include"
    "/usr/local/include"
    "/opt/homebrew/include"
    "/opt/homebrew/opt/libevent/include"
)
target_compile_definitions(run_c_tests PRIVATE
    $<$<PLATFORM_ID:Linux>:LUA_USE_LINUX _GNU_SOURCE>
    $<$<PLATFORM_ID:Darwin>:LUA_USE_MACOSX>
)
target_link_directories(run_c_tests PRIVATE
    "/usr/local/mysql/lib"
    "/usr/local/lib"
    "/opt/homebrew/lib"
    "/opt/homebrew/opt/libevent/lib"
)
if(MYSQL_INCLUDE_DIR)
    target_link_libraries(run_c_tests event event_openssl ssl crypto pthread curl resolv mysqlclient lua5.3 m)
else()
    target_link_libraries(run_c_tests event event_openssl ssl crypto pthread curl resolv lua5.3 m)
endif()
set_target_properties(run_c_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Add custom test target that runs our comprehensive test suite
add_custom_target(test_suite
    COMMAND cd ${CMAKE_SOURCE_DIR}/tests && ./run_all_tests.sh --all --verbose
    DEPENDS ${MODULE_NAME}
    COMMENT "Running comprehensive LuaFan test suite (C + Lua + Performance + Coverage)"
    VERBATIM
)

# Add individual test targets for flexibility
add_custom_target(test_c
    COMMAND cd ${CMAKE_SOURCE_DIR}/tests && ./run_all_tests.sh --c-only --verbose
    DEPENDS ${MODULE_NAME}
    COMMENT "Running C unit tests"
    VERBATIM
)

add_custom_target(test_lua
    COMMAND cd ${CMAKE_SOURCE_DIR}/tests && ./run_all_tests.sh --lua-only --verbose
    DEPENDS ${MODULE_NAME}
    COMMENT "Running Lua unit tests"
    VERBATIM
)

add_custom_target(test_performance
    COMMAND cd ${CMAKE_SOURCE_DIR}/tests && ./run_all_tests.sh --quick-perf --verbose
    DEPENDS ${MODULE_NAME}
    COMMENT "Running performance benchmarks"
    VERBATIM
)
