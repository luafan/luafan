
#include "utlua.h"

#include <net/if.h>

#define LUA_UDPD_CONNECTION_TYPE "UDPD_CONNECTION_TYPE"
#define LUA_UDPD_DEST_TYPE "LUA_UDPD_DEST_TYPE"

typedef struct {
    int selfRef;

    lua_State *mainthread;
    int _ref_;
    
    int onReadRef;
    int onSendReadyRef;

    char *host;
    char *bind_host;
    int port;
    int bind_port;
    int socket_fd;
    struct sockaddr_storage addr;
    socklen_t addrlen;

    int interface;

    struct event *read_ev;
    struct event *write_ev;
} Conn;

typedef struct {
    struct sockaddr_storage si_client;
    socklen_t client_len;
} Dest;

LUA_API int lua_udpd_conn_gc(lua_State *L) {
    Conn *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
    FREE_STR(conn->bind_host)
    FREE_STR(conn->host)

    CLEAR_REF(L, conn->onReadRef)
    CLEAR_REF(L, conn->onSendReadyRef)

    if (conn->read_ev) {
        event_free(conn->read_ev);
        conn->read_ev = NULL;
    }

    if (conn->write_ev) {
        event_free(conn->write_ev);
        conn->write_ev = NULL;
    }

    if (conn->socket_fd) {
        EVUTIL_CLOSESOCKET(conn->socket_fd);
        conn->socket_fd = 0;
    }

    return 0;
}

#define BUFLEN 2000

static void udpd_writecb(evutil_socket_t fd, short what, void *arg) {
    Conn *conn = (Conn *)arg;

    if (conn->onSendReadyRef != LUA_NOREF) {
        lua_State *mainthread = conn->mainthread;
        lua_lock(mainthread);
        lua_State *co = lua_newthread(mainthread);
        PUSH_REF(mainthread);
        lua_unlock(mainthread);

        lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onSendReadyRef);
        FAN_RESUME(co, mainthread, 0);
        POP_REF(mainthread);
    }
}

static void udpd_readcb(evutil_socket_t fd, short what, void *arg) {
    Conn *conn = (Conn *)arg;

    struct sockaddr_storage si_client;
    socklen_t client_len = sizeof(si_client);

    char buf[BUFLEN];
    ssize_t len = recvfrom(conn->socket_fd, buf, BUFLEN, 0, (struct sockaddr *)&si_client, &client_len);
    if (len >= 0) {
        if (conn->onReadRef != LUA_NOREF) {
            lua_State *mainthread = conn->mainthread;
            lua_lock(mainthread);
            lua_State *co = lua_newthread(mainthread);
            PUSH_REF(mainthread);
            lua_unlock(mainthread);

            lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onReadRef);
            lua_pushlstring(co, (const char *)buf, len);

            Dest *dest = lua_newuserdata(co, sizeof(Dest));
            luaL_getmetatable(co, LUA_UDPD_DEST_TYPE);
            lua_setmetatable(co, -2);

            memcpy(&dest->si_client, &si_client, sizeof(si_client));
            dest->client_len = client_len;

            FAN_RESUME(co, mainthread, 2);
            POP_REF(mainthread);
        }
    }
}

static int setnonblock(int fd) {
    int flags;

    flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return flags;
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0)
        return -1;

    return 0;
}

static int luaudpd_reconnect(Conn *conn, lua_State *L) {
    if (conn->socket_fd) {
        EVUTIL_CLOSESOCKET(conn->socket_fd);
        conn->socket_fd = 0;
    }

    if (conn->write_ev) {
        event_free(conn->write_ev);
        conn->write_ev = NULL;
    }

    if (conn->read_ev) {
        event_free(conn->read_ev);
        conn->read_ev = NULL;
    }

    int socket_fd = 0;
    if ((socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        return 0;
    }

    conn->socket_fd = socket_fd;
    
    int value = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) == -1) {
        return 0;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value)) == -1) {
        return 0;
    }

#ifdef IP_BOUND_IF
    if (conn->interface) {
        setsockopt(socket_fd, IPPROTO_IP, IP_BOUND_IF, &conn->interface, sizeof(conn->interface));
    }
