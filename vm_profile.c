#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ptrace.h>

#include "internal.h"
#include "internal/thread.h"
#include "internal/vm.h"
#include "ruby/debug.h"
#include "ruby/thread.h"
#include "vm_core.h"

#define sigev_notify_thread_id _sigev_un._tid

struct signal_handler_data {
    rb_thread_t *target_thread;
};

struct sample {
    /* Meta */
    rb_thread_t *thread;

    /* Stack frames */
    int captured_frames;
    VALUE iseqs[200];
    int lines[200];
};

/* Globals */

VALUE rb_mRuby;
VALUE rb_mProfiler;

#define MAX_SAMPLES 10000
static struct sample buffer[MAX_SAMPLES];
static int buffer_index = 0;
static timer_t installed_timers[100];
static int installed_timers_count = 0;

/* async-signal-safe functions only */
static void
signal_handler(int sig, siginfo_t *si, void *ucontext)
{
    struct signal_handler_data *data = (struct signal_handler_data *)(si->si_value.sival_ptr);

    // Know the current thread
    rb_execution_context_t *ec = data->target_thread->ec;
    if (ec == NULL) {
        return;
    }
    assert(ec->thread_ptr->nt->thread_id == pthread_self());
    if (ec->thread_ptr->nt->thread_id != pthread_self()) {
        return;
    }

    // Prepare a sample slot
    if (buffer_index >= MAX_SAMPLES) {
        return;
    }
    struct sample *sample = &buffer[buffer_index++];
    sample = memset(sample, 0, sizeof(struct sample));

    // Set meta fields
    sample->thread = data->target_thread;

    // Grab backtrace for the target thread
    int captured_frames;
    if (ec->thread_ptr->status != THREAD_RUNNABLE) {
        return;
    }
    captured_frames = thread_profile_frames(ec, 0, 200, sample->iseqs, sample->lines);
    sample->captured_frames = captured_frames;

    return;
}

static void
install_signal_handler(void)
{
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) == -1) {
        rb_raise(rb_eRuntimeError, "Failed to set signal handler: %s", strerror(errno));
    }
}

static void
uninstall_signal_handler(void)
{
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGPROF, &sa, NULL) == -1) {
        rb_raise(rb_eRuntimeError, "Failed to reset signal handler: %s", strerror(errno));
    }
}

static void
install_timer_to_thread(VALUE thval)
{
    rb_thread_t *th = rb_thread_ptr(thval);
    rb_nativethread_id_t thread_id = th->nt->thread_id;
    assert(pthread_self() == thread_id);

    if (installed_timers_count >= 100) {
        rb_raise(rb_eRuntimeError, "Too many timers installed");
    }
    timer_t timer = installed_timers[installed_timers_count++];

    // Get CPU clock ID for the native thread
    clockid_t cpu_timer_clock_id;
    if (pthread_getcpuclockid(thread_id, &cpu_timer_clock_id) != 0) {
        rb_raise(rb_eRuntimeError, "Failed to get CPU clock ID");
    }

    // Data to be passed to the signal handler
    struct signal_handler_data *data = xmalloc(sizeof(struct signal_handler_data));
    data->target_thread = th;

    // Install kernel timer
    struct sigevent sev;
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev.sigev_notify_thread_id = gettid(); // Linux-only
    sev.sigev_value.sival_ptr = (void *)data;

    int res = timer_create(cpu_timer_clock_id, &sev, &timer);
    if (res == -1) {
        rb_raise(rb_eRuntimeError, "Failed to create timer: %s", strerror(errno));
    }

    // Arm the timer to fire every 10 ms
    // TODO: Make the interval configurable (in Hz?)
    struct itimerspec its = {
        .it_value = {
            .tv_sec = 0,
            .tv_nsec = 10 * 1000000, // per 10 ms
        },
        .it_interval = {
            .tv_sec = 0,
            .tv_nsec = 10 * 1000000, // per 10 ms
        },
    };
    if (timer_settime(timer, 0, &its, NULL) == -1) {
        rb_raise(rb_eRuntimeError, "Failed to configure timer: %s", strerror(errno));
    }
}

