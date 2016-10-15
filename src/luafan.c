#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "utlua.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static struct event *mainevent;
static int main_ref;
static lua_State *mainState;

static void main_handler(const int fd, const short which, void *arg) {
  evtimer_del(mainevent);
  free(mainevent);

  lua_State *co = lua_newthread(mainState);
  PUSH_REF(mainState);

  lua_rawgeti(co, LUA_REGISTRYINDEX, main_ref);
  utlua_resume(co, NULL, 0);

  luaL_unref(co, LUA_REGISTRYINDEX, main_ref);
  main_ref = LUA_NOREF;

  POP_REF(mainState);
}

LUA_API int luafan_start(lua_State *L) {
  if (lua_gettop(L) > 0) {
    if (lua_isfunction(L, 1)) {
      lua_settop(L, 1);

      main_ref = luaL_ref(L, LUA_REGISTRYINDEX);
      mainState = utlua_mainthread(L);

      struct timeval t = {0, 1};
      mainevent = malloc(sizeof(struct event));
      evtimer_set(mainevent, main_handler, NULL);
      event_mgr_init();
      event_base_set(event_mgr_base(), mainevent);
      evtimer_add(mainevent, &t);
    }
  }
  event_mgr_loop();
  return 0;
}

LUA_API int luafan_stop(lua_State *L) {
  event_mgr_break();
  return 0;
}

// -- luafan_sleep start --
struct sleep_args {
  lua_State *L;
  int threadRef;
  struct event clockevent;
};

static void clock_handler(const int fd, const short which, void *arg) {
  struct sleep_args *args = (struct sleep_args *)arg;

  lua_State *L = args->L;
  int threadRef = args->threadRef;
  evtimer_del(&args->clockevent);

  free(args);

  lua_lock(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, threadRef);
  lua_State *co = lua_tothread(L, -1);
  lua_pop(L, 1);
  lua_unlock(L);

  utlua_resume(co, NULL, 0);

  luaL_unref(L, LUA_REGISTRYINDEX, threadRef);
}

LUA_API int luafan_sleep(lua_State *L) {
  lua_Number sec = luaL_checknumber(L, 1);
  struct event_base *base = event_mgr_base();

  struct sleep_args *args = malloc(sizeof(struct sleep_args));
  memset(args, 0, sizeof(struct sleep_args));

  args->L = utlua_mainthread(L);
  lua_pushthread(L);
  args->threadRef = luaL_ref(L, LUA_REGISTRYINDEX);

  struct timeval t = {0};
  d2tv(sec, &t);
  evtimer_set(&args->clockevent, clock_handler, args);
  event_base_set(base, &args->clockevent);
  evtimer_add(&args->clockevent, &t);

  return lua_yield(L, 0);
}
// -- luafan_sleep end --

// -- start hex2data data2hex --
static unsigned char strToChar(char a, char b) {
  char encoder[3] = {'\0', '\0', '\0'};
  encoder[0] = a;
  encoder[1] = b;
  return (char)strtol(encoder, NULL, 16);
}

LUA_API int hex2data(lua_State *L) {
  if (!lua_isstring(L, 1)) {
    return 0;
  }
  size_t length = 0;
  const char *bytes = lua_tolstring(L, 1, &length);

  char *r = (char *)malloc(length / 2 + 1);
  char *index = r;

  while ((*bytes) && (*(bytes + 1))) {
    *index = strToChar(*bytes, *(bytes + 1));
    index++;
    bytes += 2;
  }
  *index = '\0';

  lua_pushlstring(L, r, length / 2);

  free(r);

  return 1;
}

LUA_API int data2hex(lua_State *L) {
  if (!lua_isstring(L, 1)) {
    return 0;
  }
  static const char hexdigits[] = "0123456789ABCDEF";
  size_t len = 0;
  const char *data = lua_tolstring(L, 1, &len);
  const size_t numBytes = len;
  const char *bytes = data;
  char *strbuf = (char *)malloc(numBytes * 2 + 1);
  char *hex = strbuf;
  int i = 0;
  for (i = 0; i < numBytes; ++i) {
    const unsigned char c = *(bytes++);
    *hex++ = hexdigits[(c >> 4) & 0xF];
    *hex++ = hexdigits[(c)&0xF];
  }
  *hex = 0;

  lua_pushlstring(L, strbuf, numBytes * 2);

  free(strbuf);

  return 1;
}
// -- end hex2data data2hex --

LUA_API int luafan_gettime(lua_State *L) {
  struct timeval v;
  gettimeofday(&v, NULL);

  lua_pushinteger(L, v.tv_sec);
  lua_pushinteger(L, v.tv_usec);

  return 2;
}

static int luafan_push_result(lua_State *L, int result) {
  if (result == -1) {
    lua_pushboolean(L, false);
    lua_pushstring(L, strerror(errno));
    return 2;
  } else {
    lua_pushinteger(L, result);
    return 1;
  }
}

LUA_API int luafan_fork(lua_State *L) {
  int result = fork();
  return luafan_push_result(L, result);
}

LUA_API int luafan_getpid(lua_State *L) {
  lua_pushinteger(L, getpid());
  return 1;
}

LUA_API int luafan_getdtablesize(lua_State *L) {
  lua_pushinteger(L, getdtablesize());
  return 1;
}

LUA_API int luafan_setpgid(lua_State *L) {
  int result = setpgid(luaL_optinteger(L, 1, 0), luaL_optinteger(L, 2, 0));
  return luafan_push_result(L, result);
}

LUA_API int luafan_open(lua_State *L) {
  int result = open(luaL_checkstring(L, 1), luaL_optinteger(L, 2, O_RDWR),
                    luaL_optinteger(L, 3, 0));
  return luafan_push_result(L, result);
}