#endif

    if (conn->bind_host) {
        char portbuf[6];
        evutil_snprintf(portbuf, sizeof(portbuf), "%d", conn->bind_port);

        struct evutil_addrinfo hints = {0};
        struct evutil_addrinfo *answer = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_flags = EVUTIL_AI_ADDRCONFIG;
        int err = evutil_getaddrinfo(conn->bind_host, portbuf, &hints, &answer);
        if (err < 0) {
            luaL_error(L, "invalid address %s:%d", conn->bind_host, conn->bind_port);
        }

        // printf("binding fd=%d %s:%d\n", socket_fd, conn->bind_host,
        // conn->bind_port);

        int ret = bind(socket_fd, answer->ai_addr, answer->ai_addrlen);
        evutil_freeaddrinfo(answer);

        if (ret == -1) {
            luaL_error(L, "udp bind: %s", strerror(errno));
            return 0;
        }
    } else {
        struct sockaddr_in addr;
        memset((char *)&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(conn->bind_port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(socket_fd, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
            return 0;
        }
    }

    if (!conn->bind_port) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        if (getsockname(socket_fd, (struct sockaddr *)&addr, &len) == -1) {
            return 0;
        }

        conn->bind_port = ntohs(addr.sin_port);
    }

    if (setnonblock(socket_fd) < 0) {
        return 0;
    }

    if (conn->onSendReadyRef != LUA_NOREF) {
        conn->write_ev = event_new(event_mgr_base(), socket_fd, EV_WRITE, udpd_writecb, conn);
        event_add(conn->write_ev, NULL);
    } else {
        conn->write_ev = NULL;
    }

    if (conn->onReadRef != LUA_NOREF) {
        conn->read_ev = event_new(event_mgr_base(), socket_fd, EV_PERSIST | EV_READ, udpd_readcb, conn);
        event_add(conn->read_ev, NULL);
    } else {
        conn->read_ev = NULL;
    }

    return 1;
}

void udpd_conn_new_callback(int errcode, struct evutil_addrinfo *addr, void *ptr) {
    Conn *conn = ptr;
    lua_State *L = NULL;
    REF_STATE_GET(conn, L);

    if (errcode) {
        if (conn->selfRef == LUA_NOREF) {
            lua_settop(L, 1);
        }

        lua_pushnil(L);
        lua_pushfstring(L, "'%s' -> %s", conn->host, evutil_gai_strerror(errcode));

        if (conn->selfRef != LUA_NOREF) {
            FAN_RESUME(L, NULL, 2);
            
            CLEAR_REF(L, conn->selfRef);
        }
    } else {
        memcpy(&conn->addr, addr->ai_addr, addr->ai_addrlen);
        conn->addrlen = addr->ai_addrlen;

        evutil_freeaddrinfo(addr);

        luaudpd_reconnect(conn, L);

        if (conn->selfRef != LUA_NOREF) {
            // Conn userdata on the top.
            lua_rawgeti(L, LUA_REGISTRYINDEX, conn->selfRef);
            FAN_RESUME(L, NULL, 1);

            CLEAR_REF(L, conn->selfRef);
        }
    }

    REF_STATE_CLEAR(conn);
}

LUA_API int lua_udpd_conn_rebind(lua_State *L) {
    Conn *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
    luaudpd_reconnect(conn, L);

    return 0;
}

