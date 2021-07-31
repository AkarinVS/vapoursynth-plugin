Plugin akarin
=============

Expr
----

`akarin.Expr(clip[] clips, string[] expr[, int format, int opt=1, int boundary=0])`

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
- The `trunc` / `round` / `floor` operators that truncates/rounds/floors to integers.
- (\*) Arbitrarily named temporary variables (modeled after [Forth](https://en.wikipedia.org/wiki/Forth_(programming_language)))
  - Pop a value and store to variable `var`: `var!`
  - Read a variable `var` and push onto stack: `var@`
- (\*) Static relative pixel access (modeled after [AVS+ Expr](http://avisynth.nl/index.php/Expr#Pixel_addressing))
  - Use `x[relX,relY]` to access the pixel (relX, relY) relative to current coordinate, where -width < relX < width and -height < relY < height. Off screen pixels will be either cloned from the respective edge (clamped) or use the pixel mirror from the respective edge (mirrored). Both relX and relY should be constant.
  - Optionally, use `:m` or `:c` suffixes to specify mirrored and clamped boundary conditions, respectively.
  - The `boundary` argument specifies the default boundary condition for all relative pixel accesses without explicit specification:
    - 0 means clamped
    - 1 means mirroed
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
 b'sin', b'cos', b'%', # operators
 b'N', b'X', b'Y', b'pi', b'width', b'height', # constants
 b'trunc', b'round', b'floor',  # truncation, round and floor
 b'@', b'!', # temporary variable access
 b'x[x,y]'   # relative pixel access
]
```

There are two implementations:
1. The legacy jitasm based one.
If you encounter issues and suspect it's related to this JIT, you could set the `CPU_LEVEL` environment variable to 0/1/2 to force the *maximum* x86 ISA limit to interpreter/sse2/avx2, respectively. The actual ISA used will be determined based on runtime hardware capabilities and the limit (default to no limit).
When reporting issues, please also try limiting the ISA to a lower level (at least try setting `CPU_LEVEL` to 0 to force using the interpreter) and see the problem still persists.

2. The new LLVM based implementation (lexpr). Features labeled with (\*) is only available in this new implementation.
If the `opt` argument is set to 1 (default), then it will activate an integer optimization mode, where intermediate values are computed with 32-bit integer for as long as possible. You have to make sure the intermediate value is always representable with int32 to use this optimization (as arithmetics will warp around in this mode.)
