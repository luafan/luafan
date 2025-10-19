#include "utlua.h"
#include <event2/event.h>
#include <event2/dns.h>
#include <stdlib.h>
#include <string.h>
#include "event_mgr.h"

#define LUA_EVDNS_TYPE "EVDNS_TYPE"

// EVDNS userdata structure
typedef struct {
    struct evdns_base *dnsbase;
    int is_default;  // Flag to indicate if this is the default DNS base
} lua_evdns_t;

// Create new EVDNS base with custom nameservers
LUA_API int evdns_create(lua_State *L) {
    event_mgr_init();

    // Create userdata
    lua_evdns_t *dns = lua_newuserdata(L, sizeof(lua_evdns_t));
    dns->dnsbase = NULL;
    dns->is_default = 0;

    // Set metatable
    luaL_getmetatable(L, LUA_EVDNS_TYPE);
    lua_setmetatable(L, -2);

    // Check if nameservers parameter is provided
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        // No nameservers provided, use default DNS base
        dns->dnsbase = event_mgr_dnsbase();
        dns->is_default = 1;
        return 1;
    }

    // Parse nameservers parameter
    const char **nameservers = NULL;
    int nameserver_count = 0;

    if (lua_isstring(L, 1)) {
        // Single nameserver as string
        nameserver_count = 1;
        nameservers = malloc(sizeof(char*));
        nameservers[0] = lua_tostring(L, 1);
    } else if (lua_istable(L, 1)) {
        // Multiple nameservers as table
        nameserver_count = lua_objlen(L, 1);
        if (nameserver_count > 0) {
            nameservers = malloc(nameserver_count * sizeof(char*));
            for (int i = 1; i <= nameserver_count; i++) {
                lua_rawgeti(L, 1, i);
                if (lua_isstring(L, -1)) {
                    nameservers[i-1] = lua_tostring(L, -1);
                } else {
                    nameservers[i-1] = NULL;
                }
                lua_pop(L, 1);
            }
        }
    } else {
        if (nameservers) free(nameservers);
        luaL_error(L, "nameservers must be a string or table of strings");
        return 0;
    }

    // Create new DNS base
    dns->dnsbase = evdns_base_new(event_mgr_base_current(), EVDNS_BASE_INITIALIZE_NAMESERVERS);
    if (!dns->dnsbase) {
        if (nameservers) free(nameservers);
        // Fallback to default DNS base
        dns->dnsbase = event_mgr_dnsbase();
        dns->is_default = 1;
        return 1;
    }

    // Clear default nameservers and add custom ones
    evdns_base_clear_nameservers_and_suspend(dns->dnsbase);

    int added_count = 0;
    for (int i = 0; i < nameserver_count; i++) {
        if (nameservers[i] && strlen(nameservers[i]) > 0) {
            if (evdns_base_nameserver_ip_add(dns->dnsbase, nameservers[i]) == 0) {
                added_count++;
            }
        }
    }

    if (nameservers) free(nameservers);

    if (added_count == 0) {
        // No valid nameservers added, fallback to default
        evdns_base_free(dns->dnsbase, 0);
        dns->dnsbase = event_mgr_dnsbase();
        dns->is_default = 1;
    } else {
        evdns_base_resume(dns->dnsbase);
        dns->is_default = 0;
    }

    return 1;
}

// Get the underlying evdns_base pointer (for use by other modules)
struct evdns_base* evdns_get_base(lua_State *L, int index) {
    lua_evdns_t *dns = luaL_checkudata(L, index, LUA_EVDNS_TYPE);
    return dns->dnsbase;
}

// Check if this is a custom DNS base (not the default one)
int evdns_is_custom(lua_State *L, int index) {
    lua_evdns_t *dns = luaL_checkudata(L, index, LUA_EVDNS_TYPE);
    return !dns->is_default;
}

// EVDNS garbage collection
LUA_API int evdns_gc(lua_State *L) {
    lua_evdns_t *dns = luaL_checkudata(L, 1, LUA_EVDNS_TYPE);

    // Only free custom DNS bases, not the default one
    if (dns->dnsbase && !dns->is_default) {
        evdns_base_free(dns->dnsbase, 0);
        dns->dnsbase = NULL;
    }

    return 0;
}

// EVDNS tostring
LUA_API int evdns_tostring(lua_State *L) {
    lua_evdns_t *dns = luaL_checkudata(L, 1, LUA_EVDNS_TYPE);

    if (dns->is_default) {
        lua_pushstring(L, "<evdns: default>");
    } else {
        lua_pushstring(L, "<evdns: custom>");
    }

    return 1;
}

// Module registration
static const luaL_Reg evdnslib[] = {
    {"create", evdns_create},
    {NULL, NULL}
};

// Module initialization
LUA_API int luaopen_fan_evdns(lua_State *L) {
    // Register EVDNS metatable
    luaL_newmetatable(L, LUA_EVDNS_TYPE);

    lua_pushcfunction(L, evdns_gc);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, evdns_tostring);
    lua_setfield(L, -2, "__tostring");

    // Set __index to self
    lua_pushstring(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);

    lua_pop(L, 1);

    // Create and return module table
    lua_newtable(L);
    luaL_register(L, NULL, evdnslib);

    return 1;
}
