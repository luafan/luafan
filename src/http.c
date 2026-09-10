
#include "utlua.h"

#define KEY_COOKIE_JAR "http.cookiejar"
#define KEY_CAINFO "http.cainfo"
#define KEY_CAPATH "http.capath"

#include <curl/curl.h>

#define MSG_OUT stdout /* Send info to stdout, change to stderr if you want */

typedef struct _ConnInfo ConnInfo;
typedef struct _ResumeInfo ResumeInfo;

#define HTTP_RUNTIME_MAIN (-1)

typedef struct _HttpRuntime HttpRuntime;

struct _HttpRuntime {
    int worker_id;
    struct event_base *base;
    struct event *timer_event;
    struct event *timer_check_multi_info;
    CURLM *multi;
    int still_running;
    ConnInfo *inflight_head;
    ResumeInfo *resume_head;
};

static HttpRuntime main_runtime = { .worker_id = HTTP_RUNTIME_MAIN };
static HttpRuntime worker_runtimes[EVENT_MGR_MAX_WORKERS];

/* Proxy + DNS globals defined here for all platforms. External code (iOS app
   TunnelService.m, Android JNI bridge, future macOS settings UI) may override
   these via extern + strong definition; our definitions are weak so the
   stronger app-supplied one wins at link time. */
__attribute__((weak)) char *proxyHost = NULL;
__attribute__((weak)) long proxyPort = 0;
__attribute__((weak)) char *proxyUsername = NULL;
__attribute__((weak)) char *proxyPassword = NULL;
__attribute__((weak)) int proxyType = INT16_MAX;  /* sentinel: "no proxy configured" */
__attribute__((weak)) char *dns_servers = NULL;

extern int GLOBAL_VERBOSE;

#if defined(ANDROID) || defined(__ANDROID__)
#include "jni.h"
#include <curl/curl.h>
extern JavaVM *cachedJVM;
#define IncrNetworkActivity() (void)0;
#define DecrNetworkActivity() (void)0;
#elif TARGET_OS_IPHONE
extern void IncrNetworkActivity();
extern void DecrNetworkActivity();
#else
/* No iOS-style network-activity indicator on macOS. */
#define IncrNetworkActivity() (void)0;
#define DecrNetworkActivity() (void)0;
#endif

/* incrRef/decrRef are defined in lua-apple/lua53/luauser.c on Apple platforms
   and in the Android JNI bridge. On Linux they are no-ops. */
#if TARGET_OS_MAC
extern void incrRef(lua_State *L);
extern void decrRef(lua_State *L);
#elif TARGET_OS_IPHONE
extern void incrRef(lua_State *L);
extern void decrRef(lua_State *L);
#else
#define incrRef(L) (void)0;
#define decrRef(L) (void)0;
#endif

/* Information associated with a specific easy handle */
struct _ConnInfo {
    HttpRuntime *runtime;
    CURL *easy;
    //    char *url;
    char error[CURL_ERROR_SIZE];

    struct curl_slist *outputHeaders;

    BYTEARRAY input;

    lua_State *mainthread;
    lua_State *L;

    int completed; // set to 1 after http_getpost_complete, prevents double-process

    int verbose;

    int onprogressref;
    int onheaderref;
    int onwriteref;
    int onreadref;

    int coref; // unref on ResumeInfo

    int headerref;
    int retref;
    int bodyref;

    //    struct curl_slist *headers;

    struct curl_slist *resolve;

    struct _ConnInfo *next; // linked list for in-flight tracking
    struct _ConnInfo *prev;
};

/* In-flight connections are owned by their runtime. */
static void inflight_add(ConnInfo *conn) {
    HttpRuntime *runtime = conn->runtime;
    conn->prev = NULL;
    conn->next = runtime->inflight_head;
    if (runtime->inflight_head) {
        runtime->inflight_head->prev = conn;
    }
    runtime->inflight_head = conn;
}

static void inflight_remove(ConnInfo *conn) {
    HttpRuntime *runtime = conn->runtime;
    if (conn->prev) {
        conn->prev->next = conn->next;
    } else if (runtime->inflight_head == conn) {
        runtime->inflight_head = conn->next;
    }
    if (conn->next) {
        conn->next->prev = conn->prev;
    }
    conn->prev = NULL;
    conn->next = NULL;
}

struct _ResumeInfo {
    struct event *resume_timer;
    HttpRuntime *runtime;
    lua_State *L;
    int coref;
    ResumeInfo *next;
};

/* Information associated with a specific socket */
typedef struct _SockInfo {
    HttpRuntime *runtime;
    curl_socket_t sockfd;
    CURL *easy;
    int action;
    long timeout;
    struct event *ev;
    int evset;
} SockInfo;

static CURLSH *share_handle = NULL;

#define CURL_TIMEOUT_DEFAULT 60

enum { HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_HEAD, HTTP_DELETE, HTTP_UPDATE, HTTP_PATCH };

/* Update the event timer after curl_multi library calls.
 * timeout_ms < 0 means "delete the timer" (libcurl contract). */
static int multi_timer_cb(CURLM *multi_handle, long timeout_ms, void *data) {
    struct timeval timeout;
    HttpRuntime *runtime = (HttpRuntime *)data;
    (void)multi_handle; /* unused */

    if (!runtime || !runtime->timer_event)
        return 0;
    if (timeout_ms < 0) {
        evtimer_del(runtime->timer_event);
        return 0;
    }

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    evtimer_add(runtime->timer_event, &timeout);
    return 0;
}

/* Die if we get a bad CURLMcode somewhere */
static const char *mcode_or_die(const char *where, CURLMcode code) {
    if (CURLM_OK != code) {
        const char *s = curl_multi_strerror(code);
        LOGD("ERROR: %s returns %s\n", where, s);
        return s;
    }

    return NULL;
}

static void resume_cb(int fd, short kind, void *userp);
static void timer_cb(int fd, short kind, void *userp);
static void timer_check_multi_info_cb(int fd, short kind, void *userp);
static int sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp);

static void http_resume_failure(ConnInfo *conn) {
    lua_State *mainthread = conn->mainthread ? conn->mainthread :
                            (conn->L ? utlua_mainthread(conn->L) : NULL);
    if (mainthread && conn->coref != LUA_NOREF) {
        lua_lock(mainthread);
        luaL_unref(mainthread, LUA_REGISTRYINDEX, conn->coref);
        lua_unlock(mainthread);
        conn->coref = LUA_NOREF;
    }
    if (mainthread) {
        decrRef(mainthread);
    }
}

