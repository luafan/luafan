#include "utlua.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>

static int luafan_push_result(lua_State *L, int result)
{
  if (result == -1)
  {
    lua_pushboolean(L, false);
    lua_pushstring(L, strerror(errno));
    return 2;
  }
  else
  {
    lua_pushinteger(L, result);
    return 1;
  }
}

LUA_API int luafan_fork(lua_State *L)
{
  int result = fork();
  return luafan_push_result(L, result);
}

LUA_API int luafan_getpid(lua_State *L)
{
  lua_pushinteger(L, getpid());
  return 1;
}

LUA_API int luafan_getdtablesize(lua_State *L)
{
#ifndef __ANDROID__
  lua_pushinteger(L, getdtablesize());
#else
  lua_pushinteger(L, sysconf(_SC_OPEN_MAX));
#endif
  return 1;
}


LUA_API int luafan_setpgid(lua_State *L)
{
  int result = setpgid((pid_t) luaL_optinteger(L, 1, 0), (pid_t) luaL_optinteger(L, 2, 0));
  return luafan_push_result(L, result);
}

LUA_API int luafan_open(lua_State *L)
{
  int result = open(luaL_checkstring(L, 1), (int) luaL_optinteger(L, 2, O_RDWR),
                    luaL_optinteger(L, 3, 0));
  return luafan_push_result(L, result);
}

LUA_API int luafan_close(lua_State *L)
{
  int result = close((int) luaL_checkinteger(L, 1));
  return luafan_push_result(L, result);
}

LUA_API int luafan_setsid(lua_State *L)
{
  int result = setsid();
  return luafan_push_result(L, result);
}

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if TARGET_OS_IOS == 0
extern char *__progname;
#endif


LUA_API int luafan_setprogname(lua_State *L)
{
#if TARGET_OS_IOS == 0
  size_t size = 0;
  const char *name = luaL_checklstring(L, 1, &size);
  memset(__progname, 0, 128);
  strncpy(__progname, name, 127 > size ? size : 127);
#endif

  return 0;
}

LUA_API int luafan_getpgid(lua_State *L)
{
  int result = getpgid((pid_t) luaL_optinteger(L, 1, 0));
  return luafan_push_result(L, result);
}

#ifndef DISABLE_AFFINIY

#ifdef __APPLE__
#include <sys/sysctl.h>

#define SYSCTL_CORE_COUNT "machdep.cpu.core_count"

typedef struct cpu_set
{
  uint32_t count;
} cpu_set_t;

static inline void CPU_ZERO(cpu_set_t *cs) { cs->count = 0; }

static inline void CPU_SET(int num, cpu_set_t *cs) { cs->count |= (1 << num); }

static inline int CPU_ISSET(int num, cpu_set_t *cs)
{
  return (cs->count & (1 << num));
}

int sched_getaffinity(pid_t pid, size_t cpu_size, cpu_set_t *cpu_set)
{
  int32_t core_count = 0;
  size_t len = sizeof(core_count);
  int ret = sysctlbyname(SYSCTL_CORE_COUNT, &core_count, &len, 0, 0);
  if (ret)
  {
    printf("error while get core count %d\n", ret);
    return -1;
  }
  cpu_set->count = 0;
  for (int i = 0; i < core_count; i++)
  {
    cpu_set->count |= (1 << i);
  }

  return 0;
}
#else
#include <sched.h>
#endif

static int get_cpu_count()
{
  return sysconf(_SC_NPROCESSORS_CONF);
}

LUA_API int luafan_getaffinity(lua_State *L)
{
  unsigned long bitmask = 0;
  cpu_set_t mask;
  CPU_ZERO(&mask);

  if (sched_getaffinity(0, sizeof(cpu_set_t), &mask) == -1)
  {
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
  }
  else
  {
    int cpu_count = get_cpu_count();
    int i = 0;
    for (; i < cpu_count; i++)
    {
      if (CPU_ISSET(i, &mask))
      {
        bitmask |= (unsigned long)0x01 << i;
      }
    }

    lua_pushinteger(L, bitmask);
    return 1;
  }
}

LUA_API int luafan_setaffinity(lua_State *L)
{
#ifdef __linux__
  unsigned long mask_value = luaL_checkinteger(L, 1);
  cpu_set_t mask;
  CPU_ZERO(&mask);

  int cpu_count = get_cpu_count();
  int i = 0;
  for (; i < cpu_count; i++)
  {
    if (mask_value & ((unsigned long)0x01 << i))
    {
      CPU_SET(i, &mask);
    }
  }

  if (sched_setaffinity(0, sizeof(cpu_set_t), &mask) == -1)
  {
    lua_pushnil(L);
    lua_pushfstring(L, "sched_setaffinity: %s", strerror(errno));
    return 2;
  }
  else
  {
    lua_pushboolean(L, 1);
    return 1;
  }
#else
  lua_pushboolean(L, 1);
  return 1;
#endif
}

LUA_API int luafan_getcpucount(lua_State *L)
{
  lua_pushinteger(L, get_cpu_count());
  return 1;
}
#endif

LUA_API int luafan_kill(lua_State *L)
{
  if (kill(luaL_optinteger(L, 1, -1), (pid_t) luaL_optinteger(L, 2, SIGTERM)))
  {
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
  }
  else
  {
    lua_pushboolean(L, true);
    return 1;
  }
}

LUA_API int luafan_waitpid(lua_State *L)
{
  int stat = 0;
  int result =
      waitpid((pid_t) luaL_optinteger(L, 1, -1), &stat, (int) luaL_optinteger(L, 2, -1));
  if (result == -1)
  {
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
  }
  else
  {
    lua_pushinteger(L, result);
    lua_pushinteger(L, stat);
    return 2;
  }
}

#ifdef __ANDROID__
#include "ifaddrs.h"
#else
#include <ifaddrs.h>
#endif

LUA_API int luafan_getinterfaces(lua_State *L)
{
  struct ifaddrs *ifaddr, *ifa;
  char host[NI_MAXHOST];

  if (getifaddrs(&ifaddr) == -1)
  {
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
  }

  lua_newtable(L);
  int count = 1;
  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
  {
    lua_newtable(L);
    if (ifa->ifa_name) {
        lua_pushstring(L, ifa->ifa_name);
        lua_setfield(L, -2, "name");
    }
    if (ifa->ifa_addr)
    {
      if (ifa->ifa_addr->sa_family == AF_INET ||
          ifa->ifa_addr->sa_family == AF_INET6)
      {
        if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host,
                        NI_MAXHOST, NULL, 0, NI_NUMERICHOST) == 0)
        {
          lua_pushstring(L, host);
          lua_setfield(L, -2, "host");
        }
        lua_pushstring(L,
                       ifa->ifa_addr->sa_family == AF_INET ? "inet" : "inet6");
        lua_setfield(L, -2, "type");
      }
    }
    if (ifa->ifa_netmask)
    {
      if (getnameinfo(ifa->ifa_netmask, sizeof(struct sockaddr_in), host,
                      NI_MAXHOST, NULL, 0, NI_NUMERICHOST) == 0)
      {
        lua_pushstring(L, host);
        lua_setfield(L, -2, "netmask");
      }
    }
    if (ifa->ifa_dstaddr)
    {
      if (getnameinfo(ifa->ifa_dstaddr, sizeof(struct sockaddr_in), host,
                      NI_MAXHOST, NULL, 0, NI_NUMERICHOST) == 0)
      {
        lua_pushstring(L, host);
        lua_setfield(L, -2, "dst");
      }
    }
    lua_rawseti(L, -2, count++);
  }

  freeifaddrs(ifaddr);

  return 1;
}
