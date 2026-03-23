#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "extconf.h"

#include "internal.h"
#include "internal/thread.h"
#include "internal/vm.h"
#include "ruby.h"
#include "ruby/ruby.h"
#include "ruby/debug.h"
#include "ruby/thread.h"
#include "vm_core.h"

#if defined(HAVE_TIMER_CREATE) && defined(HAVE_CONST_CLOCK_THREAD_CPUTIME_ID)
#  define HAS_PER_THREAD_TIMER
#  define sigev_notify_thread_id _sigev_un._tid
#  define gettid() (syscall(SYS_gettid))
#endif

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

#define PROF_RINGBUFFER_MAX_SIZE 1024

struct prof_ringbuffer {
    int size;
    atomic_int head;
    atomic_int tail;
    struct sample *samples;
};

struct prof_session {
    struct prof_ringbuffer *ringbuf;

    struct sample *collected_samples;
    int collected_samples_capa;
    int collected_samples_count;

    bool is_running;

    pthread_t sample_collector_thread_handle;

#ifdef HAS_PER_THREAD_TIMER
    timer_t installed_timers[100]; // TODO: Make expandable
    int installed_timers_count;
#endif
};

static struct prof_ringbuffer * prof_ringbuffer_new(int size);
static void prof_ringbuffer_free(struct prof_ringbuffer *ringbuf);
// async-signal-safe
static bool prof_ringbuffer_push(struct prof_ringbuffer *ringbuf, struct sample *sample);
static bool prof_ringbuffer_pop(struct prof_ringbuffer *ringbuf, struct sample *out);

/* Globals */

VALUE rb_cRubyVM;
VALUE rb_mProfile;
static struct prof_session *sess;

/* Ringbuf impl */

static struct prof_ringbuffer *
prof_ringbuffer_new(int size) {
    if (size <= 0) { return NULL; }

    struct prof_ringbuffer *ringbuf = xmalloc(sizeof(struct prof_ringbuffer));
    if (!ringbuf) { goto err; }
    ringbuf->size = size + 1; // One extra slot is required to distinguish full from empty
    ringbuf->head = 0;
    ringbuf->tail = 0;
    // Use xmalloc so that deallocation via xfree in prof_ringbuffer_free is consistent
    ringbuf->samples = xmalloc(ringbuf->size * sizeof(struct sample));
    if (!ringbuf->samples) { goto err_free_ringbuf; }
    return ringbuf;

err_free_ringbuf:
    free(ringbuf);
err:
    return NULL;
}

static void
prof_ringbuffer_free(struct prof_ringbuffer *ringbuf) {
    xfree(ringbuf->samples);
    xfree(ringbuf);
}

// Returns 0 on success, 1 on failure (buffer full).
static bool
prof_ringbuffer_push(struct prof_ringbuffer *ringbuf, struct sample *sample) {
    // Tail is only modified by the producer thread (us), so relaxed ordering is sufficient
    const int current_tail = atomic_load_explicit(&ringbuf->tail, memory_order_relaxed);
    const int next_tail = (current_tail + 1) % ringbuf->size;

    // Check head to see if buffer is full. If next_tail == head, the buffer is full.
    // Use acquire ordering to synchronize with the head update in prof_ringbuffer_pop().
    // This ensures we see the latest head value.
    if (next_tail == atomic_load_explicit(&ringbuf->head, memory_order_acquire)) {
        return false;  // Buffer full
    }

    // Copy the sample from the provided input pointer to the buffer.
    ringbuf->samples[current_tail] = *sample;

    // Use release ordering when updating tail to ensure the sample write is visible
    // to the consumer before they see the new tail value
    atomic_store_explicit(&ringbuf->tail, next_tail, memory_order_release);
    return true;
}

// Returns 0 on success, 1 on failure (buffer empty).
static bool
prof_ringbuffer_pop(struct prof_ringbuffer *ringbuf, struct sample *out) {
    // Head won't be modifed by the producer thread. It is safe to use relaxed ordering.
    const int current_head = atomic_load_explicit(&ringbuf->head, memory_order_relaxed);

    // Check tail to see if buffer is empty. If head == tail, the buffer is empty.
    // Use acquire ordering to synchronize with the tail update in prof_ringbuffer_push().
    // This ensures we see the latest tail value.
    if (current_head == atomic_load_explicit(&ringbuf->tail, memory_order_acquire)) {
        return false;  // Buffer empty
    }

    // Copy the sample from the buffer to the provided output pointer.
    *out = ringbuf->samples[current_head];

    // Use release ordering when updating head to ensure the sample read is complete
    // before the producer sees the new head value
    atomic_store_explicit(&ringbuf->head, (current_head + 1) % ringbuf->size, memory_order_release);
    return true;
}

/* END of ring buffer implementation */

