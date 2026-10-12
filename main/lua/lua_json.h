/*
 * SPDX-FileCopyrightText: 2026 MPX-Dog Project Contributors
 * SPDX-License-Identifier: MIT
 *
 * Converting between cJSON values and Lua values, for script `args` and
 * tools.call() (lua_tools.cc).
 *
 *   JSON object  <-> table with string keys
 *   JSON array   <-> sequence table {[1]=..., [n]=...}
 *   null         <-> nil (an array keeps its length: null elements are
 *                    left as holes, which Lua cannot count past)
 *
 * An empty Lua table becomes {} (an object): Lua cannot tell the two apart.
 */
#pragma once

#include "cJSON.h"

extern "C" {
#include "lua.h"
}

/**
 * Push @p item as a Lua value (nil for nullptr). Returns false, pushing
 * nothing, past the nesting limit or out of Lua stack -- it does not raise,
 * so a caller holding C++ or cJSON allocations is not longjmp'd past them.
 */
bool lua_json_push(lua_State *L, const cJSON *item);

/**
 * Convert the Lua value at @p idx to JSON. Returns nullptr and sets @p err
 * (a static string) for values JSON cannot hold: functions, userdata,
 * tables with keys that are neither strings nor a 1..n sequence, cycles
 * or nesting past the limit. Caller frees the result.
 */
cJSON *lua_json_from(lua_State *L, int idx, const char **err);
