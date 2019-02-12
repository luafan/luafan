#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "utlua.h"

#define LUA_FIFO_CONNECTION_TYPE "FIFO_CONNECTION_TYPE"

typedef struct
{
  int socket;
  char *name;
  int delete_on_close;
  int onReadRef;

  int onSendReadyRef;
  int onDisconnectedRef;

  lua_State *mainthread;

  struct event *read_ev;
  struct event *write_ev;
} FIFO;

static void fifo_write_cb(evutil_socket_t fd, short event, void *arg)
{
  FIFO *fifo = (FIFO *)arg;

  if (fifo->onSendReadyRef != LUA_NOREF)
  {
    lua_State *mainthread = fifo->mainthread;
    lua_lock(mainthread);
    lua_State *co = lua_newthread(mainthread);
    PUSH_REF(mainthread);
    lua_unlock(mainthread);

    lua_rawgeti(co, LUA_REGISTRYINDEX, fifo->onSendReadyRef);
    FAN_RESUME(co, mainthread, 0);
    POP_REF(mainthread);
  }
}

static void fifo_read_cb(evutil_socket_t fd, short event, void *arg)
{
  FIFO *fifo = (FIFO *)arg;

  char buf[READ_BUFF_LEN];
  size_t len = read(fd, buf, READ_BUFF_LEN);

  if (len <= 0)
  {
    if (len < 0 && (errno == EAGAIN || errno == EINTR))
    {
      printf("fifo_read_cb: %s\n", strerror(errno));
      return;
    }
    if (fifo->onDisconnectedRef != LUA_NOREF)
    {
      lua_State *mainthread = fifo->mainthread;
      lua_lock(mainthread);
      lua_State *co = lua_newthread(mainthread);
      PUSH_REF(mainthread);
      lua_unlock(mainthread);

      lua_rawgeti(co, LUA_REGISTRYINDEX, fifo->onDisconnectedRef);

      lua_pushstring(co, len < 0 && errno ? strerror(errno) : "pipe closed.");

      FAN_RESUME(co, mainthread, 1);
      POP_REF(mainthread);
    }
    else
    {
      printf("fifo_read_cb:%s: %s\n", fifo->name, len < 0 && errno ? strerror(errno) : "pipe closed.");
    }

    // if (fifo->read_ev) {
    //   event_free(fifo->read_ev);
    //   fifo->read_ev = NULL;
    // }
    return;
  }

  if (fifo->onReadRef != LUA_NOREF)
  {
    lua_State *mainthread = fifo->mainthread;
    lua_lock(mainthread);
    lua_State *co = lua_newthread(mainthread);
    PUSH_REF(mainthread);
    lua_unlock(mainthread);

    lua_rawgeti(co, LUA_REGISTRYINDEX, fifo->onReadRef);
    lua_pushlstring(co, buf, len);
    FAN_RESUME(co, mainthread, 1);
    POP_REF(mainthread);
  }
}

LUA_API int luafan_fifo_connect(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1);

  lua_getfield(L, 1, "name");
  const char *fifoname = luaL_checkstring(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 1, "mode");
  lua_Integer mode = luaL_optinteger(L, -1, 0600);
  lua_pop(L, 1);

  int found_fifo = 0;
  struct stat st;
  if (lstat(fifoname, &st) == 0)
  {
    if (S_ISREG(st.st_mode))
    {
      luaL_error(L, "regual file exist: %s", fifoname);
    }
    else if (S_ISFIFO(st.st_mode))
    {
      found_fifo = 1;
    }
  }

  if (!found_fifo)
  {
    // printf("creating %s\n", fifoname);
    unlink(fifoname);
    int ret = mkfifo(fifoname, mode);
    if (ret != 0)
    {
      perror(fifoname);
      return 0;
    }
  }

  FIFO *fifo = (FIFO *)lua_newuserdata(L, sizeof(FIFO));
  memset(fifo, 0, sizeof(FIFO));

  fifo->name = strdup(fifoname);

  luaL_getmetatable(L, LUA_FIFO_CONNECTION_TYPE);
  lua_setmetatable(L, -2);
  fifo->mainthread = utlua_mainthread(L);
  // fifo->read_ev = NULL;
  // fifo->write_ev = NULL;
  // fifo->name = NULL;

  lua_getfield(L, 1, "delete_on_close");
  fifo->delete_on_close = lua_toboolean(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 1, "rwmode");
  const char *rwmode = luaL_optstring(L, -1, "rn");
  lua_pop(L, 1);

  int rwmodei = -1;

  if (strstr(rwmode, "r"))
  {
    rwmodei = O_RDONLY;
    SET_FUNC_REF_FROM_TABLE(L, fifo->onReadRef, 1, "onread")
  }
  else
  {
    fifo->onReadRef = LUA_NOREF;
  }

  if (strstr(rwmode, "w"))
  {
    if (rwmodei == O_RDONLY)
    {
      rwmodei = O_RDWR;
    }
    else
    {
      rwmodei = O_WRONLY;
    }

    SET_FUNC_REF_FROM_TABLE(L, fifo->onSendReadyRef, 1, "onsendready")
    SET_FUNC_REF_FROM_TABLE(L, fifo->onDisconnectedRef, 1, "ondisconnected")
  }
  else
  {
    fifo->onSendReadyRef = LUA_NOREF;
    fifo->onDisconnectedRef = LUA_NOREF;
  }

  int socket = open(fifoname, rwmodei | O_NONBLOCK, 0);
  if (socket == -1)
  {
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
  }

  fifo->socket = socket;

  if (fifo->onSendReadyRef != LUA_NOREF)
  {
    fifo->write_ev =
        event_new(event_mgr_base(), socket, EV_WRITE, fifo_write_cb, fifo);
  }
  else
  {
    fifo->write_ev = NULL;
  }

  if (fifo->onReadRef != LUA_NOREF)
  {
    fifo->read_ev = event_new(event_mgr_base(), socket, EV_PERSIST | EV_READ,
                              fifo_read_cb, fifo);
    event_add(fifo->read_ev, NULL);
  }
  else
  {
    fifo->read_ev = NULL;
  }

  return 1;
}

