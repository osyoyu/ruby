require 'mkmf'
$VPATH << '$(topdir)' << '$(top_srcdir)'
$INCFLAGS << " -I$(topdir) -I$(top_srcdir)"

# Per-thread CPU timers are Linux-only
have_func("timer_create")
have_const("CLOCK_THREAD_CPUTIME_ID")

create_header
create_makefile("profile")
