To build a standalone plugin:
```
g++ -o akarin2.dll -shared -gdb ngx*.cc -I. -I ../include -static -DSTANDALONE_NGX
```

To use the plugin:
Rename the patched `nvngx_dlisr.dll` file as `akarin2.dlisr.dll`, and put in the
same directory as `akarin2.dll`:

```
core.std.LoadPlugin(r'/absolute/path/to/akarin2.dll')
c = core.std.BlankClip(format=vs.RGBS)   # only support RGBS clips
res = core.akarin2.DLISR(c, scale=2)
res.set_output()
```