LUA_API int udpd_new(lua_State *L) {
    event_mgr_init();

    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);

    Conn *conn = lua_newuserdata(L, sizeof(Conn));
    memset(conn, 0, sizeof(Conn));

    lua_getfield(L, 1, "interface");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *interface = lua_tostring(L, -1);
        conn->interface = if_nametoindex(interface);
    }
    lua_pop(L, 1);

    SET_FUNC_REF_FROM_TABLE(L, conn->onReadRef, 1, "onread")
    SET_FUNC_REF_FROM_TABLE(L, conn->onSendReadyRef, 1, "onsendready")

    DUP_STR_FROM_TABLE(L, conn->host, 1, "host")
    SET_INT_FROM_TABLE(L, conn->port, 1, "port")

    DUP_STR_FROM_TABLE(L, conn->bind_host, 1, "bind_host")
    SET_INT_FROM_TABLE(L, conn->bind_port, 1, "bind_port")

    luaL_getmetatable(L, LUA_UDPD_CONNECTION_TYPE);
    lua_setmetatable(L, -2);

    REF_STATE_SET(conn, L);
    
    conn->selfRef = LUA_NOREF;

    char portbuf[6];
    evutil_snprintf(portbuf, sizeof(portbuf), "%d", conn->port);

    struct evutil_addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = EVUTIL_AI_ADDRCONFIG;

    struct evdns_getaddrinfo_request *req =
        evdns_getaddrinfo(event_mgr_dnsbase(), conn->host, portbuf, &hints, udpd_conn_new_callback, conn);
    if (req == NULL) {
        return lua_gettop(L) - 1;
    } else {
        conn->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);
        return lua_yield(L, 0);
    }
}

struct make_dest_callback_data {
    const char *host;
    
    lua_State *mainthread;
    int _ref_;
    
    int yielded;
};

void udpd_conn_make_dest_callback(int errcode, struct evutil_addrinfo *addr, void *ptr) {
    struct make_dest_callback_data *data = ptr;
    lua_State *L = NULL;
    REF_STATE_GET(data, L);

    if (errcode) {
        lua_pushnil(L);
        lua_pushfstring(L, "'%s' -> %s", data->host, evutil_gai_strerror(errcode));

        if (data->yielded) {
            FAN_RESUME(L, NULL, 2);
            REF_STATE_CLEAR(data);
        }
    } else {
        Dest *dest = lua_newuserdata(L, sizeof(Dest));
        luaL_getmetatable(L, LUA_UDPD_DEST_TYPE);
        lua_setmetatable(L, -2);

        memcpy(&dest->si_client, addr->ai_addr, addr->ai_addrlen);
        dest->client_len = addr->ai_addrlen;

        if (data->yielded) {
            FAN_RESUME(L, NULL, 1);
            REF_STATE_CLEAR(data);
        }

        evutil_freeaddrinfo(addr);
        free(data);
    }
}

LUA_API int udpd_conn_make_dest(lua_State *L) {
    event_mgr_init();

    const char *host = luaL_checkstring(L, 1);
    char portbuf[6];
    evutil_snprintf(portbuf, sizeof(portbuf), "%d", (int)luaL_checkinteger(L, 2));

    lua_settop(L, 2);

    struct make_dest_callback_data *data = malloc(sizeof(struct make_dest_callback_data));

    data->host = host;
    data->yielded = false;
    REF_STATE_SET(data, L);

    struct evutil_addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = EVUTIL_AI_ADDRCONFIG;

    struct evdns_getaddrinfo_request *req =
        evdns_getaddrinfo(event_mgr_dnsbase(), host, portbuf, &hints, udpd_conn_make_dest_callback, data);
    if (req == NULL) {
        // return all the values pushed on the stack by callback.
        return lua_gettop(L) - 2;
    } else {
        data->yielded = true;
        return lua_yield(L, 0);
    }
}

static const luaL_Reg udpdlib[] = {{"new", udpd_new}, {"make_dest", udpd_conn_make_dest}, {NULL, NULL}};

LUA_API int udpd_conn_tostring(lua_State *L) {
    Conn *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
    lua_pushfstring(L, "<host: %s, port: %d, bind_host: %s, bind_port: %d>", conn->host, conn->port, conn->bind_host,
                    conn->bind_port);
    return 1;
}
LUA_API int udpd_conn_send(lua_State *L) {
    Conn *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);

    if (!conn->socket_fd) {
        lua_pushnil(L);
        lua_pushliteral(L, "socket was not created.");
        return 2;
    }

    ssize_t ret = 0;
    if (data && len >= 0) {
        if (lua_gettop(L) > 2) {
            Dest *dest = luaL_checkudata(L, 3, LUA_UDPD_DEST_TYPE);
            ret = sendto(conn->socket_fd, data, len, 0, (struct sockaddr *)&dest->si_client, dest->client_len);
        } else {
            ret = sendto(conn->socket_fd, data, len, 0, (struct sockaddr *)&conn->addr, conn->addrlen);
        }
    }

    if (ret < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    } else {
        lua_pushinteger(L, ret);
        return 1;
    }
}

