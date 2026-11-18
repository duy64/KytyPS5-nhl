#ifndef KYTY_COMMON_HITCH_WHY_H_
#define KYTY_COMMON_HITCH_WHY_H_

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// NHL27HITCHWHY: where the time of a slow presented frame went. Always on (KYTY_HITCH_WHY=0 turns
// it off), so a scored run and a player's log name their own hitches. Spans sit only at slow
// events (a fault, a readback, an unmap, a compile, a wait), never per draw.
//
//   NHL27HITCHWHY t=304.82 dt=518.5ms rate=0 chg=120 flip=503.1 d_idle=3.2 c_idle=480.0 d_rest=1.4
//                 g_read=0.3+450.2+12.0/3/300.1 d_unmap=0.4+35.0+0.0/2/30.1 ...
//
// Printed for a frame over 40 ms, or over 22 ms while the game asks for 60 Hz (rate=0). chg = ms
// since the flip rate last changed (left out after 5 s), flip = the longest gap between two flip
// requests of the game inside the frame, d_idle / c_idle = how long the draw thread and the
// command thread waited for work, d_slot = how long the draw thread waited for a resolve thread,
// d_rest = draw-thread time nothing else covers (the draws themselves).
// A cause reads self+wait+hop/count/max in ms: its own work, its waits for the GPU or the
// scheduler, its waits for a command on the draw thread (on the draw thread itself: the command's
// own run time), how many, and the longest one in full.
// The prefix is the thread: d_ draw (execution) thread, c_ command thread, p_ the presenting
// thread, q_ the scheduler's thread that waits for the GPU and then runs queued readbacks (a game
// thread waiting for such a readback shows the same interval as its own wait), f_ the game thread
// that asks for flips, r_ the resolve threads, g_ every other thread (the game's other threads,
// compile workers). Times are exclusive: a span leaves out the spans inside it, and a wait counts
// for the work it is inside, so the causes of one thread add up.
//
//   NHL27WHYSUM t=310.00 win=5.01 frames=300 slow=3 rate=0 mixed=0 dt=5010 d_idle=1200 d_slot=40
//               d_rest=3600 c_idle=900 c_rest=3900 f_read=2.0+310.5+20.1/61/9.8 ...
//
// The same sums over every frame of about 5 s, slow or not (ms, max = the longest single one):
// what a normal frame costs. mixed=1 when the flip rate changed inside. c_rest = command-thread
// time nothing else covers.
namespace Common::HitchWhy {

enum class Cause : uint8_t {
	None, // A wait outside any work span.
	Fault,
	Read, // Readback of GPU-written memory (BufferCache::ReadMemory).
	Unmap,
	Pipe,        // Pipeline creation.
	Compile,     // Shader permutation compile.
	CompileWait, // The draw thread waits for a worker's compile.
	Urgent,      // The draw thread runs commands other threads wait for.
	Meta,        // Readback of GPU-written color metadata.
	Io,          // Guest file reads.
	Count
};

enum class Kind : uint8_t {
	Work,
	Wait, // For the GPU or the scheduler.
	Hop   // For a command to run on the draw thread.
};

enum class Role : uint8_t { Other, Draw, Cp, Present, Prio, Flip, Resolve, Count };

[[nodiscard]] inline bool Enabled() noexcept {
	static const bool on = [] {
		const char* value = std::getenv("KYTY_HITCH_WHY");
		return value == nullptr || value[0] != '0';
	}();
	return on;
}

[[nodiscard]] inline uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	        .count());
}