LUA_API int luafan_close(lua_State *L) {
  int result = close(luaL_checkinteger(L, 1));
  return luafan_push_result(L, result);
}

LUA_API int luafan_setsid(lua_State *L) {
  int result = setsid();
  return luafan_push_result(L, result);
}

extern char *__progname;

LUA_API int luafan_setprogname(lua_State *L) {
  size_t size = 0;
  const char *name = luaL_checklstring(L, 1, &size);
  memset(__progname, 0, 128);
  strncpy(__progname, name, 127 > size ? size : 127);

  return 0;
}

LUA_API int luafan_getpgid(lua_State *L) {
  int result = getpgid(luaL_optinteger(L, 1, 0));
  return luafan_push_result(L, result);
}

#ifdef __linux__
#define __USE_GNU
#include <sched.h>
#endif

#ifdef __APPLE__
#include <sys/sysctl.h>

#define SYSCTL_CORE_COUNT "machdep.cpu.core_count"

typedef struct cpu_set { uint32_t count; } cpu_set_t;

static inline void CPU_ZERO(cpu_set_t *cs) { cs->count = 0; }

static inline void CPU_SET(int num, cpu_set_t *cs) { cs->count |= (1 << num); }

static inline int CPU_ISSET(int num, cpu_set_t *cs) {
  return (cs->count & (1 << num));
}

int sched_getaffinity(pid_t pid, size_t cpu_size, cpu_set_t *cpu_set) {
  int32_t core_count = 0;
  size_t len = sizeof(core_count);
  int ret = sysctlbyname(SYSCTL_CORE_COUNT, &core_count, &len, 0, 0);
  if (ret) {
    printf("error while get core count %d\n", ret);
    return -1;
  }
  cpu_set->count = 0;
  for (int i = 0; i < core_count; i++) {
    cpu_set->count |= (1 << i);
  }

  return 0;
}

#endif

static int get_cpu_count() { return sysconf(_SC_NPROCESSORS_CONF); }

LUA_API int luafan_getaffinity(lua_State *L) {
  unsigned long bitmask = 0;
  cpu_set_t mask;
  CPU_ZERO(&mask);

  if (sched_getaffinity(0, sizeof(cpu_set_t), &mask) == -1) {
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
  } else {
    int cpu_count = get_cpu_count();
    int i = 0;
    for (; i < cpu_count; i++) {
      if (CPU_ISSET(i, &mask)) {
        bitmask |= (unsigned long)0x01 << i;
      }
    }

    lua_pushinteger(L, bitmask);
    return 1;
  }
}

LUA_API int luafan_setaffinity(lua_State *L) {
#ifdef __linux__
  unsigned long mask_value = luaL_checkinteger(L, 1);
  cpu_set_t mask;
  CPU_ZERO(&mask);

  int cpu_count = get_cpu_count();
  int i = 0;
  for (; i < cpu_count; i++) {
    if (mask_value & ((unsigned long)0x01 << i)) {
      CPU_SET(i, &mask);
    }
  }

  if (sched_setaffinity(0, sizeof(cpu_set_t), &mask) == -1) {
    lua_pushnil(L);
    lua_pushfstring(L, "sched_setaffinity: %s", strerror(errno));
    return 2;
  } else {
    lua_pushboolean(L, 1);
    return 1;
  }
#else
  lua_pushboolean(L, 1);
  return 1;
#endif
}

LUA_API int luafan_getcpucount(lua_State *L) {
  lua_pushinteger(L, get_cpu_count());
  return 1;
}

LUA_API int luafan_kill(lua_State *L) {
  if (kill(luaL_optinteger(L, 1, -1), luaL_optinteger(L, 2, SIGTERM))) {
    lua_pushboolean(L, false);
    lua_pushstring(L, strerror(errno));
    return 2;
  } else {
    lua_pushboolean(L, true);
    return 1;
  }
}

LUA_API int luafan_waitpid(lua_State *L) {
  int stat = 0;
  int result =
      waitpid(luaL_optinteger(L, 1, -1), &stat, luaL_optinteger(L, 2, -1));
  if (result == -1) {
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
  } else {
    lua_pushinteger(L, result);
    lua_pushinteger(L, stat);
    return 2;
  }
}

LUA_API int luafan_gettop(lua_State *L) {
  lua_pushinteger(L, lua_gettop(utlua_mainthread(L)));
  return 1;
}

static const struct luaL_Reg fanlib[] = {
    {"loop", luafan_start},
    {"loopbreak", luafan_stop},

    {"sleep", luafan_sleep},
    {"gettime", luafan_gettime},
    {"gettop", luafan_gettop},

    {"data2hex", data2hex},
    {"hex2data", hex2data},

    {"fork", luafan_fork},
    {"getpid", luafan_getpid},
    {"waitpid", luafan_waitpid},
    {"kill", luafan_kill},
    {"setpgid", luafan_setpgid},
    {"getpgid", luafan_getpgid},
    {"setsid", luafan_setsid},
    {"getdtablesize", luafan_getdtablesize},
    {"open", luafan_open},
    {"close", luafan_close},
    {"setprogname", luafan_setprogname},
    {"setaffinity", luafan_setaffinity},
    {"getaffinity", luafan_getaffinity},
    {"getcpucount", luafan_getcpucount},

    {NULL, NULL},
};

LUA_API int luaopen_fan(lua_State *L) {
#if (LUA_VERSION_NUM < 502)
  utlua_set_mainthread(L);
#endif

  lua_newtable(L);
  luaL_register(L, "fan", fanlib);
  return 1;
}
