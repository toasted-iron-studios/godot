/**************************************************************************/
/*  zippy_ledger.cpp                                                      */
/**************************************************************************/

#include "core/profiling/profiling.h"

#ifdef GODOT_USE_ZIPPY

#include "core/config/engine.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"

#include <time.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ZippyLedger {

int level = 0;
thread_local bool tl_main = false;
const void *keyed_a[KEYED_SLOTS] = {};
const void *keyed_b[KEYED_SLOTS] = {};
uint32_t keyed_id[KEYED_SLOTS] = {};

static constexpr uint32_t MAX_IDS = 1 << 14;
static constexpr uint32_t OVERFLOW_ID = 1;
static constexpr int MAX_DEPTH = 96;
static const char *KIND_NAMES[] = { "zone", "container", "csharp", "gdscript", "native-process", "render", "callable", "gpu" };
static const char *COUNTER_NAMES[] = { "upload.buffer.bytes", "upload.buffers", "upload.texture.bytes", "upload.textures", "pipeline.compiles", "loads" };

struct Entry {
	uint32_t id;
	bool marker;
	uint64_t start;
	uint64_t child;
};

struct Accum {
	uint64_t ticks;
	uint32_t count;
	uint32_t serial;
};

static Entry stack[MAX_DEPTH];
static uint32_t depth = 0;
static Accum accum[MAX_IDS];
static uint32_t touched[MAX_IDS];
static uint32_t touched_count = 0;
static uint32_t serial = 1;
static uint32_t next_id = 2;
static uint64_t frame_zones = 0;
static uint64_t frame_lost = 0;
static uint64_t frame_misnest = 0;
static std::atomic<uint64_t> counters[COUNTER_MAX];

static uint64_t frame_seq = 0;
static uint64_t frame_start_ticks = 0;
static uint64_t frame_start_ns = 0;
static uint64_t calibration_ticks = 0;
static uint64_t calibration_ns = 0;
static double ns_per_tick = 1.0;
static double self_ns_per_zone = 0.0;

static HashMap<String, uint32_t> render_ids;
static HashMap<String, uint32_t> keyed_names;
static HashMap<uint32_t, uint32_t> keyed_aliases;
static uint64_t keyed_unstored = 0;
static HashMap<String, uint32_t> gpu_ids;

static FILE *out = nullptr;
static std::mutex queue_mutex;
static std::condition_variable queue_cv;
static std::vector<uint64_t> queue_words;
static std::vector<uint64_t> frame_words;
static uint32_t self_id = 0;
static std::vector<std::string> queue_text;
static std::thread writer;
static bool stopping = false;

enum Record : uint64_t {
	RECORD_FRAME = 1,
	RECORD_GPU = 2,
	RECORD_CALIBRATION = 3,
};

uint64_t monotonic_ns() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

static void json_string(std::string &r_out, const char *p_text) {
	r_out += '"';
	for (const char *c = p_text; *c; c++) {
		const unsigned char ch = (unsigned char)*c;
		if (ch == '"' || ch == '\\') {
			r_out += '\\';
			r_out += char(ch);
		} else if (ch < 0x20) {
			char escaped[8];
			snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
			r_out += escaped;
		} else {
			r_out += char(ch);
		}
	}
	r_out += '"';
}

static void push_text(std::string &&p_line) {
	std::lock_guard<std::mutex> lock(queue_mutex);
	queue_text.push_back(std::move(p_line));
}

static uint32_t register_name(const char *p_name, const char *p_function, const char *p_file, uint32_t p_line, uint8_t p_kind) {
	if (next_id >= MAX_IDS) {
		return OVERFLOW_ID;
	}
	const uint32_t id = next_id++;
	if (p_kind == KIND_ZONE && (strcmp(p_name, "OS_LinuxBSD::run") == 0 || strcmp(p_name, "Main::iteration") == 0)) {
		p_kind = KIND_CONTAINER;
	}
	std::string line = "{\"n\":" + std::to_string(id) + ",\"name\":";
	json_string(line, p_name);
	line += ",\"kind\":\"";
	line += KIND_NAMES[p_kind];
	line += '"';
	if (p_function) {
		line += ",\"fn\":";
		json_string(line, p_function);
	}
	if (p_file) {
		std::string at = std::string(p_file) + ":" + std::to_string(p_line);
		line += ",\"at\":";
		json_string(line, at.c_str());
	}
	line += "}\n";
	push_text(std::move(line));
	return id;
}