namespace Detail {
constexpr int Roles    = static_cast<int>(Role::Count);
constexpr int Causes   = static_cast<int>(Cause::Count);
constexpr int MaxDepth = 8;

struct Line {
	double   self  = 0.0;
	double   wait  = 0.0;
	double   hop   = 0.0;
	double   max   = 0.0;
	uint64_t count = 0;
};

struct Slot {
	std::atomic<uint64_t> self_ns {0};
	std::atomic<uint64_t> wait_ns {0};
	std::atomic<uint64_t> hop_ns {0};
	std::atomic<uint64_t> count {0};
	std::atomic<uint64_t> max_ns {0};
};

// One writer each (the draw thread, the command thread), written at every idle gap.
struct alignas(64) Idle {
	std::atomic<uint64_t> ns {0};
};

// The open spans of a thread. Indices, not pointers: a span whose destructor never runs (a
// thread that ends inside one) can only mislabel later spans, never touch a dead stack frame.
struct Stack {
	uint64_t child_ns[MaxDepth];
	Cause    cause[MaxDepth];
	Kind     kind[MaxDepth];
	uint8_t  depth;
	Role     role;
};

inline Slot                  g_slots[Roles][Causes];
inline Idle                  g_idle[Roles];
inline Idle                  g_draw_slot; // The draw thread waited for a resolve thread (one writer).
inline std::atomic<uint64_t> g_flip_last_ns {0};
inline std::atomic<uint64_t> g_flip_gap_ns {0};
inline std::atomic<int>      g_rate {0};
inline std::atomic<uint64_t> g_rate_change_ns {0};
inline std::atomic<uint64_t> g_left_play_ns {0}; // When the rate last went from 60 Hz to a lower one.
inline thread_local Stack    t_stack {};

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
inline void Append(char* text, size_t size, size_t& used, const char* format, ...) noexcept {
	if (used >= size) {
		return;
	}
	va_list args;
	va_start(args, format);
	const int n = std::vsnprintf(text + used, size - used, format, args);
	va_end(args);
	used += n > 0 ? static_cast<size_t>(n) : 0;
}

inline void RaiseMax(std::atomic<uint64_t>& max, uint64_t value) noexcept {
	auto previous = max.load(std::memory_order_relaxed);
	while (previous < value && !max.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
	}
}

inline void AppendCauses(char* text, size_t size, size_t& used, const Line (&lines)[Roles][Causes], double least) noexcept {
	static constexpr const char* role_names[Roles]   = {"g_", "d_", "c_", "p_", "q_", "f_", "r_"};
	static constexpr const char* cause_names[Causes] = {"wait",    "fault", "read",   "unmap", "pipe",
	                                                     "compile", "cwait", "urgent", "meta",  "io"};
	for (int r = 0; r < Roles; r++) {
		for (int c = 0; c < Causes; c++) {
			const auto& line = lines[r][c];
			if (line.self + line.wait + line.hop >= least) {
				Append(text, size, used, " %s%s=%.1f+%.1f+%.1f/%llu/%.1f", role_names[r], cause_names[c], line.self, line.wait, line.hop,
				       static_cast<unsigned long long>(line.count), line.max);
			}
		}
	}
}

inline void Print(char* text, size_t size, size_t used) noexcept {
	if (used >= size - 1) {
		used = size - 2;
	}
	text[used]     = '\n';
	text[used + 1] = '\0';
	std::fputs(text, stdout);
}
} // namespace Detail

// Which thread this is; the draw and command threads set it once when they start.
inline void SetRole(Role role) noexcept {
	Detail::t_stack.role = role;
}

// Lock-free and allocation-free: a span may sit in the page-fault path.
class Span final {
public:
	explicit Span(Cause cause, Kind kind = Kind::Work, bool active = true) noexcept {
		if (!active || !Enabled()) {
			return;
		}
		auto& stack = Detail::t_stack;
		if (stack.depth >= Detail::MaxDepth) {
			return;
		}
		m_depth                 = stack.depth++;
		stack.child_ns[m_depth] = 0;
		stack.cause[m_depth]    = cause;
		stack.kind[m_depth]     = kind;
		m_start                 = NowNs();
	}
	explicit Span(Kind wait) noexcept: Span(Cause::None, wait) {}
	~Span() {
		if (m_depth == Inactive) {
			return;
		}
		auto&          stack   = Detail::t_stack;
		const uint64_t elapsed = NowNs() - m_start;
		const uint64_t inside  = stack.child_ns[m_depth];
		const uint64_t self    = elapsed > inside ? elapsed - inside : 0;
		const Kind     kind    = stack.kind[m_depth];
		Cause          cause   = stack.cause[m_depth];
		stack.depth            = m_depth;
		if (m_depth > 0) {
			stack.child_ns[m_depth - 1] += elapsed;
		}
		if (kind != Kind::Work) {
			// A wait counts for the work it is inside.
			cause = Cause::None;
			for (int i = static_cast<int>(m_depth) - 1; i >= 0; i--) {
				if (stack.kind[i] == Kind::Work) {
					cause = stack.cause[i];
					break;
				}
			}
		}
		auto& slot = Detail::g_slots[static_cast<int>(stack.role)][static_cast<int>(cause)];
		(kind == Kind::Work ? slot.self_ns : kind == Kind::Wait ? slot.wait_ns : slot.hop_ns)
		    .fetch_add(self, std::memory_order_relaxed);
		if (kind == Kind::Work || cause == Cause::None) {
			slot.count.fetch_add(1, std::memory_order_relaxed);
			Detail::RaiseMax(slot.max_ns, elapsed);
		}
	}
	Span(const Span&)            = delete;
	Span& operator=(const Span&) = delete;

private:
	static constexpr uint8_t Inactive = 0xff;
	uint64_t                 m_start  = 0;
	uint8_t                  m_depth  = Inactive;
};

// The draw thread or the command thread waited this long for work (their own thread only).
inline void AddIdle(Role role, uint64_t ns) noexcept {
	auto& idle = Detail::g_idle[static_cast<int>(role)].ns;
	idle.store(idle.load(std::memory_order_relaxed) + ns, std::memory_order_relaxed);
}