/* async-signal-safe functions only */
static void
signal_handler(int sig, siginfo_t *si, void *ucontext)
{
    struct signal_handler_data *data = (struct signal_handler_data *)(si->si_value.sival_ptr);

    // Prepare a sample slot
    struct sample sample;
    memset(&sample, 0, sizeof(struct sample));

    bool is_processwide_signal;
    if (data == NULL) {
        // This signal was likely generated from a process-wide timer (setitimer()).
        // Here, we guess that the current thread has consumed CPU time causing to expire,
        // but this is nothing more than a guess, and may be incorrect depending on
        // kernel implementation.
        // Also will be incorrect when a native thread which does not have the GVL has
        // triggered the timer.
        // ec = rb_current_ec_noinline();
        is_processwide_signal = true;
    } else {
        // This signal was generated from a per-thread timer (timer_create()).
        is_processwide_signal = false;
    }

    if (is_processwide_signal) {
        sample.captured_frames = rb_profile_frames(0, 200, sample.iseqs, sample.lines);
    } else {
        rb_execution_context_t *ec = data->target_thread->ec;
        // assert(ec->thread_ptr->nt->thread_id == pthread_self());
        // if (ec->thread_ptr->nt->thread_id != pthread_self()) {
        //     return;
        // }

        // Set meta fields
        sample.thread = data->target_thread;

        // Grab backtrace for the target thread
        if (ec->thread_ptr->status != THREAD_RUNNABLE) {
            return;
        }
        sample.captured_frames = rb_profile_thread_frames0(ec, 0, 200, sample.iseqs, sample.lines);
    }

    int res = prof_ringbuffer_push(sess->ringbuf, &sample);
    if (!res) {
        // Buffer full, drop the sample
        return;
    }

    return;
}

static void
install_signal_handler(void)
{
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
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

#ifndef HAS_PER_THREAD_TIMER
static void
install_timer_to_process(void)
{
    struct itimerval interval = {
        .it_interval = { .tv_sec = 0, .tv_usec = 10 * 1000 },
        .it_value = { .tv_sec = 0, .tv_usec = 10 * 1000 },
    };
    int res = setitimer(ITIMER_PROF, &interval, NULL);
    if (res != 0) {
        rb_bug("failed");
    }
}
#endif

#ifdef HAS_PER_THREAD_TIMER
static void
install_timer_to_thread(VALUE thval)
{
#ifdef HAS_PER_THREAD_TIMER
    rb_thread_t *th = rb_thread_ptr_uninlined(thval);
    rb_nativethread_id_t thread_id = th->nt->thread_id;
    assert(pthread_self() == thread_id);

    if (sess->installed_timers_count >= 100) {
        rb_raise(rb_eRuntimeError, "Too many timers installed");
    }
    timer_t *timerid = &sess->installed_timers[sess->installed_timers_count++];

    // Get CPU clock ID for the native thread
    clockid_t cpu_timer_clock_id;
    if (pthread_getcpuclockid(thread_id, &cpu_timer_clock_id) != 0) {
        rb_raise(rb_eRuntimeError, "Failed to get CPU clock ID");
    }

    // Data to be passed to the signal handler
    struct signal_handler_data *data = xmalloc(sizeof(struct signal_handler_data)); // TODO: leak
    data->target_thread = th;

    // Install kernel timer
    struct sigevent sev;
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev.sigev_notify_thread_id = gettid();
    sev.sigev_value.sival_ptr = (void *)data;

    int res = timer_create(cpu_timer_clock_id, &sev, timerid);
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

    if (timer_settime(*timerid, 0, &its, NULL) == -1) {
        rb_raise(rb_eRuntimeError, "Failed to configure timer: %s", strerror(errno));
    }
#endif
}
#endif

static void
disarm_all_timers(void)
{
#ifdef HAS_PER_THREAD_TIMER
    // Disarm and delete all per-thread timers
    for (int i = 0; i < sess->installed_timers_count; i++) {
        if (timer_delete(sess->installed_timers[i]) == -1) {
            rb_raise(rb_eRuntimeError, "Failed to disarm timer: %s", strerror(errno));
        }
    }
    sess->installed_timers_count = 0;
#else
    // Disarm process-wide timer
    struct itimerval zero = {0};
    if (setitimer(ITIMER_PROF, &zero, NULL) == -1) {
        rb_raise(rb_eRuntimeError, "Failed to disarm timer: %s", strerror(errno));
    }
#endif
}

static void
thread_callback(rb_event_flag_t flag, const rb_internal_thread_event_data_t *data, void *custom_data)
{
#ifdef HAS_PER_THREAD_TIMER
    if (flag == RUBY_INTERNAL_THREAD_EVENT_STARTED) {
        install_timer_to_thread(data->thread);
    }
#endif
}

// Ensures that the session's sample array has capacity for at least one more sample
// Returns true if capacity was expanded / was already sufficient, false on failure.
static bool
ensure_sample_capacity(void)
{
    int original_capa = sess->collected_samples_capa;
    int original_count = sess->collected_samples_count;

    if (sess->collected_samples == NULL) {
        rb_bug("unreachable");
    }

    // Check if we need to expand
    if (sess->collected_samples_count < sess->collected_samples_capa) {
        return true;
    }

    // Calculate new size (double the current size)
    int new_capa = sess->collected_samples_capa * 2;

    // Reallocate the array
    struct sample *new_sample_storage = xrealloc(sess->collected_samples, (size_t)new_capa * sizeof(struct sample));
    if (new_sample_storage == NULL) {
        goto fail;
    }

    sess->collected_samples = new_sample_storage;
    sess->collected_samples_capa = new_capa;

    return true;

fail:
    sess->collected_samples_capa = original_capa;
    sess->collected_samples_count = original_count;

    return false;
}

static void *
sample_collection_thread(void *arg)
{
    while (sess->is_running) {
        // Take samples from the ring buffer
        struct sample sample;
        while (prof_ringbuffer_pop(sess->ringbuf, &sample) == true) {
            // Ensure we have capacity before adding a new sample
            if (!ensure_sample_capacity()) {
                // Failed to expand buffer
                printf("Failed to expand sample buffer. Dropping sample\n");
                break;
            }

            sess->collected_samples[sess->collected_samples_count++] = sample;
        }

        // Sleep for 100 ms
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000, };
        nanosleep(&ts, NULL);
    }

    return NULL;
}

