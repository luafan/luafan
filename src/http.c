
#include "utlua.h"

#define KEY_COOKIE_JAR "http.cookiejar"
#define KEY_CAINFO "http.cainfo"
#define KEY_CAPATH "http.capath"

#include <curl/curl.h>

#define MSG_OUT stdout /* Send info to stdout, change to stderr if you want */

static struct event *timer_event;
static struct event *timer_check_multi_info;
static CURLM *multi;
static int still_running;

#if TARGET_OS_IPHONE || defined(ANDROID) || defined(__ANDROID__)
extern char *proxyHost;
extern long proxyPort;
extern char *proxyUsername;
extern char *proxyPassword;
extern int proxyType;
extern int GLOBAL_VERBOSE;
#endif

/* Information associated with a specific easy handle */
typedef struct _ConnInfo
{
    CURL *easy;
    //    char *url;
    char error[CURL_ERROR_SIZE];

    struct curl_slist *outputHeaders;

    BYTEARRAY input;

    lua_State *mainthread;
    lua_State *L;

    int verbose;

    int onprogressref;
    int onheaderref;
    int onwriteref;
    int onreadref;

    int coref; // unref on ResumeInfo

    int headerref;
    int retref;
    int bodyref;

    struct curl_slist *headers;
} ConnInfo;

typedef struct
{
    struct event *resume_timer;
    lua_State *L;
    int coref;
} ResumeInfo;

/* Information associated with a specific socket */
typedef struct _SockInfo
{
    curl_socket_t sockfd;
    CURL *easy;
    int action;
    long timeout;
    struct event *ev;
    int evset;
} SockInfo;

#if defined(ANDROID) || defined(__ANDROID__)

#include "jni.h"
#include <curl/curl.h>
extern JavaVM *cachedJVM;
extern char *dns_servers;
#define IncrNetworkActivity() (void)0;
#define DecrNetworkActivity() (void)0;
extern void incrRef(lua_State *L);
extern void decrRef(lua_State *L);

#elif TARGET_OS_IPHONE

static char *dns_servers = NULL;
extern void IncrNetworkActivity();
extern void DecrNetworkActivity();
extern void incrRef(lua_State *L);
extern void decrRef(lua_State *L);

#else

#define IncrNetworkActivity() (void)0;
#define DecrNetworkActivity() (void)0;
#define incrRef(L) (void)0;
#define decrRef(L) (void)0;

#endif

#if TARGET_OS_IPHONE || defined(ANDROID) || defined(__ANDROID__)
static CURLSH *share_handle = NULL;
#endif

#define CURL_TIMEOUT_DEFAULT 60

enum
{
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT,
    HTTP_HEAD,
    HTTP_DELETE,
    HTTP_UPDATE
};

/* Update the event timer after curl_multi library calls */
static int multi_timer_cb(CURLM *multi, long timeout_ms, void *data)
{
    struct timeval timeout;
    (void)multi; /* unused */

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    //    fprintf(MSG_OUT, "multi_timer_cb: Setting timeout to %ld ms\n",
    //    timeout_ms);

    evtimer_add(timer_event, &timeout);
    return 0;
}

/* Die if we get a bad CURLMcode somewhere */
static const char *mcode_or_die(const char *where, CURLMcode code)
{
    if (CURLM_OK != code)
    {
        const char *s = curl_multi_strerror(code);
        fprintf(MSG_OUT, "ERROR: %s returns %s\n", where, s);
        return s;
    }

    return NULL;
}

static void resume_cb(int fd, short kind, void *userp);

static void http_getpost_complete(ConnInfo *conn)
{
    lua_State *L = conn->L;
    lua_lock(L);

    if (conn->bodyref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->bodyref);
        conn->bodyref = LUA_NOREF;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, conn->retref);
    luaL_unref(L, LUA_REGISTRYINDEX, conn->retref);

    if (strlen(conn->error) < CURL_ERROR_SIZE && *conn->error != 0)
    {
        lua_pushstring(L, conn->error);
        lua_setfield(L, -2, "error");
        // LOGE("%s", conn->error);
    }

    struct curl_slist *cookies = NULL;
    if (curl_easy_getinfo(conn->easy, CURLINFO_COOKIELIST, &cookies) ==
        CURLE_OK)
    {
        struct curl_slist *nc = cookies;
        lua_newtable(L);

        int i = 1;
        while (nc)
        {
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

    if (conn->input.total > 0)
    {
        lua_pushlstring(L, (const char *)conn->input.buffer, conn->input.total);
    }
    else
    {
        //        lua_pushliteral(L, "");
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "body");

    lua_pushinteger(L, responseCode);
    lua_setfield(L, -2, "responseCode");

    bytearray_dealloc(&conn->input);

    if (conn->onprogressref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onprogressref);
        conn->onprogressref = LUA_NOREF;
    }

    if (conn->onheaderref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onheaderref);
        conn->onheaderref = LUA_NOREF;
    }

    if (conn->onreadref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onreadref);
        conn->onreadref = LUA_NOREF;
    }

    if (conn->onwriteref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onwriteref);
        conn->onwriteref = LUA_NOREF;
    }

    luaL_unref(L, LUA_REGISTRYINDEX, conn->headerref);

    DecrNetworkActivity();

    lua_unlock(L);

    ResumeInfo *info = malloc(sizeof(ResumeInfo));
    info->L = L;
    info->coref = conn->coref;
    info->resume_timer = evtimer_new(event_mgr_base(), resume_cb, info);
    struct timeval tv = {0, 1};
    event_add(info->resume_timer, &tv);

    conn->coref = LUA_NOREF;
}