class IdleScope final {
public:
	explicit IdleScope(Role role) noexcept: m_start(Enabled() ? NowNs() : 0), m_role(role) {}
	~IdleScope() {
		if (m_start != 0) {
			AddIdle(m_role, NowNs() - m_start);
		}
	}
	IdleScope(const IdleScope&)            = delete;
	IdleScope& operator=(const IdleScope&) = delete;

private:
	uint64_t m_start;
	Role     m_role;
};

// The draw thread waits for a resolve thread to finish an operation's inputs.
class SlotWait final {
public:
	SlotWait() noexcept: m_start(Enabled() ? NowNs() : 0) {}
	~SlotWait() {
		if (m_start != 0) {
			auto& slot = Detail::g_draw_slot.ns;
			slot.store(slot.load(std::memory_order_relaxed) + (NowNs() - m_start), std::memory_order_relaxed);
		}
	}
	SlotWait(const SlotWait&)            = delete;
	SlotWait& operator=(const SlotWait&) = delete;

private:
	uint64_t m_start;
};

// The game asked for a flip.
inline void NoteFlipRequest() noexcept {
	if (!Enabled()) {
		return;
	}
	if (Detail::t_stack.role == Role::Other) {
		Detail::t_stack.role = Role::Flip;
	}
	const uint64_t now  = NowNs();
	const uint64_t last = Detail::g_flip_last_ns.exchange(now, std::memory_order_relaxed);
	if (last != 0 && now > last) {
		Detail::RaiseMax(Detail::g_flip_gap_ns, now - last);
	}
}

// The game changed the flip rate it asks for (0 = 60 Hz, 1 = 30 Hz).
inline void NoteRate(int rate) noexcept {
	const auto now      = NowNs();
	const int  previous = Detail::g_rate.exchange(rate, std::memory_order_relaxed);
	if (previous == 0 && rate != 0) {
		Detail::g_left_play_ns.store(now, std::memory_order_relaxed);
	}
	Detail::g_rate_change_ns.store(now, std::memory_order_relaxed);
}

// The flip rate the game asks for now (0 = 60 Hz: play; NHL 27 runs menus, editors and scenes at
// 30 Hz). Kept whether or not the hitch line is on. Any thread.
[[nodiscard]] inline int Rate() noexcept {
	return Detail::g_rate.load(std::memory_order_relaxed);
}

// Whether the game asks for 60 Hz, or stopped doing so less than hold_ms ago. Any thread.
// False until the game first set a flip rate (boot screens).
[[nodiscard]] inline bool InPlay(uint64_t hold_ms = 0) noexcept {
	if (Detail::g_rate_change_ns.load(std::memory_order_relaxed) == 0) {
		return false;
	}
	if (Rate() == 0) {
		return true;
	}
	if (hold_ms == 0) {
		return false;
	}
	const auto left = Detail::g_left_play_ns.load(std::memory_order_relaxed);
	const auto now  = NowNs();
	return left != 0 && now >= left && now - left < hold_ms * 1000000ull;
}

