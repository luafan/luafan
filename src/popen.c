#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "utlua.h"

#include <signal.h>
#include <sys/wait.h>
#include <spawn.h>

extern char **environ;

#define LUA_POPEN_TYPE "POPEN_CONNECTION_TYPE"

typedef struct {
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    pid_t child_pid;

    int onReadRef;
    int onStderrRef;
    int onDisconnectedRef;

    lua_State *mainthread;

    struct event *stdout_ev;
    struct event *stderr_ev;

    int closed;
} POPEN;

static void popen_try_disconnected(POPEN *p, const char *reason) {
    // Only fire when both stdout and stderr events are gone
    if (p->stdout_ev || p->stderr_ev) return;
    if (p->onDisconnectedRef == LUA_NOREF) return;

    // Try to reap child — both pipes have EOF'd so the child is exiting.
    // Retry with short delays to give the child time to exit.
    int status = 0;
    int exit_code = -1;
    if (p->child_pid > 0) {
        for (int i = 0; i < 10; i++) {
            pid_t ret = waitpid(p->child_pid, &status, WNOHANG);
            if (ret > 0) {
                if (WIFEXITED(status)) {
                    exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    exit_code = 128 + WTERMSIG(status);
                }
                p->child_pid = -1;
                break;
            }
            if (i < 9) usleep(1000);  // 1ms between retries
        }
    }

    lua_State *mainthread = p->mainthread;
    lua_lock(mainthread);
    fan_cb_setup_t cbs = fan_cb_setup(mainthread, p->onDisconnectedRef);
    if (!cbs.co) {
        lua_unlock(mainthread);
        return;
    }

    if (!lua_isfunction(cbs.co, -1)) {
        lua_pop(cbs.co, 1);
        lua_unlock(mainthread);
        FAN_CB_CLEANUP(mainthread, cbs);
        return;
    }

    lua_pushstring(cbs.co, reason);
    lua_pushinteger(cbs.co, exit_code);

    lua_unlock(mainthread);
    FAN_RESUME(cbs.co, mainthread, 2);
    FAN_CB_CLEANUP(mainthread, cbs);
}

static void popen_stdout_cb(evutil_socket_t fd, short event, void *arg) {
    POPEN *p = (POPEN *)arg;

    char buf[READ_BUFF_LEN];
    ssize_t len = read(fd, buf, READ_BUFF_LEN);

    if (len <= 0) {
        if (len < 0 && (errno == EAGAIN || errno == EINTR)) {
            return;
        }
        // EOF — remove event and free it
        event_free(p->stdout_ev);
        p->stdout_ev = NULL;
        popen_try_disconnected(p, len < 0 ? strerror(errno) : "stdout closed");
        return;
    }

    if (p->onReadRef != LUA_NOREF) {
        lua_State *mainthread = p->mainthread;
        lua_lock(mainthread);
        fan_cb_setup_t cbs = fan_cb_setup(mainthread, p->onReadRef);
        if (!cbs.co) {
            lua_unlock(mainthread);
            return;
        }
        if (!lua_isfunction(cbs.co, -1)) {
            lua_pop(cbs.co, 1);
            lua_unlock(mainthread);
            FAN_CB_CLEANUP(mainthread, cbs);
            return;
        }
        lua_pushlstring(cbs.co, buf, len);
        lua_unlock(mainthread);
        FAN_RESUME(cbs.co, mainthread, 1);
        FAN_CB_CLEANUP(mainthread, cbs);
    }
}

