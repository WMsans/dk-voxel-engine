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
  (no cel/outline/compositor passes) and `minimal` (also no SSAO, SSIL, glow, shadows), then an orbit
  with streaming. VSync is off. A tiling window manager can override the window size; the size used is
  written to the results.

The extension only ships a `template_debug` build, so numbers are for that build.
