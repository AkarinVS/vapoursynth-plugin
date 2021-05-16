Plugin akarin
=============

Expr
----

`akarin.Expr(clip[] clips, string[] expr[, int format])`

This works just like `std.Expr`, with the following additions:
- use `x._Combed` to access numerical frame properties of the current frame in the given clip;
- use the `N` operator to load the current frame number;
- use the `X` and `Y` operators to load the current column / row (aka `mt_lutspa`);

To debug, set `$CPU_LEVEL` to 0/1/2 to force interpreter/sse2/avx2.