static void popen_stderr_cb(evutil_socket_t fd, short event, void *arg) {
    POPEN *p = (POPEN *)arg;

    char buf[READ_BUFF_LEN];
    ssize_t len = read(fd, buf, READ_BUFF_LEN);

    if (len <= 0) {
        if (len < 0 && (errno == EAGAIN || errno == EINTR)) {
            return;
        }
        event_free(p->stderr_ev);
        p->stderr_ev = NULL;
        popen_try_disconnected(p, len < 0 ? strerror(errno) : "stderr closed");
        return;
    }

    if (p->onStderrRef != LUA_NOREF) {
        lua_State *mainthread = p->mainthread;
        lua_lock(mainthread);
        fan_cb_setup_t cbs = fan_cb_setup(mainthread, p->onStderrRef);
        if (!cbs.co) {
            lua_unlock(mainthread);
            return;
        }
        if (!lua_isfunction(cbs.co, -1)) {
            lua_pop(cbs.co, 1);
            lua_unlock(mainthread);
            FAN_CB_CLEANUP(mainthread, cbs);
            return;
        }
        lua_pushlstring(cbs.co, buf, len);
        lua_unlock(mainthread);
        FAN_RESUME(cbs.co, mainthread, 1);
        FAN_CB_CLEANUP(mainthread, cbs);
    }
}

