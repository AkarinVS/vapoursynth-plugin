Plugin akarin
=============

CAMBI
-----
`akarin.Cambi(clip clip[, int window_size = 63, float topk = 0.6, float tvi_threshold = 0.019, bint scores = False, float scaling = 1.0/window_size])`

Computes the CAMBI banding score as `CAMBI` frame property. Unlike [VapourSynth-VMAF](https://github.com/HomeOfVapourSynthEvolution/VapourSynth-VMAF), this filter is online (no need to batch process the whole video) and provides raw cambi scores (when `scores == True`).

- `clip`: Clip to calculate CAMBI score. Only Gray/YUV format with integer sample type of 8/10-bit depth (subsampling can be arbitrary as cambi only uses the Y channel.)
- `window_size` (min: 15, max: 127, default: 63): Window size to compute CAMBI. (default: 63 corresponds to ~1 degree at 4K resolution and 1.5H)
- `topk` (min: 0.0001, max: 1.0, default: 0.6): Ratio of pixels for the spatial pooling computation.
- `tvi_threshold` (min: 0.0001, max: 1.0, default: 0.019): Visibility threshold for luminance `ΔL < tvi_threshold*L_mean` for BT.1886.
- `scores` (default: False): if True, for scale i (0 <= i < 5), the GRAYS c-score frame will be stored as frame property `"CAMBI_SCALE%d" % i`.
- `scaling`: scaling factor used to normalize the c-scores for each scale returned when `scores=True`.

DLVFX
-----
`akarin.DLVFX(clip clip, int op[, float scale=1, float strength=0, int output_depth=clip.format.bits_per_sample, int num_streams=1])`

There are three operation modes ([official docs](https://docs.nvidia.com/deeplearning/maxine/pdf/vfx-sdk-programming-guide.pdf)):
- `op=0`: artefact reduction. `int strength` controls the strength (only 0 or 1 allowed).
   input requirements: `90 <= height <= 1080`, `256 <= width <= 1920+128`, `width % 128 == 0`.
- `op=1`: super resolution, `scale>1` controls the scale factor. `int strength` controls the enhancement strength (only 0 or 1 allowed).
   input requirements: see Table 1 (Scale and Resolution Support for Input Videos) in [official SDK docs](https://docs.nvidia.com/deeplearning/maxine/pdf/vfx-sdk-programming-guide.pdf), and `width >= 256`, `width % 128 == 0`, `height >= 90`.

~~- `op=2`: denoising. `float strength` controls the strength.~~ (Not working.)

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
- (\*) Dynamic pixel access using absolute coordinates. Use `absX absY x[]` to access the pixel (absX, absY) in the current frame of clip x. absX and absY can be computed using arbitrary expressions, and they are clamped to be within their respective ranges (i.e. boundary pixels are repeated indefinitely.) Only use this as a last resort as the performance is likely worse than static relative pixel access, depending on access pattern.
- (\*) Bitwise operators (`bitand`, `bitor`, `bitxor`, `bitnot`): they operate on <24b integer clips by default. If you want to process 24-32 bit integer clips, you must set `opt=1` to force integer evaluation as much as possible (but beware that 32-bit signed integer overflow will wraparound.)
- Support more bases for constants
  - hexadecimals: 0x123 or 0x123.4p5
  - octals: 023 (however, invalid octal numbers will be parsed as floating points, so "09" will be parsed the same as "9.0")
- (\*) Support **arbitrary** number of input clips. Use `srcN` to access the `N`-th input clip (i.e. `src0` is equivalent to `x`, `src25` is equivalent to `w`, etc.) There is no hardcoded limit on the number of input clips, however VS might not be able to handle too many. Up to `255` input clips have been tested.

Select
----

`akarin.Select(clip[] clip_src, clip[] prop_src, string[] expr)`

For each frame evaluate the expression `expr` where clip variables (`a-z`) references the corresponding frame from `prop_src`.
The result of the evaluation is used as an index to pick a clip from `clip_src` array which is used to satisfy the current frame request.

It could be used to replace common uses of `std.FrameEval` where you want to evaluate some metrics and then choose one of the clip to use.

In addition to all operators supported by `Expr` (except those that access pixel values, which is not possible in `Select`), `Select`  also has a few extensions:
- `argminN` and `argmaxN`: find the min/max value of the top N items on the stack and return its index. For example `2 1 0 3 argmin4` should return 2 (as the minimum value 0 is the 3rd value).
- `argsortN`: stable sort the top N elements on the stack and return their respective indices. It is used when you want to pick a rank other than the minimum or maximum, for example, the median.

Also note, unlike `Expr`, where non-existent frame property will be turned into `nan`, `Select` will use `0.0` instead.

As an example, `mvsfunc.FilterIf` can be implemented like this:
```python
x = mvsfunc.FilterIf(src, flt, '_Combed', prop_clip)             # is equivalent to:
x = core.akarin.Select([src, flt], prop_clip, 'x._Combed 1 0 ?') # when x._Combed is set and True, then pick the 2nd clip (flt)
```


PropExpr
----

`akarin.PropExpr(clip[] clips, dict=lambda: dict(key=val))`

`PropExpr` is a filter to programmatically compute numeric frame properties. Given a list of clips, it will return the first clip after modifying its frame properties as specified by the dict argument. The expressions have access to the frame property of all the clips.

`dict` is a Python lambda that returns a Python dict, where each key specifies the expression that evaluates to the new value for the frame property of the same name. The expression supports all operators supported by `Select` (i.e. no support for pixel access operators.)

For each `val`, there are three cases:
- an integer, or a float: the property `key` will be set to that value.
- an empty string: the property `key` will be removed.
- an expression string: the result of evaluating the expression specifies the value of property `key`.

Some examples:
- `PropExpr(c, lambda: dict(_FrameNumber='N'))`: this set the `_FrameNumber` frame property to the current frame number.
- `PropExpr(c, lambda: dict(A=1, B=2.1, C="x.Prop 2 *"))`: this set property `A` to constant 1, `B` to 2.1 and `C` to be the value returned by the expression `x.Prop 2 *`, which is two times the value of the existing `Prop` property.
- `PropExpr(c, lambda: dict(ToBeDeleted=''))`: this deletes the frame property `ToBeDeleted` (no error if it does not exist.)
- `PropExpr(c, lambda: dict(A='x.B', B='x.A'))`: this swaps the value of property `A` and `B` as all frame property updates are performed atomically.

Note: this peculiar form of specifying the properties is to workaround a limitation of the VS API.


Text
----

Text is an enhanced `text.Text`:
- It takes Python format string so that the text for each frame can be based on frame properties. No need to resort to `std.FrameEval` and dynamic `text.Text` filter creation. But note this filter by itself does not support any computation on the frame properties, so if you want to display, say, `x.Prop1 * 10 + y.Prop2`, you will have to use `PropExpr` before hand to compute the value (e.g. `c.akarin.PropExpr(lambda: dict(PropToShow="x.Prop1 10 * y.Prop2 +")).akarin.Text("{PropToShow}")`).
- It also support saving the formated string as a frame property (via `prop` argument), so that you can pass the formatted string to other filters (e.g. assrender).

`akarin.Text(clip[] clips, string format[, int alignment=7, int scale=1, string prop, bint strict=0, bint vspipe=0])`

`clips` are the input clips, the output will come from the first clip. It has the same restrictions are the `text.Text` filter (YUV/Gray/RGB, 8-16 bit integer or 32-bit float format).

`format` is a Python `f""`-style format string. The filter internally uses a slightly modified [{fmt}](https://fmt.dev), so please refer to [{fmt}'s syntax docs](https://fmt.dev/latest/syntax.html) for details. Properties can be specified as in `Expr`/`Select`/`PropExpr`. As a special shorthand, `{Prop}` is an alias for `{x.Prop}`. Some simple examples:
  - `{}` or `{N}`: the unamed paramenter (or `N`) is the current frame number.
  - `"Matrix: {x._Matrix}"`: it will show the matrix property in string format (i.e. `Matrix: BT.709`).
  - `"Matrix: {x._Matrix:d}"`: it will show the matrix property as a number.
  - `"Chroma: {x._ChromaLocation:.>10s}`: it will show the chroma location as a string, right aligned, and padded on the left with '.' (i.e. `Chroma: ......Left`).

The filter supports formatting int/float scalar or arrays, data (shown as string). All the rest are shown as type sepcific placeholders (e.g. `<node>` for a node).

`alignment` and `scale` arguments serve the role as in `text.Text`.

`prop`, if set, will make the filter set the specified frame property as the formatted string, and *not* overlay the string onto the frame.

`strict`, if set to `True`, will abort the encoding process if format fails for any frame (due to incorrect format string, but not missing properties.)

`vspipe` will determine whether to overlay the OSD when the script is run under vspipe. The default `False` means the OSD will only be visible when the script is run in previewers, not when encoding with vspipe. The check is done by checking the executable name of the current process for "vspipe" (Unix) or "vspipe.exe" (Windows). This setting does not affect `prop`.


Version
----

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
 b'x[]',  # dynamic pixel access
 b'bitand', b'bitor', b'bitxor', b'bitnot', # bitwise operators
 b'src0', b'src26', # arbitrary number of input clips supported
]
```
- `select_features`: a list of features for the `Select` filter.
- `text_features`: a list of features for the `Text` filter.

There are two implementations:
1. The legacy jitasm based one (deprecated, and no longer developed)
If you encounter issues and suspect it's related to this JIT, you could set the `CPU_LEVEL` environment variable to 0/1/2 to force the *maximum* x86 ISA limit to interpreter/sse2/avx2, respectively. The actual ISA used will be determined based on runtime hardware capabilities and the limit (default to no limit).
When reporting issues, please also try limiting the ISA to a lower level (at least try setting `CPU_LEVEL` to 0 to force using the interpreter) and see the problem still persists.

2. The new LLVM based implementation (aka lexpr). Features labeled with (\*) is only available in this new implementation.
If the `opt` argument is set to 1 (default 0), then it will activate an integer optimization mode, where intermediate values are computed with 32-bit integer for as long as possible. You have to make sure the intermediate value is always representable with int32 to use this optimization (as arithmetics will warp around in this mode.)


Building
--------
To build the plugin, you will need LLVM 10-12 installed (on Windows, you need to build your own) and have llvm-config exectuable in your PATH, then run:
```
meson build
ninja -C build install
```

Example LLVM build procedure on windows:
```
git clone --depth 1 https://github.com/llvm/llvm-project.git --branch release/12.x
cd llvm-project
mkdir build
cd build
cmake -A x64 -Thost=x64 -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS="" -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_USE_CRT_RELEASE=MT ../llvm
cmake --build --config Release
cmake --install ../install
```
