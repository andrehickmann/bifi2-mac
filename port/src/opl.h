/* AdLib (OPL2) synthesis, backed by AdPlug's Nuked OPL3 emulator. */
#ifndef OPL_H
#define OPL_H

#include <stdint.h>

typedef struct Opl Opl;

Opl *opl_create(int rate);
void opl_destroy(Opl *o);
void opl_write(Opl *o, int reg, int val);
/* Generates `frames` stereo frames (interleaved L/R). */
void opl_generate(Opl *o, int16_t *stereo, int frames);

#endif