// popen.spawn({
//   command = "cat",            -- string: simple command
//   -- OR --
//   command = {"cat", "-n"},    -- array: command with args
//   onread = function(data) end,       -- stdout callback
//   onstderr = function(data) end,     -- stderr callback (optional)
//   ondisconnected = function(msg, exit_code) end,  -- exit callback (optional)
//   capture_stderr = true,             -- default true
// })
LUA_API int luafan_popen_spawn(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);

    // Parse command
    char *cmd_argv[256];
    char *cmd_dup = NULL;  // mutable copy if command is a plain string
    int cmd_argc = 0;

    lua_getfield(L, 1, "command");
    if (lua_type(L, -1) == LUA_TTABLE) {
        int len = (int)lua_objlen(L, -1);
        if (len == 0) {
            return luaL_error(L, "command array is empty");
        }
        if (len > 255) {
            return luaL_error(L, "command array too large (max 255)");
        }
        for (int i = 1; i <= len; i++) {
            lua_rawgeti(L, -1, i);
            cmd_argv[cmd_argc++] = (char *)luaL_checkstring(L, -1);
            lua_pop(L, 1);
        }
        cmd_argv[cmd_argc] = NULL;
    } else if (lua_type(L, -1) == LUA_TSTRING) {
        const char *cmd_str = luaL_checkstring(L, -1);
        // Need a mutable copy for strtok_r
        cmd_dup = strdup(cmd_str);
        if (!cmd_dup) {
            return luaL_error(L, "out of memory");
        }
        char *saveptr = NULL;
        char *token = strtok_r(cmd_dup, " ", &saveptr);
        while (token && cmd_argc < 255) {
            cmd_argv[cmd_argc++] = token;
            token = strtok_r(NULL, " ", &saveptr);
        }
        cmd_argv[cmd_argc] = NULL;
    } else {
        return luaL_error(L, "command must be a string or array");
    }
    lua_pop(L, 1);

    if (cmd_argc == 0) {
        return luaL_error(L, "command is empty");
    }

    // Parse capture_stderr option (default true)
    int capture_stderr = 1;
    lua_getfield(L, 1, "capture_stderr");
    if (!lua_isnil(L, -1)) {
        capture_stderr = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    // Parse env table (optional) — merge with inherited environ
    // User-supplied keys override inherited values; missing keys are inherited.
    char **envp = environ;
    char **env_alloc = NULL;
    int env_alloc_count = 0;
    lua_getfield(L, 1, "env");
    if (lua_type(L, -1) == LUA_TTABLE) {
        // Count inherited environ entries
        int env_count = 0;
        while (environ[env_count]) env_count++;

        // Count user env keys (and stash them as Lua values on top of stack)
        // We'll iterate twice — once to size, once to fill.
        int user_count = 0;
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) user_count++;
            lua_pop(L, 1);
        }

        // Allocate envp: worst case env_count + user_count + 1 (NULL)
        env_alloc = (char **)calloc(env_count + user_count + 1, sizeof(char *));
        if (!env_alloc) {
            lua_pop(L, 1);
            return luaL_error(L, "out of memory (env)");
        }
        env_alloc_count = 0;

        // Build a flat list of "KEY=VALUE" strings from user table first;
        // each strdup'd so we can free them after spawn.
        // Track keys to skip from inherited environ.
        // Simple approach: insert user entries first; then inherited entries
        // whose KEY= prefix doesn't match any user key.
        const char **user_keys = (const char **)calloc(user_count + 1, sizeof(char *));
        size_t *user_key_lens = (size_t *)calloc(user_count + 1, sizeof(size_t));
        if (!user_keys || !user_key_lens) {
            free(user_keys); free(user_key_lens); free(env_alloc); env_alloc = NULL;
            lua_pop(L, 1);
            return luaL_error(L, "out of memory (env keys)");
        }

        int ui = 0;
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING) {
                const char *k = lua_tostring(L, -2);
                const char *v = lua_tostring(L, -1);
                size_t klen = strlen(k);
                size_t vlen = strlen(v);
                char *entry = (char *)malloc(klen + 1 + vlen + 1);
                if (entry) {
                    memcpy(entry, k, klen);
                    entry[klen] = '=';
                    memcpy(entry + klen + 1, v, vlen);
                    entry[klen + 1 + vlen] = '\0';
                    env_alloc[env_alloc_count++] = entry;
                    user_keys[ui] = k;
                    user_key_lens[ui] = klen;
                    ui++;
                }
            }
            lua_pop(L, 1);
        }

        // Append inherited environ entries whose key isn't shadowed
        for (int i = 0; i < env_count; i++) {
            const char *e = environ[i];
            const char *eq = strchr(e, '=');
            size_t klen = eq ? (size_t)(eq - e) : strlen(e);
            int shadowed = 0;
            for (int j = 0; j < ui; j++) {
                if (user_key_lens[j] == klen && memcmp(user_keys[j], e, klen) == 0) {
                    shadowed = 1; break;
                }
            }
            if (!shadowed) {
                // Inherited entries are stable strings in environ; safe to reuse pointer.
                env_alloc[env_alloc_count++] = (char *)e;
            }
        }
        env_alloc[env_alloc_count] = NULL;
        envp = env_alloc;

        free(user_keys);
        free(user_key_lens);
    }
    lua_pop(L, 1);

    // Create pipes
    int pipe_stdin[2], pipe_stdout[2], pipe_stderr[2];

    if (pipe(pipe_stdin) < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "pipe(stdin): %s", strerror(errno));
        return 2;
    }
    if (pipe(pipe_stdout) < 0) {
        close(pipe_stdin[0]); close(pipe_stdin[1]);
        lua_pushnil(L);
        lua_pushfstring(L, "pipe(stdout): %s", strerror(errno));
        return 2;
    }
    if (capture_stderr && pipe(pipe_stderr) < 0) {
        close(pipe_stdin[0]); close(pipe_stdin[1]);
        close(pipe_stdout[0]); close(pipe_stdout[1]);
        lua_pushnil(L);
        lua_pushfstring(L, "pipe(stderr): %s", strerror(errno));
        return 2;
    }

    // Set up posix_spawn file actions for child process stdin/stdout/stderr redirection
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);

    // Child: redirect stdin from pipe read end
    posix_spawn_file_actions_adddup2(&file_actions, pipe_stdin[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, pipe_stdin[0]);
    posix_spawn_file_actions_addclose(&file_actions, pipe_stdin[1]);

    // Child: redirect stdout to pipe write end
    posix_spawn_file_actions_adddup2(&file_actions, pipe_stdout[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, pipe_stdout[0]);
    posix_spawn_file_actions_addclose(&file_actions, pipe_stdout[1]);

    if (capture_stderr) {
        posix_spawn_file_actions_adddup2(&file_actions, pipe_stderr[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&file_actions, pipe_stderr[0]);
        posix_spawn_file_actions_addclose(&file_actions, pipe_stderr[1]);
    }

    // Set up spawn attributes
    posix_spawnattr_t spawn_attr;
    posix_spawnattr_init(&spawn_attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&spawn_attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif

    pid_t pid;
    int spawn_err = posix_spawnp(&pid, cmd_argv[0], &file_actions, &spawn_attr, cmd_argv, envp);

    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&spawn_attr);

    // Free the dup'd command string if we made one
    free(cmd_dup); cmd_dup = NULL;

    // Free user-allocated env entries (the first env_alloc_count entries that we malloc'd).
    // Note: inherited environ pointers we appended later must NOT be freed.
    // We freed user entries first in the array; count them by scanning until we hit one
    // of environ[]. Simpler: track count of user entries separately.
    if (env_alloc) {
        // We malloc'd entries while ui-counting; rebuild ui by walking env_alloc until
        // we reach the inherited section. We added user entries first, so positions 0..(ui-1)
        // were malloc'd. But we lost `ui` scope; instead, we know each malloc'd entry's pointer
        // is NOT inside environ[]. Check by pointer comparison against environ array.
        for (int i = 0; i < env_alloc_count; i++) {
            char *p = env_alloc[i];
            int from_environ = 0;
            for (int j = 0; environ[j]; j++) {
                if (environ[j] == p) { from_environ = 1; break; }
            }
            if (!from_environ) free(p);
        }
        free(env_alloc);
    }

    if (spawn_err != 0) {
        close(pipe_stdin[0]); close(pipe_stdin[1]);
        close(pipe_stdout[0]); close(pipe_stdout[1]);
        if (capture_stderr) { close(pipe_stderr[0]); close(pipe_stderr[1]); }
        lua_pushnil(L);
        lua_pushfstring(L, "posix_spawn: %s", strerror(spawn_err));
        return 2;
    }

    // === Parent process ===
    close(pipe_stdin[0]);
    close(pipe_stdout[1]);
    if (capture_stderr) close(pipe_stderr[1]);

    // Set non-blocking
    fcntl(pipe_stdout[0], F_SETFL, O_NONBLOCK);
    if (capture_stderr) fcntl(pipe_stderr[0], F_SETFL, O_NONBLOCK);

    // Create userdata
    POPEN *p = (POPEN *)lua_newuserdata(L, sizeof(POPEN));
    memset(p, 0, sizeof(POPEN));
    p->stdin_fd = pipe_stdin[1];
    p->stdout_fd = pipe_stdout[0];
    p->stderr_fd = capture_stderr ? pipe_stderr[0] : -1;
    p->child_pid = pid;
    p->closed = 0;
    p->mainthread = utlua_mainthread(L);

    luaL_getmetatable(L, LUA_POPEN_TYPE);
    lua_setmetatable(L, -2);

    SET_FUNC_REF_FROM_TABLE(L, p->onReadRef, 1, "onread")
    SET_FUNC_REF_FROM_TABLE(L, p->onStderrRef, 1, "onstderr")
    SET_FUNC_REF_FROM_TABLE(L, p->onDisconnectedRef, 1, "ondisconnected")

    // Register read events with separate callbacks
    if (p->onReadRef != LUA_NOREF) {
        p->stdout_ev = event_new(event_mgr_base(), p->stdout_fd, EV_PERSIST | EV_READ, popen_stdout_cb, p);
        event_add(p->stdout_ev, NULL);
    }

    if (capture_stderr && p->onStderrRef != LUA_NOREF) {
        p->stderr_ev = event_new(event_mgr_base(), p->stderr_fd, EV_PERSIST | EV_READ, popen_stderr_cb, p);
        event_add(p->stderr_ev, NULL);
    }

    return 1;
}

LUA_API int luafan_popen_send(lua_State *L) {
    POPEN *p = luaL_checkudata(L, 1, LUA_POPEN_TYPE);
    size_t data_len;
    const char *data = luaL_optlstring(L, 2, NULL, &data_len);

    if (p->closed || p->stdin_fd < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "stdin is closed");
        return 2;
    }

    if (data && data_len > 0) {
        ssize_t written = write(p->stdin_fd, data, data_len);
        if (written < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                lua_pushinteger(L, 0);
                return 1;
            }
            lua_pushnil(L);
            lua_pushstring(L, strerror(errno));
            return 2;
        }
        lua_pushinteger(L, written);
    } else {
        lua_pushinteger(L, 0);
    }

    return 1;
}

