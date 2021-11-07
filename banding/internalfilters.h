#ifndef BANDING_INTERNALFILTERS_H
#define BANDING_INTERNALFILTERS_H

#include "VapourSynth.h"

#ifdef __cplusplus
extern "C" {
#endif

void VS_CC bandingInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin);

#ifdef __cplusplus
}
#endif

#endif // BANDING_INTERNALFILTERS_H