static uint32_t register_string(HashMap<String, uint32_t> &r_map, const String &p_name, uint8_t p_kind) {
	HashMap<String, uint32_t>::Iterator found = r_map.find(p_name);
	if (found) {
		return found->value;
	}
	const uint32_t id = register_name(p_name.utf8().get_data(), nullptr, nullptr, 0, p_kind);
	r_map.insert(p_name, id);
	return id;
}

_FORCE_INLINE_ static void credit(uint32_t p_id, uint64_t p_ticks, uint32_t p_count) {
	Accum &a = accum[p_id];
	if (a.serial != serial) {
		a.serial = serial;
		a.ticks = 0;
		a.count = 0;
		touched[touched_count++] = p_id;
	}
	a.ticks += p_ticks;
	a.count += p_count;
}

_FORCE_INLINE_ static void pop_one(uint64_t p_now) {
	const Entry &e = stack[--depth];
	const uint64_t duration = p_now - e.start;
	credit(e.id, duration - e.child, 1);
	if (depth) {
		stack[depth - 1].child += duration;
	}
}

_FORCE_INLINE_ static uint32_t push(uint32_t p_id, bool p_marker, uint64_t p_now) {
	if (unlikely(depth >= MAX_DEPTH)) {
		frame_lost++;
		return 0;
	}
	stack[depth] = { p_id, p_marker, p_now, 0 };
	frame_zones++;
	return ++depth;
}

_FORCE_INLINE_ static uint32_t site_id(Site *p_site) {
	if (unlikely(p_site->id == 0)) {
		p_site->id = register_name(p_site->name, p_site->function, p_site->file, p_site->line, KIND_ZONE);
	}
	return p_site->id;
}

uint32_t begin_site(Site *p_site) {
	if (!tl_main) {
		return 0;
	}
	return push(site_id(p_site), false, ticks());
}

uint32_t begin_id(uint32_t p_id) {
	return push(p_id, false, ticks());
}

void end(uint32_t p_depth) {
	if (unlikely(p_depth > depth)) {
		frame_misnest++;
		return;
	}
	const uint64_t now = ticks();
	while (depth >= p_depth) {
		pop_one(now);
	}
}

uint32_t next_site(uint32_t p_depth, Site *p_site) {
	if (!tl_main) {
		return 0;
	}
	const uint32_t id = site_id(p_site);
	const uint64_t now = ticks();
	if (p_depth == 0 || p_depth > depth) {
		if (p_depth) {
			frame_misnest++;
		}
		return push(id, false, now);
	}
	while (depth >= p_depth) {
		pop_one(now);
	}
	return push(id, false, now);
}

uint32_t add_keyed(const void *p_a, const void *p_b, uint8_t p_kind, const String &p_name) {
	// Keys that name the same thing (one script class loaded as several script objects) share a row.
	HashMap<String, uint32_t>::Iterator named = keyed_names.find(p_name);
	const uint32_t id = named ? named->value : register_name(p_name.utf8().get_data(), nullptr, nullptr, 0, p_kind);
	if (named) {
		keyed_aliases[id]++;
	} else {
		keyed_names.insert(p_name, id);
	}
	uint32_t slot = uint32_t((uintptr_t(p_a) >> 4) * 0x9E3779B1u ^ (uintptr_t(p_b) >> 3) * 0x85EBCA77u) & (KEYED_SLOTS - 1);
	for (uint32_t probe = 0; probe < 16; probe++, slot = (slot + 1) & (KEYED_SLOTS - 1)) {
		if (keyed_a[slot] == nullptr && p_a != nullptr) {
			keyed_b[slot] = p_b;
			keyed_id[slot] = id;
			keyed_a[slot] = p_a;
			return id;
		}
	}
	keyed_unstored++;
	return id;
}

void render_marker(const String &p_name) {
	if (level < LEVEL_FULL || !tl_main || p_name.is_empty()) {
		return;
	}
	const uint64_t now = ticks();
	if (depth && stack[depth - 1].marker) {
		pop_one(now);
	}
	if (p_name[0] == '<' || p_name[0] == '>') {
		return;
	}
	push(register_string(render_ids, p_name, KIND_RENDER), true, now);
}

