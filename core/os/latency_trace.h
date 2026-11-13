/**************************************************************************/
/*  latency_trace.h                                                       */
/**************************************************************************/

#pragma once

#include <atomic>
#include <cstdint>

// Local measurement patch: per-frame pipeline timestamps (CLOCK_MONOTONIC_RAW, ns).
// Active only when the GODOT_LATENCY_LOG environment variable names an output file.
namespace LatencyTrace {
bool enabled();
uint64_t now_ns();
void log(const char *p_format, ...);
void flush();

// Frame ids handed from RenderingServerDefault::draw() (main thread) to _draw() (render thread).
void push_draw_id(uint64_t p_id);
uint64_t pop_draw_id();

// Id of the frame _draw() is currently recording; read by the Vulkan driver at present time.
extern std::atomic<uint64_t> current_draw_id;
// Stamp taken just before DisplayServer::process_events() in the OS run loop.
extern std::atomic<uint64_t> events_ns;
// Duration of the most recent frame-slot fence wait in RenderingDevice::_stall_for_frame.
extern std::atomic<int64_t> last_fence_wait_ns;
} //namespace LatencyTrace