static void
disarm_timer(timer_t timer)
{
    struct itimerspec its = {
        .it_value = {
            .tv_sec = 0,
            .tv_nsec = 0,
        },
        .it_interval = {
            .tv_sec = 0,
            .tv_nsec = 0,
        },
    };
    if (timer_settime(timer, 0, &its, NULL) == -1) {
        rb_raise(rb_eRuntimeError, "Failed to disarm timer: %s", strerror(errno));
    }
}

static void
disarm_all_timers(void)
{
    for (int i = 0; i < installed_timers_count; i++) {
        disarm_timer(installed_timers[i]);
    }
    installed_timers_count = 0;
}

void
thread_callback(rb_event_flag_t flag, const rb_internal_thread_event_data_t *data, void *custom_data)
{
    if (flag == RUBY_INTERNAL_THREAD_EVENT_STARTED) {
        install_timer_to_thread(data->thread);
    }
}

/**
 * Enable the profiler.
 */
VALUE
rb_profiler_enable(VALUE self)
{
    // Install signal handler
    install_signal_handler();

    // Install timer on all threads on the current Ractor
    VALUE rb_cThread = rb_const_get(rb_cObject, rb_intern("Thread"));
    VALUE threads = rb_funcall(rb_cThread, rb_intern("list"), 0);
    for (int i = 0; i < RARRAY_LEN(threads); i++) {
        VALUE thread = rb_ary_entry(threads, i);
        install_timer_to_thread(thread);
    }

    // Register a callback to install timer on newly created threads
    rb_internal_thread_add_event_hook(&thread_callback, RUBY_INTERNAL_THREAD_EVENT_STARTED, NULL);

    return Qtrue;
}

/**
 * Disable the profiler and return collected data.
 */
VALUE
rb_profiler_disable(VALUE self)
{
    // Cleanup
    disarm_all_timers();
    uninstall_signal_handler();

    // Construct a Ruby::Profile::ProfileBuilder object
    VALUE rb_cProfileBB = rb_const_get(rb_mProfiler, rb_intern("ProfileBuilderBuilder"));
    VALUE builder = rb_funcall(rb_cProfileBB, rb_intern("new"), 0);

    for (int i = 0; i < buffer_index; i++) {
        VALUE stack = rb_ary_new();
        struct sample *sample = &buffer[i];
        for (int j = 0; j < sample->captured_frames; j++) {
            VALUE frame = rb_hash_new();
            VALUE iseq = sample->iseqs[j];

            VALUE name = rb_profile_frame_full_label(iseq);
            rb_hash_aset(frame, ID2SYM(rb_intern("name")), name);

            VALUE file = rb_profile_frame_path(iseq);
            rb_hash_aset(frame, ID2SYM(rb_intern("file")), file);

            int line = sample->lines[j];
            rb_hash_aset(frame, ID2SYM(rb_intern("line")), INT2NUM(line));

            rb_hash_aset(frame, ID2SYM(rb_intern("address")), ULONG2NUM(iseq));
            rb_ary_push(stack, frame);
        }
        rb_funcall(builder, rb_intern("add_sample"), 1, stack);
    }

    return rb_funcall(builder, rb_intern("to_profile"), 0);
}

/* called from Init_vm() in vm.c */
void
Init_vm_profile(void)
{
    rb_mRuby = rb_const_get(rb_cObject, rb_intern("RubyVM"));
    rb_mProfiler = rb_define_module_under(rb_mRuby, "Profiler");
    rb_define_class_under(rb_mProfiler, "ProfileBuilderBuilder", rb_cObject);
    rb_define_module_function(rb_mProfiler, "enable", rb_profiler_enable, 0);
    rb_define_module_function(rb_mProfiler, "disable", rb_profiler_disable, 0);
}

#include "vm_profile.rbinc"
