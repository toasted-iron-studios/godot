/**************************************************************************/
/*  zippy_ledger.h                                                        */
/**************************************************************************/

#pragma once

// Frame ledger for profiling builds (scons profiler=zippy): every main-thread frame, from one loop top to the
// next, is divided into the exclusive time of named zones. Enabled at launch by ZIPPY_LEDGER (output file) and
// ZIPPY_LEDGER_LEVEL (off, light or full; full when unset). Light records the engine's static zones, GPU pass
// timestamps and events; full adds keyed zones for script calls, native internal processing and render passes.

#include "core/typedefs.h"

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

class String;
class StringName;

namespace ZippyLedger {

enum Kind : uint8_t {
	KIND_ZONE,
	KIND_CONTAINER, // The run loop and Main::iteration: their own time is not attributed to any work.
	KIND_CSHARP,
	KIND_GDSCRIPT,
	KIND_NATIVE_PROCESS,
	KIND_RENDER,
	KIND_CALLABLE,
	KIND_GPU,
	KIND_SHADER,
};

enum Counter : uint8_t {
	COUNTER_UPLOAD_BUFFER_BYTES,
	COUNTER_UPLOAD_BUFFERS,
	COUNTER_UPLOAD_TEXTURE_BYTES,
	COUNTER_UPLOAD_TEXTURES,
	COUNTER_PIPELINE_COMPILES,
	COUNTER_LOADS,
	COUNTER_MAX,
};

struct Site {
	const char *name;
	const char *function;
	const char *file;
	uint32_t line;
	uint32_t id;
};

constexpr int LEVEL_LIGHT = 1;
constexpr int LEVEL_FULL = 2;
constexpr uint32_t KEYED_SLOTS = 1 << 16;

extern int level;
extern thread_local bool tl_main;
extern const void *keyed_a[KEYED_SLOTS];
extern const void *keyed_b[KEYED_SLOTS];
extern uint32_t keyed_id[KEYED_SLOTS];

uint64_t monotonic_ns();

_FORCE_INLINE_ uint64_t ticks() {
#if defined(__x86_64__) || defined(_M_X64)
	return __rdtsc();
#else
	return monotonic_ns();
#endif
}

void initialize();
void finalize();
void frame_mark();

uint32_t begin_site(Site *p_site);
uint32_t begin_id(uint32_t p_id);
uint32_t next_site(uint32_t p_depth, Site *p_site);
void end(uint32_t p_depth);

uint32_t add_keyed(const void *p_a, const void *p_b, uint8_t p_kind, const String &p_name);
void render_marker(const String &p_name);

void count(Counter p_counter, uint64_t p_value);
void upload(bool p_texture, uint64_t p_bytes);
void event(const char *p_kind, const String &p_name, uint64_t p_begin_ns, uint64_t p_end_ns, uint64_t p_value);
void gpu_frame(uint64_t p_frame, uint32_t p_count, const String *p_names, const uint64_t *p_cpu_usec, const uint64_t *p_gpu_ns);
void gpu_calibration(uint64_t p_gpu_ns, uint64_t p_cpu_ns, uint64_t p_deviation_ns);

// Frame structure: at full level, one frame in STRUCTURE_INTERVAL records every render and compute pass it records.
constexpr uint64_t STRUCTURE_INTERVAL = 120;
enum Load : uint8_t {
	LOAD_LOAD,
	LOAD_CLEAR,
	LOAD_IGNORE,
};
extern bool structure_frame;
void pass_begin(bool p_compute, uint32_t p_width, uint32_t p_height);
void pass_attachment(const void *p_texture, const char *p_format, uint32_t p_samples, Load p_load, bool p_depth);
void pass_shader(const String &p_shader);
void pass_texture(const void *p_texture, bool p_write);
void pass_draw(uint32_t p_vertices, uint32_t p_instances, bool p_indexed);
void pass_dispatch(uint32_t p_x, uint32_t p_y, uint32_t p_z);
void pass_end();

// Experiment kit (measurement only, chosen at launch while the ledger is on): passes of renderer sections whose name holds
// ZIPPY_KIT_SKIP_DRAWS record no draws, those holding ZIPPY_KIT_SCISSOR draw into a 1x1 region, and ZIPPY_KIT_GPU_DELAY=N adds N
// clears of a 64 MiB buffer to every frame, timed as the renderer section "Zippy GPU Delay".
extern bool kit_active;
extern int kit_gpu_delay;
bool kit_skip_draws();
bool kit_scissor();

// A zone keyed by two pointers, named on first sight by p_name(); 0 unless full level on the main thread.
template <typename F>
_FORCE_INLINE_ uint32_t keyed(const void *p_a, const void *p_b, uint8_t p_kind, F &&p_name) {
	if (likely(level < LEVEL_FULL) || !tl_main) {
		return 0;
	}
	uint32_t slot = uint32_t((uintptr_t(p_a) >> 4) * 0x9E3779B1u ^ (uintptr_t(p_b) >> 3) * 0x85EBCA77u) & (KEYED_SLOTS - 1);
	for (uint32_t probe = 0; probe < 16; probe++, slot = (slot + 1) & (KEYED_SLOTS - 1)) {
		if (keyed_a[slot] == p_a && keyed_b[slot] == p_b) {
			return keyed_id[slot];
		}
		if (keyed_a[slot] == nullptr) {
			break;
		}
	}
	return add_keyed(p_a, p_b, p_kind, p_name());
}

} // namespace ZippyLedger

class ZippyLedgerZone {
	uint32_t depth = 0;

public:
	_FORCE_INLINE_ explicit ZippyLedgerZone(ZippyLedger::Site *p_site) {
		if (unlikely(ZippyLedger::level)) {
			depth = ZippyLedger::begin_site(p_site);
		}
	}
	_FORCE_INLINE_ explicit ZippyLedgerZone(uint32_t p_id) {
		if (unlikely(p_id)) {
			depth = ZippyLedger::begin_id(p_id);
		}
	}
	_FORCE_INLINE_ ~ZippyLedgerZone() {
		if (unlikely(depth)) {
			ZippyLedger::end(depth);
		}
	}
	_FORCE_INLINE_ void next(ZippyLedger::Site *p_site) {
		if (unlikely(ZippyLedger::level)) {
			depth = ZippyLedger::next_site(depth, p_site);
		}
	}
	_FORCE_INLINE_ void end_now() {
		if (depth) {
			ZippyLedger::end(depth);
			depth = 0;
		}
	}
};

// An event timed from construction to destruction on any thread; the name is built only when the ledger is on.
class ZippyLedgerEvent {
	const char *kind;
	ZippyLedger::Counter counter;
	uint64_t begin = 0;

public:
	_FORCE_INLINE_ ZippyLedgerEvent(const char *p_kind, ZippyLedger::Counter p_counter) :
			kind(p_kind), counter(p_counter) {
		if (unlikely(ZippyLedger::level)) {
			begin = ZippyLedger::monotonic_ns();
		}
	}
	template <typename F>
	_FORCE_INLINE_ void finish(F &&p_name, uint64_t p_value = 0) {
		if (unlikely(begin)) {
			ZippyLedger::event(kind, p_name(), begin, ZippyLedger::monotonic_ns(), p_value);
			ZippyLedger::count(counter, 1);
			begin = 0;
		}
	}
};