void count(Counter p_counter, uint64_t p_value) {
	if (level) {
		counters[p_counter].fetch_add(p_value, std::memory_order_relaxed);
	}
}

void upload(bool p_texture, uint64_t p_bytes) {
	if (!level) {
		return;
	}
	count(p_texture ? COUNTER_UPLOAD_TEXTURE_BYTES : COUNTER_UPLOAD_BUFFER_BYTES, p_bytes);
	count(p_texture ? COUNTER_UPLOAD_TEXTURES : COUNTER_UPLOAD_BUFFERS, 1);
	if (p_bytes >= 256 * 1024) {
		const uint64_t now = monotonic_ns();
		event(p_texture ? "upload.texture" : "upload.buffer", String(), now, now, p_bytes);
	}
}

void event(const char *p_kind, const String &p_name, uint64_t p_begin_ns, uint64_t p_end_ns, uint64_t p_value) {
	if (!level) {
		return;
	}
	std::string line = "{\"e\":";
	json_string(line, p_kind);
	line += ",\"t\":" + std::to_string(p_begin_ns) + ",\"d\":" + std::to_string(p_end_ns - p_begin_ns);
	if (p_value) {
		line += ",\"v\":" + std::to_string(p_value);
	}
	line += tl_main ? ",\"main\":1,\"name\":" : ",\"main\":0,\"name\":";
	json_string(line, p_name.utf8().get_data());
	line += "}\n";
	push_text(std::move(line));
}

void gpu_frame(uint64_t p_frame, uint32_t p_count, const String *p_names, const uint64_t *p_cpu_usec, const uint64_t *p_gpu_ns) {
	if (!level || !tl_main || p_count == 0) {
		return;
	}
	// OS ticks count from engine start on GODOT_CLOCK; the offset maps them onto CLOCK_MONOTONIC.
	const int64_t os_offset_ns = int64_t(monotonic_ns()) - int64_t(OS::get_singleton()->get_ticks_usec() * 1000);
	std::vector<uint64_t> words;
	words.reserve(4 + 3 * p_count);
	words.push_back(RECORD_GPU);
	words.push_back(2 + 3 * p_count);
	words.push_back(p_frame);
	words.push_back(p_count);
	for (uint32_t i = 0; i < p_count; i++) {
		words.push_back(register_string(gpu_ids, p_names[i], KIND_GPU));
		words.push_back(uint64_t(int64_t(p_cpu_usec[i] * 1000) + os_offset_ns));
		words.push_back(p_gpu_ns[i]);
	}
	std::lock_guard<std::mutex> lock(queue_mutex);
	queue_words.insert(queue_words.end(), words.begin(), words.end());
}

void gpu_calibration(uint64_t p_gpu_ns, uint64_t p_cpu_ns, uint64_t p_deviation_ns) {
	if (!level) {
		return;
	}
	std::lock_guard<std::mutex> lock(queue_mutex);
	queue_words.insert(queue_words.end(), { RECORD_CALIBRATION, 3, p_gpu_ns, p_cpu_ns, p_deviation_ns });
}

