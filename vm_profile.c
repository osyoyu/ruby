#include "vm_core.h"

/* called from Init_vm() in vm.c */
void
Init_vm_profile(void)
{
    VALUE rb_mRuby = rb_define_module("Ruby");
    VALUE rb_mProfiler = rb_define_module_under(rb_mRuby, "Profiler");
}