LUA_API int udpd_conn_send_request(lua_State *L) {
    Conn *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
    if (conn->write_ev) {
        event_add(conn->write_ev, NULL);
    } else {
        if (conn->onSendReadyRef == LUA_NOREF) {
            luaL_error(L, "onsendready not defined.");
        }
        luaL_error(L, "not writable.");
    }

    return 0;
}

LUA_API int lua_udpd_dest_host(lua_State *L) {
    Dest *dest = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);
    char buf[20] = {0};
    struct sockaddr_in *addr = (struct sockaddr_in *)&dest->si_client;
    const char *out = inet_ntop(AF_INET, (void *)(&addr->sin_addr), buf, 20);
    if (out) {
        lua_pushstring(L, buf);
        return 1;
    }

    return 0;
}

LUA_API int lua_udpd_dest_port(lua_State *L) {
    Dest *dest = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);
    struct sockaddr_in *addr = (struct sockaddr_in *)&dest->si_client;
    lua_pushinteger(L, ntohs(addr->sin_port));
    return 1;
}

LUA_API int lua_udpd_dest_tostring(lua_State *L) {
    Dest *dest = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);

    char buf[20];
    struct sockaddr_in *addr = (struct sockaddr_in *)&dest->si_client;
    const char *out = inet_ntop(AF_INET, (void *)(&addr->sin_addr), buf, 20);
    if (out) {
        lua_pushfstring(L, "%s:%d", out, ntohs(addr->sin_port));
        return 1;
    }

    return 0;
}

LUA_API int lua_udpd_dest_eq(lua_State *L) {
    Dest *dest1 = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);
    Dest *dest2 = luaL_checkudata(L, 2, LUA_UDPD_DEST_TYPE);

    if (dest1->client_len != dest2->client_len) {
        lua_pushboolean(L, 0);
        return 1;
    }

    if (memcmp(&dest1->si_client, &dest2->si_client, dest1->client_len) == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }

    lua_pushboolean(L, 0);
    return 1;
}

LUA_API int udpd_conn_get_port(lua_State *L) {
    Conn *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
    lua_pushinteger(L, regress_get_socket_port(conn->socket_fd));
    return 1;
}

LUA_API int luaopen_fan_udpd(lua_State *L) {
    luaL_newmetatable(L, LUA_UDPD_CONNECTION_TYPE);

    lua_pushcfunction(L, &udpd_conn_send);
    lua_setfield(L, -2, "send");

    lua_pushcfunction(L, &udpd_conn_send_request);
    lua_setfield(L, -2, "send_req");

    lua_pushcfunction(L, &lua_udpd_conn_gc);
    lua_setfield(L, -2, "close");

    lua_pushcfunction(L, &lua_udpd_conn_rebind);
    lua_setfield(L, -2, "rebind");

    lua_pushcfunction(L, &udpd_conn_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pushcfunction(L, &udpd_conn_get_port);
    lua_setfield(L, -2, "getPort");

    lua_pushstring(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);

    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, &lua_udpd_conn_gc);
    lua_rawset(L, -3);
    lua_pop(L, 1);

    luaL_newmetatable(L, LUA_UDPD_DEST_TYPE);
    lua_pushcfunction(L, &lua_udpd_dest_host);
    lua_setfield(L, -2, "getHost");

    lua_pushcfunction(L, &lua_udpd_dest_port);
    lua_setfield(L, -2, "getPort");

    lua_pushcfunction(L, &lua_udpd_dest_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pushcfunction(L, &lua_udpd_dest_eq);
    lua_setfield(L, -2, "__eq");

    lua_pushstring(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);

    lua_pop(L, 1);

    lua_newtable(L);
    luaL_register(L, "udpd", udpdlib);

    return 1;
}
