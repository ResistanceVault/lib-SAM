#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "say_backend.h"

#if defined(_WIN32)
#undef LUAMOD_API
#define LUAMOD_API __declspec(dllexport)
#endif

typedef struct {
    unsigned char *data;
    size_t size;
} say_blob_t;

static say_blob_t *check_blob(lua_State *L, int index)
{
    return (say_blob_t *)luaL_checkudata(L, index, "say.blob");
}

static int blob_get_data(lua_State *L)
{
    say_blob_t *blob = check_blob(L, 1);
    lua_pushinteger(L, (lua_Integer)(uintptr_t)blob->data);
    return 1;
}

static int blob_get_size(lua_State *L)
{
    say_blob_t *blob = check_blob(L, 1);
    lua_pushinteger(L, (lua_Integer)blob->size);
    return 1;
}

static int blob_gc(lua_State *L)
{
    say_blob_t *blob = check_blob(L, 1);
    free(blob->data);
    blob->data = NULL;
    blob->size = 0;
    return 0;
}

static void push_defaults_table(lua_State *L)
{
    lua_createtable(L, 0, 12);

    lua_pushstring(L, "en");
    lua_setfield(L, -2, "language");

    lua_pushinteger(L, 44100);
    lua_setfield(L, -2, "sample_rate");

    lua_pushinteger(L, 5);
    lua_setfield(L, -2, "frame_ms");

    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "phonemes");

    lua_pushstring(L, "raw");
    lua_setfield(L, -2, "format");

    lua_pushnumber(L, 1.0);
    lua_setfield(L, -2, "gain");

    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "phone");

    lua_pushinteger(L, 72);
    lua_setfield(L, -2, "speed");

    lua_pushinteger(L, 64);
    lua_setfield(L, -2, "pitch");

    lua_pushinteger(L, 128);
    lua_setfield(L, -2, "mouth");

    lua_pushinteger(L, 128);
    lua_setfield(L, -2, "throat");

    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "sing");
}

static void read_optional_string(lua_State *L, int index, const char *field, const char **out)
{
    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        *out = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
}

static void read_optional_boolean(lua_State *L, int index, const char *field, int *out)
{
    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        *out = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
}

static void read_optional_integer(lua_State *L, int index, const char *field, int *out)
{
    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        *out = (int)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
}

static void read_optional_number(lua_State *L, int index, const char *field, double *out)
{
    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        *out = (double)luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);
}

static void read_options(lua_State *L, int index, say_options_t *options)
{
    const char *format_name = NULL;
    int amiga = 0;

    say_default_options(options);
    if (lua_isnoneornil(L, index)) {
        return;
    }

    luaL_checktype(L, index, LUA_TTABLE);

    read_optional_string(L, index, "language", &options->language);
    read_optional_string(L, index, "lang", &options->language);
    read_optional_integer(L, index, "sample_rate", &options->sample_rate);
    read_optional_integer(L, index, "rate", &options->sample_rate);
    read_optional_integer(L, index, "frame_ms", &options->frame_ms);
    read_optional_boolean(L, index, "phonemes", &options->phonemes);
    read_optional_number(L, index, "gain", &options->gain);
    read_optional_boolean(L, index, "phone", &options->phone);
    read_optional_integer(L, index, "speed", &options->speed);
    read_optional_integer(L, index, "pitch", &options->pitch);
    read_optional_integer(L, index, "mouth", &options->mouth);
    read_optional_integer(L, index, "throat", &options->throat);
    read_optional_boolean(L, index, "sing", &options->sing);
    read_optional_boolean(L, index, "amiga", &amiga);
    if (amiga) {
        luaL_error(L, "amiga = true is unsupported by the SAM backend");
    }

    lua_getfield(L, index, "format");
    if (!lua_isnil(L, -1)) {
        format_name = luaL_checkstring(L, -1);
        if (!say_parse_format_name(format_name, &options->format)) {
            luaL_error(L, "unsupported output format");
        }
    }
    lua_pop(L, 1);
}

