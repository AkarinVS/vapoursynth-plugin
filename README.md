Plugin akarin
=============

DLVFX
-----
`akarin.DLVFX(clip clip, int op[, float scale=1, float strength=0, int output_depth=clip.format.bits_per_sample, int num_streams=1])`

There are three operation modes:
- `op=0`: artefact reduction. `int strength` controls the strength.
- `op=1`: super resolution, `scale>1` controls the scale factor. `int strength` controls the enhancement strength.
- `op=2`: denoising. `float strength` controls the strength. (Not working.)

Usage Notes:
- Only 32-bit floating point RGB and 8-bit integer RGB24 clips are supported as input `clip`.
- The output defaults to the same format as the input, however, you can set `output_depth` to 32 (RGBS) or 8 (RGB24) to override the default.
- Setting `num_streams>1` will improve the performance by parallelizing processing of multiple frames on the GPU and will improve performance, as long as your GPU is capable enough to handle it.

This filter requires appropriate [Video Effects library (v0.6 beta)](https://www.nvidia.com/en-us/geforce/broadcasting/broadcast-sdk/resources/) to be installed. (This library is too large to be bundled with the plugin.)
This filter also requires RTX-capable NVidia GPU to run.

DLISR
-----

`akarin.DLISR(clip clip, [, int scale=2])`

This filter will use Nvidia [NGX Technology](https://developer.nvidia.com/rtx/ngx) DLISR DNN to scale up an input clip.
Input clip must be in `vs.RGBS` format.
The `scale` parameter can only be 2/4/8 and note that this filter uses considerable amount of GPU memory (e.g. 2GB for 2x scaling 1080p input)

This filter requires `nvngx_dlisr.dll` to be present in the same directory as this plugin.
This filter requires RTX-capable NVidia GPU to run.

Warning: <br>
Due to peculiar nature of its implementation, this filter only works if it is the *only* CUDA filter in your script and it will always automatically choose the GPU to use. Please make sure to use CPU versions of other filters if you plan to use `DLISR` in the script (note that it's fairly computation extensive, so using other GPU filters will likely only slow things down anyway.)

Expr
----

`akarin.Expr(clip[] clips, string[] expr[, int format, int opt=0, int boundary=0])`

This works just like [`std.Expr`](http://www.vapoursynth.com/doc/functions/expr.html) (esp. with the same SIMD JIT support on x86 hosts), with the following additions:
- use `x.PlaneStatsAverage` to load the `PlaneStatsAverage` frame property of the current frame in the given clip `x`.
  - Any scalar numerical frame properties can be used;
  - If the property does not exist for a frame, the value will be NaN, which will be clamped to the maximum value.
- use the `N` operator to load the current frame number;
- use the `X` and `Y` operators to load the current column / row (aka `mt_lutspa`);
  - (\*) use the `width` and `height` to access the current frame's dimension.
- The `sin` operator. The implementation is reasonably accurate for input magnitude up to 1e5 (absolute error up to 2e-6). Do not pass input whose magnitude is larger than 2e5.
- The `cos` operator. The implementation is reasonably accurate for input magnitude up to 1e5 (absolute error up to 2e-6). Do not pass input whose magnitude is larger than 2e5.
- The `%` binary operator, which implements C `fmodf`, e.g. `trunc` can be implemented as `dup 1.0 % -`.
- (\*) The `clip` and `clamp` operators, which clamps an input to be within a given bound. `x 16 235 clip` is equivalent to `x 16 max 235 min`.
- The `**` operator can be used as an alias for `pow`.
- The `trunc` / `round` / `floor` operators that truncates/rounds/floors to integers.
- (\*) Arbitrarily named temporary variables (modeled after [Forth](https://en.wikipedia.org/wiki/Forth_(programming_language)))
  - Pop a value and store to variable `var`: `var!`
  - Read a variable `var` and push onto stack: `var@`
- (\*) `dropN` drops the top N items from the stack (N>=1, and defaults to 1). `1 2 drop` is equivalent to `1`.
- (\*) `sortN` sorts the top N items on the stack (N>=1), after this operator, the top will be the smallest element.
- (\*) Static relative pixel access (modeled after [AVS+ Expr](http://avisynth.nl/index.php/Expr#Pixel_addressing))
  - Use `x[relX,relY]` to access the pixel (relX, relY) relative to current coordinate, where -width < relX < width and -height < relY < height. Off screen pixels will be either cloned from the respective edge (clamped) or use the pixel mirror from the respective edge (mirrored). Both relX and relY should be constant.
  - Optionally, use `:m` or `:c` suffixes to specify mirrored and clamped boundary conditions, respectively.
  - The `boundary` argument specifies the default boundary condition for all relative pixel accesses without explicit specification:
    - 0 means clamped
    - 1 means mirrored
- Support more bases for constants
  - hexadecimals: 0x123 or 0x123.4p5
  - octals: 023 (however, invalid octal numbers will be parsed as floating points, so "09" will be parsed the same as "9.0")

`akarin.Version()`

Use this function to query the version and features of the plugin. It will return a Python dict with the following keys:
- `version`: the version byte string
- `expr_backend`: `llvm` (for lexpr) or `jitasm` (legacy).
- `expr_features`: a list of byte strings for all supported features. e.g. here is the list for lexpr:
```python
[
 b'x.property',  # frame property access
 b'sin', b'cos', b'%', b'clip', b'clamp', b'**', # operators
 b'N', b'X', b'Y', b'pi', b'width', b'height', # constants
 b'trunc', b'round', b'floor',  # truncation, round and floor
 b'var@', b'var!', # temporary variable access
 b'x[x,y]',  # relative pixel access
 b'x[x,y]:m' # relative pixel access with mirrored boundary condition
 b'drop', # dropN support
 b'sort', # sortN support
]
```

There are two implementations:
1. The legacy jitasm based one (deprecated, and no longer developed)
If you encounter issues and suspect it's related to this JIT, you could set the `CPU_LEVEL` environment variable to 0/1/2 to force the *maximum* x86 ISA limit to interpreter/sse2/avx2, respectively. The actual ISA used will be determined based on runtime hardware capabilities and the limit (default to no limit).
When reporting issues, please also try limiting the ISA to a lower level (at least try setting `CPU_LEVEL` to 0 to force using the interpreter) and see the problem still persists.

2. The new LLVM based implementation (aka lexpr). Features labeled with (\*) is only available in this new implementation.
If the `opt` argument is set to 1 (default 0), then it will activate an integer optimization mode, where intermediate values are computed with 32-bit integer for as long as possible. You have to make sure the intermediate value is always representable with int32 to use this optimization (as arithmetics will warp around in this mode.)
