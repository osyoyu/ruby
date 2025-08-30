#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ruby/ruby.h"
#include "ruby/debug.h"
#include "ruby/thread.h"
#include "internal/thread.h"
#include "vm_core.h"

void
install_signal_handler(VALUE thval)
{
    rb_thread_t *th = (rb_thread_t *)DATA_PTR(thval);
    printf("%lu", th->nt->thread_id);
}

/**
 * Enable the profiler.
 */
VALUE
rb_profiler_enable(VALUE self)
{
    // Collect all active threads on the current Ractor
    VALUE thread_class = rb_const_get(rb_cObject, rb_intern("Thread"));
    VALUE threads = rb_funcall(thread_class, rb_intern("list"), 0);
    for (int i = 0; i < RARRAY_LEN(threads); i++) {
        VALUE thread = rb_ary_entry(threads, i);
        install_signal_handler(thread);
    }

    return Qtrue;
}

/**
 * Disable the profiler and return collected data.
 */
VALUE
rb_profiler_disable(VALUE self)
{
    return Qnil;
}

void
Init_profiler(void)
{
    VALUE rb_mRuby = rb_define_module("Ruby");
    VALUE rb_mProfiler = rb_define_module_under(rb_mRuby, "Profiler");
    rb_define_module_function(rb_mProfiler, "enable", rb_profiler_enable, 0);
    rb_define_module_function(rb_mProfiler, "disable", rb_profiler_disable, 0);

    // rb_internal_thread_add_event_hook(&thread_callback, RUBY_INTERNAL_THREAD_EVENT_STARTED, NULL);
}
