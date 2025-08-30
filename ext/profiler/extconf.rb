# frozen_string_literal: true
require 'mkmf'
$VPATH << '$(topdir)' << '$(top_srcdir)'
$INCFLAGS << " -I$(topdir) -I$(top_srcdir)"

# Option 1: Force all symbols from static library to be included
$LIBRUBYARG = "$(LIBRUBYARG_STATIC)"
$LDFLAGS << " -Wl,--whole-archive $(libdir)/libruby-static.a -Wl,--no-whole-archive"

# Option 2: Use dynamic loading to resolve symbols at runtime
# have_library("dl", "dlopen")

create_makefile("profiler")
