Plugin akarin
=============

Expr
----

`akarin.Expr(clip[] clips, string[] expr[, int format])`

This works just like [`std.Expr`](http://www.vapoursynth.com/doc/functions/expr.html) (esp. with the same SIMD JIT support on x86 hosts), with the following additions:
- use `x.PlaneStatsAverage` to load the `PlaneStatsAverage` frame property of the current frame in the given clip `x`.
  - Any scalar numerical frame properties can be used;
  - If the property does not exist for a frame, the value will be NaN, which will be clamped to the maximum value.
- use the `N` operator to load the current frame number;
- use the `X` and `Y` operators to load the current column / row (aka `mt_lutspa`);
- The `sin` operator. The implementation is reasonable accurate for input magnitude up to 1e5 (absolute error up to 2e-6). Do not pass input whose magnitude is larger than 2e5.

If you encounter issues and suspect it's related to the JIT, you could set the `CPU_LEVEL` environment variable to 0/1/2 to force the *maximum* x86 ISA limit to interpreter/sse2/avx2, respectively. The actual ISA used will be determined based on runtime hardware capabilities and the limit (default to no limit).

When reporting issues, please also try limiting the ISA to a lower level (at least try setting `CPU_LEVEL` to 0 to force using the interpreter) and see the problem still persists.