LUA_API int luafan_popen_close_stdin(lua_State *L) {
    POPEN *p = luaL_checkudata(L, 1, LUA_POPEN_TYPE);

    if (p->stdin_fd >= 0) {
        close(p->stdin_fd);
        p->stdin_fd = -1;
    }

    lua_pushboolean(L, 1);
    return 1;
}

LUA_API int luafan_popen_close(lua_State *L) {
    POPEN *p = luaL_checkudata(L, 1, LUA_POPEN_TYPE);

    if (p->closed) {
        lua_pushboolean(L, 1);
        return 1;
    }
    p->closed = 1;

    CLEAR_REF(L, p->onReadRef)
    CLEAR_REF(L, p->onStderrRef)
    CLEAR_REF(L, p->onDisconnectedRef)

    if (p->stdout_ev) { event_free(p->stdout_ev); p->stdout_ev = NULL; }
    if (p->stderr_ev) { event_free(p->stderr_ev); p->stderr_ev = NULL; }

    if (p->stdin_fd >= 0) { close(p->stdin_fd); p->stdin_fd = -1; }
    if (p->stdout_fd >= 0) { close(p->stdout_fd); p->stdout_fd = -1; }
    if (p->stderr_fd >= 0) { close(p->stderr_fd); p->stderr_fd = -1; }

    if (p->child_pid > 0) {
        kill(p->child_pid, SIGTERM);
        int status = 0;
        pid_t ret = waitpid(p->child_pid, &status, WNOHANG);
        if (ret == 0) {
            usleep(10000);
            ret = waitpid(p->child_pid, &status, WNOHANG);
            if (ret == 0) {
                kill(p->child_pid, SIGKILL);
                waitpid(p->child_pid, &status, 0);
            }
        }
        p->child_pid = -1;
    }

    lua_pushboolean(L, 1);
    return 1;
}

