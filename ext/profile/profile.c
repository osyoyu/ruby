#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "ractor_core.h"
#include "ruby.h"
#include "ruby/internal/intern/thread.h"
#include "ruby/internal/intern/vm.h"
#include "ruby/internal/value.h"
#include "ruby/st.h"
#include "vm_core.h"
#include "vm_sync.h"

#define PROF_DEBUG_LOG(...) printf(__VA_ARGS__)

// TODO: Make this configurable via Ruby API
#define PROFILE_PERIOD_NS (10L * 1000 * 1000) /* 10ms */

/* Ruby constants and IDs */

VALUE rb_cRubyVM;
VALUE rb_mProfile;

static bool profile_enabled = false;
static volatile sig_atomic_t profile_sigprof_count = 0;

// Table to track per-native-thread timers.
static st_table *profile_thread_timers = NULL;
typedef struct profile_timer_entry {
    pid_t tid;
    timer_t timer;
} profile_timer_entry_t;

static void
profile_sigprof_handler(int sig, siginfo_t *info, void *ucontext)
{
    assert(sig == SIGPROF);
    profile_sigprof_count++;
}

/* Install processwide SIGPROF handler. */
static void
profile_install_sigprof_handler(void)
{
    struct sigaction sa = {0};
    sa.sa_sigaction = profile_sigprof_handler;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGPROF); // Mask SIGPROFs when handler is running
    sa.sa_flags = SA_SIGINFO | SA_RESTART;

    if (sigaction(SIGPROF, &sa, NULL) != 0) {
        rb_bug("sigaction(SIGPROF) failed");
    }
}

/* Timer and signal lifecycle */

// Install and arm a per-thread CPU timer that delivers SIGPROF.
static int
configure_kernel_timer(pid_t tid, timer_t *out)
{
    clockid_t cpu_clock;
    if (clock_getcpuclockid(tid, &cpu_clock) != 0) {
        return -1;
    }

    struct sigevent sev = {0};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev._sigev_un._tid = tid; // Send signal to tid (Linux-only)
    if (timer_create(cpu_clock, &sev, out) != 0) {
        return -1;
    }

    struct itimerspec its = {0};
    its.it_interval.tv_sec = PROFILE_PERIOD_NS / 1000000000L;
    its.it_interval.tv_nsec = PROFILE_PERIOD_NS % 1000000000L;
    its.it_value = its.it_interval;
    if (timer_settime(*out, 0, &its, NULL) != 0) {
        timer_delete(*out);
        return -1;
    }

    return 0;
}

static void
install_native_thread_timer(struct rb_native_thread *nt)
{
    if (nt == NULL) {
        rb_bug("unreachable");
        return;
    }

    pid_t tid = nt->tid;
    if (tid <= 0) {
        return;
    }

    if (profile_thread_timers == NULL) {
        profile_thread_timers = st_init_numtable();
    }

    st_data_t value;
    if (st_lookup(profile_thread_timers, (st_data_t)nt, &value)) {
        return;
    }

    timer_t timer = 0;
    if (configure_kernel_timer(tid, &timer) != 0) {
        return;
    }

    // Bookkeeping
    profile_timer_entry_t *entry = malloc(sizeof(profile_timer_entry_t));
    entry->timer = timer;
    entry->tid = tid;
    st_insert(profile_thread_timers, (st_data_t)nt, (st_data_t)entry);
}

static void
install_timers_to_living_threads(void)
{
    rb_vm_t *vm = rb_vm_get_current();
    rb_ractor_t *r;
    rb_thread_t *th;

    // Iterate over all Ractors and their threads
    ccan_list_for_each(&vm->ractor.set, r, vmlr_node) {
        ccan_list_for_each(&r->threads.set, th, lt_node) {
            install_native_thread_timer(th->nt);
        }
    }
}

static void
destroy_timer_entry(profile_timer_entry_t *entry)
{
    if (entry == NULL) return;
    // timer_delete(entry->timer);
    free(entry);
}

