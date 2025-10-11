#include "utlua.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#if DEBUG_THREAD_TRACKING

// Global reference counters
int g_thread_ref_count = 0;
int g_function_ref_count = 0;

// Whether initialized
static int tracker_initialized = 0;

// Maximum tracking records
#define MAX_THREAD_RECORDS 1000
#define MAX_FUNCTION_RECORDS 2000

// Reference types
typedef enum {
    REF_TYPE_THREAD = 0,
    REF_TYPE_FUNCTION,
    REF_TYPE_OTHER
} ref_type_t;

// Reference record structure
typedef struct {
    int ref;
    ref_type_t type;
    char location[256];
    time_t created_time;
    int active;
} ref_record_t;

static ref_record_t thread_records[MAX_THREAD_RECORDS];
static ref_record_t function_records[MAX_FUNCTION_RECORDS];
static int next_thread_index = 0;
static int next_function_index = 0;

// Initialize reference tracker
void thread_tracker_init(void) {
    if (tracker_initialized) return;

    g_thread_ref_count = 0;
    g_function_ref_count = 0;
    memset(thread_records, 0, sizeof(thread_records));
    memset(function_records, 0, sizeof(function_records));
    next_thread_index = 0;
    next_function_index = 0;
    tracker_initialized = 1;

    printf("[REF_TRACKER] Initialized - tracking threads and functions\n");
}

// Find thread record
static ref_record_t* find_thread_record(int ref) {
    for (int i = 0; i < MAX_THREAD_RECORDS; i++) {
        if (thread_records[i].active && thread_records[i].ref == ref) {
            return &thread_records[i];
        }
    }
    return NULL;
}

// Find function record
static ref_record_t* find_function_record(int ref) {
    for (int i = 0; i < MAX_FUNCTION_RECORDS; i++) {
        if (function_records[i].active && function_records[i].ref == ref) {
            return &function_records[i];
        }
    }
    return NULL;
}

// Get next available thread record slot
static ref_record_t* get_next_thread_record() {
    // First try to find a free slot
    for (int i = 0; i < MAX_THREAD_RECORDS; i++) {
        if (!thread_records[i].active) {
            return &thread_records[i];
        }
    }

    // If no free slot, use circular overwrite
    ref_record_t* record = &thread_records[next_thread_index];
    next_thread_index = (next_thread_index + 1) % MAX_THREAD_RECORDS;
    return record;
}

// Get next available function record slot
static ref_record_t* get_next_function_record() {
    // First try to find a free slot
    for (int i = 0; i < MAX_FUNCTION_RECORDS; i++) {
        if (!function_records[i].active) {
            return &function_records[i];
        }
    }

    // If no free slot, use circular overwrite
    ref_record_t* record = &function_records[next_function_index];
    next_function_index = (next_function_index + 1) % MAX_FUNCTION_RECORDS;
    return record;
}

// Log thread creation
void thread_tracker_log_create(const char* location, int ref) {
    if (!tracker_initialized) {
        thread_tracker_init();
    }

    g_thread_ref_count++;

    // Check if already exists (prevent duplicates)
    ref_record_t* existing = find_thread_record(ref);
    if (existing) {
        printf("[REF_TRACKER] WARNING: Thread ref %d already exists at %s, overwriting\n",
               ref, existing->location);
    }

    // Get record slot
    ref_record_t* record = existing ? existing : get_next_thread_record();

    record->ref = ref;
    record->type = REF_TYPE_THREAD;
    record->created_time = time(NULL);
    record->active = 1;
    strncpy(record->location, location ? location : "unknown", sizeof(record->location) - 1);
    record->location[sizeof(record->location) - 1] = '\0';

    printf("[REF_TRACKER] CREATE THREAD ref=%d at %s (total=%d)\n",
           ref, location ? location : "unknown", g_thread_ref_count);
}

// Log function reference creation
void function_tracker_log_create(const char* location, int ref) {
    if (!tracker_initialized) {
        thread_tracker_init();
    }

    g_function_ref_count++;

    // Check if already exists (prevent duplicates)
    ref_record_t* existing = find_function_record(ref);
    if (existing) {
        printf("[REF_TRACKER] WARNING: Function ref %d already exists at %s, overwriting\n",
               ref, existing->location);
    }

    // Get record slot
    ref_record_t* record = existing ? existing : get_next_function_record();

    record->ref = ref;
    record->type = REF_TYPE_FUNCTION;
    record->created_time = time(NULL);
    record->active = 1;
    strncpy(record->location, location ? location : "unknown", sizeof(record->location) - 1);
    record->location[sizeof(record->location) - 1] = '\0';

    printf("[REF_TRACKER] CREATE FUNCTION ref=%d at %s (total=%d)\n",
           ref, location ? location : "unknown", g_function_ref_count);
}