static void http_getpost_complete(ConnInfo *conn) {
    if (conn->completed) {
        fprintf(stderr, "[http] WARNING: http_getpost_complete called twice on same conn\n");
        return;
    }
    conn->completed = 1;
    inflight_remove(conn);

    lua_State *L = conn->L;
    if (!L) {
        fprintf(stderr, "[http] WARNING: http_getpost_complete called with NULL L\n");
        // Still need to decrRef — use mainthread stored at request time
        if (conn->mainthread) {
            DecrNetworkActivity();
            decrRef(conn->mainthread);
        }
        return;
    }
    lua_lock(L);

    if (conn->bodyref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->bodyref);
        conn->bodyref = LUA_NOREF;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, conn->retref);
    luaL_unref(L, LUA_REGISTRYINDEX, conn->retref);

    if (*conn->error != 0) {
        lua_pushstring(L, conn->error);
        lua_setfield(L, -2, "error");
        // LOGE("%s", conn->error);
    }

    struct curl_slist *cookies = NULL;
    if (curl_easy_getinfo(conn->easy, CURLINFO_COOKIELIST, &cookies) == CURLE_OK) {
        struct curl_slist *nc = cookies;
        lua_newtable(L);

        int i = 1;
        while (nc) {
            lua_pushstring(L, nc->data);
            lua_rawseti(L, -2, i);

            nc = nc->next;
            i++;
        }
        curl_slist_free_all(cookies);

        lua_setfield(L, -2, "cookies");
    }

    long responseCode = -1;
    curl_easy_getinfo(conn->easy, CURLINFO_RESPONSE_CODE, &responseCode);

    double dns_time = 0;
    if (curl_easy_getinfo(conn->easy, CURLINFO_NAMELOOKUP_TIME, &dns_time) == CURLE_OK) {
        lua_pushnumber(L, dns_time);
        lua_setfield(L, -2, "dns_time");
    }

    double connect_time = 0;
    if (curl_easy_getinfo(conn->easy, CURLINFO_CONNECT_TIME, &connect_time) == CURLE_OK) {
        lua_pushnumber(L, connect_time);
        lua_setfield(L, -2, "connect_time");
    }

    double appconnect_time = 0;
    if (curl_easy_getinfo(conn->easy, CURLINFO_APPCONNECT_TIME, &appconnect_time) == CURLE_OK) {
        lua_pushnumber(L, appconnect_time);
        lua_setfield(L, -2, "appconnect_time");
    }

    double pretransfer_time = 0;
    if (curl_easy_getinfo(conn->easy, CURLINFO_PRETRANSFER_TIME, &pretransfer_time) == CURLE_OK) {
        lua_pushnumber(L, pretransfer_time);
        lua_setfield(L, -2, "pretransfer_time");
    }

    double starttransfer_time = 0;
    if (curl_easy_getinfo(conn->easy, CURLINFO_STARTTRANSFER_TIME, &starttransfer_time) == CURLE_OK) {
        lua_pushnumber(L, starttransfer_time);
        lua_setfield(L, -2, "starttransfer_time");
    }

    double total_time = 0;
    if (curl_easy_getinfo(conn->easy, CURLINFO_TOTAL_TIME, &total_time) == CURLE_OK) {
        lua_pushnumber(L, total_time);
        lua_setfield(L, -2, "total_time");
    }

    bytearray_read_ready(&conn->input);

    if (conn->input.total > 0) {
        lua_pushlstring(L, (const char *)conn->input.buffer, conn->input.total);
    } else {
        //        lua_pushliteral(L, "");
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "body");

    lua_pushinteger(L, responseCode);
    lua_setfield(L, -2, "responseCode");

    bytearray_dealloc(&conn->input);

    if (conn->onprogressref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onprogressref);
        conn->onprogressref = LUA_NOREF;
    }

    if (conn->onheaderref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onheaderref);
        conn->onheaderref = LUA_NOREF;
    }

    if (conn->onreadref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onreadref);
        conn->onreadref = LUA_NOREF;
    }

    if (conn->onwriteref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onwriteref);
        conn->onwriteref = LUA_NOREF;
    }

    luaL_unref(L, LUA_REGISTRYINDEX, conn->headerref);

    DecrNetworkActivity();

    lua_unlock(L);

    ResumeInfo *info = malloc(sizeof(ResumeInfo));
    if (!info) {
        fprintf(stderr, "Memory allocation failed for ResumeInfo: %zu bytes\n", sizeof(ResumeInfo));
        http_resume_failure(conn);
        return;
    }
    info->runtime = conn->runtime;
    info->L = L;
    info->coref = conn->coref;
    info->resume_timer = evtimer_new(conn->runtime->base, resume_cb, info);
    if (!info->resume_timer) {
        free(info);
        http_resume_failure(conn);
        return;
    }
    struct timeval tv = {0, 1};
    if (event_add(info->resume_timer, &tv) != 0) {
        event_free(info->resume_timer);
        free(info);
        http_resume_failure(conn);
        return;
    }
    info->next = conn->runtime->resume_head;
    conn->runtime->resume_head = info;

    conn->coref = LUA_NOREF;
}

/* Check for completed transfers, and remove their easy handles */
static void timer_check_multi_info_cb(int fd, short kind, void *userp) {
    HttpRuntime *runtime = (HttpRuntime *)userp;
    char *eff_url;
    CURLMsg *msg;
    int msgs_left;
    (void)fd;
    (void)kind;

    if (!runtime || !runtime->multi) return;
    while ((msg = curl_multi_info_read(runtime->multi, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            CURL *easy = msg->easy_handle;
            ConnInfo *conn = NULL;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &conn);
            curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff_url);
            if (!conn) continue;

            http_getpost_complete(conn);
            curl_multi_remove_handle(runtime->multi, easy);

            if (conn->outputHeaders) curl_slist_free_all(conn->outputHeaders);
            if (conn->resolve) curl_slist_free_all(conn->resolve);
            curl_easy_cleanup(easy);
            free(conn);
        }
    }
}

/* Called by libevent when we get action on a multi socket */
static void event_cb(int fd, short kind, void *userp) {
    HttpRuntime *runtime = (HttpRuntime *)userp;
    int action = (kind & EV_READ ? CURL_CSELECT_IN : 0) |
                 (kind & EV_WRITE ? CURL_CSELECT_OUT : 0);
    if (!runtime || !runtime->multi) return;

    CURLMcode rc = curl_multi_socket_action(runtime->multi, fd, action,
                                            &runtime->still_running);
    mcode_or_die("event_cb: curl_multi_socket_action", rc);

    struct timeval tv = {0, 100};
    if (runtime->timer_check_multi_info) {
        event_add(runtime->timer_check_multi_info, &tv);
    }

    if (runtime->still_running <= 0 && runtime->timer_event &&
        evtimer_pending(runtime->timer_event, NULL)) {
        evtimer_del(runtime->timer_event);
    }
}

/* Called by libevent when our timeout expires */
static void timer_cb(int fd, short kind, void *userp) {
    HttpRuntime *runtime = (HttpRuntime *)userp;
    (void)fd;
    (void)kind;
    if (!runtime || !runtime->multi) return;

    CURLMcode rc = curl_multi_socket_action(runtime->multi, CURL_SOCKET_TIMEOUT,
                                            0, &runtime->still_running);
    mcode_or_die("timer_cb: curl_multi_socket_action", rc);

    struct timeval tv = {0, 1000};
    if (runtime->timer_check_multi_info) {
        event_add(runtime->timer_check_multi_info, &tv);
    }
}

static void resume_cb(int fd, short kind, void *userp) {
    ResumeInfo *info = (ResumeInfo *)userp;
    HttpRuntime *runtime = info->runtime;
    if (runtime) {
        ResumeInfo **pp = &runtime->resume_head;
        while (*pp && *pp != info) {
            pp = &(*pp)->next;
        }
        if (*pp == info) {
            *pp = info->next;
        }
    }
    //    fprintf(MSG_OUT, "resume\n");
    lua_State *L = info->L;
    lua_State *mainthread = utlua_mainthread(L);
    int coref = info->coref;

    event_free(info->resume_timer);
    free(info);

    FAN_RESUME(L, NULL, 1);

    lua_lock(mainthread);
    luaL_unref(mainthread, LUA_REGISTRYINDEX, coref);
    lua_unlock(mainthread);
    decrRef(mainthread);
}

/* Clean up the SockInfo structure */
static void remsock(SockInfo *f) {
    if (f) {
        if (f->evset)
            event_free(f->ev);
        free(f);
    }
}

/* Assign information to a SockInfo structure */
static void setsock(SockInfo *f, curl_socket_t s, CURL *e, int act, void *data) {
    HttpRuntime *runtime = f ? f->runtime : (HttpRuntime *)data;
    int kind = (act & CURL_POLL_IN ? EV_READ : 0) |
               (act & CURL_POLL_OUT ? EV_WRITE : 0) | EV_PERSIST;

    if (!f || !runtime || !runtime->base) return;
    f->sockfd = s;
    f->action = act;
    f->easy = e;
    if (f->evset) {
        event_free(f->ev);
        f->ev = NULL;
        f->evset = 0;
    }
    f->ev = event_new(runtime->base, f->sockfd, kind, event_cb, runtime);
    if (!f->ev) return;
    if (event_add(f->ev, NULL) != 0) {
        event_free(f->ev);
        f->ev = NULL;
        return;
    }
    f->evset = 1;
}

/* Initialize a new SockInfo structure */
static void addsock(curl_socket_t s, CURL *easy, int action, void *data) {
    HttpRuntime *runtime = (HttpRuntime *)data;
    SockInfo *fdp = calloc(1, sizeof(SockInfo));
    if (!fdp || !runtime) {
        free(fdp);
        return;
    }
    fdp->runtime = runtime;
    setsock(fdp, s, easy, action, runtime);
    curl_multi_assign(runtime->multi, s, fdp);
}