static void push_info_table(lua_State *L, const say_result_t *result)
{
    lua_createtable(L, 0, 11);

    lua_pushstring(L, result->language);
    lua_setfield(L, -2, "language");

    lua_pushstring(L, result->format);
    lua_setfield(L, -2, "format");

    lua_pushinteger(L, result->sample_rate);
    lua_setfield(L, -2, "sample_rate");

    lua_pushinteger(L, result->frame_ms);
    lua_setfield(L, -2, "frame_ms");

    lua_pushboolean(L, result->phonemes);
    lua_setfield(L, -2, "phonemes");

    lua_pushinteger(L, result->channels);
    lua_setfield(L, -2, "channels");

    lua_pushinteger(L, result->bits_per_sample);
    lua_setfield(L, -2, "bits_per_sample");

    lua_pushinteger(L, (lua_Integer)result->sample_count);
    lua_setfield(L, -2, "sample_count");

    lua_pushinteger(L, (lua_Integer)result->size);
    lua_setfield(L, -2, "byte_count");

    lua_pushnumber(L, result->duration_seconds);
    lua_setfield(L, -2, "duration_seconds");

    lua_pushstring(L, result->pcm_encoding);
    lua_setfield(L, -2, "pcm_encoding");
}

static int l_synthesize(lua_State *L)
{
    const char *input = luaL_checkstring(L, 1);
    say_options_t options;
    say_result_t result;
    say_blob_t *blob;
    char *error_message = NULL;

    memset(&result, 0, sizeof(result));
    read_options(L, 2, &options);

    if (!say_synthesize(input, &options, 0, SAY_INPUT_LITERAL, input, &result, NULL, &error_message)) {
        const char *message = error_message != NULL ? error_message : "synthesis failed";
        lua_pushstring(L, message);
        say_free_string(error_message);
        return lua_error(L);
    }

    blob = (say_blob_t *)lua_newuserdatauv(L, sizeof(*blob), 0);
    blob->data = result.data;
    blob->size = result.size;
    luaL_getmetatable(L, "say.blob");
    lua_setmetatable(L, -2);

    result.data = NULL;
    push_info_table(L, &result);
    say_free_result(&result);
    return 2;
}

static int l_debug_report(lua_State *L)
{
    const char *input = luaL_checkstring(L, 1);
    say_options_t options;
    char *report = NULL;
    char *error_message = NULL;

    read_options(L, 2, &options);

    if (!say_synthesize(input, &options, 1, SAY_INPUT_LITERAL, input, NULL, &report, &error_message)) {
        const char *message = error_message != NULL ? error_message : "debug report failed";
        lua_pushstring(L, message);
        say_free_string(error_message);
        return lua_error(L);
    }

    lua_pushstring(L, report != NULL ? report : "");
    say_free_string(report);
    return 1;
}

static int l_default_options(lua_State *L)
{
    push_defaults_table(L);
    return 1;
}

LUAMOD_API int luaopen_say(lua_State *L)
{
    static const luaL_Reg blob_methods[] = {
        {"GetData", blob_get_data},
        {"GetSize", blob_get_size},
        {NULL, NULL}
    };
    static const luaL_Reg blob_meta[] = {
        {"__gc", blob_gc},
        {NULL, NULL}
    };
    static const luaL_Reg module_functions[] = {
        {"synthesize", l_synthesize},
        {"debug_report", l_debug_report},
        {"default_options", l_default_options},
        {NULL, NULL}
    };

    luaL_newmetatable(L, "say.blob");
    luaL_setfuncs(L, blob_meta, 0);
    lua_newtable(L);
    luaL_setfuncs(L, blob_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_newlib(L, module_functions);

    lua_pushstring(L, "en");
    lua_setfield(L, -2, "LANG_EN");
    lua_pushstring(L, "raw");
    lua_setfield(L, -2, "FORMAT_RAW");
    lua_pushstring(L, "aiff");
    lua_setfield(L, -2, "FORMAT_AIFF");
    lua_pushstring(L, "wav");
    lua_setfield(L, -2, "FORMAT_WAV");

    return 1;
}