LUA_API int luafan_fifo_send_request(lua_State *L)
{
  FIFO *fifo = luaL_checkudata(L, 1, LUA_FIFO_CONNECTION_TYPE);

  if (fifo->write_ev)
  {
    event_add(fifo->write_ev, NULL);
    lua_pushboolean(L, true);
    return 1;
  }
  else
  {
    if (fifo->onSendReadyRef == LUA_NOREF)
    {
      luaL_error(L, "onsendready not defined.");
      return 0;
    }
    else
    {
      lua_pushboolean(L, false);
      lua_pushliteral(L, "not writable.");

      return 2;
    }
  }
}

LUA_API int luafan_fifo_send(lua_State *L)
{
  FIFO *fifo = luaL_checkudata(L, 1, LUA_FIFO_CONNECTION_TYPE);
  size_t data_len;
  const char *data = luaL_optlstring(L, 2, NULL, &data_len);
  if (data && data_len > 0)
  {
    size_t len = write(fifo->socket, data, data_len);
    if (len <= 0)
    {
      if (len < 0 && (errno == EAGAIN || errno == EINTR))
      {
        printf("luafan_fifo_send: %s\n", strerror(errno));
        lua_pushinteger(L, 0);
        return 1;
      }

      if (fifo->onDisconnectedRef != LUA_NOREF)
      {
        lua_State *mainthread = fifo->mainthread;
        lua_lock(mainthread);
        lua_State *co = lua_newthread(mainthread);
        PUSH_REF(mainthread);
        lua_unlock(mainthread);

        lua_rawgeti(co, LUA_REGISTRYINDEX, fifo->onDisconnectedRef);

        lua_pushstring(co, len < 0 && errno ? strerror(errno) : "pipe closed.");

        FAN_RESUME(co, mainthread, 1);
        POP_REF(mainthread);
      }
      else
      {
        printf("luafan_fifo_send:%s: %s\n", fifo->name, len < 0 && errno ? strerror(errno) : "pipe closed.");
      }

      if (fifo->write_ev)
      {
        event_free(fifo->write_ev);
        fifo->write_ev = NULL;
      }
    }

    lua_pushinteger(L, len);
  }
  else
  {
    lua_pushinteger(L, 0);
  }

  return 1;
}

LUA_API int luafan_fifo_close(lua_State *L)
{
  FIFO *fifo = luaL_checkudata(L, 1, LUA_FIFO_CONNECTION_TYPE);

  CLEAR_REF(L, fifo->onReadRef)
  CLEAR_REF(L, fifo->onSendReadyRef)
  CLEAR_REF(L, fifo->onDisconnectedRef)

  if (event_mgr_base_current() && fifo->read_ev)
  {
    event_free(fifo->read_ev);
    fifo->read_ev = NULL;
  }

  if (event_mgr_base_current() && fifo->write_ev)
  {
    event_free(fifo->write_ev);
    fifo->write_ev = NULL;
  }

  if (fifo->socket)
  {
    close(fifo->socket);
    fifo->socket = 0;
  }

  if (fifo->delete_on_close)
  {
    if (unlink(fifo->name))
    {
      if (errno != ENOENT)
      {
        printf("unlink %s, error = %s\n", fifo->name, strerror(errno));
      }
    }
    else
    {
      // printf("unlinked %s\n", fifo->name);
    }
  }

  free(fifo->name);
  fifo->name = NULL;

  return 0;
}

LUA_API int luafan_fifo_gc(lua_State *L) { return luafan_fifo_close(L); }

static const struct luaL_Reg fifolib[] = {
    {"connect", luafan_fifo_connect}, {NULL, NULL},
};

LUA_API int luaopen_fan_fifo(lua_State *L)
{
  luaL_newmetatable(L, LUA_FIFO_CONNECTION_TYPE);
  lua_pushcfunction(L, &luafan_fifo_send);
  lua_setfield(L, -2, "send");

  lua_pushcfunction(L, &luafan_fifo_send_request);
  lua_setfield(L, -2, "send_req");

  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_rawset(L, -3);

  lua_pushstring(L, "__gc");
  lua_pushcfunction(L, &luafan_fifo_gc);
  lua_rawset(L, -3);

  lua_pushstring(L, "close");
  lua_pushcfunction(L, &luafan_fifo_close);
  lua_rawset(L, -3);

  lua_pop(L, 1);

  lua_newtable(L);
  luaL_register(L, "fifo", fifolib);
  return 1;
}
