# CGL dispatch thunks (generator for `../cglzink.m`)

`gl*` calls go through the current CGL context's table (`CGLContextObj->disp`, declared in the SDK's `gliDispatch.h`). These
files make a table whose entries drop the `GLIContext` argument and call a backend such as zink. `../build.sh` runs `gen.py`;
`map.tsv` and `backend_names.h` are measured once and kept here. Research notes: hackintosh tree,
`docs/reports/b580-zink-cgl-research-20261105.md`.

- `gen.py OUT`: from the SDK's `gliDispatch.h` and libGL's exports: `thunks.h` (used by the build), `fake_backend.h` (the
  test stand-in), `map.c`, `check.c`.
- `thunks.h` also holds, under `#ifdef CGLT_MARK`, a marking wrapper per entry (`cglt_mark_table`: runs `CGLT_MARK(i)`,
  then the entry it replaced, `cglt_marked[i]`); `../cglzink.m` puts the pixel-writing entries behind them for
  `CGLZINK_SURF_WRITEBACK=dirty` (the default; not with `flush`).
- `map.c` → `map.tsv`: measured with a numbered trap in every entry: which symbol reaches which entry (1254 of 1275 symbols
  reach 964 entries; arm64 and x86_64 identical).
- `names.py map.tsv thunks.h > backend_names.h`: the GL name a backend resolves for each entry (the core name preferred).
- `check.c`: renders a shader, texture and FBO directly and through the thunks and compares the pixels.
  Build: `clang -arch x86_64 -O1 -w check.c -framework OpenGL -o check`.

Rebuild `map.tsv` and `backend_names.h` only after a macOS update changes the table (`check.c` says "table size differs").
