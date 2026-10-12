/*
 * SPDX-FileCopyrightText: 2026 MPX-Dog Project Contributors
 * SPDX-License-Identifier: MIT
 *
 * The `tools` module: the robot's agent tools (main/agent/robot_tools.cc),
 * callable from Lua with the same names and arguments the agent uses, so a
 * script the agent writes does what the agent would have done by hand:
 *
 *   local res, err = tools.call("display_draw", { shapes = { ... } })
 *
 * Only robot_tools' table is reachable: the agent's own lua_run_script is
 * not in it, so a script cannot start another script and wait on the run
 * lock it is holding.
 */

#include "lua_bindings.h"

#include <string>

extern "C" {
#include "lauxlib.h"
}

#include "agent/robot_tools.h"
#include "lua/lua_json.h"
#include "lua/lua_vm.h"

namespace {

/* tools.call(name [, args]) -> result | nil, error
 * result is the tool's JSON reply as a table (a string if it is not JSON).
 * A reply the agent would be told is an error comes back as nil, text. */
int l_tools_call(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const robot_tools::Tool *tool = robot_tools::find(name);
    if (!tool) {
        lua_pushnil(L);
        lua_pushfstring(L, "unknown tool '%s' (tools.list() has the names)", name);
        return 2;
    }

    luaL_checkstack(L, 3, "tools.call");
    cJSON *args = nullptr;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        const char *err = nullptr;
        args = lua_json_from(L, 2, &err);
        if (!args) {
            return luaL_argerror(L, 2, err);
        }
    }

    /* Lua errors longjmp past C++ destructors, so the reply is handed to Lua
     * as a string first and everything that can raise happens after it is
     * gone. */
    bool failed = false;
    {
        const robot_tools::Reply reply = robot_tools::run(*tool, args);
        cJSON_Delete(args);
        failed = reply.error;
        lua_pushlstring(L, reply.text.data(), reply.text.size());
    }
    lua_vm_check_deadline(L);

    if (failed) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    cJSON *parsed = cJSON_Parse(lua_tostring(L, -1));
    if (!parsed) {
        return 1;  /* not JSON: the text itself */
    }
    if (lua_json_push(L, parsed)) {
        lua_remove(L, -2);  /* the text */
    }
    cJSON_Delete(parsed);
    return 1;
}

/* tools.list() -> { "robot_move", "display_draw", ... } */
int l_tools_list(lua_State *L)
{
    size_t count = 0;
    const robot_tools::Tool *table = robot_tools::all(&count);
    lua_createtable(L, (int)count, 0);
    for (size_t i = 0; i < count; i++) {
        lua_pushstring(L, table[i].name);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    return 1;
}

const luaL_Reg kToolsLib[] = {
    { "call", l_tools_call },
    { "list", l_tools_list },
    { nullptr, nullptr },
};

}  // namespace

void lua_register_tools_bindings(lua_State *L)
{
    luaL_newlib(L, kToolsLib);
    lua_setglobal(L, "tools");
}
