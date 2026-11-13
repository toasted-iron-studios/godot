# Soggy engine patches

The `soggy/<version>` branches of this fork are Godot stable tags plus a short series of profiling patches. Every patch
is inert in an ordinary build: the ledger patches compile only with `scons profiler=zippy`, and in such a build they do
nothing unless `ZIPPY_LEDGER` names an output file at launch. Nothing here ships to players.

Branch: `soggy/4.7.2`, based on the `4.7.2-stable` tag. Releases: `4.7.2-soggy.N` (see "Releases" below).

## Building

```bash
scons platform=linuxbsd target=editor module_mono_enabled=yes production=yes lto=full profiler=zippy
```

The editor binary loads the C# API assemblies from `GodotSharp/` beside it. The patches change no bound API, so the
official 4.7.2 .NET release's `GodotSharp` directory is used as is (with `Api/Release` copied over `Api/Debug` for
optimized bindings, as the release archives ship it).

## Running with the frame ledger

| Variable | Meaning |
|---|---|
| `ZIPPY_LEDGER` | Output file (JSON lines). Unset: the ledger is off and the build behaves as stock. |
| `ZIPPY_LEDGER_LEVEL` | `light` (engine zones, GPU pass timestamps, events), `full` (default: adds per-script-class, per-native-class and per-render-pass zones) or `off`. |

The file holds a header (`zippyLedger`, level, clock `CLOCK_MONOTONIC`, `selfNsPerZone`), name records (`n`: id, name,
kind, function, file:line), one frame record per main-loop iteration (`f`: sequence, process frame, frames drawn, start
`t` and duration `dt` in ns, `zc` zones begun, `z` flat triples of zone id, exclusive ns, calls, `c` counters), GPU frames
(`g`: captured frame and triples of timestamp name id, CPU record time in ns, GPU time in ns), GPU clock calibrations
(`cal`: GPU ns, CLOCK_MONOTONIC ns, deviation ns), events (`e`: kind, start, duration, thread, name) and an end record.
A frame runs from one loop top to the next; the exclusive times of its zones plus the time outside any zone add up to it.

## Patches

### 1. Latency trace and render-thread shutdown fix (local measurement patch)

- **Purpose.** `GODOT_LATENCY_LOG=<file>` writes per-frame pipeline timestamps (event poll, iteration, render sync, draw,
  present, GPU completion via a fence-only submit after present). `GODOT_LATENCY_PACING=<us>` is an experimental pacer.
  `RenderingServerDefault::finish` hands the main RenderingDevice back to the calling thread before the display server
  destroys it.
- **Touches.** `core/os/latency_trace.{h,cpp}` (new); `main/main.cpp` (`Main::iteration`);
  `platform/linuxbsd/os_linuxbsd.cpp` (`OS_LinuxBSD::run`); `servers/rendering/rendering_device.cpp` (`swap_buffers`,
  `_stall_for_frame`); `servers/rendering/rendering_server_default.cpp` (`_draw`, `draw`, `finish`);
  `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`command_queue_execute_and_present`).
- **Upstream.** The shutdown fix is https://github.com/godotengine/godot/pull/123391 (open). The latency trace has no
  upstream equivalent; the ledger (patches 2-5) records the same points and is meant to replace it.
- **Verify.** `GODOT_LATENCY_LOG=/tmp/latency.log <binary> --path <project>` writes `M`, `R`, `W` and `G` lines; quitting a
  project with `rendering/driver/threads/thread_model` set to separate threads does not crash at exit.
- **Rebase hazards.** `Main::iteration`'s frame-delay block and `_stall_for_frame` move between versions; drop the shutdown
  hunk once #123391 is merged.

### 2. Frame ledger backend behind the profile macros

- **Purpose.** A `zippy` backend for the `GodotProfile*` macros (`core/profiling/profiling.h`): zones on the main thread
  are timed with the TSC (converted to CLOCK_MONOTONIC ns), kept on a stack so each frame records exclusive time per
  zone, and written by a writer thread. Adds zones for event polling, joypad events, swap chain acquire, queue submit and
  queue present (`OS::add_frame_delay` is the limiter sleep, `driver->fence_wait` the GPU wait). Fixes the physics loop's
  navigation, physics step and iteration-end zones, which switched the frame's zone group while the step's zones were open.
- **Touches.** `core/profiling/zippy_ledger.{h,cpp}` (new); `core/profiling/profiling.{h,cpp}`,
  `core/profiling/profiling_builders.py`, `SConstruct` (`profiler` option); `main/main.cpp` (`Main::iteration`, zone
  groups only); `platform/linuxbsd/os_linuxbsd.cpp` (`OS_LinuxBSD::run`); `servers/rendering/rendering_device.cpp`
  (`screen_prepare_for_drawing`); `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`command_queue_execute_and_present`).
