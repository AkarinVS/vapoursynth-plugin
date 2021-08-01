First install appropriate Video Effects library (v0.6 beta) from https://www.nvidia.com/en-us/geforce/broadcasting/broadcast-sdk/resources/.
Make sure your environment is setup correctly by downloading
[opencv_world346.dll](https://github.com/NVIDIA/MAXINE-VFX-SDK/blob/master/samples/external/opencv/bin/opencv_world346.dll) and
[VideoEffectsApp.exe](https://github.com/NVIDIA/MAXINE-VFX-SDK/blob/master/samples/VideoEffectsApp/VideoEffectsApp.exe).
Play with VideoEffectsApp.exe to make sure it works before proceeding.

And then build the plugin like this with mingw:
```
g++ -DSTANDALONE_VFX -o akarin2.dll -shared -static vfx.cc -I ../include nvvfx/src/*.cpp -I nvvfx/include -Wall -O2
```

Example code:
```python
import os, os.path
import vapoursynth as vs
core = vs.core
import mvsfunc as mvf

core.std.LoadPlugin(os.path.abspath(os.path.join(os.getcwd(), 'akarin2.dll')))

c = core.imwri.Read('input.png')
c = mvf.Depth(c, 32) # only supports vs.RGBS formats

# OP_AR: Artefact reduction, OP_SUPERRES: super resolution, OP_DENOISE: denoise.
# strength: 0 for weak effect (weaker enhancement), 1 for strong effect (enhancement).
# scale = 2/3/4 for super resolution, otherwise unused.
OP_AR, OP_SUPERRES, OP_DENOISE = range(3)
d = core.akarin2.DLVFX(c, op=OP_SUPERRES, scale=2, strength=0)

d = core.imwri.Write(d, 'PNG', 'out-%d.png')

d.set_output()
```

This plugin is provided as is, and I haven't been able to test it locally.