/**
 * Enable the profiler.
 */
static VALUE
rb_prof_enable(VALUE self)
{
    // Initialize session
    sess = xmalloc(sizeof(struct prof_session));
    if (sess == NULL) {
        goto fail;
    }
    // Ensure all fields start in a known state
    memset(sess, 0, sizeof(struct prof_session));
    sess->ringbuf = prof_ringbuffer_new(PROF_RINGBUFFER_MAX_SIZE);
    if (sess->ringbuf == NULL) {
        goto fail;
    }
    sess->collected_samples = xmalloc(1024 * sizeof(struct sample));
    if (sess->collected_samples == NULL) {
        goto fail;
    }
    // Initialize sample buffer metadata
    sess->collected_samples_capa = 1024;
    sess->collected_samples_count = 0;
#ifdef HAS_PER_THREAD_TIMER
    sess->installed_timers_count = 0;
#endif

    // Install signal handler
    install_signal_handler();

#ifdef HAS_PER_THREAD_TIMER
    // Install timer on all threads on the current Ractor
    VALUE rb_cThread = rb_const_get(rb_cObject, rb_intern("Thread"));
    VALUE threads = rb_funcall(rb_cThread, rb_intern("list"), 0);
    for (int i = 0; i < RARRAY_LEN(threads); i++) {
        VALUE thread = rb_ary_entry(threads, i);
        install_timer_to_thread(thread);
    }
#else
    // Install timer on process
    install_timer_to_process();
#endif

    // Register a callback to install timer on newly created threads
    rb_internal_thread_add_event_hook(&thread_callback, RUBY_INTERNAL_THREAD_EVENT_STARTED, NULL);

    // Mark session as running and start a collector thread
    sess->is_running = true;
    if (pthread_create(&sess->sample_collector_thread_handle, NULL, sample_collection_thread, NULL) != 0) {
        goto fail;
    }

    return Qtrue;

fail:
    rb_raise(rb_eRuntimeError, "Failed to initialize profiling session");
}

/**
 * Disable the profiler and return collected data.
 */
static VALUE
rb_prof_disable(VALUE self)
{
    sess->is_running = false;

    // Cleanup session
    disarm_all_timers();
    uninstall_signal_handler();
    pthread_join(sess->sample_collector_thread_handle, NULL);
    prof_ringbuffer_free(sess->ringbuf);

    // Construct a Ruby::Profile::ProfileBuilder object
    VALUE rb_cProfileBB = rb_const_get(rb_mProfile, rb_intern("ProfileBuilderBuilder"));
    VALUE builder = rb_funcall(rb_cProfileBB, rb_intern("new"), 0);

    for (int i = 0; i < sess->collected_samples_count; i++) {
        struct sample *sample = &sess->collected_samples[i];
        VALUE stack = rb_ary_new();
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

    xfree(sess->collected_samples);

    return rb_funcall(builder, rb_intern("to_profile"), 0);
}

void
Init_profile(void)
{
    rb_cRubyVM = rb_const_get(rb_cObject, rb_intern("RubyVM"));
    rb_mProfile = rb_define_module_under(rb_cRubyVM, "Profile");
    rb_define_module_function(rb_mProfile, "enable", rb_prof_enable, 0);
    rb_define_module_function(rb_mProfile, "disable", rb_prof_disable, 0);

    rb_require("rubyvm/profile");
}
