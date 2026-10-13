// Thin C interface to AdPlug's Nuked OPL3 emulator, used as the AdLib chip.
#include <adplug/nemuopl.h>

extern "C" {
#include "opl.h"
}

struct Opl {
    CNemuopl chip;
    explicit Opl(int rate) : chip(rate) { chip.init(); }
};

extern "C" Opl *opl_create(int rate)
{
    return new Opl(rate);
}

extern "C" void opl_destroy(Opl *o)
{
    delete o;
}

extern "C" void opl_write(Opl *o, int reg, int val)
{
    o->chip.write(reg, val);
}

extern "C" void opl_generate(Opl *o, int16_t *stereo, int frames)
{
    o->chip.update(stereo, frames);
}
