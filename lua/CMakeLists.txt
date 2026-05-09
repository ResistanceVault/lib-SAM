cmake_minimum_required(VERSION 3.16)

set(SAY_BUNDLED_LUA_CORE_SOURCES
    src/lapi.c
    src/lcode.c
    src/lctype.c
    src/ldebug.c
    src/ldo.c
    src/ldump.c
    src/lfunc.c
    src/lgc.c
    src/llex.c
    src/lmem.c
    src/lobject.c
    src/lopcodes.c
    src/lparser.c
    src/lstate.c
    src/lstring.c
    src/ltable.c
    src/ltm.c
    src/lundump.c
    src/lvm.c
    src/lzio.c
)

set(SAY_BUNDLED_LUA_LIB_SOURCES
    src/lauxlib.c
    src/lbaselib.c
    src/lcorolib.c
    src/ldblib.c
    src/liolib.c
    src/lmathlib.c
    src/loadlib.c
    src/loslib.c
    src/lstrlib.c
    src/ltablib.c
    src/lutf8lib.c
    src/linit.c
)

add_library(say_bundled_lua SHARED
    ${SAY_BUNDLED_LUA_CORE_SOURCES}
    ${SAY_BUNDLED_LUA_LIB_SOURCES}
)

target_include_directories(say_bundled_lua PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

if(WIN32)
    target_compile_definitions(say_bundled_lua PRIVATE _CRT_SECURE_NO_WARNINGS LUA_BUILD_AS_DLL)
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_compile_definitions(say_bundled_lua PRIVATE LUA_USE_LINUX)
elseif(APPLE)
    target_compile_definitions(say_bundled_lua PRIVATE LUA_USE_MACOSX)
endif()

if(NOT WIN32)
    target_link_libraries(say_bundled_lua PRIVATE ${CMAKE_DL_LIBS} m)
endif()

set_target_properties(say_bundled_lua PROPERTIES
    OUTPUT_NAME lua54
)