// Log thread destruction
void thread_tracker_log_destroy(const char* location, int ref) {
    if (!tracker_initialized) {
        printf("[REF_TRACKER] WARNING: Destroy called before init, ref=%d\n", ref);
        return;
    }

    ref_record_t* record = find_thread_record(ref);
    if (record) {
        record->active = 0;
        g_thread_ref_count--;

        printf("[REF_TRACKER] DESTROY THREAD ref=%d at %s (was created at %s, total=%d)\n",
               ref, location ? location : "unknown", record->location, g_thread_ref_count);
    } else {
        // Likely a reference created before tracking was enabled, handle silently without affecting counters
        printf("[REF_TRACKER] INFO: Destroy untracked thread ref=%d at %s (likely created before tracking enabled)\n",
               ref, location ? location : "unknown");
        // Don't change counters since we never counted this reference
    }
}

// Log function reference destruction
void function_tracker_log_destroy(const char* location, int ref) {
    if (!tracker_initialized) {
        printf("[REF_TRACKER] WARNING: Destroy called before init, ref=%d\n", ref);
        return;
    }

    ref_record_t* record = find_function_record(ref);
    if (record) {
        record->active = 0;
        g_function_ref_count--;

        printf("[REF_TRACKER] DESTROY FUNCTION ref=%d at %s (was created at %s, total=%d)\n",
               ref, location ? location : "unknown", record->location, g_function_ref_count);
    } else {
        // Likely a reference created before tracking was enabled, handle silently without affecting counters
        printf("[REF_TRACKER] INFO: Destroy untracked function ref=%d at %s (likely created before tracking enabled)\n",
               ref, location ? location : "unknown");
        // Don't change counters since we never counted this reference
    }
}

// Generic reference destruction - try to remove from both trackers
void ref_tracker_log_destroy(const char* location, int ref) {
    if (!tracker_initialized) {
        printf("[REF_TRACKER] WARNING: Destroy called before init, ref=%d\n", ref);
        return;
    }

    // First try to remove from thread tracker
    ref_record_t* thread_record = find_thread_record(ref);
    if (thread_record) {
        thread_record->active = 0;
        g_thread_ref_count--;
        printf("[REF_TRACKER] DESTROY THREAD ref=%d at %s (was created at %s, total=%d)\n",
               ref, location ? location : "unknown", thread_record->location, g_thread_ref_count);
        return;
    }

    // If not a thread reference, try to remove from function tracker
    ref_record_t* function_record = find_function_record(ref);
    if (function_record) {
        function_record->active = 0;
        g_function_ref_count--;
        printf("[REF_TRACKER] DESTROY FUNCTION ref=%d at %s (was created at %s, total=%d)\n",
               ref, location ? location : "unknown", function_record->location, g_function_ref_count);
        return;
    }

    // If not found in either tracker, likely a reference created before tracking was enabled
    printf("[REF_TRACKER] INFO: Destroy untracked ref=%d at %s (likely created before tracking enabled)\n",
           ref, location ? location : "unknown");
}

// Get current thread reference count
int thread_tracker_get_count(void) {
    return g_thread_ref_count;
}

// Get current function reference count
int function_tracker_get_count(void) {
    return g_function_ref_count;
}

// Print all active references
void thread_tracker_print_active(void) {
    if (!tracker_initialized) {
        printf("[REF_TRACKER] Not initialized\n");
        return;
    }

    printf("[REF_TRACKER] === Active References Summary ===\n");
    printf("[REF_TRACKER] Thread references: %d\n", g_thread_ref_count);
    printf("[REF_TRACKER] Function references: %d\n", g_function_ref_count);
    printf("[REF_TRACKER] Total references: %d\n", g_thread_ref_count + g_function_ref_count);

    time_t now = time(NULL);

    // Print active thread references
    if (g_thread_ref_count > 0) {
        printf("[REF_TRACKER] --- Active Thread References ---\n");
        int thread_count = 0;
        for (int i = 0; i < MAX_THREAD_RECORDS; i++) {
            if (thread_records[i].active) {
                thread_count++;
                double age = difftime(now, thread_records[i].created_time);
                printf("[REF_TRACKER] THREAD #%d: ref=%d, age=%.0fs, location=%s\n",
                       thread_count, thread_records[i].ref, age, thread_records[i].location);
            }
        }

        if (thread_count != g_thread_ref_count) {
            printf("[REF_TRACKER] WARNING: Thread count mismatch! counted=%d, tracked=%d\n",
                   thread_count, g_thread_ref_count);
        }
    }

    // Print active function references
    if (g_function_ref_count > 0) {
        printf("[REF_TRACKER] --- Active Function References ---\n");
        int function_count = 0;
        for (int i = 0; i < MAX_FUNCTION_RECORDS; i++) {
            if (function_records[i].active) {
                function_count++;
                double age = difftime(now, function_records[i].created_time);
                printf("[REF_TRACKER] FUNCTION #%d: ref=%d, age=%.0fs, location=%s\n",
                       function_count, function_records[i].ref, age, function_records[i].location);
            }
        }

        if (function_count != g_function_ref_count) {
            printf("[REF_TRACKER] WARNING: Function count mismatch! counted=%d, tracked=%d\n",
                   function_count, g_function_ref_count);
        }
    }

    printf("[REF_TRACKER] === End of Active References ===\n");
}

