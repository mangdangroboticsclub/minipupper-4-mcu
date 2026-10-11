/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lua's `display` module: draw on the robot's screen.
 *
 * A thin layer over the drawing canvas in display/cap_display.c, the one the
 * agent's display_draw tool paints (see there for why it is a canvas that
 * outlives the script rather than mpv2-claw's LVGL widgets that die with
 * it). Paint, then display.show(); the face comes back when the hold runs
 * out.
 */

#include "lua_bindings.h"

#include <math.h>
#include <string.h>

extern "C" {
#include "lauxlib.h"
}

#include "display/cap_display.h"

#define DISPLAY_DEFAULT_HOLD_MS 15000

/* Coordinates are whole pixels, but scripts compute them (w / 2 is a float
 * in Lua), so any number is accepted and floored rather than rejected. */
static int arg_int(lua_State *L, int idx)
{
    const lua_Number n = luaL_checknumber(L, idx);
    if (n != n || n > 1e6 || n < -1e6) {
        return luaL_argerror(L, idx, "out of range");
    }
    return (int)floor(n);
}

static int opt_int(lua_State *L, int idx, int def)
{
    return lua_isnoneornil(L, idx) ? def : arg_int(L, idx);
}

/* 0xRRGGBB, or anything cap_display_parse_color() reads ("red", "#ff0000"). */
static uint32_t arg_color(lua_State *L, int idx, int32_t def)
{
    if (lua_isnoneornil(L, idx) && def >= 0) {
        return (uint32_t)def;
    }
    if (lua_type(L, idx) == LUA_TNUMBER) {
        return (uint32_t)luaL_checkinteger(L, idx) & 0xFFFFFF;
    }
    const int32_t c = cap_display_parse_color(luaL_checkstring(L, idx));
    if (c < 0) {
        return (uint32_t)luaL_argerror(L, idx, "colour: 0xRRGGBB, \"#rrggbb\" or a name like \"red\"");
    }
    return (uint32_t)c;
}

/* An integer field of an options table at idx, or def. */
static int opt_field(lua_State *L, int idx, const char *name, int def)
{
    if (!lua_istable(L, idx)) {
        return def;
    }
    lua_getfield(L, idx, name);
    const int v = lua_isnil(L, -1) ? def : (int)floor(luaL_checknumber(L, -1));
    lua_pop(L, 1);
    return v;
}

static int check(lua_State *L, esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return 0;
    case ESP_ERR_INVALID_STATE:
        return luaL_error(L, "display: this robot has no screen");
    case ESP_ERR_INVALID_ARG:
        return luaL_error(L, "display: width, height and radius must be > 0");
    case ESP_ERR_NO_MEM:
        return luaL_error(L, "display: out of memory for the canvas");
    default:
        return luaL_error(L, "display: %s", esp_err_to_name(err));
    }
}

static int l_width(lua_State *L)
{
    lua_pushinteger(L, cap_display_draw_width());
    return 1;
}

static int l_height(lua_State *L)
{
    lua_pushinteger(L, cap_display_draw_height());
    return 1;
}

static int l_rgb(lua_State *L)
{
    const int r = arg_int(L, 1) & 0xFF, g = arg_int(L, 2) & 0xFF, b = arg_int(L, 3) & 0xFF;
    lua_pushinteger(L, (r << 16) | (g << 8) | b);
    return 1;
}

/* display.clear([color]) */
static int l_clear(lua_State *L)
{
    return check(L, cap_display_draw_fill(arg_color(L, 1, 0x000000)));
}

/* display.pixel(x, y, color) */
static int l_pixel(lua_State *L)
{
    return check(L, cap_display_draw_pixel(arg_int(L, 1), arg_int(L, 2), arg_color(L, 3, -1)));
}

/* display.rect(x, y, w, h, color [, {radius=, outline=}]) */
static int l_rect(lua_State *L)
{
    return check(L, cap_display_draw_rect(arg_int(L, 1), arg_int(L, 2), arg_int(L, 3), arg_int(L, 4),
                                          arg_color(L, 5, -1), opt_field(L, 6, "radius", 0),
                                          opt_field(L, 6, "outline", 0)));
}