- **Upstream.** The macro layer and the Tracy and Perfetto backends are https://github.com/godotengine/godot/pull/104851
  (merged). No ledger backend upstream. The physics zone nesting fix could go upstream; no PR found.
- **Verify.** Build with `profiler=zippy`; `ZIPPY_LEDGER=/tmp/l.jsonl <binary> --headless --path <empty project> --quit-after 300`
  writes a header, name records including `event poll` and `Main::iteration`, about 300 frame records and an end record.
  Without `ZIPPY_LEDGER` no file is written. A build without `profiler=zippy` is unchanged.
- **Rebase hazards.** New `GodotProfileZone*` call sites upstream compile into the ledger automatically; a grouped zone
  that switches a group from inside a nested scope (as the physics loop did) shows as `misnest` in frame records.
  The backend must keep zones only on the main thread (`tl_main`).

### 3. Scene-tree phase zones and per-class zones where the engine calls scripts

- **Purpose.** `SceneTree::process` and `physics_process` divided into phases; transform notifications and call queue
  flushes as zones. At full level, `GodotProfileZoneKeyed` gives one zone per script class and method where the engine
  calls C# (`CSharpInstance::callp`, notifications, `ManagedCallable::call`), per GDScript function (the existing
  `GodotProfileZoneScript`), and per native class for `NOTIFICATION_INTERNAL_PROCESS` / `_PHYSICS_PROCESS`.
- **Touches.** `scene/main/scene_tree.cpp` (`process`, `physics_process`, `flush_transform_notifications`,
  `_process_group`); `core/object/message_queue.cpp` (`CallQueue::flush`); `core/profiling/profiling.h`;
  `modules/mono/csharp_script.cpp` (`CSharpInstance::callp`, `_call_notification`); `modules/mono/managed_callable.{h,cpp}`
  (`call`, `_profile_owner`, `_profile_name`).
- **Upstream.** The engine's C# profiling hooks are empty stubs in `csharp_script.h`; no equivalent or PR found.
- **Verify.** A C# project at full level writes name records of kind `csharp` named `Class._process` and `native-process`
  records such as `AnimationTree internal process`; their calls per frame match the node counts.
- **Rebase hazards.** `SceneTree::process` reorders phases between versions; keep one grouped zone per phase.
  `CSharpScript::type_info.class_name` and `ManagedCallable`'s fields are internal and may be renamed.

### 4. Render CPU zones named as the GPU passes; GPU timestamps on in the ledger

- **Purpose.** While the ledger is on, the renderer captures its pass timestamps every frame (capacity raised to 4096), each
  `RENDER_TIMESTAMP` also switches a CPU zone of the same name (full level), and every captured frame's timestamps go to the
  ledger. `VK_EXT_calibrated_timestamps` (requested only while the ledger is on) reads a GPU timestamp and CLOCK_MONOTONIC
  together every 60 frames, so GPU passes land on the ledger's clock.
- **Touches.** `servers/rendering/renderer_rd/storage_rd/utilities.cpp` (`capture_timestamp`, `capture_timestamps_begin`);
  `servers/rendering/rendering_server_default.{h,cpp}` (`_draw`, `_ledger_gpu_frame`);
  `servers/rendering/rendering_device.{h,cpp}` (`initialize`, `timestamp_calibrate`);
  `servers/rendering/rendering_device_driver.h` (`timestamp_calibrate`, default false);
  `drivers/vulkan/rendering_device_driver_vulkan.{h,cpp}` (extension request, function pointer, `timestamp_calibrate`).