/* Check for completed transfers, and remove their easy handles */
static void timer_check_multi_info_cb(int fd, short kind, void *userp)
{
    char *eff_url;
    CURLMsg *msg;
    int msgs_left;

    //    printf("REMAINING: %d\n", still_running);
    while ((msg = curl_multi_info_read(multi, &msgs_left)))
    {
        //        printf("MSG_LEFT: %d\n", msgs_left);
        if (msg->msg == CURLMSG_DONE)
        {
            CURL *easy = msg->easy_handle;

            ConnInfo *conn = NULL;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &conn);
            curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff_url);
            //            printf("DONE: %s => (%d) %s\n", eff_url, msg->data.result,
            //            conn->error);

            http_getpost_complete(conn);

            curl_multi_remove_handle(multi, easy);
            //            free(conn->url);

            if (conn->outputHeaders)
            {
                curl_slist_free_all(conn->outputHeaders);
            }
            curl_easy_cleanup(easy);
            free(conn);
        }
    }
}

/* Called by libevent when we get action on a multi socket */
static void event_cb(int fd, short kind, void *userp)
{
    //    fprintf(MSG_OUT, "event_cb %d %d\n", fd, kind);
    int action = (kind & EV_READ ? CURL_CSELECT_IN : 0) |
                 (kind & EV_WRITE ? CURL_CSELECT_OUT : 0);

    CURLMcode rc = curl_multi_socket_action(multi, fd, action, &still_running);
    mcode_or_die("event_cb: curl_multi_socket_action", rc);

    struct timeval tv = {0, 100};
    event_add(timer_check_multi_info, &tv);

    if (still_running <= 0)
    {
        // fprintf(MSG_OUT, "last transfer done, kill timeout\n");
        if (evtimer_pending(timer_event, NULL))
        {
            evtimer_del(timer_event);
        }
    }
}

/* Called by libevent when our timeout expires */
static void timer_cb(int fd, short kind, void *userp)
{
    CURLMcode rc;
    (void)fd;
    (void)kind;

    rc = curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &still_running);
    mcode_or_die("timer_cb: curl_multi_socket_action", rc);

    struct timeval tv = {0, 1000};
    event_add(timer_check_multi_info, &tv);
}

static void resume_cb(int fd, short kind, void *userp)
{
    ResumeInfo *info = (ResumeInfo *)userp;
    //    fprintf(MSG_OUT, "resume\n");
    lua_State *L = info->L;
    lua_State *mainthread = utlua_mainthread(L);
    int coref = info->coref;

    event_free(info->resume_timer);
    free(info);

    FAN_RESUME(L, NULL, 1);

    luaL_unref(mainthread, LUA_REGISTRYINDEX, coref);
    decrRef(mainthread);
}

/* Clean up the SockInfo structure */
static void remsock(SockInfo *f)
{
    if (f)
    {
        if (f->evset)
            event_free(f->ev);
        free(f);
    }
}

/* Assign information to a SockInfo structure */
static void setsock(SockInfo *f, curl_socket_t s, CURL *e, int act,
                    void *data)
{
    int kind = (act & CURL_POLL_IN ? EV_READ : 0) |
               (act & CURL_POLL_OUT ? EV_WRITE : 0) | EV_PERSIST;

    f->sockfd = s;
    f->action = act;
    f->easy = e;
    if (f->evset)
        event_free(f->ev);
    f->ev = event_new(event_mgr_base(), f->sockfd, kind, event_cb, data);
    f->evset = 1;
    event_add(f->ev, NULL);
}

/* Initialize a new SockInfo structure */
static void addsock(curl_socket_t s, CURL *easy, int action, void *data)
{
    SockInfo *fdp = calloc(1, sizeof(SockInfo));
    setsock(fdp, s, easy, action, data);
    curl_multi_assign(multi, s, fdp);
}

/* CURLMOPT_SOCKETFUNCTION */
static int sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp)
{
    SockInfo *fdp = (SockInfo *)sockp;
    //    const char *whatstr[]={ "none", "IN", "OUT", "INOUT", "REMOVE" };

    //  fprintf(MSG_OUT,
    //          "socket callback: s=%d e=%p what=%s ", s, e, whatstr[what]);
    if (what == CURL_POLL_REMOVE)
    {
        //    fprintf(MSG_OUT, "\n");
        remsock(fdp);
    }
    else
    {
        if (!fdp)
        {
            //      fprintf(MSG_OUT, "Adding data: %s\n", whatstr[what]);
            addsock(s, e, what, cbp);
        }
        else
        {
            //      fprintf(MSG_OUT,
            //              "Changing action from %s to %s\n",
            //              whatstr[fdp->action], whatstr[what]);
            setsock(fdp, s, e, what, cbp);
        }
    }
    return 0;
}

static size_t filldata(char *ptr, size_t size, size_t nmemb, ConnInfo *conn)
{
    int i = 0;
    //    fprintf(MSG_OUT, "filldata %zu %zu\n", size, nmemb);
    for (; i < size; i++)
    {
        bytearray_writebuffer(&conn->input, ptr + i * nmemb, nmemb);
    }
    return size * nmemb;
}