// Once per presented frame, from the presenting thread only. t_s is the time NHL27HITCH prints.
inline void Frame(double t_s, double dt_ms) noexcept {
	if (!Enabled()) {
		return;
	}
	if (Detail::t_stack.role == Role::Other) {
		Detail::t_stack.role = Role::Present;
	}
	using Detail::Line;
	struct Seen {
		uint64_t self, wait, hop, count;
	};
	struct Window {
		Line     lines[Detail::Roles][Detail::Causes];
		double   idle[Detail::Roles];
		double   slot, dt, start_s;
		uint64_t start_ns;
		int      frames, slow;
		bool     open;
	};
	static Seen     seen[Detail::Roles][Detail::Causes] {};
	static uint64_t seen_idle[Detail::Roles] {};
	static uint64_t seen_slot = 0;
	static Window   window {};
	Line            lines[Detail::Roles][Detail::Causes];
	double          idle_ms[Detail::Roles];
	double          spans_ms[Detail::Roles] {};
	const auto      ms     = [](uint64_t ns) { return static_cast<double>(ns) / 1e6; };
	const uint64_t  now_ns = NowNs();
	if (!window.open) {
		window          = Window {};
		window.open     = true;
		window.start_s  = t_s - dt_ms / 1000.0;
		window.start_ns = now_ns;
	}
	for (int r = 0; r < Detail::Roles; r++) {
		const uint64_t idle = Detail::g_idle[r].ns.load(std::memory_order_relaxed);
		idle_ms[r]          = ms(idle - seen_idle[r]);
		seen_idle[r]        = idle;
		window.idle[r] += idle_ms[r];
		for (int c = 0; c < Detail::Causes; c++) {
			auto&      slot = Detail::g_slots[r][c];
			auto&      was  = seen[r][c];
			const Seen now {slot.self_ns.load(std::memory_order_relaxed), slot.wait_ns.load(std::memory_order_relaxed),
			                slot.hop_ns.load(std::memory_order_relaxed), slot.count.load(std::memory_order_relaxed)};
			const uint64_t max =
			    slot.max_ns.load(std::memory_order_relaxed) != 0 ? slot.max_ns.exchange(0, std::memory_order_relaxed) : 0;
			auto& line = lines[r][c];
			line       = {ms(now.self - was.self), ms(now.wait - was.wait), ms(now.hop - was.hop), ms(max), now.count - was.count};
			was        = now;
			spans_ms[r] += line.self + line.wait + line.hop;
			auto& sum = window.lines[r][c];
			sum.self += line.self;
			sum.wait += line.wait;
			sum.hop += line.hop;
			sum.count += line.count;
			sum.max = line.max > sum.max ? line.max : sum.max;
		}
	}
	const uint64_t slot_ns = Detail::g_draw_slot.ns.load(std::memory_order_relaxed);
	const double   slot_ms = ms(slot_ns - seen_slot);
	seen_slot              = slot_ns;
	const uint64_t flip_gap =
	    Detail::g_flip_gap_ns.load(std::memory_order_relaxed) != 0 ? Detail::g_flip_gap_ns.exchange(0, std::memory_order_relaxed) : 0;
	const int      rate    = Detail::g_rate.load(std::memory_order_relaxed);
	const uint64_t changed = Detail::g_rate_change_ns.load(std::memory_order_relaxed);
	const bool     slow    = dt_ms > (rate == 0 ? 22.0 : 40.0);
	const int      draw    = static_cast<int>(Role::Draw);
	const int      cp      = static_cast<int>(Role::Cp);
	window.slot += slot_ms;
	window.dt += dt_ms;
	window.frames++;
	window.slow += slow ? 1 : 0;
	char   text[4096];
	size_t used = 0;
	if (slow) {
		// A wait is booked when it ends, so one that began in an earlier frame is cut to this frame.
		const double draw_idle = idle_ms[draw] < dt_ms ? idle_ms[draw] : dt_ms;
		const double cp_idle   = idle_ms[cp] < dt_ms ? idle_ms[cp] : dt_ms;
		const double rest      = dt_ms - draw_idle - slot_ms - spans_ms[draw];
		Detail::Append(text, sizeof(text), used, "NHL27HITCHWHY t=%.2f dt=%.1fms rate=%d", t_s, dt_ms, rate);
		if (changed != 0) {
			const double since = ms(now_ns - changed);
			if (since < 5000.0) {
				Detail::Append(text, sizeof(text), used, " chg=%.0f", since);
			}
		}
		Detail::Append(text, sizeof(text), used, " flip=%.1f d_idle=%.1f c_idle=%.1f d_slot=%.1f d_rest=%.1f", ms(flip_gap), draw_idle, cp_idle,
		               slot_ms, rest > 0.0 ? rest : 0.0);
		Detail::AppendCauses(text, sizeof(text), used, lines, 0.1);
		Detail::Print(text, sizeof(text), used);
	}
	if (t_s - window.start_s >= 5.0) {
		double draw_spans = 0.0;
		double cp_spans   = 0.0;
		for (int c = 0; c < Detail::Causes; c++) {
			draw_spans += window.lines[draw][c].self + window.lines[draw][c].wait + window.lines[draw][c].hop;
			cp_spans += window.lines[cp][c].self + window.lines[cp][c].wait + window.lines[cp][c].hop;
		}
		const double draw_rest = window.dt - window.idle[draw] - window.slot - draw_spans;
		const double cp_rest   = window.dt - window.idle[cp] - cp_spans;
		used                   = 0;
		Detail::Append(text, sizeof(text), used,
		               "NHL27WHYSUM t=%.2f win=%.2f frames=%d slow=%d rate=%d mixed=%d dt=%.0f d_idle=%.0f d_slot=%.0f d_rest=%.0f c_idle=%.0f "
		               "c_rest=%.0f",
		               t_s, t_s - window.start_s, window.frames, window.slow, rate, changed >= window.start_ns ? 1 : 0, window.dt,
		               window.idle[draw], window.slot, draw_rest > 0.0 ? draw_rest : 0.0, window.idle[cp], cp_rest > 0.0 ? cp_rest : 0.0);
		Detail::AppendCauses(text, sizeof(text), used, window.lines, 1.0);
		Detail::Print(text, sizeof(text), used);
		window.open = false;
	}
}

} // namespace Common::HitchWhy

#endif /* KYTY_COMMON_HITCH_WHY_H_ */
