#ifndef TEXT_INTERNALFILTERS_H
#define TEXT_INTERNALFILTERS_H

#include "VapourSynth.h"

void VS_CC textInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin);
void VS_CC tmplInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin);

#endif // INTERNALFILTERS_H