// Periodic monitoring function
void thread_tracker_periodic_check(void) {
    if (!tracker_initialized) {
        return;
    }

    int thread_count = thread_tracker_get_count();
    int function_count = function_tracker_get_count();
    int total_count = thread_count + function_count;

    // If there are uncollected references, print details
    if (total_count > 0) {
        printf("[REF_TRACKER] === Periodic Check ===\n");
        printf("[REF_TRACKER] Thread refs: %d, Function refs: %d, Total: %d\n",
              thread_count, function_count, total_count);

        time_t now = time(NULL);
        int long_lived_threads = 0;
        int long_lived_functions = 0;

        // Check thread references
        if (thread_count > 0) {
            printf("[REF_TRACKER] --- Thread References ---\n");
            for (int i = 0; i < MAX_THREAD_RECORDS; i++) {
                if (thread_records[i].active) {
                    double age = difftime(now, thread_records[i].created_time);

                    // Mark references living longer than 30 seconds as potentially leaked
                    if (age > 30.0) {
                        long_lived_threads++;
                        printf("[REF_TRACKER] ⚠️  Long-lived THREAD ref=%d, age=%.0fs, location=%s\n",
                              thread_records[i].ref, age, thread_records[i].location);
                    } else {
                        printf("[REF_TRACKER] Active THREAD ref=%d, age=%.0fs, location=%s\n",
                              thread_records[i].ref, age, thread_records[i].location);
                    }
                }
            }
        }

        // Check function references
        if (function_count > 0) {
            printf("[REF_TRACKER] --- Function References ---\n");
            for (int i = 0; i < MAX_FUNCTION_RECORDS; i++) {
                if (function_records[i].active) {
                    double age = difftime(now, function_records[i].created_time);

                    // Mark references living longer than 30 seconds as potentially leaked
                    if (age > 30.0) {
                        long_lived_functions++;
                        printf("[REF_TRACKER] ⚠️  Long-lived FUNCTION ref=%d, age=%.0fs, location=%s\n",
                              function_records[i].ref, age, function_records[i].location);
                    } else {
                        printf("[REF_TRACKER] Active FUNCTION ref=%d, age=%.0fs, location=%s\n",
                              function_records[i].ref, age, function_records[i].location);
                    }
                }
            }
        }

        int total_long_lived = long_lived_threads + long_lived_functions;
        if (total_long_lived > 0) {
            printf("[REF_TRACKER] ⚠️  Found %d potentially leaked references! (Threads: %d, Functions: %d)\n",
                  total_long_lived, long_lived_threads, long_lived_functions);
        }

        printf("[REF_TRACKER] === End Periodic Check ===\n");
    } else {
        printf("[REF_TRACKER] ✅ Periodic Check: No uncollected references\n");
    }
}

// Functions added to Lua interface
static int lua_thread_tracker_get_count(lua_State *L) {
    lua_pushinteger(L, thread_tracker_get_count());
    return 1;
}

static int lua_function_tracker_get_count(lua_State *L) {
    lua_pushinteger(L, function_tracker_get_count());
    return 1;
}

static int lua_ref_tracker_get_total_count(lua_State *L) {
    lua_pushinteger(L, thread_tracker_get_count() + function_tracker_get_count());
    return 1;
}

static int lua_thread_tracker_periodic_check(lua_State *L) {
    thread_tracker_periodic_check();
    return 0;
}

static int lua_thread_tracker_print_active(lua_State *L) {
    thread_tracker_print_active();
    return 0;
}

// Register Lua functions
void thread_tracker_register_lua_functions(lua_State *L) {
    // Thread reference related
    lua_pushcfunction(L, lua_thread_tracker_get_count);
    lua_setglobal(L, "get_thread_ref_count");

    // Function reference related
    lua_pushcfunction(L, lua_function_tracker_get_count);
    lua_setglobal(L, "get_function_ref_count");

    // Total reference count
    lua_pushcfunction(L, lua_ref_tracker_get_total_count);
    lua_setglobal(L, "get_total_ref_count");

    // Monitoring functions
    lua_pushcfunction(L, lua_thread_tracker_print_active);
    lua_setglobal(L, "print_active_refs");

    lua_pushcfunction(L, lua_thread_tracker_periodic_check);
    lua_setglobal(L, "ref_tracker_periodic_check");

    // Keep backward compatible old function names
    lua_pushcfunction(L, lua_thread_tracker_print_active);
    lua_setglobal(L, "print_active_thread_refs");

    lua_pushcfunction(L, lua_thread_tracker_periodic_check);
    lua_setglobal(L, "thread_tracker_periodic_check");
}

#endif // DEBUG_THREAD_TRACKING
