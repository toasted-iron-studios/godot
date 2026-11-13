/**************************************************************************/
/*  latency_trace.cpp                                                     */
/**************************************************************************/

#include "latency_trace.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>

namespace LatencyTrace {

static FILE *open_trace_file() {
	const char *path = getenv("GODOT_LATENCY_LOG");
	if (path == nullptr || path[0] == '\0') {
		return nullptr;
	}
	FILE *file = fopen(path, "w");
	if (file != nullptr) {
		setvbuf(file, nullptr, _IOFBF, 1 << 20);
	}
	return file;
}

static FILE *trace_file = open_trace_file();
static std::mutex draw_ids_mutex;
static std::deque<uint64_t> draw_ids;

std::atomic<uint64_t> current_draw_id{ 0 };
std::atomic<uint64_t> events_ns{ 0 };
std::atomic<int64_t> last_fence_wait_ns{ 0 };

bool enabled() {
	return trace_file != nullptr;
}

uint64_t now_ns() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

void log(const char *p_format, ...) {
	if (trace_file == nullptr) {
		return;
	}
	va_list args;
	va_start(args, p_format);
	vfprintf(trace_file, p_format, args);
	va_end(args);
}

void flush() {
	if (trace_file != nullptr) {
		fflush(trace_file);
	}
}

void push_draw_id(uint64_t p_id) {
	std::lock_guard<std::mutex> lock(draw_ids_mutex);
	draw_ids.push_back(p_id);
}

uint64_t pop_draw_id() {
	std::lock_guard<std::mutex> lock(draw_ids_mutex);
	if (draw_ids.empty()) {
		return UINT64_MAX;
	}
	uint64_t id = draw_ids.front();
	draw_ids.pop_front();
	return id;
}

} //namespace LatencyTrace
