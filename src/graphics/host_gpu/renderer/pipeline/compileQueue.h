#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_COMPILEQUEUE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_COMPILEQUEUE_H_

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace Libs::Graphics {

// The jobs of the shader compile workers (pipelineCache.cpp), without any GPU type so it is
// tested on its own (tests/CompileQueueTests.cpp).
//
// With two classes (KYTY_COMPILE_URGENT_FIRST=1) the workers take, in this order:
//   urgent  jobs the execution thread is waiting for right now (Promote ... Urgent)
//   live    jobs the resolve thread queued for a draw it has just seen
//   stored  the start-up backlog of the programs of earlier sessions
// each first in, first out. So a draw on screen no longer waits behind a backlog of programs
// nothing asks for yet.
//
// With one class (the default, the queue as it was) every job is taken first in, first out, and
// Take and Promote do nothing.
//
// Only live jobs can be taken back (Take): the execution thread compiles such a program itself
// instead of waiting for a free worker. Stored jobs are never taken back: several of them share
// a program, and compiling one elsewhere would store its record a second time.
enum class CompileClass : uint8_t { Live, Stored };
enum class CompileLevel : uint8_t { Live, Urgent };

struct CompileQueueStats {
	uint64_t taken       = 0; // live jobs taken back (Take)
	uint64_t promoted    = 0; // jobs moved ahead (Promote)
	size_t   live_max    = 0; // most urgent + live jobs waiting at once
	size_t   live_left   = 0; // urgent + live jobs waiting now
	size_t   stored_left = 0; // stored jobs waiting now
};

template <typename Key, typename Job, typename Hash = std::hash<Key>>
class CompileQueue {
public:
	explicit CompileQueue(bool two_classes = false): m_two_classes(two_classes) {}

	CompileQueue(const CompileQueue&)            = delete;
	CompileQueue& operator=(const CompileQueue&) = delete;

	// Before any job is pushed.
	void SetTwoClasses(bool two_classes) {
		const std::lock_guard lock(m_mutex);
		m_two_classes = two_classes;
	}
	[[nodiscard]] bool TwoClasses() const {
		const std::lock_guard lock(m_mutex);
		return m_two_classes;
	}

	void Push(const Key& key, std::unique_ptr<Job> job, CompileClass job_class) {
		{
			const std::lock_guard lock(m_mutex);
			const bool            backlog = m_two_classes && job_class == CompileClass::Stored;
			auto&                 list    = backlog ? m_stored : m_live;
			list.push_back(Node {.key       = key,
			                     .job       = std::move(job),
			                     .job_class = job_class,
			                     .where     = backlog ? Where::Stored : Where::Live,
			                     .sequence  = ++m_next_sequence});
			m_index.emplace(key, std::prev(list.end()));
			NoteSizes();
		}
		m_available.notify_one();
	}

	// A worker: waits for the next job. Null once the queue is stopped (jobs still queued are
	// then left to the destructor).
	[[nodiscard]] std::unique_ptr<Job> Pop() {
		std::unique_lock lock(m_mutex);
		m_available.wait(lock, [this] { return m_stopping || !EmptyLocked(); });
		if (m_stopping) {
			return nullptr;
		}
		auto& list = !m_urgent.empty() ? m_urgent : (!m_live.empty() ? m_live : m_stored);
		return Remove(list, list.begin());
	}

	// The queued live job of the key, removed from the queue. Null when there is none: the key
	// has no job, a worker already runs it, or its jobs are stored ones. Null with one class.
	[[nodiscard]] std::unique_ptr<Job> Take(const Key& key) {
		const std::lock_guard lock(m_mutex);
		if (!m_two_classes) {
			return nullptr;
		}
		const auto [first, last] = m_index.equal_range(key);
		for (auto entry = first; entry != last; ++entry) {
			const auto node = entry->second;
			if (node->job_class == CompileClass::Live) {
				auto& list = node->where == Where::Urgent ? m_urgent : m_live;
				m_stats.taken++;
				return Remove(list, node);
			}
		}
		return nullptr;
	}

	// Moves the queued jobs of the key ahead, keeping their order: Urgent to the front of
	// everything, Live to the end of the live jobs (out of the stored backlog). A job is never
	// moved back. The number of jobs moved; 0 with one class.
	size_t Promote(const Key& key, CompileLevel level) {
		const std::lock_guard lock(m_mutex);
		if (!m_two_classes) {
			return 0;
		}
		const auto target = level == CompileLevel::Urgent ? Where::Urgent : Where::Live;
		// The index gives the jobs of a key in no order: move them in queue order.
		NodeIterator found[MaxPromote];
		size_t       count = 0;
		const auto [first, last] = m_index.equal_range(key);
		for (auto entry = first; entry != last && count < MaxPromote; ++entry) {
			if (entry->second->where < target) {
				found[count++] = entry->second;
			}
		}
		if (count == 0) {
			return 0;
		}
		std::sort(found, found + count, [](const NodeIterator& a, const NodeIterator& b) { return a->sequence < b->sequence; });
		// Urgent jobs of one key go in front of urgent jobs promoted earlier: the execution
		// thread waits for one program at a time, the one it asked for last.
		auto& to       = target == Where::Urgent ? m_urgent : m_live;
		auto  position = target == Where::Urgent ? to.begin() : to.end();
		for (size_t index = 0; index < count; index++) {
			const auto node = found[index];
			auto&      from = node->where == Where::Stored ? m_stored : m_live;
			to.splice(position, from, node);
			node->where = target;
		}
		m_stats.promoted += count;
		NoteSizes();
		return count;
	}

	void Stop() {
		{
			const std::lock_guard lock(m_mutex);
			m_stopping = true;
		}
		m_available.notify_all();
	}

	[[nodiscard]] bool Empty() const {
		const std::lock_guard lock(m_mutex);
		return EmptyLocked();
	}

	[[nodiscard]] CompileQueueStats Stats() const {
		const std::lock_guard lock(m_mutex);
		auto                  stats = m_stats;
		stats.live_left             = m_urgent.size() + m_live.size();
		stats.stored_left           = m_stored.size();
		return stats;
	}

private:
	// In the order the workers take them (Promote compares these).
	enum class Where : uint8_t { Stored, Live, Urgent };
	struct Node {
		Key                  key;
		std::unique_ptr<Job> job;
		CompileClass         job_class = CompileClass::Live;
		Where                where     = Where::Live;
		uint64_t             sequence  = 0;
	};
	using NodeList     = std::list<Node>;
	using NodeIterator = typename NodeList::iterator;
	// More jobs of one key than this are promoted over several calls.
	static constexpr size_t MaxPromote = 64;

	[[nodiscard]] bool EmptyLocked() const { return m_urgent.empty() && m_live.empty() && m_stored.empty(); }

	void NoteSizes() { m_stats.live_max = std::max(m_stats.live_max, m_urgent.size() + m_live.size()); }

	std::unique_ptr<Job> Remove(NodeList& list, NodeIterator node) {
		const auto [first, last] = m_index.equal_range(node->key);
		for (auto entry = first; entry != last; ++entry) {
			if (entry->second == node) {
				m_index.erase(entry);
				break;
			}
		}
		auto job = std::move(node->job);
		list.erase(node);
		return job;
	}

	mutable std::mutex                                  m_mutex;
	std::condition_variable                             m_available;
	NodeList                                            m_urgent;
	NodeList                                            m_live;
	NodeList                                            m_stored;
	std::unordered_multimap<Key, NodeIterator, Hash>    m_index;
	CompileQueueStats                                   m_stats;
	uint64_t                                            m_next_sequence = 0;
	bool                                                m_two_classes   = false;
	bool                                                m_stopping      = false;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_COMPILEQUEUE_H_ */
