#ifndef DAS_LUA_HEADERS_H
#define DAS_LUA_HEADERS_H

#ifdef DAS_EXPORT_LUA

// Lua's official headers intentionally omit extern "C".
// C++ users must provide their own linkage wrapper (see etc/lua.hpp in
// the Lua source tree).  Without this, the C++ compiler will mangle
// Lua API symbols (e.g. luaL_newstate → _Z13luaL_newstatev), causing
// link failures against the C-compiled static library.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#endif // DAS_EXPORT_LUA

#endif // DAS_LUA_HEADERS_H
