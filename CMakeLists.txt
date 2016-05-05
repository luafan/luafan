cmake_minimum_required(VERSION 2.8)

project(fan)

#SET(CMAKE_BUILD_TYPE "Debug")

#include_directories ("/usr/local/include/lua5.3")
include_directories ("/usr/local/include/luajit-2.0" "/usr/include/luajit-2.0")

include_directories ("/usr/local/Cellar/mariadb/5.5.30/include/mysql")
include_directories ("/usr/local/Cellar/openssl/1.0.2g/include/")
include_directories ("/usr/local/include")

LINK_DIRECTORIES ("/usr/local/Cellar/openssl/1.0.2g/lib" "/usr/local/lib")

IF( ${CMAKE_SYSTEM_NAME} MATCHES "Windows" )
    SET( PRMAN_COMPILE_FLAGS "/c /nologo /MT /TP /DWIN32" )
    SET( PRMAN_LINK_FLAGS "/nologo /dll /LIBPATH:\"%RMANTREE%\\lib\" libprman.lib" )
    add_definitions( -DLUA_WIN )
ELSEIF( ${CMAKE_SYSTEM_NAME} MATCHES "Darwin" )
    SET( PRMAN_COMPILE_FLAGS "-c" )
    SET( PRMAN_LINK_FLAGS "-undefined dynamic_lookup" )
    add_definitions( -DLUA_USE_MACOSX )
ELSEIF( ${CMAKE_SYSTEM_NAME} MATCHES "Linux" )
    SET( PRMAN_COMPILE_FLAGS "-c -fPIC" )
    SET( PRMAN_LINK_FLAGS "-shared" )
    add_definitions( -DLUA_USE_LINUX )
ENDIF()

SET (MODULE_NAME   "luafan")
SET (OUTPUT_NAME   "fan")

file(GLOB MODULE_SOURCE
    "src/*.h"
    "src/*.c"
)

add_library(${MODULE_NAME} MODULE ${MODULE_SOURCE})

target_link_libraries(${MODULE_NAME} event)
target_link_libraries(${MODULE_NAME} event_openssl)
target_link_libraries(${MODULE_NAME} ssl)
target_link_libraries(${MODULE_NAME} crypto)

target_link_libraries(${MODULE_NAME} curl)
target_link_libraries(${MODULE_NAME} resolv)
target_link_libraries(${MODULE_NAME} mysqlclient)

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