/* CURLMOPT_SOCKETFUNCTION */
static int sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp) {
    SockInfo *fdp = (SockInfo *)sockp;
    //    const char *whatstr[]={ "none", "IN", "OUT", "INOUT", "REMOVE" };

    //  fprintf(MSG_OUT,
    //          "socket callback: s=%d e=%p what=%s ", s, e, whatstr[what]);
    if (what == CURL_POLL_REMOVE) {
        //    fprintf(MSG_OUT, "\n");
        remsock(fdp);
    } else {
        if (!fdp) {
            //      fprintf(MSG_OUT, "Adding data: %s\n", whatstr[what]);
            addsock(s, e, what, cbp);
        } else {
            //      fprintf(MSG_OUT,
            //              "Changing action from %s to %s\n",
            //              whatstr[fdp->action], whatstr[what]);
            setsock(fdp, s, e, what, cbp);
        }
    }
    return 0;
}

static size_t filldata(char *ptr, size_t size, size_t nmemb, ConnInfo *conn) {
    if (conn->completed) return 0;
    int i = 0;
    //    fprintf(MSG_OUT, "filldata %zu %zu\n", size, nmemb);
    for (; i < size; i++) {
        bytearray_writebuffer(&conn->input, ptr + i * nmemb, nmemb);
    }
    return size * nmemb;
}

