# Benchmarks

```
godot --headless --path . -s benchmarks/bench_core.gd            # engine: generation, streaming, edits, queries
godot --headless --path . -s benchmarks/bench_core.gd -- --quick # fewer repetitions
godot --path . -s benchmarks/bench_render.gd                     # the demo scene, windowed: fps, GPU ms
godot --path . -s benchmarks/bench_render.gd -- --profile=level --frames=150 --resolution=1280x720
```

Each run prints a table and writes `benchmarks/results/<core|render>_<time>.json` with the machine info.

- `bench_lib.gd`: shared timing, percentile and result/JSON helpers.
- `bench_core.gd`: a bounded procedural world meshed with 1/2/4/8/default worker threads, the mini level,
  an endless world's first load (with and without far levels), a viewer flying at 10/30/100 m/s in fixed
  60 Hz steps, and on a settled world: edit call cost and edit-to-remesh latency per tool, raycasts,
  `get_density` and glb tree voxelizing. It sets `OS.low_processor_usage_mode_sleep_usec = 0`, because
  headless Godot otherwise sleeps 6.9 ms every frame.
- `bench_render.gd`: `demo/main.tscn` per profile, three fixed views under `full` (as shipped), `no_post`
  (no `CelEffect` pass) and `minimal` (also no SSAO, SSIL, glow, shadows, each only where the scene has
  it), then an orbit with streaming, then `play`: walking while digging at the tools' rate and bombing
  every 1.5 s, and a fly-mode sprint, with `frames_over_16.7ms`. The demo's `DynamicResolution` stays
  active, so every row records the `render_scale` it ran at. VSync is off. A tiling window manager can
  override the window size; the size used is written to the results.

M1, 1080p, frame p99 in ms (`render_2026-11-20T12-31-41` before, `render_2026-11-20T13-59-14` after):

| | before | after | render scale |
|---|---|---|---|
| level views (full) | 25.6 / 29.3 / 26.9 | 10.9 / 14.7 / 11.8 | 1.0 |
| level orbit | 26.1 | 11.2 | 1.0 |
| level play walk+dig / fly | (not measured) | 15.8 / 14.4 | 1.0 |
| procedural views (full) | 36.0 / 34.1 / 34.9 | 15.6 / 15.5 / 15.3 | 0.75–0.85 |
| procedural orbit | 44.5 | 17.0 | 0.7 |
| procedural play walk+dig / fly | (not measured) | 15.9 / 15.3 | 0.7 |

Without dynamic resolution the procedural views are 17.9 / 16.3 ms p50 (10 km view distance, ~3.1 M
triangles); the level profile never leaves native resolution.

The extension only ships a `template_debug` build, so numbers are for that build.
