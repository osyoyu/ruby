# frozen_string_literal: true
require 'mkmf'
$VPATH << '$(topdir)' << '$(top_srcdir)'
$INCFLAGS << " -I$(topdir) -I$(top_srcdir)"

# Check for timer_create with CLOCK_THREAD_CPUTIME_ID
have_func("timer_create", "time.h")
have_const("CLOCK_THREAD_CPUTIME_ID", "time.h")

create_makefile("profiler")