void frame_mark() {
	if (!level || !tl_main) {
		return;
	}
	const uint64_t now = ticks();
	const uint64_t now_ns = monotonic_ns();
	if (now_ns - calibration_ns > 2000000000ull && now > calibration_ticks) {
		ns_per_tick = double(now_ns - calibration_ns) / double(now - calibration_ticks);
	}
	// Zones open across the mark are credited up to now and continue in the next frame.
	for (uint32_t i = depth; i-- > 0;) {
		const uint64_t open_child = i + 1 < depth ? now - stack[i + 1].start : 0;
		credit(stack[i].id, now - stack[i].start - stack[i].child - open_child, 0);
	}
	for (uint32_t i = 0; i < depth; i++) {
		stack[i].start = now;
		stack[i].child = 0;
	}
	if (frame_start_ticks) {
		std::vector<uint64_t> &words = frame_words;
		words.clear();
		words.push_back(RECORD_FRAME);
		words.push_back(0);
		words.push_back(frame_seq++);
		Engine *engine = Engine::get_singleton();
		words.push_back(engine ? engine->get_process_frames() : 0);
		words.push_back(engine ? engine->get_frames_drawn() : 0);
		words.push_back(frame_start_ns);
		words.push_back(now_ns - frame_start_ns);
		words.push_back(frame_lost);
		words.push_back(frame_misnest);
		words.push_back(frame_zones);
		words.push_back(touched_count);
		for (uint32_t i = 0; i < touched_count; i++) {
			const Accum &a = accum[touched[i]];
			words.push_back(touched[i]);
			words.push_back(uint64_t(double(a.ticks) * ns_per_tick + 0.5));
			words.push_back(a.count);
		}
		const size_t counter_index = words.size();
		words.push_back(0);
		for (int c = 0; c < COUNTER_MAX; c++) {
			const uint64_t value = counters[c].exchange(0, std::memory_order_relaxed);
			if (value) {
				words.push_back(c);
				words.push_back(value);
				words[counter_index]++;
			}
		}
		words[1] = words.size() - 2;
		std::lock_guard<std::mutex> lock(queue_mutex);
		queue_words.insert(queue_words.end(), words.begin(), words.end());
	}
	serial++;
	touched_count = 0;
	frame_zones = 0;
	frame_lost = 0;
	frame_misnest = 0;
	frame_start_ticks = now;
	frame_start_ns = now_ns;
	// The time spent writing this record belongs to the frame that has just begun.
	credit(self_id, ticks() - now, 1);
}

static void format_words(const std::vector<uint64_t> &p_words, std::string &r_out) {
	size_t i = 0;
	while (i + 2 <= p_words.size()) {
		const uint64_t type = p_words[i];
		const uint64_t length = p_words[i + 1];
		const uint64_t *w = p_words.data() + i + 2;
		i += 2 + length;
		if (type == RECORD_FRAME) {
			r_out += "{\"f\":" + std::to_string(w[0]) + ",\"pf\":" + std::to_string(w[1]) + ",\"fd\":" + std::to_string(w[2]) +
					",\"t\":" + std::to_string(w[3]) + ",\"dt\":" + std::to_string(w[4]);
			if (w[5]) {
				r_out += ",\"lost\":" + std::to_string(w[5]);
			}
			if (w[6]) {
				r_out += ",\"misnest\":" + std::to_string(w[6]);
			}
			r_out += ",\"zc\":" + std::to_string(w[7]) + ",\"z\":[";
			const uint64_t zones = w[8];
			for (uint64_t z = 0; z < zones; z++) {
				r_out += (z ? "," : "") + std::to_string(w[9 + 3 * z]) + "," + std::to_string(w[10 + 3 * z]) + "," + std::to_string(w[11 + 3 * z]);
			}
			r_out += "]";
			const uint64_t *c = w + 9 + 3 * zones;
			if (c[0]) {
				r_out += ",\"c\":{";
				for (uint64_t k = 0; k < c[0]; k++) {
					r_out += (k ? ",\"" : "\"") + std::string(COUNTER_NAMES[c[1 + 2 * k]]) + "\":" + std::to_string(c[2 + 2 * k]);
				}
				r_out += "}";
			}
			r_out += "}\n";
		} else if (type == RECORD_GPU) {
			r_out += "{\"g\":" + std::to_string(w[0]) + ",\"ts\":[";
			for (uint64_t k = 0; k < w[1]; k++) {
				r_out += (k ? "," : "") + std::to_string(w[2 + 3 * k]) + "," + std::to_string(w[3 + 3 * k]) + "," + std::to_string(w[4 + 3 * k]);
			}
			r_out += "]}\n";
		} else if (type == RECORD_CALIBRATION) {
			r_out += "{\"cal\":[" + std::to_string(w[0]) + "," + std::to_string(w[1]) + "," + std::to_string(w[2]) + "]}\n";
		}
	}
}

static void drain() {
	// The two word buffers alternate, so neither the main thread's appends nor this swap reallocate once warm.
	static std::vector<uint64_t> words;
	std::vector<std::string> text;
	words.clear();
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		words.swap(queue_words);
		text.swap(queue_text);
	}
	// Names precede the frames that use them: both were queued under one lock and names are written first.
	std::string buffer;
	for (const std::string &line : text) {
		buffer += line;
	}
	format_words(words, buffer);
	if (!buffer.empty()) {
		fwrite(buffer.data(), 1, buffer.size(), out);
		fflush(out);
	}
}

