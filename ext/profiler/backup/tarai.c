#include "ruby/ruby.h"

VALUE rb_tarai(VALUE self, VALUE x, VALUE y, VALUE z);

int
tarai(int x, int y, int z)
{
    if (x <= y) {
        return y;
    } else {
        return tarai(tarai(x - 1, y, z), tarai(y - 1, z, x), tarai(z - 1, x, y));
    }
}

VALUE
rb_tarai(VALUE self, VALUE x, VALUE y, VALUE z)
{
    int xi = NUM2INT(x);
    int yi = NUM2INT(y);
    int zi = NUM2INT(z);
    int ret = tarai(xi, yi, zi);
    return INT2NUM(ret);
}
