#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/ptrace.h>

#include "internal.h"
#include "internal/thread.h"
#include "internal/vm.h"
#include "ruby/debug.h"
#include "ruby/thread.h"
#include "vm_core.h"
#include "ruby/ruby.h"

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

struct prof_ringbuffer {
    int size;
    atomic_int head;
    atomic_int tail;
    struct sample *samples;
};

static struct prof_ringbuffer * prof_ringbuffer_new(int size);
static void prof_ringbuffer_free(struct prof_ringbuffer *ringbuf);
// async-signal-safe
static bool prof_ringbuffer_push(struct prof_ringbuffer *ringbuf, struct sample *sample);
static bool prof_ringbuffer_pop(struct prof_ringbuffer *ringbuf, struct sample *out);

/* Globals */

static VALUE rb_mRuby;
static VALUE rb_mProfiler;

static timer_t installed_timers[100];
static int installed_timers_count = 0;

static struct prof_ringbuffer *ringbuf = NULL;

static struct sample *sample_storage;
static int sample_storage_capacity = 0;
static int sample_storage_index = 0;

static bool is_running = false;
static pthread_t sample_collector_thread_handle;

/* Buffer storage */

static struct prof_ringbuffer *
prof_ringbuffer_new(int size) {
    if (size <= 0) { return NULL; }

    struct prof_ringbuffer *ringbuf = malloc(sizeof(struct prof_ringbuffer));
    if (!ringbuf) { goto err; }
    ringbuf->size = size + 1; // One extra slot is required to distinguish full from empty
    ringbuf->head = 0;
    ringbuf->tail = 0;
    ringbuf->samples = malloc(ringbuf->size * sizeof(struct sample));
    if (!ringbuf->samples) { goto err_free_ringbuf; }
    return ringbuf;

err_free_ringbuf:
    free(ringbuf);
err:
    return NULL;
}

static void
prof_ringbuffer_free(struct prof_ringbuffer *ringbuf) {
    free(ringbuf->samples);
    free(ringbuf);
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
    struct sample sample;
    memset(&sample, 0, sizeof(struct sample));

    // Set meta fields
    sample.thread = data->target_thread;

    // Grab backtrace for the target thread
    int captured_frames;
    if (ec->thread_ptr->status != THREAD_RUNNABLE) {
        return;
    }
    captured_frames = thread_profile_frames(ec, 0, 200, sample.iseqs, sample.lines);
    sample.captured_frames = captured_frames;

    int res = prof_ringbuffer_push(ringbuf, &sample);
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

static void
thread_callback(rb_event_flag_t flag, const rb_internal_thread_event_data_t *data, void *custom_data)
{
    if (flag == RUBY_INTERNAL_THREAD_EVENT_STARTED) {
        install_timer_to_thread(data->thread);
    }
}

// Ensures that the session's sample array has capacity for at least one more sample
// Returns true if successful, false if memory allocation failed
static bool
ensure_sample_capacity()
{
    if (sample_storage == NULL) {
        // Initial allocation
        sample_storage_capacity = 1024;
        sample_storage = xmalloc(sample_storage_capacity * sizeof(struct sample));
        if (sample_storage == NULL) {
            rb_bug("allocation failed");
        }
        return true;
    }

    // Check if we need to expand
    if (sample_storage_index < sample_storage_capacity) {
        return true;
    }

    // Calculate new size (double the current size)
    size_t new_capacity = sample_storage_capacity * 2;

    // Reallocate the array
    struct sample *new_sample_storage = xrealloc(sample_storage, new_capacity * sizeof(struct sample));
    if (new_sample_storage == NULL) {
        rb_bug("allocation failed");
    }

    sample_storage = new_sample_storage;
    sample_storage_capacity = new_capacity;

    return true;
}

static void *
sample_collection_thread(void *arg)
{
    while (is_running) {
        // Take samples from the ring buffer
        struct sample sample;
        while (prof_ringbuffer_pop(ringbuf, &sample) == true) {
            // Ensure we have capacity before adding a new sample
            if (!ensure_sample_capacity()) {
                // Failed to expand buffer
                printf("Failed to expand sample buffer. Dropping sample\n");
                break;
            }

            sample_storage[sample_storage_index++] = sample;
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
rb_profiler_enable(VALUE self)
{
    // Initialize ring buffer
    ringbuf = prof_ringbuffer_new(1000);
    if (ringbuf == NULL) {
        rb_raise(rb_eRuntimeError, "Failed to create ring buffer");
    }

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

    // Start a collector thread
    is_running = true;
    if (pthread_create(&sample_collector_thread_handle, NULL, sample_collection_thread, NULL) != 0) {
        rb_raise(rb_eRuntimeError, "Failed to spawn sample collector thread");
    }

    return Qtrue;
}

/**
 * Disable the profiler and return collected data.
 */
static VALUE
rb_profiler_disable(VALUE self)
{
    is_running = false;

    // Cleanup
    disarm_all_timers();
    uninstall_signal_handler();
    pthread_join(sample_collector_thread_handle, NULL);

    // Ensure the Ruby-side builder is available
    rb_require("rubyvm/profiler");

    // Construct a Ruby::Profile::ProfileBuilder object
    VALUE rb_cProfileBB = rb_const_get(rb_mProfiler, rb_intern("ProfileBuilderBuilder"));
    VALUE builder = rb_funcall(rb_cProfileBB, rb_intern("new"), 0);

    for (int i = 0; i < sample_storage_index; i++) {
        struct sample *sample = &sample_storage[i];
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

    return rb_funcall(builder, rb_intern("to_profile"), 0);
}

void
Init_profiler(void)
{
    // Define RubyVM::Profiler and its module methods
    rb_mRuby = rb_const_get(rb_cObject, rb_intern("RubyVM"));
    rb_mProfiler = rb_define_module_under(rb_mRuby, "Profiler");
    rb_define_module_function(rb_mProfiler, "enable", rb_profiler_enable, 0);
    rb_define_module_function(rb_mProfiler, "disable", rb_profiler_disable, 0);

    // Load Ruby part (defines ProfileBuilderBuilder etc.)
    rb_require("rubyvm/profiler");
}