static void writer_loop() {
	std::unique_lock<std::mutex> lock(queue_mutex);
	while (!stopping) {
		queue_cv.wait_for(lock, std::chrono::milliseconds(100));
		lock.unlock();
		drain();
		lock.lock();
	}
}

static double measure_self_cost() {
	static Site site = { "ledger self-cost probe", __FUNCTION__, __FILE__, __LINE__, 0 };
	const uint32_t id = site_id(&site);
	const int pairs = 200000;
	const uint64_t begin_ns = monotonic_ns();
	for (int i = 0; i < pairs; i++) {
		const uint32_t d = begin_id(id);
		end(d);
	}
	const uint64_t elapsed = monotonic_ns() - begin_ns;
	serial++;
	touched_count = 0;
	frame_zones = 0;
	return double(elapsed) / pairs;
}

void initialize() {
	const char *path = getenv("ZIPPY_LEDGER");
	if (path == nullptr || path[0] == '\0') {
		return;
	}
	const char *level_name = getenv("ZIPPY_LEDGER_LEVEL");
	if (level_name != nullptr && strcmp(level_name, "off") == 0) {
		return;
	}
	const int wanted = (level_name != nullptr && strcmp(level_name, "light") == 0) ? LEVEL_LIGHT : LEVEL_FULL;
	out = fopen(path, "wb");
	if (out == nullptr) {
		fprintf(stderr, "zippy ledger: cannot open %s; the ledger is off.\n", path);
		return;
	}
	setvbuf(out, nullptr, _IOFBF, 1 << 20);
	tl_main = true;
	calibration_ticks = ticks();
	calibration_ns = monotonic_ns();
	while (monotonic_ns() - calibration_ns < 20000000ull) {
	}
	ns_per_tick = double(monotonic_ns() - calibration_ns) / double(ticks() - calibration_ticks);
	level = wanted;
	self_ns_per_zone = measure_self_cost();
	self_id = register_name("ledger frame record", __FUNCTION__, __FILE__, __LINE__, KIND_ZONE);
	queue_words.reserve(1 << 20);
	frame_words.reserve(1 << 14);
	std::string header = "{\"zippyLedger\":1,\"level\":\"";
	header += level == LEVEL_FULL ? "full" : "light";
	header += "\",\"clock\":\"CLOCK_MONOTONIC\",\"timeUnit\":\"ns\",\"pid\":" + std::to_string(getpid());
	char cost[64];
	snprintf(cost, sizeof(cost), "%.2f", self_ns_per_zone);
	header += ",\"selfNsPerZone\":";
	header += cost;
	header += ",\"counters\":[";
	for (int c = 0; c < COUNTER_MAX; c++) {
		header += (c ? ",\"" : "\"") + std::string(COUNTER_NAMES[c]) + "\"";
	}
	header += "]}\n";
	fwrite(header.data(), 1, header.size(), out);
	writer = std::thread(writer_loop);
}

void finalize() {
	if (out == nullptr) {
		return;
	}
	const int was = level;
	level = 0;
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		stopping = true;
	}
	queue_cv.notify_all();
	if (writer.joinable()) {
		writer.join();
	}
	drain();
	std::string tail = "{\"end\":true,\"frames\":" + std::to_string(frame_seq) + ",\"level\":" + std::to_string(was);
	// Extra keys per keyed row (a script class seen as several script objects) and lookups the key table could not store.
	tail += ",\"keyedUnstored\":" + std::to_string(keyed_unstored) + ",\"keyedAliases\":{";
	bool first = true;
	for (const KeyValue<uint32_t, uint32_t> &alias : keyed_aliases) {
		tail += (first ? "\"" : ",\"") + std::to_string(alias.key) + "\":" + std::to_string(alias.value);
		first = false;
	}
	tail += "}}\n";
	fwrite(tail.data(), 1, tail.size(), out);
	fclose(out);
	out = nullptr;
	render_ids.clear();
	gpu_ids.clear();
	keyed_names.clear();
	keyed_aliases.clear();
}

} // namespace ZippyLedger

void godot_init_profiler() {
	ZippyLedger::initialize();
}

void godot_cleanup_profiler() {
	ZippyLedger::finalize();
}

#endif // GODOT_USE_ZIPPY
