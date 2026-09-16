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
    PATHS
        /usr/local/mysql/include
        /usr/local/include
        /opt/homebrew/include
        /usr/include
)
find_library(MYSQL_CLIENT_LIBRARY
    NAMES mariadb mysqlclient
    PATHS
        /usr/local/mysql/lib
        /usr/local/lib
        /opt/homebrew/lib
        /usr/lib
)
if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_CLIENT_LIBRARY)
    list(FILTER MODULE_SOURCE EXCLUDE REGEX ".*/mariadb/.*")
    list(FILTER MODULE_SOURCE EXCLUDE REGEX "luamariadb\\.c$")
    message(STATUS "MariaDB client headers/library not found, excluding mariadb sources")
else()
    message(STATUS "Using MariaDB client: ${MYSQL_INCLUDE_DIR}, ${MYSQL_CLIENT_LIBRARY}")
endif()

# ---------------------------------------------------------------------------
# Optional build modes (see docs/threading-model.md, "Build shapes")
# ---------------------------------------------------------------------------
# LUAFAN_CORE_LOCK_HOOK=ON: the interpreter that will load fan.so was built with
# the global lua_lock hook (fan_lua_lock.c added to CORE_O, -include
# fan_lua_lock.h, symbols exported with -Wl,-E). fan.so must then NOT carry its
# own copy of the lock implementation: two copies would mean two recursive
# mutexes and two thread-local depth counters -- silent on ELF (symbol
# interposition hides it) and plainly wrong on Mach-O. LuaCoreLockHooked() then
# reports "hooked core" as well, so event_mgr skips its resume wrapper.
option(LUAFAN_CORE_LOCK_HOOK "Interpreter provides the global Lua lock (module must not carry a copy)" OFF)
# LUAFAN_TESTING=ON: compile the FAN_LOCK_PROBE diagnostics used by
# tests/lua/test_lock_granularity.lua (fan.diag_lock_sleep). Off by default so
# release builds expose no lock-timing lever.
option(LUAFAN_TESTING "Enable test-only diagnostics (FAN_LOCK_PROBE)" OFF)

# run_c_tests links a stock interpreter, so it always needs the module-side lock.
set(MODULE_SOURCE_FULL ${MODULE_SOURCE})
if(LUAFAN_CORE_LOCK_HOOK)
    list(FILTER MODULE_SOURCE EXCLUDE REGEX "fan_lua_lock\\.c$")
    message(STATUS "LUAFAN_CORE_LOCK_HOOK=ON: fan.so will use the interpreter's lock implementation")
endif()

add_library(${MODULE_NAME} MODULE ${MODULE_SOURCE})

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_include_directories(${MODULE_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(${MODULE_NAME} PRIVATE
        "-include${CMAKE_CURRENT_SOURCE_DIR}/src/fan_lua_lock.h")
endif()

if(LUAFAN_CORE_LOCK_HOOK)
    target_compile_definitions(${MODULE_NAME} PRIVATE FAN_CORE_LOCK_HOOK=1)
endif()
if(LUAFAN_TESTING)
    target_compile_definitions(${MODULE_NAME} PRIVATE FAN_LOCK_PROBE=1)
endif()

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
    "/usr/local/Cellar/mariadb/5.5.30/include/mysql"
    "/usr/local/Cellar/openssl/1.0.2g/include/"
    "/usr/local/include"
    "/opt/homebrew/include"
    "/opt/homebrew/opt/libevent/include"
)
if(MYSQL_INCLUDE_DIR)
    target_include_directories(${MODULE_NAME} PRIVATE ${MYSQL_INCLUDE_DIR})
endif()

# Link directories (target-scoped)
target_link_directories(${MODULE_NAME} PRIVATE
    "/usr/local/lib"
    "/usr/local/mysql/lib"
    "/usr/local/Cellar/openssl/1.0.2g/lib"
    "/opt/homebrew/lib"
    "/opt/homebrew/opt/libevent/lib"
)

target_link_libraries(${MODULE_NAME} event_pthreads)
target_link_libraries(${MODULE_NAME} event_openssl)
target_link_libraries(${MODULE_NAME} event)
target_link_libraries(${MODULE_NAME} ssl)
target_link_libraries(${MODULE_NAME} crypto)
target_link_libraries(${MODULE_NAME} z)

target_link_libraries(${MODULE_NAME} curl)
target_link_libraries(${MODULE_NAME} resolv)
if(MYSQL_CLIENT_LIBRARY)
    target_link_libraries(${MODULE_NAME} ${MYSQL_CLIENT_LIBRARY})
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
add_executable(run_c_tests ${C_TEST_SOURCES} ${MODULE_SOURCE_FULL})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(run_c_tests PRIVATE
        "-include${CMAKE_CURRENT_SOURCE_DIR}/src/fan_lua_lock.h")
endif()
# The C tests only use FAN_LOCK_PROBE through the module API, but keep the flag
# in sync so both binaries of a LUAFAN_TESTING build behave the same.
if(LUAFAN_TESTING)
    target_compile_definitions(run_c_tests PRIVATE FAN_LOCK_PROBE=1)
endif()
target_include_directories(run_c_tests PRIVATE
    "tests/c/framework"
    "src"
    "/usr/include/lua5.3"
    "/usr/local/include/lua5.3"
    "/usr/local/include/luajit-2.0"
    "/usr/include/luajit-2.0"
    "/opt/homebrew/opt/luajit/include/luajit-2.1"
    "/usr/local/include"
    "/opt/homebrew/include"
    "/opt/homebrew/opt/libevent/include"
)
if(MYSQL_INCLUDE_DIR)
    target_include_directories(run_c_tests PRIVATE ${MYSQL_INCLUDE_DIR})
endif()
target_compile_definitions(run_c_tests PRIVATE
    $<$<PLATFORM_ID:Linux>:LUA_USE_LINUX _GNU_SOURCE>
    $<$<PLATFORM_ID:Darwin>:LUA_USE_MACOSX>
)
target_link_directories(run_c_tests PRIVATE
    "/usr/local/mysql/lib"
    "/usr/local/Cellar/openssl/1.0.2g/lib"
    "/usr/local/lib"
    "/opt/homebrew/lib"
    "/opt/homebrew/opt/libevent/lib"
)
target_link_libraries(run_c_tests event_pthreads event_openssl event ssl crypto z curl resolv lua5.3 m)
if(MYSQL_CLIENT_LIBRARY)
    target_link_libraries(run_c_tests ${MYSQL_CLIENT_LIBRARY})
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
