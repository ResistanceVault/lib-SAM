cmake_minimum_required(VERSION 3.21)

project(lib_SAM C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
option(SAY_BUILD_TESTS "Build smoke tests" ON)

set(SAY_BIN_DIR "${CMAKE_SOURCE_DIR}/bin")
set(SAY_LUA_BIN_DIR "${SAY_BIN_DIR}/lua")

add_subdirectory(lua)

set(SAY_CORE_SOURCES
    src/reciter.c
    src/sam.c
    src/render.c
    src/debug.c
    src/processframes.c
    src/createtransitions.c
    src/say_backend.c
)

add_library(sam_core STATIC ${SAY_CORE_SOURCES})
target_include_directories(sam_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_definitions(sam_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(NOT WIN32)
    target_link_libraries(sam_core PUBLIC m)
endif()

add_executable(tts src/main.c)
target_link_libraries(tts PRIVATE sam_core)
set_target_properties(tts PROPERTIES
    OUTPUT_NAME tts
    RUNTIME_OUTPUT_DIRECTORY "${SAY_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SAY_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAY_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SAY_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${SAY_BIN_DIR}"
)

add_library(say MODULE src/lua_say.c)
target_include_directories(say PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lua/src
)
target_link_libraries(say PRIVATE sam_core say_bundled_lua)
if(WIN32)
    target_compile_definitions(say PRIVATE LUA_BUILD_AS_DLL _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(say PROPERTIES
    PREFIX ""
    OUTPUT_NAME say
    RUNTIME_OUTPUT_DIRECTORY "${SAY_LUA_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SAY_LUA_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SAY_LUA_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAY_LUA_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SAY_LUA_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${SAY_LUA_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${SAY_LUA_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${SAY_LUA_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SAY_LUA_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${SAY_LUA_BIN_DIR}"
)

add_executable(lua_cli lua/src/lua.c)
target_include_directories(lua_cli PRIVATE ${CMAKE_SOURCE_DIR}/lua/src)
target_link_libraries(lua_cli PRIVATE say_bundled_lua)
if(WIN32)
    target_compile_definitions(lua_cli PRIVATE LUA_BUILD_AS_DLL _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(lua_cli PROPERTIES
    OUTPUT_NAME lua
    RUNTIME_OUTPUT_DIRECTORY "${SAY_LUA_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SAY_LUA_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAY_LUA_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SAY_LUA_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${SAY_LUA_BIN_DIR}"
)

set_target_properties(say_bundled_lua PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SAY_LUA_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SAY_LUA_BIN_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lua"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SAY_LUA_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAY_LUA_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SAY_LUA_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${SAY_LUA_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${SAY_LUA_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${SAY_LUA_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SAY_LUA_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${SAY_LUA_BIN_DIR}"
)

if(SAY_BUILD_TESTS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME say_smoke
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/smoke.py" "${CMAKE_SOURCE_DIR}"
        )
    endif()
endif()