static size_t fillheader(void *ptr, size_t size, size_t nmemb, void *userdata) {
    ConnInfo *conn = (ConnInfo *)userdata;
    if (conn->completed) return size * nmemb;
    lua_State *L = conn->L;

    lua_lock(L);
    size_t total = size * nmemb;
    char *offset = ptr;

    lua_rawgeti(L, LUA_REGISTRYINDEX, conn->headerref);
    //    printf("lua_gettop(L)=%d\n", lua_gettop(L));

    int meetspace = 0;
    int i = 0;
    for (; i < total; i++) {
        if (!meetspace && *(offset + i) == ' ') {
            meetspace = 1;
            continue;
        }

        if (!meetspace && *(offset + i) == ':' && i + 3 < total) {
            // check exist or not, if exist, change value field to array.
            lua_pushlstring(L, offset, i); // key
            lua_rawget(L, -2);             // header table
            int mix_values = 0;
            if (!lua_isnil(L, -1)) {
                if (!lua_istable(L, -1)) {
                    lua_newtable(L);
                    lua_pushlstring(L, offset, i); // key
                    lua_pushvalue(L, -2);
                    lua_rawset(L, -5); // <header table -5><exist value -4><newtable
                    // -3><key -2><newtablecopy -1>
                    lua_pushvalue(L, -2);
                    lua_rawseti(L, -2, 1);
                    lua_remove(L, -2);
                }
                mix_values = 1;
            } else {
                lua_pop(L, 1);
            }

            const char *front = offset + i + 1;
            const char *end = offset + total - 1;
            size_t size = total - i - 2;

            for (; size && isspace(*front); size--, front++)
                ;
            for (; size && isspace(*end); size--, end--)
                ;

            if (mix_values) {
                lua_pushlstring(L, front, (size_t)(end - front) + 1);
                lua_rawseti(L, -2, (int)lua_objlen(L, -2) + 1);
                lua_pop(L, 1);
            } else {
                lua_pushlstring(L, offset, i); // key
                lua_pushlstring(L, front, (size_t)(end - front) + 1);
                lua_rawset(L, -3);
            }

            if (strstr(offset, "Content-Type") == offset) {
                const char *charset_offset = strstr(front, "charset=");
                if (charset_offset) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, conn->retref);
                    lua_pushlstring(L, charset_offset + strlen("charset="),
                                    (size_t)(end - charset_offset - strlen("charset=")) + 1);
                    lua_setfield(L, -2, "charset"); // response
                    lua_pop(L, 1);
                }
            }
        }
    }

    //    printf("lua_gettop(L)=%d\n", lua_gettop(L));

    if (conn->onheaderref != LUA_NOREF && total <= 2) {
        long responseCode = -1;
        curl_easy_getinfo(conn->easy, CURLINFO_RESPONSE_CODE, &responseCode);

        lua_pushliteral(L, "responseCode");
        lua_pushinteger(L, responseCode);
        lua_rawset(L, -3); // header table

        lua_rawgeti(L, LUA_REGISTRYINDEX, conn->onheaderref);
        if (!lua_isfunction(L, -1)) {
            LOGE("http onheader: onheaderref=%d resolved to %s, expected function\n",
                 conn->onheaderref, luaL_typename(L, -1));
            lua_pop(L, 1); // pop non-function
            lua_pop(L, 1); // pop header table
            lua_unlock(L);
            return size * nmemb;
        }

        // push header table as argument
        lua_pushvalue(L, -2); // copy header table above function
        lua_remove(L, -3);    // remove original header table from below function

        int pcall_status = lua_pcall(L, 1, 0, 0);
        if (pcall_status != 0) {
            LOGE("http onheader pcall error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1); // pop header table
    }

    lua_unlock(L);

    return size * nmemb;
}

static int onprogress(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    ConnInfo *conn = (ConnInfo *)clientp;
    if (conn->completed) return 1; // abort
    lua_State *L = conn->mainthread;

    lua_lock(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, conn->onprogressref);
    if (!lua_isfunction(L, -1)) {
        LOGE("http onprogress: onprogressref=%d resolved to %s, expected function\n",
             conn->onprogressref, luaL_typename(L, -1));
        lua_pop(L, 1);
        lua_unlock(L);
        return 0;
    }
    lua_pushinteger(L, dltotal);
    lua_pushinteger(L, dlnow);
    lua_pushinteger(L, ultotal);
    lua_pushinteger(L, ulnow);

    int status = lua_pcall(L, 4, 1, 0);
    long ret = 0;
    if (status == 0 && lua_gettop(L) > 0) {
        if (lua_type(L, -1) == LUA_TNUMBER) {
            ret = lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    } else if (status != 0) {
        LOGE("http onprogress pcall error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_unlock(L);
    return (int)ret;
}

static size_t onwrite(char *ptr, size_t size, size_t nmemb, void *userdata) {
    ConnInfo *conn = (ConnInfo *)userdata;
    if (conn->completed) return 0;
    lua_State *L = conn->mainthread;

    lua_lock(L);
    // Use pcall instead of FAN_RESUME to prevent yielding inside curl callback.
    // Yielding would let the event loop process other events (timers), which can
    // trigger http_getpost_complete and unref our callback slots while we're still
    // inside this curl callback — causing use-after-free.
    lua_rawgeti(L, LUA_REGISTRYINDEX, conn->onwriteref);
    if (!lua_isfunction(L, -1)) {
        LOGE("http onwrite: onwriteref=%d resolved to %s, expected function\n",
             conn->onwriteref, luaL_typename(L, -1));
        lua_pop(L, 1);
        lua_unlock(L);
        return 0;
    }
    lua_pushlstring(L, ptr, size * nmemb);

    int status = lua_pcall(L, 1, 1, 0);
    long ret = 0;
    if (status == 0 && lua_gettop(L) > 0) {
        if (lua_type(L, -1) == LUA_TNUMBER) {
            ret = lua_tointeger(L, -1);
        } else {
            ret = size * nmemb;
        }
        lua_pop(L, 1);
    } else {
        if (status != 0) {
            LOGE("http onwrite pcall error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        ret = size * nmemb;
    }

    lua_unlock(L);
    return (int)ret;
}

static size_t onread(void *ptr, size_t size, size_t nmemb, void *userdata) {
    ConnInfo *conn = (ConnInfo *)userdata;
    if (conn->completed) return 0;
    lua_State *L = conn->mainthread;

    lua_lock(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, conn->onreadref);
    if (!lua_isfunction(L, -1)) {
        LOGE("http onread: onreadref=%d resolved to %s, expected function\n",
             conn->onreadref, luaL_typename(L, -1));
        lua_pop(L, 1);
        lua_unlock(L);
        return 0;
    }

    size_t accept_size = size * nmemb;
    lua_pushinteger(L, accept_size);

    int status = lua_pcall(L, 1, 2, 0);
    long ret = 0;
    if (status == 0) {
        if (lua_isstring(L, 1)) {
            size_t len = 0;
            const char *str = lua_tolstring(L, 1, &len);
            ret = accept_size < len ? accept_size : len;
            memcpy(ptr, str, ret);
        } else if (lua_isnil(L, 1) && lua_gettop(L) > 1) {
            if (lua_isstring(L, 2) && strcasecmp(lua_tostring(L, 2), "abort") == 0) {
                ret = CURL_READFUNC_ABORT;
            }
        }
        lua_settop(L, 0);
    } else {
        LOGE("http onread pcall error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_unlock(L);
    return ret;
}

int debug_callback(CURL *curl_handle, curl_infotype infotype, char *buf, size_t size, void *data) {
    ConnInfo *conn = (ConnInfo *)data;
    lua_State *L = conn->mainthread;

    lua_lock(L);
    if (conn->verbose) {
        FILE *curlLogFile = NULL;

        lua_getfield(L, LUA_REGISTRYINDEX, "curlLogFile");
        if (lua_islightuserdata(L, -1)) {
            curlLogFile = lua_touserdata(L, -1);
        } else {
            lua_getfield(L, LUA_REGISTRYINDEX, "dataPath");
            if (lua_type(L, -1) == LUA_TSTRING) {
                lua_pushstring(L, "/verbose.log");
                lua_concat(L, 2);

                LOGD("set verbose %s\n", lua_tostring(L, -1));
                curlLogFile = fopen(lua_tostring(L, -1), "w");

                if (curlLogFile) {
                    lua_pushlightuserdata(L, curlLogFile);
                    lua_setfield(L, LUA_REGISTRYINDEX, "curlLogFile");
                }
            } else {
                LOGD("set verbose verbose.log\n");
                curlLogFile = fopen("verbose.log", "a");

                if (curlLogFile) {
                    lua_pushlightuserdata(L, curlLogFile);
                    lua_setfield(L, LUA_REGISTRYINDEX, "curlLogFile");
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        if (curlLogFile) {
            fwrite("> ", 2, 1, curlLogFile);
            fwrite(buf, size, 1, curlLogFile);
            fwrite("\n", 1, 1, curlLogFile);
            fflush(curlLogFile);
        }
    }

    lua_getglobal(L, "print");

    if (lua_isfunction(L, -1)) {
        switch (infotype) {
            case CURLINFO_TEXT:
                lua_pushstring(L, "CURLINFO_TEXT");
                break;
            case CURLINFO_DATA_IN:
                lua_pushstring(L, "CURLINFO_DATA_IN");
                break;
            case CURLINFO_DATA_OUT:
                lua_pushstring(L, "CURLINFO_DATA_OUT");
                break;
            case CURLINFO_END:
                lua_pushstring(L, "CURLINFO_END");
                break;
            case CURLINFO_HEADER_IN:
                lua_pushstring(L, "CURLINFO_HEADER_IN");
                break;
            case CURLINFO_HEADER_OUT:
                lua_pushstring(L, "CURLINFO_HEADER_OUT");
                break;
            case CURLINFO_SSL_DATA_IN:
                lua_pushstring(L, "CURLINFO_SSL_DATA_IN");
                break;
            case CURLINFO_SSL_DATA_OUT:
                lua_pushstring(L, "CURLINFO_SSL_DATA_OUT");
                break;
            default:
                lua_pushstring(L, "CURLINFO");
                break;
        }

        if (size > 0 && size < 1024) {
            lua_pushlstring(L, buf, size);
        } else {
            lua_pushinteger(L, size);
        }

        lua_pcall(L, 2, 0, 0);
    } else {
        lua_pop(L, 1);
    }

    lua_unlock(L);
    return 0;
}

static int http_runtime_prepare(HttpRuntime *runtime) {
    if (!runtime->base) {
        runtime->base = runtime->worker_id >= 0
            ? event_mgr_worker_base(runtime->worker_id)
            : event_mgr_base();
    }
    if (!runtime->base) {
        return -1;
    }
    if (!runtime->timer_event) {
        runtime->timer_event = evtimer_new(runtime->base, timer_cb, runtime);
        if (!runtime->timer_event) {
            return -1;
        }
    }
    if (!runtime->timer_check_multi_info) {
        runtime->timer_check_multi_info = evtimer_new(
            runtime->base, timer_check_multi_info_cb, runtime);
        if (!runtime->timer_check_multi_info) {
            event_free(runtime->timer_event);
            runtime->timer_event = NULL;
            return -1;
        }
    }
    if (!runtime->multi) {
        runtime->multi = curl_multi_init();
        if (!runtime->multi) {
            if (runtime->timer_check_multi_info) {
                event_free(runtime->timer_check_multi_info);
                runtime->timer_check_multi_info = NULL;
            }
            if (runtime->timer_event) {
                event_free(runtime->timer_event);
                runtime->timer_event = NULL;
            }
            return -1;
        }
        curl_multi_setopt(runtime->multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
        curl_multi_setopt(runtime->multi, CURLMOPT_SOCKETDATA, runtime);
        curl_multi_setopt(runtime->multi, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
        curl_multi_setopt(runtime->multi, CURLMOPT_TIMERDATA, runtime);
    }
    return 0;
}

/* Raise a Lua error from a helper called while the coarse Lua lock is held by
 * http_get()/http_post()/... (see the lock contract on http_getpost()).
 *
 * luaL_error() longjmps out of this C function, so the level taken by the
 * caller would never be released and the global recursive Lua mutex would stay
 * owned by this thread — every worker thread then blocks in lua_lock() forever
 * (docs/threading-model.md §2). Releasing our level first keeps the lock depth
 * balanced across the longjmp, exactly like a C function that never locked. */
static int http_lua_error(lua_State *L, const char *msg) {
    lua_unlock(L);
    return luaL_error(L, "%s", msg);
}

static HttpRuntime *http_runtime_for_request(lua_State *L) {
    int current_worker = event_mgr_current_worker_id();
    int requested_worker = current_worker;
    int worker_specified = 0;

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "worker");
        if (!lua_isnil(L, -1)) {
            worker_specified = 1;
            if (!lua_isinteger(L, -1)) {
                lua_pop(L, 1);
                http_lua_error(L, "http worker must be an integer");
            }
            requested_worker = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }

    if (!worker_specified && current_worker < 0) {
        requested_worker = HTTP_RUNTIME_MAIN;
    }
    if (requested_worker < HTTP_RUNTIME_MAIN) {
        http_lua_error(L, "http worker must be -1 or a non-negative worker id");
    }
    if (requested_worker == HTTP_RUNTIME_MAIN) {
        if (current_worker >= 0 && worker_specified) {
            http_lua_error(L, "worker thread cannot use the main HTTP runtime");
        }
        return &main_runtime;
    }
    if (requested_worker >= event_mgr_worker_count()) {
        http_lua_error(L, "http worker is unavailable");
    }
    if (current_worker != requested_worker) {
        http_lua_error(L, "http worker must match the current event worker");
    }
    return &worker_runtimes[requested_worker];
}

/* Lock contract: every public entry point below (http_get/http_post/...) takes
 * the global Lua lock for the whole call, so this function and its helpers run
 * with that coarse level held. Any Lua error raised here MUST go through
 * http_lua_error() so the level is released before the longjmp — otherwise the
 * recursive mutex stays owned by this thread and all worker threads deadlock in
 * lua_lock() (docs/threading-model.md §2). */
static int http_getpost(lua_State *L, int method) {
    HttpRuntime *runtime = http_runtime_for_request(L);
    ConnInfo *conn = calloc(1, sizeof(ConnInfo));
    if (!conn) {
        return http_lua_error(L, "http request allocation failed");
    }
    conn->runtime = runtime;

    conn->onheaderref = LUA_NOREF;
    conn->onprogressref = LUA_NOREF;
    conn->onreadref = LUA_NOREF;
    conn->onwriteref = LUA_NOREF;
    conn->coref = LUA_NOREF;
    conn->bodyref = LUA_NOREF;

    int oncompleteref = LUA_NOREF;

    bytearray_alloc(&conn->input, 1024);

    conn->easy = curl_easy_init();

    if (!conn->easy) {
        bytearray_dealloc(&conn->input);
        free(conn);
        http_lua_error(L, "curl_easy_init() failed");
        return 0;  // unreachable, http_lua_error longjmps
    }

    //    printf("lua_gettop(L)=%d\n", lua_gettop(L));

    lua_newtable(L); // response

    lua_newtable(L); // header
    lua_pushvalue(L, -1);
    conn->headerref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_setfield(L, -2, "headers");

    conn->retref = luaL_ref(L, LUA_REGISTRYINDEX);

    curl_easy_setopt(conn->easy, CURLOPT_WRITEFUNCTION, filldata);
    curl_easy_setopt(conn->easy, CURLOPT_WRITEDATA, conn);

    curl_easy_setopt(conn->easy, CURLOPT_HEADERFUNCTION, fillheader);
    curl_easy_setopt(conn->easy, CURLOPT_HEADERDATA, conn);
    curl_easy_setopt(conn->easy, CURLOPT_NOSIGNAL, 1);

    curl_easy_setopt(conn->easy, CURLOPT_ERRORBUFFER, conn->error);
    curl_easy_setopt(conn->easy, CURLOPT_PRIVATE, conn);

    const char *err = NULL;

    switch (method) {
        case HTTP_GET:
            curl_easy_setopt(conn->easy, CURLOPT_POST, 0);
            break;
        case HTTP_POST:
            curl_easy_setopt(conn->easy, CURLOPT_POST, 1);
            break;
        case HTTP_PUT:
            curl_easy_setopt(conn->easy, CURLOPT_POST, 1);
            curl_easy_setopt(conn->easy, CURLOPT_CUSTOMREQUEST, "PUT");
            break;
        case HTTP_HEAD:
            curl_easy_setopt(conn->easy, CURLOPT_CUSTOMREQUEST, "HEAD");
            curl_easy_setopt(conn->easy, CURLOPT_NOBODY, 1);
            break;
        case HTTP_DELETE:
            curl_easy_setopt(conn->easy, CURLOPT_CUSTOMREQUEST, "DELETE");
            break;
        case HTTP_UPDATE:
            /* Legacy non-standard verb kept for backward compatibility.
             * Do NOT add new callers — use HTTP_PATCH for RFC 5789 PATCH. */
            curl_easy_setopt(conn->easy, CURLOPT_CUSTOMREQUEST, "UPDATE");
            break;
        case HTTP_PATCH:
            /* CURLOPT_POST=1 primes the upload state machine so the body
             * from POSTFIELDS is actually sent; CUSTOMREQUEST rewrites the
             * request-line verb. Same pattern libcurl docs recommend for PATCH. */
            curl_easy_setopt(conn->easy, CURLOPT_POST, 1);
            curl_easy_setopt(conn->easy, CURLOPT_CUSTOMREQUEST, "PATCH");
            break;
        default:
            break;
    }

    if (lua_isstring(L, 1)) {
        lua_newtable(L);
        lua_pushvalue(L, 1);
        lua_setfield(L, -2, "url");
        lua_replace(L, 1);
    }

    if (lua_istable(L, 1)) {
        lua_pushliteral(L, "verbose");
        lua_gettable(L, 1);
        conn->verbose = luaL_optnumber(L, -1, GLOBAL_VERBOSE);
        lua_pop(L, 1);

        lua_pushliteral(L, "url");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_URL, lua_tostring(L, -1));
        } else {
            err = "invalid url type in table parameter";
            goto ERROR;
        }
        lua_pop(L, 1);

        //    LOGD("verbose = %d", args->verbose);
        if (conn->verbose) {
            curl_easy_setopt(conn->easy, CURLOPT_VERBOSE, 1);
            curl_easy_setopt(conn->easy, CURLOPT_DEBUGFUNCTION, debug_callback);
            curl_easy_setopt(conn->easy, CURLOPT_DEBUGDATA, conn);
        }

        lua_pushliteral(L, "dns_servers");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1)) {
            if (conn->verbose) {
                LOGD("using dns_servers: %s", lua_tostring(L, -1));
            }
            curl_easy_setopt(conn->easy, CURLOPT_DNS_SERVERS, lua_tostring(L, -1));
        } else if (!lua_isnil(L, -1)) {
            LOGE("invalid dns_servers type in table parameter");
        }
        else {
            if (dns_servers) {
                curl_easy_setopt(conn->easy, CURLOPT_DNS_SERVERS, dns_servers);
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "onprogress");
        if (lua_isfunction(L, -1)) {
            conn->onprogressref = luaL_ref(L, LUA_REGISTRYINDEX);
            curl_easy_setopt(conn->easy, CURLOPT_PROGRESSFUNCTION, onprogress);
            curl_easy_setopt(conn->easy, CURLOPT_PROGRESSDATA, conn);
            curl_easy_setopt(conn->easy, CURLOPT_NOPROGRESS, 0);
        } else if (lua_isnil(L, -1)) {
            conn->onprogressref = LUA_NOREF;
            lua_pop(L, 1);
        } else {
            LOGE("invalid onprogress type in table parameter");
            lua_pop(L, 1);
        }

        curl_easy_setopt(conn->easy, CURLOPT_LOW_SPEED_LIMIT, 1);

        lua_getfield(L, 1, "timeout");
        if (lua_isnumber(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_LOW_SPEED_TIME, lua_tointeger(L, -1));
        } else if (lua_isnil(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_LOW_SPEED_TIME, CURL_TIMEOUT_DEFAULT);
        } else {
            LOGE("invalid timeout type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "conntimeout");
        if (lua_isnumber(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_TIMEOUT, lua_tointeger(L, -1));
        } else if (lua_isnil(L, -1)) {
            //            curl_easy_setopt(conn->easy, CURLOPT_TIMEOUT,
            //            CURL_TIMEOUT_DEFAULT);
        } else {
            LOGE("invalid conntimeout type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "ssl_verifypeer");
        if (lua_isnumber(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYPEER, lua_tonumber(L, -1));
        } else if (lua_isnil(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYPEER, 1);
        } else {
            LOGE("invalid ssl_verifypeer type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "ssl_verifyhost");
        if (lua_isnumber(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYHOST, lua_tonumber(L, -1));
        } else if (lua_isnil(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYHOST, 2);
        } else {
            LOGE("invalid ssl_verifyhost type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslcert");
        if (lua_isstring(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_SSLCERT, lua_tostring(L, -1));
        } else if (!lua_isnil(L, -1)) {
            LOGE("invalid sslcert type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslcertpasswd");
        if (lua_isstring(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_SSLCERTPASSWD, lua_tostring(L, -1));
        } else if (!lua_isnil(L, -1)) {
            LOGE("invalid sslcertpasswd type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslcerttype");
        if (lua_isstring(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_SSLCERTTYPE, lua_tostring(L, -1));
        } else if (!lua_isnil(L, -1)) {
            LOGE("invalid sslcerttype type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslkey");
        if (lua_isstring(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_SSLKEY, lua_tostring(L, -1));
        } else if (!lua_isnil(L, -1)) {
            LOGE("invalid sslkey type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslkeypasswd");
        if (lua_isstring(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_SSLKEYPASSWD, lua_tostring(L, -1));
        } else if (!lua_isnil(L, -1)) {
            LOGE("invalid sslkeypasswd type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslkeytype");
        if (lua_isstring(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_SSLKEYTYPE, lua_tostring(L, -1));
        } else if (!lua_isnil(L, -1)) {
            LOGE("invalid sslkeytype type in table parameter");
        }
        lua_pop(L, 1);

        lua_pushliteral(L, "cainfo");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_CAINFO, lua_tostring(L, -1));
        } else if (!lua_isnil(L, -1)) {
            LOGE("invalid cainfo type in table parameter");
        } else {
            lua_getfield(L, LUA_REGISTRYINDEX, KEY_CAINFO);
            if (lua_isstring(L, -1)) {
                curl_easy_setopt(conn->easy, CURLOPT_CAINFO, lua_tostring(L, -1));
            } else {
#if TARGET_OS_IPHONE || defined(ANDROID) || defined(__ANDROID__)
#else
                curl_easy_setopt(conn->easy, CURLOPT_CAINFO, "cert.pem");
#endif
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        lua_pushliteral(L, "capath");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_CAPATH, lua_tostring(L, -1));
        } else if (!lua_isnil(L, -1)) {
            LOGE("invalid capath type in table parameter");
        } else {
            lua_getfield(L, LUA_REGISTRYINDEX, KEY_CAPATH);
            if (lua_isstring(L, -1)) {
                curl_easy_setopt(conn->easy, CURLOPT_CAPATH, lua_tostring(L, -1));
            } else {
                curl_easy_setopt(conn->easy, CURLOPT_CAPATH, ".");
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        lua_pushliteral(L, "headers");
        lua_gettable(L, 1);
        if (lua_istable(L, -1)) {
            struct curl_slist *headers = NULL;

            int t = lua_gettop(L);
            lua_pushnil(L); /* first key */
            while (lua_next(L, t) != 0) {
                size_t keylen = 0;
                const char *key = lua_tolstring(L, -2, &keylen);
                if (lua_type(L, -1) == LUA_TNUMBER) {
                    lua_Number n = lua_tonumber(L, -1);
                    if (n == floor(n)) {
                        lua_pushinteger(L, (int)n);
                        lua_remove(L, -2);
                    }
                }
                size_t valuelen = 0;
                const char *value = lua_tolstring(L, -1, &valuelen);

                if (value && valuelen) {
                    char *buf = malloc(keylen + valuelen + 3);
                    if (!buf) {
                        // Critical fix: Handle memory allocation failure
                        fprintf(stderr, "Memory allocation failed for HTTP header: %zu bytes\n", keylen + valuelen + 3);
                        err = "Memory allocation failure";
                        goto ERROR;
                    }
                    strcpy(buf, key);
                    strcpy(buf + keylen, ": ");
                    strcpy(buf + keylen + 2, value);

                    headers = curl_slist_append(headers, buf);

                    free(buf);
                } else {
                    printf("[http] ignore header '%s' and value with type '%s'\n", key,
                           lua_typename(L, lua_type(L, -1)));
                }

                /* removes 'value'; keeps 'key' for next iteration */
                lua_pop(L, 1);
            }
            lua_pop(L, 1);

            if (headers) {
                curl_easy_setopt(conn->easy, CURLOPT_HTTPHEADER, headers);
                conn->outputHeaders = headers;
            }
        } else if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
        } else {
            err = "invalid headers type in table parameter";
            goto ERROR;
        }

        if (proxyType != INT16_MAX) {
            curl_easy_setopt(conn->easy, CURLOPT_PROXYTYPE, proxyType);
            if (proxyHost) {
                curl_easy_setopt(conn->easy, CURLOPT_PROXY, proxyHost);
            }
            if (proxyPort) {
                curl_easy_setopt(conn->easy, CURLOPT_PROXYPORT, proxyPort);
            }
            if (proxyUsername) {
                curl_easy_setopt(conn->easy, CURLOPT_PROXYUSERNAME, proxyUsername);
            }
            if (proxyPassword) {
                curl_easy_setopt(conn->easy, CURLOPT_PROXYPASSWORD, proxyPassword);
            }
            curl_easy_setopt(conn->easy, CURLOPT_HEADEROPT, CURLHEADER_SEPARATE);
            curl_easy_setopt(conn->easy, CURLOPT_NOPROXY, "127.0.0.1,localhost");
        }

        lua_pushliteral(L, "proxytunnel");
        lua_gettable(L, 1);
        if (lua_isnumber(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_HTTPPROXYTUNNEL, lua_tointeger(L, -1));
        } else if (!lua_isnil(L, -1)) {
            err = "invalid proxytunnel type in table parameter";
            goto ERROR;
        }
        lua_pop(L, 1);

        const char *proxy = NULL;
        int proxyport = 0;
        const char *proxyuser = NULL;
        const char *proxypassword = NULL;

        lua_pushliteral(L, "proxy");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1)) {
            proxy = lua_tostring(L, -1);
        } else if (!lua_isnil(L, -1)) {
            err = "invalid proxy type in table parameter";
            goto ERROR;
        }
        lua_pop(L, 1);

        lua_pushliteral(L, "proxyport");
        lua_gettable(L, 1);
        if (lua_isnumber(L, -1)) {
            proxyport = lua_tointeger(L, -1);
        } else if (!lua_isnil(L, -1)) {
            err = "invalid proxyport type in table parameter";
            goto ERROR;
        }
        lua_pop(L, 1);

        lua_pushliteral(L, "proxyuser");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1)) {
            proxyuser = lua_tostring(L, -1);
        } else if (!lua_isnil(L, -1)) {
            err = "invalid proxyuser type in table parameter";
            goto ERROR;
        }
        lua_pop(L, 1);

        lua_pushliteral(L, "proxypassword");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1)) {
            proxypassword = lua_tostring(L, -1);
        } else if (!lua_isnil(L, -1)) {
            err = "invalid proxypassword type in table parameter";
            goto ERROR;
        }
        lua_pop(L, 1);

        /* proxy alone is enough when it is a full URL, e.g. http://user:pass@host:port
           (libcurl parses userinfo/host/port from CURLOPT_PROXY). proxyport /
           proxyuser / proxypassword remain optional overrides for host-only form. */
        if (proxy) {
            if (proxyport > 0) {
                LOGD("set proxy %s:%d", proxy, proxyport);
            } else {
                LOGD("set proxy %s", proxy);
            }
            curl_easy_setopt(conn->easy, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
            curl_easy_setopt(conn->easy, CURLOPT_PROXY, proxy);
            if (proxyport > 0) {
                curl_easy_setopt(conn->easy, CURLOPT_PROXYPORT, proxyport);
            }
            if (proxyuser) {
                curl_easy_setopt(conn->easy, CURLOPT_PROXYUSERNAME, proxyuser);
            }
            if (proxypassword) {
                curl_easy_setopt(conn->easy, CURLOPT_PROXYPASSWORD, proxypassword);
            }
            curl_easy_setopt(conn->easy, CURLOPT_HEADEROPT, CURLHEADER_SEPARATE);
            curl_easy_setopt(conn->easy, CURLOPT_NOPROXY, "127.0.0.1,localhost");
        }

        lua_getfield(L, 1, "onsend");
        if (lua_isfunction(L, -1)) {
            conn->onreadref = luaL_ref(L, LUA_REGISTRYINDEX);
            curl_easy_setopt(conn->easy, CURLOPT_READFUNCTION, onread);
            curl_easy_setopt(conn->easy, CURLOPT_READDATA, conn);
        } else if (lua_isnil(L, -1)) {
            conn->onreadref = LUA_NOREF;
            lua_pop(L, 1);

            // when onsend not defined, check body
            lua_pushliteral(L, "body");
            lua_gettable(L, 1);
            if (lua_isstring(L, -1)) {
                size_t len = 0;
                const char *data = lua_tolstring(L, -1, &len);
                lua_pushvalue(L, -1);
                conn->bodyref = luaL_ref(L, LUA_REGISTRYINDEX);

                curl_easy_setopt(conn->easy, CURLOPT_POSTFIELDSIZE, len);
                curl_easy_setopt(conn->easy, CURLOPT_POSTFIELDS, data);
            } else if (lua_isnil(L, -1)) {
                // No body provided — for POST/PUT methods, set empty body to prevent
                // curl from using fread() which blocks the event loop.
                if (method == HTTP_POST || method == HTTP_PUT || method == HTTP_UPDATE || method == HTTP_PATCH) {
                    curl_easy_setopt(conn->easy, CURLOPT_POSTFIELDSIZE, 0);
                    curl_easy_setopt(conn->easy, CURLOPT_POSTFIELDS, "");
                }
            } else {
                err = "invalid body type in table parameter";
                goto ERROR;
            }
            lua_pop(L, 1);
        } else {
            err = "invalid onsend type in table parameter";
            goto ERROR;
        }

        lua_getfield(L, 1, "onreceive");
        if (lua_isfunction(L, -1)) {
            conn->onwriteref = luaL_ref(L, LUA_REGISTRYINDEX);
            curl_easy_setopt(conn->easy, CURLOPT_WRITEFUNCTION, onwrite);
            curl_easy_setopt(conn->easy, CURLOPT_WRITEDATA, conn);
        } else if (lua_isnil(L, -1)) {
            conn->onwriteref = LUA_NOREF;
            lua_pop(L, 1);

            curl_easy_setopt(conn->easy, CURLOPT_WRITEFUNCTION, filldata);
            curl_easy_setopt(conn->easy, CURLOPT_WRITEDATA, conn);
        } else {
            err = "invalid onreceive type in table parameter";
            goto ERROR;
        }

        lua_getfield(L, 1, "onheader");
        if (lua_isfunction(L, -1)) {
            conn->onheaderref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else if (lua_isnil(L, -1)) {
            conn->onheaderref = LUA_NOREF;
            lua_pop(L, 1);
        } else {
            err = "invalid onheader type in table parameter";
            goto ERROR;
        }

        lua_getfield(L, 1, "oncomplete");
        if (lua_isfunction(L, -1)) {
            oncompleteref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
        } else {
            err = "invalid oncomplete type in table parameter";
            goto ERROR;
        }

        lua_getfield(L, 1, "resolve");
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char *resolve = lua_tostring(L, -1);
            struct curl_slist *host = curl_slist_append(NULL, resolve);
            curl_easy_setopt(conn->easy, CURLOPT_RESOLVE, host);
            conn->resolve = host;
        }

        lua_getfield(L, 1, "forbid_reuse");
        if (lua_isnumber(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_FORBID_REUSE, lua_tointeger(L, -1));
        }
        lua_pop(L, 1);

        lua_pushliteral(L, "cookiejar");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1)) {
            curl_easy_setopt(conn->easy, CURLOPT_COOKIEJAR, lua_tostring(L, -1));
            curl_easy_setopt(conn->easy, CURLOPT_COOKIEFILE, lua_tostring(L, -1));
        } else if (!lua_isnil(L, -1)) {
            err = "invalid cookiejar type in table parameter";
            goto ERROR;
        } else {
            lua_getfield(L, LUA_REGISTRYINDEX, KEY_COOKIE_JAR);
            if (lua_isstring(L, -1)) {
                curl_easy_setopt(conn->easy, CURLOPT_COOKIEJAR, lua_tostring(L, -1));
                curl_easy_setopt(conn->easy, CURLOPT_COOKIEFILE, lua_tostring(L, -1));
            } else {
                curl_easy_setopt(conn->easy, CURLOPT_COOKIEJAR, "/dev/null");
                curl_easy_setopt(conn->easy, CURLOPT_COOKIEFILE, "/dev/null");
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    } else {
        err = "invalid parameter";
        goto ERROR;
    }

    conn->mainthread = utlua_mainthread(L);

    if (oncompleteref != LUA_NOREF) {
        lua_lock(conn->mainthread);
        conn->L = lua_newthread(conn->mainthread);
        conn->coref = luaL_ref(conn->mainthread, LUA_REGISTRYINDEX);
        lua_rawgeti(conn->L, LUA_REGISTRYINDEX, oncompleteref);
        lua_unlock(conn->mainthread);
    } else {
        conn->L = L;
        lua_pushthread(L);
        conn->coref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    runtime = conn->runtime;
    if (http_runtime_prepare(runtime) != 0) {
        err = "http runtime initialization failed";
        goto ERROR;
    }

    if (share_handle) {
        curl_easy_setopt(conn->easy, CURLOPT_SHARE, share_handle);
    }

    CURLMcode rc = curl_multi_add_handle(runtime->multi, conn->easy);
    if (rc != CURLM_OK) {
        err = mcode_or_die("new_conn: curl_multi_add_handle", rc);
        goto ERROR;
    }

    IncrNetworkActivity();

    inflight_add(conn);
    incrRef(L);
    if (oncompleteref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, oncompleteref);
        return LUA_OK;
    } else {
        return LUA_YIELD;
    }

ERROR:
    if (conn->easy) {
        curl_easy_cleanup(conn->easy);
    }

    bytearray_dealloc(&conn->input);

    if (conn->onprogressref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onprogressref);
    }

    if (conn->onheaderref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onheaderref);
    }

    if (conn->onreadref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onreadref);
    }

    if (conn->onwriteref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onwriteref);
    }

    if (conn->coref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->coref);
    }

    if (oncompleteref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, oncompleteref);
    }

    luaL_unref(L, LUA_REGISTRYINDEX, conn->headerref);
    luaL_unref(L, LUA_REGISTRYINDEX, conn->retref);

    free(conn);

    if (err) {
        lua_unlock(L);
        luaL_error(L, "%s", err);
    }

    return LUA_OK;
}

LUA_API int http_get(lua_State *L) {
    lua_lock(L);
    if (http_getpost(L, HTTP_GET) == LUA_YIELD) {
        lua_unlock(L);
        return lua_yield(L, 0);
    } else {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_post(lua_State *L) {
    lua_lock(L);
    if (http_getpost(L, HTTP_POST) == LUA_YIELD) {
        lua_unlock(L);
        return lua_yield(L, 0);
    } else {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_put(lua_State *L) {
    lua_lock(L);
    if (http_getpost(L, HTTP_PUT) == LUA_YIELD) {
        lua_unlock(L);
        return lua_yield(L, 0);
    } else {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_update(lua_State *L) {
    lua_lock(L);
    if (http_getpost(L, HTTP_UPDATE) == LUA_YIELD) {
        lua_unlock(L);
        return lua_yield(L, 0);
    } else {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_patch(lua_State *L) {
    lua_lock(L);
    if (http_getpost(L, HTTP_PATCH) == LUA_YIELD) {
        lua_unlock(L);
        return lua_yield(L, 0);
    } else {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_delete(lua_State *L) {
    lua_lock(L);
    if (http_getpost(L, HTTP_DELETE) == LUA_YIELD) {
        lua_unlock(L);
        return lua_yield(L, 0);
    } else {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_head(lua_State *L) {
    lua_lock(L);
    if (http_getpost(L, HTTP_HEAD) == LUA_YIELD) {
        lua_unlock(L);
        return lua_yield(L, 0);
    } else {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_cookiejar(lua_State *L) {
    luaL_checkstring(L, 1);
    lua_settop(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, KEY_COOKIE_JAR);
    return 0;
}

LUA_API int http_cainfo(lua_State *L) {
    luaL_checkstring(L, 1);
    lua_settop(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, KEY_CAINFO);
    return 0;
}

LUA_API int http_capath(lua_State *L) {
    luaL_checkstring(L, 1);
    lua_settop(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, KEY_CAPATH);
    return 0;
}

LUA_API int http_escape(lua_State *L) {
    size_t size = 0;
    const char *str = luaL_checklstring(L, 1, &size);
    char *escaped = curl_escape(str, (int)size);
    if (escaped) {
        lua_pushstring(L, escaped);
        free(escaped);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

LUA_API int http_unescape(lua_State *L) {
    size_t size = 0;
    const char *s = luaL_checklstring(L, 1, &size);
    char *unescaped = curl_unescape(s, (int)size);
    if (unescaped) {
        lua_pushstring(L, unescaped);
        free(unescaped);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static const luaL_Reg httplib[] = {{"get", http_get},
                                   {"post", http_post},
                                   {"put", http_put},
                                   {"head", http_head},
                                   {"update", http_update},
                                   {"patch", http_patch},
                                   {"delete", http_delete},
                                   {"cookiejar", http_cookiejar},
                                   {"cainfo", http_cainfo},
                                   {"capath", http_capath},
                                   {"escape", http_escape},
                                   {"unescape", http_unescape},
                                   {NULL, NULL}};

static pthread_mutex_t share_lock;

void lock_function(CURL *handle, curl_lock_data data, curl_lock_access access, void *userptr) {
    pthread_mutex_lock(&share_lock);
}

void unlock_function(CURL *handle, curl_lock_data data, void *userptr) {
    pthread_mutex_unlock(&share_lock);
}

#if TARGET_OS_IPHONE || (defined(__APPLE__) && !defined(ANDROID) && !defined(__ANDROID__))
#include <arpa/inet.h>
#include <resolv.h>
#include <sys/socket.h>
#include <sys/types.h>

#define BUFF_LEN 1024

struct reset_dns_servers_arg {
    struct event *ev;
};

static void reset_dns_servers_cb(int fd, short kind, void *userp) {
    struct reset_dns_servers_arg *arg = (struct reset_dns_servers_arg *)userp;

    char buf[BUFF_LEN] = {0};
    int offset = 0;

    if (dns_servers) {
        free(dns_servers);
        dns_servers = NULL;
    }

    struct evdns_base *dns_base = event_mgr_dnsbase();
    if (dns_base) {
        evdns_base_clear_nameservers_and_suspend(dns_base);
    }

    struct __res_state res = {0};
    int result = res_ninit(&res);

    if (result == 0) {
        union res_9_sockaddr_union *addr_union = malloc(res.nscount * sizeof(union res_9_sockaddr_union));
        res_getservers(&res, addr_union, res.nscount);

        for (int i = 0; i < res.nscount; i++) {
            if (addr_union[i].sin.sin_family == AF_INET) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(addr_union[i].sin.sin_addr), ip, INET_ADDRSTRLEN);

                if (dns_base) {
                    evdns_base_nameserver_ip_add(dns_base, ip);
                }

                memcpy(buf + offset, ip, strlen(ip));
                offset += strlen(ip);
                memcpy(buf + offset, ",", 1);
                offset += 1;

                if (offset > BUFF_LEN - 21) {
                    break;
                }
            } else if (addr_union[i].sin6.sin6_family == AF_INET6) {
                char ip[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET6, &(addr_union[i].sin6.sin6_addr), ip, INET6_ADDRSTRLEN);
                if (dns_base) {
                    evdns_base_nameserver_ip_add(dns_base, ip);
                }

                memcpy(buf + offset, ip, strlen(ip));
                offset += strlen(ip);
                memcpy(buf + offset, ",", 1);
                offset += 1;

                if (offset > BUFF_LEN - 21) {
                    break;
                }
            } else {
                //                printf("Undefined family.\n");
            }
        }
        free(addr_union);
    }
    res_ndestroy(&res);

    if (dns_base) {
        evdns_base_resume(dns_base);
    }

    dns_servers = strdup(buf);

    event_free(arg->ev);
    free(arg);
}

extern void reset_dns_servers() {
    struct reset_dns_servers_arg *arg = (struct reset_dns_servers_arg *) malloc(sizeof(struct reset_dns_servers_arg));

    arg->ev = evtimer_new(event_mgr_base(), reset_dns_servers_cb, arg);
    struct timeval tv = {0, 1000};
    event_add(arg->ev, &tv);
}

#endif

// Called from event_mgr.c's cleanup paths BEFORE the event_base is freed.
// Tears down the curl multi handle and our timer events while the base is
// still valid, so libcurl's internal callbacks (which remove sockets from
// libevent) can run without dereferencing a freed base.
static void cleanup_http_runtime(HttpRuntime *runtime) {
    // First, drain all in-flight connections that never received CURLMSG_DONE.
    // Each one holds an incrRef that would otherwise leak.
    while (runtime->inflight_head) {
        ConnInfo *conn = runtime->inflight_head;
        inflight_remove(conn);

        if (!conn->completed) {
            conn->completed = 1;
            lua_State *L = conn->L;
            lua_State *mainthread = conn->mainthread ? conn->mainthread : (L ? utlua_mainthread(L) : NULL);

            if (L) {
                lua_lock(L);
                if (conn->bodyref != LUA_NOREF) {
                    luaL_unref(L, LUA_REGISTRYINDEX, conn->bodyref);
                }
                if (conn->onprogressref != LUA_NOREF) {
                    luaL_unref(L, LUA_REGISTRYINDEX, conn->onprogressref);
                }
                if (conn->onheaderref != LUA_NOREF) {
                    luaL_unref(L, LUA_REGISTRYINDEX, conn->onheaderref);
                }
                if (conn->onreadref != LUA_NOREF) {
                    luaL_unref(L, LUA_REGISTRYINDEX, conn->onreadref);
                }
                if (conn->onwriteref != LUA_NOREF) {
                    luaL_unref(L, LUA_REGISTRYINDEX, conn->onwriteref);
                }
                luaL_unref(L, LUA_REGISTRYINDEX, conn->headerref);
                luaL_unref(L, LUA_REGISTRYINDEX, conn->retref);
                if (conn->coref != LUA_NOREF) {
                    lua_State *unref_L = mainthread ? mainthread : L;
                    luaL_unref(unref_L, LUA_REGISTRYINDEX, conn->coref);
                }
                lua_unlock(L);
            }

            bytearray_dealloc(&conn->input);
            DecrNetworkActivity();

            if (mainthread) {
                decrRef(mainthread);
            }
        }

        if (conn->outputHeaders) {
            curl_slist_free_all(conn->outputHeaders);
        }
        if (conn->resolve) {
            curl_slist_free_all(conn->resolve);
        }
        if (conn->easy) {
            if (runtime->multi) {
                curl_multi_remove_handle(runtime->multi, conn->easy);
            }
            curl_easy_cleanup(conn->easy);
        }
        free(conn);
    }

    /* Pending Lua resume timers hold a registry ref + decrRef. If the runtime
     * is torn down before they fire, release them here instead of leaking the
     * ResumeInfo block and its pin. */
    while (runtime->resume_head) {
        ResumeInfo *info = runtime->resume_head;
        runtime->resume_head = info->next;
        lua_State *L = info->L;
        lua_State *mainthread = L ? utlua_mainthread(L) : NULL;
        if (info->resume_timer) {
            event_free(info->resume_timer);
            info->resume_timer = NULL;
        }
        if (mainthread && info->coref != LUA_NOREF) {
            lua_lock(mainthread);
            luaL_unref(mainthread, LUA_REGISTRYINDEX, info->coref);
            lua_unlock(mainthread);
        }
        if (mainthread) {
            decrRef(mainthread);
        }
        free(info);
    }

    if (runtime->multi) {
        // curl_multi_cleanup invokes socket/timer callbacks to detach pending
        // sockets from the event base. The base must still be alive for those
        // callbacks to succeed.
        curl_multi_cleanup(runtime->multi);
        runtime->multi = NULL;
    }
    if (runtime->timer_event) {
        event_free(runtime->timer_event);
        runtime->timer_event = NULL;
    }
    if (runtime->timer_check_multi_info) {
        event_free(runtime->timer_check_multi_info);
        runtime->timer_check_multi_info = NULL;
    }
    /* The owning event base is freed by event_mgr_loop_cleanup() after this
     * function returns. Do not retain a dangling pointer across a restart. */
    runtime->base = NULL;
    runtime->still_running = 0;
}

void cleanup_http_curl(void) {
    cleanup_http_runtime(&main_runtime);
    for (int i = 0; i < EVENT_MGR_MAX_WORKERS; i++) {
        if (worker_runtimes[i].base || worker_runtimes[i].multi ||
            worker_runtimes[i].inflight_head || worker_runtimes[i].timer_event ||
            worker_runtimes[i].timer_check_multi_info) {
            cleanup_http_runtime(&worker_runtimes[i]);
        }
    }
}

LUA_API int luaopen_fan_http_core(lua_State *L) {
    curl_global_init(CURL_GLOBAL_ALL);

    main_runtime.worker_id = HTTP_RUNTIME_MAIN;
    main_runtime.base = event_mgr_base();
    for (int i = 0; i < EVENT_MGR_MAX_WORKERS; i++) {
        worker_runtimes[i].worker_id = i;
    }

    if (!share_handle) {
        share_handle = curl_share_init();
        curl_share_setopt(share_handle, CURLSHOPT_LOCKFUNC, lock_function);
        curl_share_setopt(share_handle, CURLSHOPT_UNLOCKFUNC, unlock_function);
        curl_share_setopt(share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
#ifdef CURL_LOCK_DATA_CONNECT
        curl_share_setopt(share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
#endif

        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&share_lock, &a);
    }
    if (http_runtime_prepare(&main_runtime) != 0) {
        return luaL_error(L, "http runtime initialization failed");
    }

    lua_newtable(L);
    luaL_register(L, "http", httplib);

    lua_pushstring(L, curl_version());
    lua_setfield(L, -2, "curl_version");

    return 1;
}