static void
remove_native_thread_timer(struct rb_native_thread *nt)
{
    if (profile_thread_timers == NULL || nt == NULL) { return; }

    st_data_t key = (st_data_t)nt;
    st_data_t value;
    if (!st_delete(profile_thread_timers, &key, &value)) { return; }

    destroy_timer_entry((profile_timer_entry_t *)value);
}

static int
remove_all_thread_timers_i(st_data_t key, st_data_t value, st_data_t data)
{
    destroy_timer_entry((profile_timer_entry_t *)value);
    return ST_DELETE;
}

static void
remove_all_thread_timers(void)
{
    if (profile_thread_timers == NULL) { return; }
    st_foreach(profile_thread_timers, remove_all_thread_timers_i, 0);
}

// VM profile event callback
static void
profile_event_hook(rb_profile_event_type_t event_type, const void *event_data, void *user_data)
{
    if (!profile_enabled) { return; }

    const rb_profile_event_native_thread_create_data_t *create_data;
    const rb_profile_event_native_thread_destroy_data_t *destroy_data;

    switch (event_type) {
        case RB_PROFILE_EVENT_TYPE_NATIVE_THREAD_CREATED: // A native thread has been created.
            create_data = (const rb_profile_event_native_thread_create_data_t *)event_data;
            PROF_DEBUG_LOG("NATIVE_THREAD_CREATED nt=%p\n", create_data->native_thread);
            install_native_thread_timer((struct rb_native_thread *)create_data->native_thread);
            break;

        case RB_PROFILE_EVENT_TYPE_NATIVE_THREAD_DESTROYED: // A native thread has been destroyed.
            destroy_data = (const rb_profile_event_native_thread_destroy_data_t *)event_data;
            PROF_DEBUG_LOG("NATIVE_THREAD_DESTROYED nt=%p\n", destroy_data->native_thread);
            remove_native_thread_timer((struct rb_native_thread *)destroy_data->native_thread);
            break;

        default:
            break;
    }
}

VALUE
prof_enable_i(void *_)
{
    // Install processwide SIGPROF handler
    profile_install_sigprof_handler();

    // Install timers to all threads alive
    install_timers_to_living_threads();

    // Register profile event hook to track native thread lifecycle
    if (!rb_profile_event_hook_set(profile_event_hook, NULL)) {
        return Qfalse;
    }

    return Qtrue;
}

VALUE
prof_disable_i(void *_)
{
    // Unregister profile event hook
    rb_profile_event_hook_clear();

    // Remove all thread timers
    remove_all_thread_timers();

    return Qtrue;
}

/* Ruby API */

/**
 * Enable the profiler.
 */
static VALUE
rb_prof_enable(VALUE self)
{
    if (profile_enabled) return Qfalse;
    profile_sigprof_count = 0;
    rb_vm_lock_with_barrier(prof_enable_i, NULL);
    profile_enabled = true;

    return Qtrue;
}

/**
 * Disable the profiler.
 */
static VALUE
rb_prof_disable(VALUE self)
{
    if (!profile_enabled) { rb_raise(rb_eRuntimeError, "Profiler is not enabled"); }
    profile_enabled = false;
    rb_vm_lock_with_barrier(prof_disable_i, NULL);

    PROF_DEBUG_LOG("SIGPROF count=%d\n", (int)profile_sigprof_count);

    return Qtrue;
}

RUBY_FUNC_EXPORTED void
Init_profile(void)
{
    rb_ext_ractor_safe(true);
    rb_mProfile = rb_define_module("Profile");
    rb_cRubyVM = rb_const_get(rb_cObject, rb_intern("RubyVM"));
    rb_define_module_function(rb_mProfile, "enable", rb_prof_enable, 0);
    rb_define_module_function(rb_mProfile, "disable", rb_prof_disable, 0);

    return;
}
