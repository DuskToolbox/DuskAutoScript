cmake_minimum_required(VERSION 3.24)
project(DasLua544 LANGUAGES C)
# message(${PROJECT_NAME})
aux_source_directory(. ALL_SRC)
# aux_source_directory returns absolute paths, so we must match
# by filename at the end of the path. Exclude standalone programs
# and test infrastructure that should NOT be in the static library:
#   lua.c     - standalone Lua interpreter (defines main)
#   luac.c    - Lua compiler (removed in 5.4.x, kept for compatibility)
#   onelua.c  - single-file Lua interpreter (includes lua.c, defines main)
#   ltests.c  - Lua internal test/debug infrastructure (54KB, not runtime)
set(LIB_SRC ${ALL_SRC})
list(FILTER LIB_SRC EXCLUDE REGEX "(lua\\.c|luac\\.c|onelua\\.c|ltests\\.c)$")

set(DAS_LUA_TARGET DasLua546)
add_library(${DAS_LUA_TARGET} ${LIB_SRC})
target_include_directories(${DAS_LUA_TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(${DAS_LUA_TARGET} PUBLIC -DLUA_COMPAT_5_3)
add_library(Das::Lua ALIAS ${DAS_LUA_TARGET})

# ADD_EXECUTABLE(luaexec lua.c)
# target_link_libraries(luaexec lua)
# set_target_properties(luaexec PROPERTIES OUTPUT_NAME lua)

# add_executable(luac luac.c)

# target_link_libraries(luac lua)