- **Upstream.** Godot does not use calibrated timestamps; no PR found.
- **Verify.** A rendering project with the ledger on writes `g` records every frame and a `cal` record every 60 frames with a
  deviation of a few microseconds; `render`-kind names match `gpu`-kind names.
- **Rebase hazards.** Metal reports zero timestamps in 4.7 (real in 4.8); a driver without the extension writes no `cal`
  records. `_draw`'s frame-profile block and `max_timestamp_query_elements` move between versions.

### 5. Ledger events for pipeline compiles, resource loads and uploads

- **Purpose.** Render and compute pipeline creation and each resource loader call are events (start, duration, thread, name)
  from any thread; buffer and texture uploads are counted per frame, and one of 256 KiB or more is also an event.
- **Touches.** `servers/rendering/rendering_device.cpp` (`render_pipeline_create`, `compute_pipeline_create`,
  `_buffer_initialize`, `_buffer_update`, `texture_create`, `texture_update`); `core/io/resource_loader.cpp` (`_load`);
  `core/profiling/zippy_ledger.{h,cpp}`, `core/profiling/profiling.h`.
- **Upstream.** None.
- **Verify.** Loading a scene with the ledger on writes `load` events with `res://` paths, and a first frame writes
  `pipeline.render` events named after shaders.
- **Rebase hazards.** Pipeline creation moved to worker threads in 4.4 (events from any thread are supported); the RD upload
  paths are rewritten often.

### 6. Frame structure records for the GPU frame model

- **Purpose.** At full level, one frame in 120 records every render and compute pass in recording order: the renderer section
  (the `RENDER_TIMESTAMP` it is recorded under), framebuffer size, each attachment's texture identity, format, samples, load
  action (load, clear, ignore) and kind, the shaders bound, the textures its uniform sets read and write, draws, vertices,
  instances, indexed draws, dispatches and workgroups. Written as an `s` record with the frame's sequence number.
- **Touches.** `servers/rendering/rendering_device.{h,cpp}` (`draw_list_begin`, `draw_list_bind_render_pipeline`,
  `draw_list_bind_uniform_set`, `draw_list_draw`, `draw_list_end`, `compute_list_begin`, `compute_list_bind_compute_pipeline`,
  `compute_list_bind_uniform_set`, `compute_list_dispatch`, `compute_list_end`, `_ledger_texture_key`, `_ledger_pass_textures`);
  `core/profiling/zippy_ledger.{h,cpp}`.
- **Upstream.** None; RenderDoc and the RD graph's debug labels are the nearest equivalents.
- **Verify.** A full-level ledger of a 3D scene holds `s` records every 120 frames whose passes name sections such as
  `Render Opaque Pass` with depth and color attachments and non-zero draws and vertices.
- **Rebase hazards.** `draw_list_begin`'s attachment loop and the uniform set's `draw_trackers` change between versions; texture
  identity is the render graph tracker (its parent for slices). Indirect draws and `draw_list_begin_for_screen` are not recorded.

## Releases

Each release is a tag `4.7.2-soggy.N` on this branch with a Linux x86_64 archive (`godot-4.7.2-soggy.N-linux-x86_64.tar.gz`:
the editor binary and `GodotSharp/`) and a `SHA256SUMS` file. macOS and Windows builds come later.

## Upgrading to a new Godot version

1. Fetch the tags from godotengine/godot (`git fetch --tags https://github.com/godotengine/godot.git`) and create `soggy/<new-version>` from the new `<new-version>-stable` tag.
2. Cherry-pick the series in order (`git cherry-pick <first>^..<last>` from the old branch), resolving conflicts patch by
   patch with the hazards above in mind; drop a patch whose upstream equivalent has landed and say so here.
3. Rebuild with the command above, matching the game's `Godot.NET.Sdk` pin and the official .NET release's `GodotSharp`.
4. Run every "Verify" step above, then the game's frame ledger runs (each frame's slots within 1% of the frame).
5. Update this file, release `<new-version>-soggy.1`, and keep the old branch.