/* display.circle(cx, cy, r, color [, {outline=}]) */
static int l_circle(lua_State *L)
{
    return check(L, cap_display_draw_circle(arg_int(L, 1), arg_int(L, 2), arg_int(L, 3),
                                            arg_color(L, 4, -1), opt_field(L, 5, "outline", 0)));
}

/* display.line(x1, y1, x2, y2, color [, width]) */
static int l_line(lua_State *L)
{
    return check(L, cap_display_draw_line(arg_int(L, 1), arg_int(L, 2), arg_int(L, 3), arg_int(L, 4),
                                          arg_color(L, 5, -1), opt_int(L, 6, 2)));
}

/* display.arc(cx, cy, r, start_deg, end_deg, color [, width]) */
static int l_arc(lua_State *L)
{
    return check(L, cap_display_draw_arc(arg_int(L, 1), arg_int(L, 2), arg_int(L, 3), arg_int(L, 4),
                                         arg_int(L, 5), arg_color(L, 6, -1), opt_int(L, 7, 4)));
}

/* display.text(x, y, str, color [, {size=14|28|48, align="left"|"center"|"right", w=}]) */
static int l_text(lua_State *L)
{
    const int x = arg_int(L, 1), y = arg_int(L, 2);
    const char *text = luaL_checkstring(L, 3);
    const uint32_t color = arg_color(L, 4, 0xFFFFFF);
    int align = -1;
    if (lua_istable(L, 5)) {
        lua_getfield(L, 5, "align");
        const char *a = lua_tostring(L, -1);
        if (a && (strcmp(a, "center") == 0 || strcmp(a, "centre") == 0)) {
            align = 0;
        } else if (a && strcmp(a, "right") == 0) {
            align = 1;
        }
        lua_pop(L, 1);
    }
    return check(L, cap_display_draw_text(x, y, text, color, opt_field(L, 5, "size", 14), align,
                                          opt_field(L, 5, "w", 0)));
}

/* display.blit(x, y, w, h, data): data is w*h*2 bytes of RGB565, little-endian. */
static int l_blit(lua_State *L)
{
    const int x = arg_int(L, 1), y = arg_int(L, 2), w = arg_int(L, 3), h = arg_int(L, 4);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 5, &len);
    if (w <= 0 || h <= 0 || len < (size_t)w * (size_t)h * 2) {
        return luaL_error(L, "display.blit: need %d bytes (w*h*2), got %d", w * h * 2, (int)len);
    }
    return check(L, cap_display_draw_blit(x, y, w, h, (const uint8_t *)data, len));
}

/* display.show([hold_ms]): 0 = until something else takes the screen. */
static int l_show(lua_State *L)
{
    const int hold = opt_int(L, 1, DISPLAY_DEFAULT_HOLD_MS);
    check(L, cap_display_draw_show(hold < 0 ? 0 : (uint32_t)hold));
    return 0;
}

/* display.face(): back to the eyes now. */
static int l_face(lua_State *L)
{
    (void)L;
    (void)cap_display_clear();
    return 0;
}

static const struct luaL_Reg DISPLAY_LIB[] = {
    { "width",  l_width  },
    { "height", l_height },
    { "rgb",    l_rgb    },
    { "clear",  l_clear  },
    { "pixel",  l_pixel  },
    { "rect",   l_rect   },
    { "circle", l_circle },
    { "line",   l_line   },
    { "arc",    l_arc    },
    { "text",   l_text   },
    { "blit",   l_blit   },
    { "show",   l_show   },
    { "face",   l_face   },
    { NULL, NULL }
};

void lua_register_display_bindings(lua_State *L)
{
    lua_createtable(L, 0, sizeof(DISPLAY_LIB) / sizeof(DISPLAY_LIB[0]) - 1);
    for (const struct luaL_Reg *lib = DISPLAY_LIB; lib->func != NULL; lib++) {
        lua_pushcfunction(L, lib->func);
        lua_setfield(L, -2, lib->name);
    }
    lua_setglobal(L, "display");
}