static size_t fillheader(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    ConnInfo *conn = (ConnInfo *)userdata;
    lua_State *L = conn->L;

    lua_lock(L);
    size_t total = size * nmemb;
    char *offset = ptr;

    lua_rawgeti(L, LUA_REGISTRYINDEX, conn->headerref);
    //    printf("lua_gettop(L)=%d\n", lua_gettop(L));

    int meetspace = 0;
    int i = 0;
    for (; i < total; i++)
    {
        if (!meetspace && *(offset + i) == ' ')
        {
            meetspace = 1;
            continue;
        }

        if (!meetspace && *(offset + i) == ':' && i + 3 < total)
        {
            // check exist or not, if exist, change value field to array.
            lua_pushlstring(L, offset, i); // key
            lua_rawget(L, -2);             // header table
            int mix_values = 0;
            if (!lua_isnil(L, -1))
            {
                if (!lua_istable(L, -1))
                {
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
            }
            else
            {
                lua_pop(L, 1);
            }

            const char *front = offset + i + 1;
            const char *end = offset + total - 1;
            size_t size = total - i - 2;

            for (; size && isspace(*front); size--, front++)
                ;
            for (; size && isspace(*end); size--, end--)
                ;

            if (mix_values)
            {
                lua_pushlstring(L, front, (size_t)(end - front) + 1);
                lua_rawseti(L, -2, (int)lua_objlen(L, -2) + 1);
                lua_pop(L, 1);
            }
            else
            {
                lua_pushlstring(L, offset, i); // key
                lua_pushlstring(L, front, (size_t)(end - front) + 1);
                lua_rawset(L, -3);
            }

            if (strstr(offset, "Content-Type") == offset)
            {
                const char *charset_offset = strstr(front, "charset=");
                if (charset_offset)
                {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, conn->retref);
                    lua_pushlstring(L, charset_offset + strlen("charset="),
                                    (size_t)(end - charset_offset - strlen("charset=")) +
                                        1);
                    lua_setfield(L, -2, "charset"); // response
                    lua_pop(L, 1);
                }
            }
        }
    }

    //    printf("lua_gettop(L)=%d\n", lua_gettop(L));

    if (conn->onheaderref != LUA_NOREF && total <= 2)
    {
        long responseCode = -1;
        curl_easy_getinfo(conn->easy, CURLINFO_RESPONSE_CODE, &responseCode);

        lua_pushliteral(L, "responseCode");
        lua_pushinteger(L, responseCode);
        lua_rawset(L, -3); // header table

        lua_State *co = lua_newthread(L);
        PUSH_REF(L);

        lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onheaderref);
        lua_xmove(L, co, 1);

        FAN_RESUME(co, L, 1);

        POP_REF(L);
    }
    else
    {
        lua_pop(L, 1); // pop header table
    }

    lua_unlock(L);

    return size * nmemb;
}

static int onprogress(void *clientp, double dltotal, double dlnow,
                      double ultotal, double ulnow)
{
    ConnInfo *conn = (ConnInfo *)clientp;
    lua_State *L = conn->mainthread;

    lua_lock(L);
    lua_State *co = lua_newthread(L);
    PUSH_REF(L);

    lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onprogressref);
    lua_unlock(L);

    lua_pushinteger(co, dltotal);
    lua_pushinteger(co, dlnow);
    lua_pushinteger(co, ultotal);
    lua_pushinteger(co, ulnow);

    int status = FAN_RESUME(co, L, 4);
    long ret = 0;
    if (status == 0 && lua_gettop(co) > 0)
    {
        if (lua_type(co, 1) == LUA_TNUMBER)
        {
            ret = lua_tointeger(co, 1);
        }
    }

    POP_REF(L);

    return (int)ret;
}

static size_t onwrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    ConnInfo *conn = (ConnInfo *)userdata;
    lua_State *L = conn->mainthread;

    lua_lock(L);
    lua_State *co = lua_newthread(L);
    PUSH_REF(L);
    lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onwriteref);
    lua_unlock(L);

    lua_pushlstring(co, ptr, size * nmemb);

    int status = FAN_RESUME(co, L, 1);
    long ret = 0;
    if (status == 0 && lua_gettop(co) > 0)
    {
        if (lua_type(co, 1) == LUA_TNUMBER)
        {
            ret = lua_tointeger(co, 1);
        }
    }
    else
    {
        ret = size * nmemb;
    }

    POP_REF(L);

    return (int)ret;
}

static size_t onread(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    ConnInfo *conn = (ConnInfo *)userdata;
    lua_State *L = conn->mainthread;

    lua_lock(L);
    lua_State *co = lua_newthread(L);
    PUSH_REF(L);

    size_t accept_size = size * nmemb;

    lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onreadref);
    lua_unlock(L);

    lua_pushinteger(co, accept_size);

    int status = FAN_RESUME(co, L, 1);
    long ret = 0;
    if (status == 0 && lua_gettop(co) > 0)
    {
        if (lua_isstring(co, 1))
        {
            size_t len = 0;
            const char *str = lua_tolstring(co, 1, &len);
            ret = accept_size < len ? accept_size : len;
            memcpy(ptr, str, ret);
        }
        else if (lua_isnil(co, 1) && lua_gettop(co) > 1)
        {
            if (lua_isstring(co, 2) &&
                strcasecmp(lua_tostring(co, 2), "abort") == 0)
            {
                ret = CURL_READFUNC_ABORT;
            }
        }
    }
    POP_REF(L);
    return ret;
}

