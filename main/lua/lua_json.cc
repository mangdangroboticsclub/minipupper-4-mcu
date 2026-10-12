/*
 * SPDX-FileCopyrightText: 2026 MPX-Dog Project Contributors
 * SPDX-License-Identifier: MIT
 *
 * See lua_json.h.
 */

#include "lua_json.h"

#include <cmath>

namespace {

/* Deep enough for display_draw's shapes -> points -> [x, y]; shallow enough
 * that a cycle fails fast instead of eating the stack. */
constexpr int kMaxDepth = 16;

/* Lua stack slots one level of either conversion needs. */
constexpr int kStackPerLevel = 4;

/* Pushes exactly one value, or nothing and returns false. */
bool push_value(lua_State *L, const cJSON *item, int depth)
{
    if (depth > kMaxDepth || !lua_checkstack(L, kStackPerLevel)) {
        return false;
    }

    if (!item || cJSON_IsNull(item) || cJSON_IsInvalid(item)) {
        lua_pushnil(L);
    } else if (cJSON_IsBool(item)) {
        lua_pushboolean(L, cJSON_IsTrue(item));
    } else if (cJSON_IsNumber(item)) {
        const double v = item->valuedouble;
        if (std::floor(v) == v && std::fabs(v) < 9.0e15) {
            lua_pushinteger(L, (lua_Integer)v);
        } else {
            lua_pushnumber(L, v);
        }
    } else if (cJSON_IsString(item)) {
        lua_pushstring(L, item->valuestring ? item->valuestring : "");
    } else if (cJSON_IsArray(item)) {
        lua_createtable(L, cJSON_GetArraySize(item), 0);
        lua_Integer i = 1;
        for (const cJSON *child = item->child; child; child = child->next, i++) {
            if (!push_value(L, child, depth + 1)) {
                lua_pop(L, 1);
                return false;
            }
            lua_rawseti(L, -2, i);
        }
    } else {  /* object */
        lua_createtable(L, 0, cJSON_GetArraySize(item));
        for (const cJSON *child = item->child; child; child = child->next) {
            if (!child->string) continue;
            if (!push_value(L, child, depth + 1)) {
                lua_pop(L, 1);
                return false;
            }
            lua_setfield(L, -2, child->string);
        }
    }
    return true;
}

/* A table is a JSON array when its keys are exactly the integers 1..n. */
bool is_sequence(lua_State *L, int idx)
{
    const lua_Unsigned n = lua_rawlen(L, idx);
    if (n == 0) return false;
    lua_Unsigned count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        lua_pop(L, 1);  /* value */
        int is_int = 0;
        const lua_Integer k = lua_tointegerx(L, -1, &is_int);
        if (lua_type(L, -1) != LUA_TNUMBER || !is_int || k < 1 || (lua_Unsigned)k > n) {
            lua_pop(L, 1);  /* key */
            return false;
        }
        count++;
    }
    return count == n;
}

cJSON *from_value(lua_State *L, int idx, int depth, const char **err)
{
    /* No luaL_* raising here: a Lua error longjmps past the cJSON tree being
     * built, and it would leak. */
    if (depth > kMaxDepth) {
        *err = "table nested too deep (or it contains itself)";
        return nullptr;
    }
    if (!lua_checkstack(L, kStackPerLevel)) {
        *err = "out of Lua stack";
        return nullptr;
    }
    idx = lua_absindex(L, idx);

    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        return cJSON_CreateNull();
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER:
        return cJSON_CreateNumber(lua_tonumber(L, idx));
    case LUA_TSTRING:
        return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE:
        break;
    default:
        *err = "only nil, booleans, numbers, strings and tables can be sent";
        return nullptr;
    }

    if (is_sequence(L, idx)) {
        const lua_Unsigned n = lua_rawlen(L, idx);
        cJSON *arr = cJSON_CreateArray();
        for (lua_Unsigned i = 1; i <= n; i++) {
            lua_rawgeti(L, idx, (lua_Integer)i);
            cJSON *child = from_value(L, -1, depth + 1, err);
            lua_pop(L, 1);
            if (!child) {
                cJSON_Delete(arr);
                return nullptr;
            }
            cJSON_AddItemToArray(arr, child);
        }
        return arr;
    }

    cJSON *obj = cJSON_CreateObject();
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 2);
            cJSON_Delete(obj);
            *err = "table keys must be strings, or 1..n for a list";
            return nullptr;
        }
        cJSON *child = from_value(L, -1, depth + 1, err);
        if (!child) {
            lua_pop(L, 2);
            cJSON_Delete(obj);
            return nullptr;
        }
        cJSON_AddItemToObject(obj, lua_tostring(L, -2), child);
        lua_pop(L, 1);  /* value; keep the key for lua_next */
    }
    return obj;
}

}  // namespace

bool lua_json_push(lua_State *L, const cJSON *item)
{
    return push_value(L, item, 0);
}

cJSON *lua_json_from(lua_State *L, int idx, const char **err)
{
    const char *unused = nullptr;
    return from_value(L, idx, 0, err ? err : &unused);
}