LUA_API int luafan_popen_gc(lua_State *L) {
    return luafan_popen_close(L);
}

LUA_API int luafan_popen_getpid(lua_State *L) {
    POPEN *p = luaL_checkudata(L, 1, LUA_POPEN_TYPE);
    if (p->child_pid > 0) {
        lua_pushinteger(L, p->child_pid);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

LUA_API int luafan_popen_is_alive(lua_State *L) {
    POPEN *p = luaL_checkudata(L, 1, LUA_POPEN_TYPE);

    if (p->child_pid <= 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int status = 0;
    pid_t ret = waitpid(p->child_pid, &status, WNOHANG);
    if (ret == 0) {
        lua_pushboolean(L, 1);
    } else {
        p->child_pid = -1;
        lua_pushboolean(L, 0);
    }

    return 1;
}

static const struct luaL_Reg popenlib[] = {
    {"spawn", luafan_popen_spawn},
    {NULL, NULL},
};

LUA_API int luaopen_fan_popen(lua_State *L) {
    luaL_newmetatable(L, LUA_POPEN_TYPE);

    lua_pushcfunction(L, &luafan_popen_send);
    lua_setfield(L, -2, "send");

    lua_pushcfunction(L, &luafan_popen_close_stdin);
    lua_setfield(L, -2, "close_stdin");

    lua_pushcfunction(L, &luafan_popen_close);
    lua_setfield(L, -2, "close");

    lua_pushcfunction(L, &luafan_popen_getpid);
    lua_setfield(L, -2, "getpid");

    lua_pushcfunction(L, &luafan_popen_is_alive);
    lua_setfield(L, -2, "is_alive");

    lua_pushstring(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);

    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, &luafan_popen_gc);
    lua_rawset(L, -3);

    lua_pop(L, 1);

    lua_newtable(L);
    luaL_register(L, "popen", popenlib);
    return 1;
}