int debug_callback(CURL *curl_handle, curl_infotype infotype, char *buf,
                   size_t size, void *data)
{
    ConnInfo *conn = (ConnInfo *)data;
    lua_State *L = conn->mainthread;

    lua_lock(L);
    if (conn->verbose)
    {
        FILE *curlLogFile = NULL;

        lua_getfield(L, LUA_REGISTRYINDEX, "curlLogFile");
        if (lua_islightuserdata(L, -1))
        {
            curlLogFile = lua_touserdata(L, -1);
        }
        else
        {
            lua_getfield(L, LUA_REGISTRYINDEX, "dataPath");
            if (lua_type(L, -1) == LUA_TSTRING)
            {
                lua_pushstring(L, "/verbose.log");
                lua_concat(L, 2);

                LOGD("set verbose %s\n", lua_tostring(L, -1));
                curlLogFile = fopen(lua_tostring(L, -1), "w");

                if (curlLogFile)
                {
                    lua_pushlightuserdata(L, curlLogFile);
                    lua_setfield(L, LUA_REGISTRYINDEX, "curlLogFile");
                }
            }
            else
            {
                LOGD("set verbose verbose.log\n");
                curlLogFile = fopen("verbose.log", "a");

                if (curlLogFile)
                {
                    lua_pushlightuserdata(L, curlLogFile);
                    lua_setfield(L, LUA_REGISTRYINDEX, "curlLogFile");
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        if (curlLogFile)
        {
            fwrite("> ", 2, 1, curlLogFile);
            fwrite(buf, size, 1, curlLogFile);
            fwrite("\n", 1, 1, curlLogFile);
            fflush(curlLogFile);
        }
    }

    lua_getglobal(L, "print");

    if (lua_isfunction(L, -1))
    {
        switch (infotype)
        {
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

        if (size > 0 && size < 1024)
        {
            lua_pushlstring(L, buf, size);
        }
        else
        {
            lua_pushinteger(L, size);
        }

        lua_pcall(L, 2, 0, 0);
    }
    else
    {
        lua_pop(L, 1);
    }

    lua_unlock(L);
    return 0;
}

static int http_getpost(lua_State *L, int method)
{
    ConnInfo *conn = calloc(1, sizeof(ConnInfo));

    conn->onheaderref = LUA_NOREF;
    conn->onprogressref = LUA_NOREF;
    conn->onreadref = LUA_NOREF;
    conn->onwriteref = LUA_NOREF;
    conn->coref = LUA_NOREF;
    conn->bodyref = LUA_NOREF;

    int oncompleteref = LUA_NOREF;

    bytearray_alloc(&conn->input, 1024);

    conn->easy = curl_easy_init();

    if (!conn->easy)
    {
        fprintf(MSG_OUT, "curl_easy_init() failed, exiting!\n");
        exit(2);
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

    switch (method)
    {
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
        curl_easy_setopt(conn->easy, CURLOPT_CUSTOMREQUEST, "UPDATE");
        break;
    default:
        break;
    }

    if (lua_isstring(L, 1))
    {
        lua_newtable(L);
        lua_pushvalue(L, 1);
        lua_setfield(L, -2, "url");
        lua_replace(L, 1);
    }

    if (lua_istable(L, 1))
    {
        lua_pushliteral(L, "verbose");
        lua_gettable(L, 1);
#if TARGET_OS_IPHONE || defined(ANDROID) || defined(__ANDROID__)
        conn->verbose = luaL_optnumber(L, -1, GLOBAL_VERBOSE);
#else
        conn->verbose = luaL_optnumber(L, -1, 0);
#endif
        lua_pop(L, 1);

        lua_pushliteral(L, "url");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_URL, lua_tostring(L, -1));
        }
        else
        {
            err = "invalid url type in table parameter";
            goto ERROR;
        }
        lua_pop(L, 1);

        //    LOGD("verbose = %d", args->verbose);
        if (conn->verbose)
        {
            curl_easy_setopt(conn->easy, CURLOPT_VERBOSE, 1);
            curl_easy_setopt(conn->easy, CURLOPT_DEBUGFUNCTION, debug_callback);
            curl_easy_setopt(conn->easy, CURLOPT_DEBUGDATA, conn);
        }

        lua_pushliteral(L, "dns_servers");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1))
        {
            if (conn->verbose)
            {
                LOGD("using dns_servers: %s", lua_tostring(L, -1));
            }
            curl_easy_setopt(conn->easy, CURLOPT_DNS_SERVERS, lua_tostring(L, -1));
        }
        else if (!lua_isnil(L, -1))
        {
            LOGE("invalid dns_servers type in table parameter");
        }
#if TARGET_OS_IPHONE || defined(ANDROID) || defined(__ANDROID__)
        else
        {
            if (dns_servers)
            {
                curl_easy_setopt(conn->easy, CURLOPT_DNS_SERVERS, dns_servers);
            }
        }
#endif
        lua_pop(L, 1);

        lua_getfield(L, 1, "onprogress");
        if (lua_isfunction(L, -1))
        {
            conn->onprogressref = luaL_ref(L, LUA_REGISTRYINDEX);
            curl_easy_setopt(conn->easy, CURLOPT_PROGRESSFUNCTION, onprogress);
            curl_easy_setopt(conn->easy, CURLOPT_PROGRESSDATA, conn);
            curl_easy_setopt(conn->easy, CURLOPT_NOPROGRESS, 0);
        }
        else if (lua_isnil(L, -1))
        {
            conn->onprogressref = LUA_NOREF;
            lua_pop(L, 1);
        }
        else
        {
            LOGE("invalid onprogress type in table parameter");
            lua_pop(L, 1);
        }

        curl_easy_setopt(conn->easy, CURLOPT_LOW_SPEED_LIMIT, 1);

        lua_getfield(L, 1, "timeout");
        if (lua_isnumber(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_LOW_SPEED_TIME,
                             lua_tointeger(L, -1));
        }
        else if (lua_isnil(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_LOW_SPEED_TIME,
                             CURL_TIMEOUT_DEFAULT);
        }
        else
        {
            LOGE("invalid timeout type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "conntimeout");
        if (lua_isnumber(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_TIMEOUT, lua_tointeger(L, -1));
        }
        else if (lua_isnil(L, -1))
        {
            //            curl_easy_setopt(conn->easy, CURLOPT_TIMEOUT,
            //            CURL_TIMEOUT_DEFAULT);
        }
        else
        {
            LOGE("invalid conntimeout type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "ssl_verifypeer");
        if (lua_isnumber(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYPEER, lua_tonumber(L, -1));
        }
        else if (lua_isnil(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYPEER, 1);
        }
        else
        {
            LOGE("invalid ssl_verifypeer type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "ssl_verifyhost");
        if (lua_isnumber(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYHOST, lua_tonumber(L, -1));
        }
        else if (lua_isnil(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYHOST, 2);
        }
        else
        {
            LOGE("invalid ssl_verifyhost type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslcert");
        if (lua_isstring(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_SSLCERT, lua_tostring(L, -1));
        }
        else if (!lua_isnil(L, -1))
        {
            LOGE("invalid sslcert type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslcertpasswd");
        if (lua_isstring(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_SSLCERTPASSWD, lua_tostring(L, -1));
        }
        else if (!lua_isnil(L, -1))
        {
            LOGE("invalid sslcertpasswd type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslcerttype");
        if (lua_isstring(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_SSLCERTTYPE, lua_tostring(L, -1));
        }
        else if (!lua_isnil(L, -1))
        {
            LOGE("invalid sslcerttype type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslkey");
        if (lua_isstring(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_SSLKEY, lua_tostring(L, -1));
        }
        else if (!lua_isnil(L, -1))
        {
            LOGE("invalid sslkey type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslkeypasswd");
        if (lua_isstring(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_SSLKEYPASSWD, lua_tostring(L, -1));
        }
        else if (!lua_isnil(L, -1))
        {
            LOGE("invalid sslkeypasswd type in table parameter");
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sslkeytype");
        if (lua_isstring(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_SSLKEYTYPE, lua_tostring(L, -1));
        }
        else if (!lua_isnil(L, -1))
        {
            LOGE("invalid sslkeytype type in table parameter");
        }
        lua_pop(L, 1);

        lua_pushliteral(L, "cainfo");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_CAINFO, lua_tostring(L, -1));
        }
        else if (!lua_isnil(L, -1))
        {
            LOGE("invalid cainfo type in table parameter");
        }
        else
        {
            lua_getfield(L, LUA_REGISTRYINDEX, KEY_CAINFO);
            if (lua_isstring(L, -1))
            {
                curl_easy_setopt(conn->easy, CURLOPT_CAINFO, lua_tostring(L, -1));
            }
            else
            {
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
        if (lua_isstring(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_CAPATH, lua_tostring(L, -1));
        }
        else if (!lua_isnil(L, -1))
        {
            LOGE("invalid capath type in table parameter");
        }
        else
        {
            lua_getfield(L, LUA_REGISTRYINDEX, KEY_CAPATH);
            if (lua_isstring(L, -1))
            {
                curl_easy_setopt(conn->easy, CURLOPT_CAPATH, lua_tostring(L, -1));
            }
            else
            {
                curl_easy_setopt(conn->easy, CURLOPT_CAPATH, ".");
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        lua_pushliteral(L, "headers");
        lua_gettable(L, 1);
        if (lua_istable(L, -1))
        {
            struct curl_slist *headers = NULL;

            int t = lua_gettop(L);
            lua_pushnil(L); /* first key */
            while (lua_next(L, t) != 0)
            {
                size_t keylen = 0;
                const char *key = lua_tolstring(L, -2, &keylen);
                if (lua_type(L, -1) == LUA_TNUMBER)
                {
                    lua_Number n = lua_tonumber(L, -1);
                    if (n == floor(n))
                    {
                        lua_pushinteger(L, (int)n);
                        lua_remove(L, -2);
                    }
                }
                size_t valuelen = 0;
                const char *value = lua_tolstring(L, -1, &valuelen);

                if (value && valuelen)
                {
                    char *buf = malloc(keylen + valuelen + 3);
                    strcpy(buf, key);
                    strcpy(buf + keylen, ": ");
                    strcpy(buf + keylen + 2, value);

                    headers = curl_slist_append(headers, buf);

                    free(buf);
                }
                else
                {
                    printf("[http] ignore header '%s' and value with type '%s'\n", key, lua_typename(L, lua_type(L, -1)));
                }

                /* removes 'value'; keeps 'key' for next iteration */
                lua_pop(L, 1);
            }
            lua_pop(L, 1);

            if (headers)
            {
                curl_easy_setopt(conn->easy, CURLOPT_HTTPHEADER, headers);
                conn->outputHeaders = headers;
            }
        }
        else if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
        }
        else
        {
            err = "invalid headers type in table parameter";
            goto ERROR;
        }

#if TARGET_OS_IPHONE || defined(ANDROID) || defined(__ANDROID__)

        if (proxyType != INT16_MAX)
        {
            curl_easy_setopt(conn->easy, CURLOPT_PROXYTYPE, proxyType);
            if (proxyHost)
            {
                curl_easy_setopt(conn->easy, CURLOPT_PROXY, proxyHost);
            }
            if (proxyPort)
            {
                curl_easy_setopt(conn->easy, CURLOPT_PROXYPORT, proxyPort);
            }
            if (proxyUsername)
            {
                curl_easy_setopt(conn->easy, CURLOPT_PROXYUSERNAME, proxyUsername);
            }
            if (proxyPassword)
            {
                curl_easy_setopt(conn->easy, CURLOPT_PROXYPASSWORD, proxyPassword);
            }
            curl_easy_setopt(conn->easy, CURLOPT_HEADEROPT, CURLHEADER_SEPARATE);
            curl_easy_setopt(conn->easy, CURLOPT_NOPROXY, "127.0.0.1,localhost");
        }

#endif
        lua_pushliteral(L, "proxytunnel");
        lua_gettable(L, 1);
        if (lua_isnumber(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_HTTPPROXYTUNNEL,
                             lua_tointeger(L, -1));
        }
        else if (!lua_isnil(L, -1))
        {
            err = "invalid proxytunnel type in table parameter";
            goto ERROR;
        }
        lua_pop(L, 1);

        const char *proxy = NULL;
        int proxyport = 0;

        lua_pushliteral(L, "proxy");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1))
        {
            proxy = lua_tostring(L, -1);
        }
        else if (!lua_isnil(L, -1))
        {
            err = "invalid proxy type in table parameter";
            goto ERROR;
        }
        lua_pop(L, 1);

        lua_pushliteral(L, "proxyport");
        lua_gettable(L, 1);
        if (lua_isnumber(L, -1))
        {
            proxyport = lua_tointeger(L, -1);
        }
        else if (!lua_isnil(L, -1))
        {
            err = "invalid proxyport type in table parameter";
            goto ERROR;
        }
        lua_pop(L, 1);

        if (proxy && proxyport > 0)
        {
            LOGD("set proxy %s:%d", proxy, proxyport);
            curl_easy_setopt(conn->easy, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
            curl_easy_setopt(conn->easy, CURLOPT_PROXY, proxy);
            curl_easy_setopt(conn->easy, CURLOPT_PROXYPORT, proxyport);
#ifdef CURLHEADER_SEPARATE
            curl_easy_setopt(conn->easy, CURLOPT_HEADEROPT, CURLHEADER_SEPARATE);
#endif
            curl_easy_setopt(conn->easy, CURLOPT_NOPROXY, "127.0.0.1,localhost");
        }

        lua_getfield(L, 1, "onsend");
        if (lua_isfunction(L, -1))
        {
            conn->onreadref = luaL_ref(L, LUA_REGISTRYINDEX);
            curl_easy_setopt(conn->easy, CURLOPT_READFUNCTION, onread);
            curl_easy_setopt(conn->easy, CURLOPT_READDATA, conn);
        }
        else if (lua_isnil(L, -1))
        {
            conn->onreadref = LUA_NOREF;
            lua_pop(L, 1);

            // when onsend not defined, check body
            lua_pushliteral(L, "body");
            lua_gettable(L, 1);
            if (lua_isstring(L, -1))
            {
                size_t len = 0;
                const char *data = lua_tolstring(L, -1, &len);
                lua_pushvalue(L, -1);
                conn->bodyref = luaL_ref(L, LUA_REGISTRYINDEX);

                curl_easy_setopt(conn->easy, CURLOPT_POSTFIELDSIZE, len);
                curl_easy_setopt(conn->easy, CURLOPT_POSTFIELDS, data);
            }
            else if (!lua_isnil(L, -1))
            {
                err = "invalid body type in table parameter";
                goto ERROR;
            }
            lua_pop(L, 1);
        }
        else
        {
            err = "invalid onsend type in table parameter";
            goto ERROR;
        }

        lua_getfield(L, 1, "onreceive");
        if (lua_isfunction(L, -1))
        {
            conn->onwriteref = luaL_ref(L, LUA_REGISTRYINDEX);
            curl_easy_setopt(conn->easy, CURLOPT_WRITEFUNCTION, onwrite);
            curl_easy_setopt(conn->easy, CURLOPT_WRITEDATA, conn);
        }
        else if (lua_isnil(L, -1))
        {
            conn->onwriteref = LUA_NOREF;
            lua_pop(L, 1);

            curl_easy_setopt(conn->easy, CURLOPT_WRITEFUNCTION, filldata);
            curl_easy_setopt(conn->easy, CURLOPT_WRITEDATA, conn);
        }
        else
        {
            err = "invalid onreceive type in table parameter";
            goto ERROR;
        }

        lua_getfield(L, 1, "onheader");
        if (lua_isfunction(L, -1))
        {
            conn->onheaderref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        else if (lua_isnil(L, -1))
        {
            conn->onheaderref = LUA_NOREF;
            lua_pop(L, 1);
        }
        else
        {
            err = "invalid onheader type in table parameter";
            goto ERROR;
        }

        lua_getfield(L, 1, "oncomplete");
        if (lua_isfunction(L, -1))
        {
            oncompleteref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        else if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
        }
        else
        {
            err = "invalid oncomplete type in table parameter";
            goto ERROR;
        }

        lua_getfield(L, 1, "forbid_reuse");
        if (lua_isnumber(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_FORBID_REUSE, lua_tointeger(L, -1));
        }
        lua_pop(L, 1);

        lua_pushliteral(L, "cookiejar");
        lua_gettable(L, 1);
        if (lua_isstring(L, -1))
        {
            curl_easy_setopt(conn->easy, CURLOPT_COOKIEJAR, lua_tostring(L, -1));
            curl_easy_setopt(conn->easy, CURLOPT_COOKIEFILE, lua_tostring(L, -1));
        }
        else if (!lua_isnil(L, -1))
        {
            err = "invalid cookiejar type in table parameter";
            goto ERROR;
        }
        else
        {
            lua_getfield(L, LUA_REGISTRYINDEX, KEY_COOKIE_JAR);
            if (lua_isstring(L, -1))
            {
                curl_easy_setopt(conn->easy, CURLOPT_COOKIEJAR, lua_tostring(L, -1));
                curl_easy_setopt(conn->easy, CURLOPT_COOKIEFILE, lua_tostring(L, -1));
            }
            else
            {
                curl_easy_setopt(conn->easy, CURLOPT_COOKIEJAR, "cookies.txt");
                curl_easy_setopt(conn->easy, CURLOPT_COOKIEFILE, "cookies.txt");
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    else
    {
        err = "invalid parameter";
        goto ERROR;
    }

    conn->mainthread = utlua_mainthread(L);

    if (oncompleteref != LUA_NOREF)
    {
        lua_lock(conn->mainthread);
        conn->L = lua_newthread(conn->mainthread);
        conn->coref = luaL_ref(conn->mainthread, LUA_REGISTRYINDEX);
        lua_unlock(conn->mainthread);

        lua_rawgeti(conn->L, LUA_REGISTRYINDEX, oncompleteref);
    }
    else
    {
        conn->L = L;
        lua_pushthread(L);
        conn->coref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    if (!multi)
    {
        multi = curl_multi_init();

        curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
        curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, NULL);
        curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
        curl_multi_setopt(multi, CURLMOPT_TIMERDATA, NULL);
#if TARGET_OS_IPHONE || defined(ANDROID) || defined(__ANDROID__)
        curl_multi_setopt(multi, CURLOPT_SHARE, share_handle);
#endif
    }

    CURLMcode rc = curl_multi_add_handle(multi, conn->easy);
    if (rc != CURLM_OK)
    {
        err = mcode_or_die("new_conn: curl_multi_add_handle", rc);
        goto ERROR;
    }

    IncrNetworkActivity();

    incrRef(L);
    if (oncompleteref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, oncompleteref);
        return LUA_OK;
    }
    else
    {
        return LUA_YIELD;
    }

ERROR:
    if (conn->easy)
    {
        curl_easy_cleanup(conn->easy);
    }

    bytearray_dealloc(&conn->input);

    if (conn->onprogressref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onprogressref);
    }

    if (conn->onheaderref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onheaderref);
    }

    if (conn->onreadref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onreadref);
    }

    if (conn->onwriteref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->onwriteref);
    }

    if (conn->coref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, conn->coref);
    }

    if (oncompleteref != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, oncompleteref);
    }

    luaL_unref(L, LUA_REGISTRYINDEX, conn->headerref);
    luaL_unref(L, LUA_REGISTRYINDEX, conn->retref);

    free(conn);

    if (err)
    {
        lua_unlock(L);
        luaL_error(L, err);
    }

    return LUA_OK;
}

LUA_API int http_get(lua_State *L)
{
    lua_lock(L);
    if (http_getpost(L, HTTP_GET) == LUA_YIELD)
    {
        lua_unlock(L);
        return lua_yield(L, 0);
    }
    else
    {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_post(lua_State *L)
{
    lua_lock(L);
    if (http_getpost(L, HTTP_POST) == LUA_YIELD)
    {
        lua_unlock(L);
        return lua_yield(L, 0);
    }
    else
    {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_put(lua_State *L)
{
    lua_lock(L);
    if (http_getpost(L, HTTP_PUT) == LUA_YIELD)
    {
        lua_unlock(L);
        return lua_yield(L, 0);
    }
    else
    {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_update(lua_State *L)
{
    lua_lock(L);
    if (http_getpost(L, HTTP_UPDATE) == LUA_YIELD)
    {
        lua_unlock(L);
        return lua_yield(L, 0);
    }
    else
    {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_delete(lua_State *L)
{
    lua_lock(L);
    if (http_getpost(L, HTTP_DELETE) == LUA_YIELD)
    {
        lua_unlock(L);
        return lua_yield(L, 0);
    }
    else
    {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_head(lua_State *L)
{
    lua_lock(L);
    if (http_getpost(L, HTTP_HEAD) == LUA_YIELD)
    {
        lua_unlock(L);
        return lua_yield(L, 0);
    }
    else
    {
        lua_unlock(L);
        return 0;
    }
}

LUA_API int http_cookiejar(lua_State *L)
{
    luaL_checkstring(L, 1);
    lua_settop(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, KEY_COOKIE_JAR);
    return 0;
}

LUA_API int http_cainfo(lua_State *L)
{
    luaL_checkstring(L, 1);
    lua_settop(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, KEY_CAINFO);
    return 0;
}

LUA_API int http_capath(lua_State *L)
{
    luaL_checkstring(L, 1);
    lua_settop(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, KEY_CAPATH);
    return 0;
}

LUA_API int http_escape(lua_State *L)
{
    size_t size = 0;
    const char *str = luaL_checklstring(L, 1, &size);
    char *escaped = curl_escape(str, (int)size);
    if (escaped)
    {
        lua_pushstring(L, escaped);
        free(escaped);
    }
    else
    {
        lua_pushnil(L);
    }
    return 1;
}

LUA_API int http_unescape(lua_State *L)
{
    size_t size = 0;
    const char *s = luaL_checklstring(L, 1, &size);
    char *unescaped = curl_unescape(s, (int)size);
    if (unescaped)
    {
        lua_pushstring(L, unescaped);
        free(unescaped);
    }
    else
    {
        lua_pushnil(L);
    }
    return 1;
}

static const luaL_Reg httplib[] = {{"get", http_get},
                                   {"post", http_post},
                                   {"put", http_put},
                                   {"head", http_head},
                                   {"update", http_update},
                                   {"delete", http_delete},
                                   {"cookiejar", http_cookiejar},
                                   {"cainfo", http_cainfo},
                                   {"capath", http_capath},
                                   {"escape", http_escape},
                                   {"unescape", http_unescape},
                                   {NULL, NULL}};

#if TARGET_OS_IPHONE || defined(ANDROID) || defined(__ANDROID__)

static pthread_mutex_t share_lock;

void lock_function(CURL *handle, curl_lock_data data, curl_lock_access access,
                   void *userptr)
{
    pthread_mutex_lock(&share_lock);
}

void unlock_function(CURL *handle, curl_lock_data data, void *userptr)
{
    pthread_mutex_unlock(&share_lock);
}
#endif

#if TARGET_OS_IPHONE
#include <arpa/inet.h>
#include <resolv.h>
#include <sys/socket.h>
#include <sys/types.h>

#define BUFF_LEN 1024

static struct event *reset_timer_event;

static void reset_dns_servers_timercb(int fd, short kind, void *userp)
{
    char buf[BUFF_LEN] = {0};
    int offset = 0;

    if (dns_servers)
    {
        free(dns_servers);
        dns_servers = NULL;
    }

    evdns_base_clear_nameservers_and_suspend(event_mgr_dnsbase());

    struct evdns_base *dns_base = event_mgr_dnsbase();

    struct __res_state res = {0};
    int result = res_ninit(&res);

    if (result == 0)
    {
        union res_9_sockaddr_union *addr_union =
            malloc(res.nscount * sizeof(union res_9_sockaddr_union));
        res_getservers(&res, addr_union, res.nscount);

        for (int i = 0; i < res.nscount; i++)
        {
            if (addr_union[i].sin.sin_family == AF_INET)
            {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(addr_union[i].sin.sin_addr), ip, INET_ADDRSTRLEN);

                evdns_base_nameserver_ip_add(dns_base, ip);

                memcpy(buf + offset, ip, strlen(ip));
                offset += strlen(ip);
                memcpy(buf + offset, ",", 1);
                offset += 1;

                if (offset > BUFF_LEN - 21)
                {
                    break;
                }
            }
            else if (addr_union[i].sin6.sin6_family == AF_INET6)
            {
                char ip[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET6, &(addr_union[i].sin6.sin6_addr), ip,
                          INET6_ADDRSTRLEN);
                evdns_base_nameserver_ip_add(dns_base, ip);

                memcpy(buf + offset, ip, strlen(ip));
                offset += strlen(ip);
                memcpy(buf + offset, ",", 1);
                offset += 1;

                if (offset > BUFF_LEN - 21)
                {
                    break;
                }
            }
            else
            {
                //                printf("Undefined family.\n");
            }
        }
        free(addr_union);
    }
    res_ndestroy(&res);

    evdns_base_resume(event_mgr_dnsbase());

    dns_servers = strdup(buf);

    event_free(reset_timer_event);
    reset_timer_event = NULL;
}

extern void reset_dns_servers()
{
    if (reset_timer_event)
    {
        return;
    }

    struct timeval tv = {0, 1};
    reset_timer_event =
        evtimer_new(event_mgr_base(), reset_dns_servers_timercb, NULL);
    event_add(reset_timer_event, &tv);
}

#endif

LUA_API int luaopen_fan_http_core(lua_State *L)
{
    curl_global_init(CURL_GLOBAL_ALL);

#if TARGET_OS_IPHONE || defined(ANDROID) || defined(__ANDROID__)
    if (!share_handle)
    {
        share_handle = curl_share_init();
        curl_share_setopt(share_handle, CURLSHOPT_LOCKFUNC, lock_function);
        curl_share_setopt(share_handle, CURLSHOPT_UNLOCKFUNC, unlock_function);
        curl_share_setopt(share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);

        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&share_lock, &a);
    }
#endif
    if (!timer_event)
    {
        timer_event = evtimer_new(event_mgr_base(), timer_cb, NULL);
    }

    if (!timer_check_multi_info)
    {
        timer_check_multi_info =
            evtimer_new(event_mgr_base(), timer_check_multi_info_cb, NULL);
    }

    lua_newtable(L);
    luaL_register(L, "http", httplib);

    lua_pushstring(L, curl_version());
    lua_setfield(L, -2, "curl_version");

    return 1;
}
