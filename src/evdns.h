#ifndef EVDNS_H
#define EVDNS_H

#include "utlua.h"
#include <event2/dns.h>

// Function to get evdns_base from Lua userdata
struct evdns_base* evdns_get_base(lua_State *L, int index);

// Function to check if this is a custom DNS base
int evdns_is_custom(lua_State *L, int index);

// Create evdns with custom nameservers
LUA_API int evdns_create(lua_State *L);

// Module initialization function
LUA_API int luaopen_fan_evdns(lua_State *L);

#endif // EVDNS_H