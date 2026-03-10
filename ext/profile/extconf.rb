require 'mkmf'

$VPATH << '$(topdir)' << '$(top_srcdir)'
$INCFLAGS << " -I$(topdir) -I$(top_srcdir)"

create_header
create_makefile("profile")
