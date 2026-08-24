// webrtc_media.cpp
// WebRTC media manager — spec 33 Task 4 shared peer runtime.
//
// One runtime for both platforms behind the PeerSdkOps seam:
//   - PeerSession (shared_ptr map values, generation + HandlePermit + I/O gate)
//   - PeerCallbackBridge: fixed slots (decision B), never freed while the
//     Impl lives; lease protocol (fetch_add in-flight -> validate
//     open/generation -> mismatch counts stale, zero side effects)
//   - HandlePermitPool: capacity = RuntimeOptions::handle_permits (16),
//     one-to-one bound to bridge slots; covers active/creating/retired,
//     so live SDK handles never exceed the pool capacity
//   - PeerReaper: the ONLY place that calls close/release; bounded queue
//     (capacity == permits), exactly-once submission per session
//   - Two-phase offer: permit/session/bridge -> map placeholder under lock ->
//     SDK calls outside any lock -> publish or rollback by generation
//
// The legacy parallel stub state machine and the cleanup thread are deleted.
// #ifdef HAVE_KVS_WEBRTC_SDK wraps only the production adapter.
//
// --- extract_sdp_summary lives in sdp_util.cpp ---

#include "webrtc_media.h"
#include "webrtc_signaling.h"

#include <spdlog/spdlog.h>
#include <gst/video/video-event.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef HAVE_KVS_WEBRTC_SDK
extern "C" {
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>
}
#endif

// Runtime classes live in webrtc::internal::media (named namespace, not
// anonymous: Impl members reference these types, and GCC warns about
// external-linkage classes holding anonymous-namespace subobjects).
namespace webrtc {
namespace internal {
namespace media {

constexpr size_t kMaxActivePeers = 10;
constexpr size_t kMaxPeerIdLen = 256;
constexpr uint32_t kMaxWriteFailures = 100;   // fixed disconnect threshold
constexpr std::chrono::seconds kDisconnectGracePeriod{10};
constexpr std::chrono::seconds kConnectingTimeout{30};  // CONNECTING -> DISCONNECTING
// Pending (early viewer) ICE limits — spec-33 Constraints / design Component 8.
constexpr size_t kMaxPendingCandidatesPerPeer = 50;
constexpr size_t kMaxPendingPeers = 20;
constexpr size_t kMaxGlobalPendingCandidates = 200;
constexpr size_t kMaxCandidateBytes = 4096;   // single ICE candidate <= 4KiB
constexpr std::chrono::seconds kPendingIceTtl{30};

// Worst-case payload budget check (design Component 8): even the pre-cap
// per-peer product stays far below the 16MiB WebRTC budget; the runtime
// additionally enforces the tighter global cap of 200 candidates.
static_assert(kMaxPendingPeers * kMaxPendingCandidatesPerPeer * kMaxCandidateBytes
                  <= 16u * 1024u * 1024u,
              "pending ICE worst case must fit the 16MiB budget");
static_assert(kMaxGlobalPendingCandidates * kMaxCandidateBytes
                  <= 16u * 1024u * 1024u,
              "global pending ICE worst case must fit the 16MiB budget");

// Classified media counters (spec-33 req 5.4/5.5/6.1). Atomics only; the
// reap-reason map has its own mutex (touched off the hot path).
struct MediaCounters {
    std::atomic<uint64_t> local_ice_posted{0};
    std::atomic<uint64_t> local_ice_dropped_oversized{0};
    std::atomic<uint64_t> local_ice_rejected{0};
    std::atomic<uint64_t> pending_ice_expired{0};
    std::atomic<uint64_t> pending_ice_evicted{0};
    std::atomic<uint64_t> pending_ice_rejected_oversized{0};
    std::atomic<uint64_t> connecting_timeouts{0};
    std::atomic<uint64_t> reaped_total{0};

    void count_reap(const char* reason) {
        reaped_total.fetch_add(1);
        std::lock_guard<std::mutex> lock(reasons_mutex_);
        ++reap_reasons_[reason];
    }
    std::vector<std::pair<std::string, uint64_t>> reap_reasons() const {
        std::lock_guard<std::mutex> lock(reasons_mutex_);
        return {reap_reasons_.begin(), reap_reasons_.end()};
    }

private:
    mutable std::mutex reasons_mutex_;
    std::map<std::string, uint64_t> reap_reasons_;
};

// ---- PendingIceStore ----------------------------------------------------
// Early viewer ICE buffer (design Component 8): candidate FIFO per peer +
// peer LRU. Limits: per-peer 50, pending peers 20, global 200, single
// candidate <= 4KiB, TTL 30s. A full peer evicts its own oldest candidate;
// a full peer/global table evicts the least-recently-updated pending peer.
// NOT thread-safe: every method must run under the peers map mutex
// (exclusive), exactly like the map it lives next to.
class PendingIceStore {
public:
    // Returns false when the candidate is rejected (oversized).
    bool add(const std::string& peer_id, const std::string& candidate,
             std::chrono::steady_clock::time_point now, MediaCounters& counters) {
        if (candidate.size() > kMaxCandidateBytes) {
            counters.pending_ice_rejected_oversized.fetch_add(1);
            return false;
        }
        expire(now, counters);
        auto it = peers_.find(peer_id);
        if (it == peers_.end()) {
            if (peers_.size() >= kMaxPendingPeers) {
                evict_lru_peer(peer_id, counters);
            }
            it = peers_.emplace(peer_id, Entry{}).first;
        }
        Entry& entry = it->second;
        if (entry.items.size() >= kMaxPendingCandidatesPerPeer) {
            entry.items.pop_front();  // per-peer full: drop the oldest candidate
            --total_;
            counters.pending_ice_evicted.fetch_add(1);
        }
        while (total_ >= kMaxGlobalPendingCandidates) {
            if (!evict_lru_peer(peer_id, counters)) break;  // only self left
        }
        entry.items.push_back(Item{candidate, now});
        entry.last_seq = ++next_seq_;  // deterministic LRU (no clock ties)
        ++total_;
        return true;
    }

    // Removes and returns the still-fresh candidates for peer_id (FIFO).
    std::vector<std::string> take(const std::string& peer_id,
                                  std::chrono::steady_clock::time_point now,
                                  MediaCounters& counters) {
        std::vector<std::string> out;
        auto it = peers_.find(peer_id);
        if (it == peers_.end()) return out;
        for (auto& item : it->second.items) {
            if (now - item.added_at > kPendingIceTtl) {
                counters.pending_ice_expired.fetch_add(1);
            } else {
                out.push_back(std::move(item.candidate));
            }
        }
        total_ -= it->second.items.size();
        peers_.erase(it);
        return out;
    }

    void erase(const std::string& peer_id) {
        auto it = peers_.find(peer_id);
        if (it == peers_.end()) return;
        total_ -= it->second.items.size();
        peers_.erase(it);
    }

    void clear() {
        peers_.clear();
        total_ = 0;
    }

    size_t peer_count() const { return peers_.size(); }
    size_t total() const { return total_; }

private:
    struct Item {
        std::string candidate;
        std::chrono::steady_clock::time_point added_at;
    };
    struct Entry {
        std::deque<Item> items;  // FIFO
        uint64_t last_seq = 0;   // monotonic update sequence (LRU key)
    };

    void expire(std::chrono::steady_clock::time_point now, MediaCounters& counters) {
        for (auto it = peers_.begin(); it != peers_.end();) {
            auto& items = it->second.items;
            while (!items.empty() && now - items.front().added_at > kPendingIceTtl) {
                items.pop_front();
                --total_;
                counters.pending_ice_expired.fetch_add(1);
            }
            it = items.empty() ? peers_.erase(it) : std::next(it);
        }
    }

    // Evicts the least-recently-updated peer other than `keep`.
    // Returns false when no such peer exists.
    bool evict_lru_peer(const std::string& keep, MediaCounters& counters) {
        auto victim = peers_.end();
        for (auto it = peers_.begin(); it != peers_.end(); ++it) {
            if (it->first == keep) continue;
            if (victim == peers_.end() ||
                it->second.last_seq < victim->second.last_seq) {
                victim = it;
            }
        }
        if (victim == peers_.end()) return false;
        total_ -= victim->second.items.size();
        counters.pending_ice_evicted.fetch_add(victim->second.items.size());
        peers_.erase(victim);
        return true;
    }

    std::unordered_map<std::string, Entry> peers_;
    size_t total_ = 0;
    uint64_t next_seq_ = 0;
};

enum class PeerState { CONNECTING, CONNECTED, DISCONNECTING };

inline const char* peer_state_name(PeerState s) {
    switch (s) {
        case PeerState::CONNECTING:    return "CONNECTING";
        case PeerState::CONNECTED:     return "CONNECTED";
        case PeerState::DISCONNECTING: return "DISCONNECTING";
    }
    return "UNKNOWN";
}

inline std::string media_status_hex(uint32_t code) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08x", code);
    return std::string(buf);
}

class HandlePermitPool;
class PeerCallbackBridge;
class PeerReaper;

// ---- HandlePermit -----------------------------------------------------
// RAII permit bound to exactly one bridge slot index. Held from before
// PeerSdkOps::create until the Reaper finished close/release AND the slot
// is quiescent (explicit release by the Reaper; the destructor is only a
// backstop for sessions that never reached the Reaper).
class HandlePermit {
public:
    HandlePermit() = default;
    HandlePermit(HandlePermitPool* pool, size_t index)
        : pool_(pool), index_(index), held_(true) {}
    HandlePermit(HandlePermit&& o) noexcept
        : pool_(o.pool_), index_(o.index_), held_(o.held_) {
        o.held_ = false;
    }
    HandlePermit& operator=(HandlePermit&& o) noexcept {
        if (this != &o) {
            release();
            pool_ = o.pool_;
            index_ = o.index_;
            held_ = o.held_;
            o.held_ = false;
        }
        return *this;
    }
    HandlePermit(const HandlePermit&) = delete;
    HandlePermit& operator=(const HandlePermit&) = delete;
    ~HandlePermit() { release(); }

    bool valid() const { return held_; }
    size_t index() const { return index_; }
    void release();  // idempotent (defined after HandlePermitPool)

private:
    HandlePermitPool* pool_ = nullptr;
    size_t index_ = 0;
    bool held_ = false;
};

// ---- PeerSession ------------------------------------------------------
// Peer map value. The map mutex only protects the container; per-handle
// SDK I/O is serialized by io_mutex (the I/O gate); lifecycle flags are
// atomics updated by bridge callbacks / the creator / the Reaper.
struct PeerSession : std::enable_shared_from_this<PeerSession> {
    std::string peer_id;
    uint64_t generation = 0;
    HandlePermit permit;                    // returned only after free + slot quiescent
    PeerCallbackBridge* bridge = nullptr;   // fixed slot in the pool, NOT owned

    std::atomic<PeerState> state{PeerState::CONNECTING};
    std::atomic<bool> in_creation{true};    // creator-exclusive until publish
    std::atomic<bool> retired{false};       // exactly-once Reaper submission
    std::atomic<bool> keyframe_pending{false};

    // I/O gate: write/add_ice vs close/release on the same handle.
    std::mutex io_mutex;
    bool io_closed = false;                 // guarded by io_mutex
    std::unique_ptr<PeerHandle> handle;     // guarded by io_mutex

    // Stats / observability.
    std::chrono::steady_clock::time_point created_at{};
    std::atomic<int64_t> disconnected_at_ns{0};
    std::atomic<const char*> disconnect_reason{""};
    std::atomic<uint32_t> sent_candidates{0};      // local candidates generated
    std::atomic<uint32_t> received_candidates{0};
    // Broadcast-thread-only fields (also covered by io_mutex).
    uint32_t consecutive_write_failures = 0;
    bool first_frame_sent = false;
    bool keyframe_only_mode = false;               // spec 26
    int keyframe_mode_success_count = 0;
};

// Move {CONNECTING, CONNECTED} -> DISCONNECTING exactly once; the winner
// stamps the reason and timestamp. Returns true when this call won.
inline bool transition_to_disconnecting(PeerSession& s, const char* reason,
                                        std::chrono::steady_clock::time_point now) {
    PeerState cur = s.state.load();
    while (cur != PeerState::DISCONNECTING) {
        if (s.state.compare_exchange_weak(cur, PeerState::DISCONNECTING)) {
            s.disconnect_reason.store(reason);
            s.disconnected_at_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now.time_since_epoch()).count());
            return true;
        }
    }
    return false;
}

// ---- PeerCallbackBridge -----------------------------------------------
// Fixed slot (decision B): the slot address never changes for the Impl
// lifetime; recreate/reuse only flips open/generation. Every adapter
// callback entry takes an in-flight lease FIRST, then validates
// open/generation; mismatch decrements and returns (counted stale).
// Callbacks only update session atomics and (for terminal states) submit
// grace retirement to the Reaper — no Peer SDK calls, no business locks.
class PeerCallbackBridge {
public:
    PeerCallbackBridge() = default;
    PeerCallbackBridge(const PeerCallbackBridge&) = delete;
    PeerCallbackBridge& operator=(const PeerCallbackBridge&) = delete;

    // post_ice is the internal fire-and-forget path into signaling
    // (try_post_ice_candidate): enqueue only, never waits for a reply.
    using PostIceFn = std::function<bool(const std::string& peer_id,
                                         const std::string& candidate)>;

    void attach(PeerReaper* reaper, std::shared_ptr<RuntimeClock> clock,
                MediaCounters* counters, PostIceFn post_ice) {
        reaper_ = reaper;
        clock_ = std::move(clock);
        counters_ = counters;
        post_ice_ = std::move(post_ice);
    }

    // Bind for a new generation: only legal when the slot is unbound
    // (open == false && in_flight drained via the retirement sequence).
    void bind(uint64_t generation, PeerSession* session) {
        session_.store(session);
        generation_.store(generation);
        open_.store(true);
    }

    void close_gate() { open_.store(false); }

    // Slot reuse precondition: in-flight leases must drain to zero.
    void wait_quiescent() {
        while (in_flight_.load() != 0) std::this_thread::yield();
    }

    void unbind() { session_.store(nullptr); }

    uint64_t stale_events() const { return stale_.load(); }
    uint64_t exceptions() const { return exceptions_.load(); }

    // Stable callbacks handed to the adapter; they capture only this slot
    // whose address never changes (decision B). Defined after PeerReaper.
    PeerCallbacks sdk_callbacks();

private:
    class Lease {
    public:
        Lease(PeerCallbackBridge& b, uint64_t gen) : bridge_(b) {
            bridge_.in_flight_.fetch_add(1);
            valid_ = bridge_.open_.load() && gen == bridge_.generation_.load();
            if (!valid_) bridge_.stale_.fetch_add(1);
        }
        ~Lease() { bridge_.in_flight_.fetch_sub(1); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        bool valid() const { return valid_; }
    private:
        PeerCallbackBridge& bridge_;
        bool valid_ = false;
    };

    PeerReaper* reaper_ = nullptr;
    std::shared_ptr<RuntimeClock> clock_;
    MediaCounters* counters_ = nullptr;
    PostIceFn post_ice_;
    std::atomic<bool> open_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<int64_t> in_flight_{0};
    std::atomic<PeerSession*> session_{nullptr};
    std::atomic<uint64_t> stale_{0};
    std::atomic<uint64_t> exceptions_{0};
};

// ---- HandlePermitPool ---------------------------------------------------
// Owns the fixed bridge slots (never resized => stable addresses) and the
// permit free list. Permit index == slot index (one-to-one binding).
class HandlePermitPool {
public:
    explicit HandlePermitPool(size_t capacity)
        : slots_(capacity == 0 ? 1 : capacity),
          in_use_(slots_.size(), false) {}

    HandlePermitPool(const HandlePermitPool&) = delete;
    HandlePermitPool& operator=(const HandlePermitPool&) = delete;

    HandlePermit acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < in_use_.size(); ++i) {
            if (!in_use_[i]) {
                in_use_[i] = true;
                return HandlePermit(this, i);
            }
        }
        return HandlePermit{};
    }

    void release(size_t index) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < in_use_.size()) in_use_[index] = false;
    }

    PeerCallbackBridge* slot(size_t index) { return &slots_[index]; }
    size_t capacity() const { return slots_.size(); }

    size_t in_use() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t n = 0;
        for (bool used : in_use_) {
            if (used) ++n;
        }
        return n;
    }

    uint64_t total_stale() const {
        uint64_t n = 0;
        for (const auto& s : slots_) n += s.stale_events();
        return n;
    }

    uint64_t total_exceptions() const {
        uint64_t n = 0;
        for (const auto& s : slots_) n += s.exceptions();
        return n;
    }

    void attach_all(PeerReaper* reaper, const std::shared_ptr<RuntimeClock>& clock,
                    MediaCounters* counters,
                    const PeerCallbackBridge::PostIceFn& post_ice) {
        for (auto& s : slots_) s.attach(reaper, clock, counters, post_ice);
    }

private:
    std::vector<PeerCallbackBridge> slots_;  // fixed: never resized/freed
    std::vector<bool> in_use_;               // guarded by mutex_
    mutable std::mutex mutex_;
};

inline void HandlePermit::release() {
    if (!held_) return;
    held_ = false;
    if (pool_) pool_->release(index_);
}

// ---- PeerReaper -----------------------------------------------------------
// Single resident worker replacing the legacy cleanup thread. The only
// code path that calls PeerSdkOps::close/release. Bounded queue (capacity
// == permits): exactly-once submission via PeerSession::retired guarantees
// the queue can never exceed the number of live sessions.
// condition_variable driven through RuntimeClock (no busy polling, no
// fixed sleeps); the next deadline is the earliest job due time.
class PeerReaper {
public:
    using PeerMap = std::unordered_map<std::string, std::shared_ptr<PeerSession>>;

    PeerReaper(size_t capacity,
               std::shared_ptr<PeerSdkOps> ops,
               std::shared_ptr<RuntimeClock> clock,
               PeerMap* peers,
               std::shared_mutex* peers_mutex,
               PendingIceStore* pending,
               MediaCounters* counters)
        : capacity_(capacity),
          ops_(std::move(ops)),
          clock_(std::move(clock)),
          peers_(peers),
          peers_mutex_(peers_mutex),
          pending_(pending),
          counters_(counters) {}

    PeerReaper(const PeerReaper&) = delete;
    PeerReaper& operator=(const PeerReaper&) = delete;

    ~PeerReaper() { drain_and_join(); }

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    // Exactly-once submission: the first caller wins the retired CAS.
    // Returns true when the session is (already or newly) owned by the
    // Reaper. Queue overflow is an invariant violation (permits bound live
    // sessions to the queue capacity): refuse, revert, log an error and
    // never free in place.
    bool submit(std::shared_ptr<PeerSession> s,
                std::chrono::steady_clock::time_point due) {
        if (!s) return false;
        if (s->retired.exchange(true)) return true;  // already submitted
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                s->retired.store(false);
                log_invariant("reaper already stopped");
                return false;
            }
            if (queue_.size() >= capacity_) {
                s->retired.store(false);
                log_invariant("reaper queue overflow");
                return false;
            }
            queue_.push_back(Job{std::move(s), due});
        }
        cv_.notify_all();
        return true;
    }

    // CONNECTING-timeout watch (spec-33 req 5.1, design Component 7): the
    // Reaper's timer also observes sessions that never leave CONNECTING.
    // weak_ptr: a watch never extends the session lifetime. When due, a
    // still-CONNECTING session transitions to DISCONNECTING and is retired
    // after the usual 10s grace. No extra resident thread.
    void watch_connecting(const std::shared_ptr<PeerSession>& s,
                          std::chrono::steady_clock::time_point due) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_ || draining_) return;
            watches_.push_back(Watch{s, due});
        }
        cv_.notify_all();
    }

    // Fixed shutdown step: process every queued job now (due ignored),
    // then join the worker. Idempotent.
    void drain_and_join() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            draining_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        std::deque<Job> leftovers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            leftovers.swap(queue_);
        }
        // Defensive only: the shutdown order (admission closed + creations
        // drained + map cleared before drain) means leftovers cannot exist.
        for (auto& job : leftovers) {
            log_invariant("job submitted after reaper drain");
            retire(job.session);
        }
    }

    size_t depth() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    struct Job {
        std::shared_ptr<PeerSession> session;
        std::chrono::steady_clock::time_point due;
    };
    struct Watch {
        std::weak_ptr<PeerSession> session;
        std::chrono::steady_clock::time_point due;
    };

    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (draining_ && queue_.empty()) return;  // watches dropped on drain
            auto deadline = std::chrono::steady_clock::time_point::max();
            for (const auto& j : queue_) deadline = std::min(deadline, j.due);
            for (const auto& w : watches_) deadline = std::min(deadline, w.due);
            if (!draining_ && !has_due_locked()) {
                clock_->wait_until(cv_, lock, deadline,
                                   [this] { return draining_ || has_due_locked(); });
            }
            std::vector<std::shared_ptr<PeerSession>> ready;
            std::vector<std::weak_ptr<PeerSession>> due_watches;
            const auto now = clock_->now();
            for (auto it = queue_.begin(); it != queue_.end();) {
                if (draining_ || it->due <= now) {
                    ready.push_back(std::move(it->session));
                    it = queue_.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = watches_.begin(); it != watches_.end();) {
                if (draining_) {
                    it = watches_.erase(it);  // shutdown retires everything anyway
                } else if (it->due <= now) {
                    due_watches.push_back(std::move(it->session));
                    it = watches_.erase(it);
                } else {
                    ++it;
                }
            }
            if (ready.empty() && due_watches.empty()) continue;
            lock.unlock();
            for (auto& w : due_watches) check_connecting_timeout(w);
            for (auto& s : ready) retire(s);
            ready.clear();  // drop refs outside the queue lock
            due_watches.clear();
            lock.lock();
        }
    }

    bool has_due_locked() const {
        const auto now = clock_->now();
        for (const auto& j : queue_) {
            if (j.due <= now) return true;
        }
        for (const auto& w : watches_) {
            if (w.due <= now) return true;
        }
        return false;
    }

    // Outside the queue lock. A session that reached CONNECTED (or is
    // already dying) simply drops the watch — zero side effects.
    void check_connecting_timeout(const std::weak_ptr<PeerSession>& weak) {
        auto s = weak.lock();
        if (!s) return;
        if (s->state.load() != PeerState::CONNECTING) return;
        const auto now = clock_->now();
        if (!transition_to_disconnecting(*s, "connecting_timeout", now)) return;
        if (counters_) counters_->connecting_timeouts.fetch_add(1);
        auto logger = spdlog::get("webrtc");
        if (logger) {
            logger->warn("Peer {} stuck in CONNECTING past timeout, marking DISCONNECTING",
                         s->peer_id);
        }
        // Mid-creation sessions defer submission to the creator's publish
        // double-check (the retired CAS dedups when both sides submit).
        if (!s->in_creation.load()) {
            submit(s, now + kDisconnectGracePeriod);
        }
    }

    // Fixed retirement sequence (design Component 7):
    // map removal by generation -> slot open=false -> session I/O gate ->
    // close/release -> slot in-flight==0 -> unbind (slot reusable, memory
    // kept) -> return permit. Never holds the peer map mutex across SDK calls.
    void retire(const std::shared_ptr<PeerSession>& s) {
        if (!s) return;
        {
            std::unique_lock<std::shared_mutex> map_lock(*peers_mutex_);
            auto it = peers_->find(s->peer_id);
            if (it != peers_->end() && it->second.get() == s.get()) {
                peers_->erase(it);
                pending_->erase(s->peer_id);
            }
        }
        if (s->bridge) s->bridge->close_gate();
        {
            std::lock_guard<std::mutex> io(s->io_mutex);
            s->io_closed = true;
            if (s->handle) {
                ops_->close(*s->handle);
                ops_->release(std::move(s->handle));
            }
        }
        if (s->bridge) {
            s->bridge->wait_quiescent();
            s->bridge->unbind();
        }
        s->permit.release();
        if (counters_) counters_->count_reap(s->disconnect_reason.load());
        auto logger = spdlog::get("webrtc");
        if (logger) {
            const double alive = std::chrono::duration<double>(
                clock_->now() - s->created_at).count();
            logger->info("Reaper: freed peer {} (gen={}, alive={:.1f}s, reason={})",
                         s->peer_id, s->generation, alive,
                         s->disconnect_reason.load());
        }
    }

    void log_invariant(const char* what) {
        auto logger = spdlog::get("webrtc");
        if (logger) logger->error("Peer reaper invariant violation: {}", what);
    }

    const size_t capacity_;
    std::shared_ptr<PeerSdkOps> ops_;
    std::shared_ptr<RuntimeClock> clock_;
    PeerMap* peers_;
    std::shared_mutex* peers_mutex_;
    PendingIceStore* pending_;
    MediaCounters* counters_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::deque<Watch> watches_;
    bool draining_ = false;
    bool stopped_ = false;
    std::thread worker_;
};

// ---- PeerCallbackBridge::sdk_callbacks (needs PeerReaper) ----------------

inline PeerCallbacks PeerCallbackBridge::sdk_callbacks() {
    PeerCallbacks cbs;
    cbs.on_state = [this](uint64_t gen, int state) {
        try {
            Lease lease(*this, gen);
            if (!lease.valid()) return;
            PeerSession* s = session_.load();
            if (!s) {
                stale_.fetch_add(1);
                return;
            }
            auto logger = spdlog::get("webrtc");
            if (state == kPeerStateConnected) {
                PeerState expected = PeerState::CONNECTING;
                if (s->state.compare_exchange_strong(expected, PeerState::CONNECTED)) {
                    // The media path consumes this flag (force-keyframe runs
                    // outside SDK callbacks; never touch GStreamer here).
                    s->keyframe_pending.store(true);
                    if (logger) {
                        const double elapsed = clock_
                            ? std::chrono::duration<double>(clock_->now() - s->created_at).count()
                            : 0.0;
                        logger->info("Peer {} connected (elapsed={:.1f}s, ice_sent={}, ice_recv={})",
                                     s->peer_id, elapsed,
                                     s->sent_candidates.load(),
                                     s->received_candidates.load());
                    }
                }
                // DISCONNECTING is never resurrected by a late CONNECTED.
            } else if (state == kPeerStateFailed || state == kPeerStateClosed) {
                const bool is_failed = (state == kPeerStateFailed);
                const char* reason = is_failed ? "connection_failed" : "connection_closed";
                const auto now = clock_ ? clock_->now() : std::chrono::steady_clock::now();
                if (transition_to_disconnecting(*s, reason, now)) {
                    if (logger) {
                        if (is_failed) {
                            logger->warn("Peer {} connection FAILED, marking DISCONNECTING", s->peer_id);
                        } else {
                            logger->info("Peer {} connection closed, marking DISCONNECTING", s->peer_id);
                        }
                    }
                    // Grace-based retirement. Mid-creation sessions are
                    // submitted by the creator at publish time instead
                    // (double-check closes the race; the retired CAS dedups
                    // when both sides submit).
                    if (!s->in_creation.load() && reaper_) {
                        reaper_->submit(s->shared_from_this(), now + kDisconnectGracePeriod);
                    }
                }
            }
            // kPeerStateOther: observability only.
        } catch (...) {
            exceptions_.fetch_add(1);  // never let C++ cross the C ABI
        }
    };
    cbs.on_local_ice = [this](uint64_t gen, std::string_view candidate) {
        try {
            Lease lease(*this, gen);
            if (!lease.valid()) return;
            PeerSession* s = session_.load();
            if (!s) {
                stale_.fetch_add(1);
                return;
            }
            s->sent_candidates.fetch_add(1);
            // Task 1: onNewIceLocalCandidate fires while the SDK holds
            // peerConnectionObjLock. Only a bounded copy (<= 4KiB) and a
            // fire-and-forget enqueue are allowed here — never wait for a
            // signaling reply, never call back into the Peer SDK.
            if (candidate.size() > kMaxCandidateBytes) {
                if (counters_) counters_->local_ice_dropped_oversized.fetch_add(1);
                return;
            }
            if (post_ice_ && post_ice_(s->peer_id, std::string(candidate))) {
                if (counters_) counters_->local_ice_posted.fetch_add(1);
            } else {
                // Not enqueued (queue full / signaling not connected / no
                // signaling wired): classified count, never blocks.
                if (counters_) counters_->local_ice_rejected.fetch_add(1);
            }
        } catch (...) {
            exceptions_.fetch_add(1);
        }
    };
    return cbs;
}

// ---- Production runtime clock (steady clock + cv waits) -------------------

class MediaSteadyClock : public RuntimeClock {
public:
    std::chrono::steady_clock::time_point now() const override {
        return std::chrono::steady_clock::now();
    }
    WaitResult wait_until(std::condition_variable& cv,
                          std::unique_lock<std::mutex>& lock,
                          std::chrono::steady_clock::time_point deadline,
                          const std::function<bool()>& notified) override {
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            cv.wait(lock, notified);
            return WaitResult::NOTIFIED;
        }
        if (cv.wait_until(lock, deadline, notified)) return WaitResult::NOTIFIED;
        return WaitResult::DEADLINE;
    }
};

// ---- StubPeerSdkOps (macOS / Linux without SDK) ---------------------------
// Preserves the legacy stub semantics on the shared runtime: create()
// publishes CONNECTED synchronously (this also exercises the sync early
// callback path for every stub test), negotiate() returns an empty answer
// so the runtime skips the signaling send.
class StubPeerHandle : public PeerHandle {
public:
    explicit StubPeerHandle(uint64_t gen) : generation(gen) {}
    uint64_t generation;
};

class StubPeerSdkOps : public PeerSdkOps {
public:
    SdkCallResult create(uint64_t generation, const PeerCallbacks& cbs,
                         std::unique_ptr<PeerHandle>& out) override {
        // Synchronous early callback (before the handle is even returned).
        if (cbs.on_state) cbs.on_state(generation, kPeerStateConnected);
        out = std::make_unique<StubPeerHandle>(generation);
        auto logger = spdlog::get("webrtc");
        if (logger) logger->debug("Stub peer ops: create, generation={}", generation);
        return {};
    }
    SdkCallResult negotiate(PeerHandle&, std::string_view,
                            std::string& answer) override {
        answer.clear();  // stub: nothing to send via signaling
        return {};
    }
    SdkCallResult add_ice(PeerHandle&, std::string_view) override { return {}; }
    SdkCallResult write_frame(PeerHandle&, const uint8_t*, size_t,
                              uint64_t, bool) override { return {}; }
    SdkCallResult close(PeerHandle&) override { return {}; }
    SdkCallResult release(std::unique_ptr<PeerHandle>) override { return {}; }
};

#ifdef HAVE_KVS_WEBRTC_SDK
// ---- ProductionPeerSdkOps (real KVS WebRTC C SDK, Linux) ------------------
// Hides SDK types behind the seam. customData for C callbacks points into a
// fixed CallbackCell pool sized like the permit pool: cell memory is never
// freed while the adapter lives, so a late callback can never dereference
// freed memory (decision B). Cell REUSE additionally relies on the Task 1
// CONFIRMED contract that per-handle callbacks are silent after
// freePeerConnection returns (release() frees the handle before recycling
// the cell).
class ProductionPeerHandle : public PeerHandle {
public:
    PRtcPeerConnection pc = nullptr;
    PRtcRtpTransceiver video = nullptr;
    size_t cell_index = SIZE_MAX;
};

class ProductionPeerSdkOps : public PeerSdkOps {
public:
    ProductionPeerSdkOps(WebRtcSignaling& signaling, std::string region,
                         size_t cell_capacity)
        : signaling_(signaling),
          region_(std::move(region)),
          cells_(cell_capacity == 0 ? 1 : cell_capacity) {}

    SdkCallResult create(uint64_t generation, const PeerCallbacks& cbs,
                         std::unique_ptr<PeerHandle>& out) override {
        auto logger = spdlog::get("webrtc");
        const size_t idx = acquire_cell();
        if (idx == cells_.size()) {
            if (logger) logger->error(
                "Peer callback cell pool exhausted (invariant violation, capacity {})",
                cells_.size());
            return {SdkCallStatus::FATAL, 0};
        }
        CallbackCell& cell = cells_[idx];
        cell.generation = generation;
        cell.cbs = cbs;
        cell.armed.store(true, std::memory_order_release);

        // RtcConfiguration: STUN + at most one TURN group from the
        // signaling ICE cache (fast owner-cache read, no SDK call).
        RtcConfiguration rtc_config;
        MEMSET(&rtc_config, 0x00, SIZEOF(RtcConfiguration));
        rtc_config.iceTransportPolicy = ICE_TRANSPORT_POLICY_ALL;
        SNPRINTF(rtc_config.iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN,
                 "stun:stun.kinesisvideo.%s.amazonaws.com:443", region_.c_str());
        uint32_t ice_count = signaling_.get_ice_config_count();
        uint32_t uri_idx = 0;
        uint32_t max_turn = (ice_count > 0) ? 1 : 0;
        for (uint32_t i = 0; i < max_turn; i++) {
            std::vector<WebRtcSignaling::IceServerInfo> servers;
            if (signaling_.get_ice_config(i, servers)) {
                for (const auto& srv : servers) {
                    if (uri_idx + 1 < MAX_ICE_SERVERS_COUNT) {
                        STRNCPY(rtc_config.iceServers[uri_idx + 1].urls,
                                srv.uri.c_str(), MAX_ICE_CONFIG_URI_LEN);
                        STRNCPY(rtc_config.iceServers[uri_idx + 1].credential,
                                srv.credential.c_str(), MAX_ICE_CONFIG_CREDENTIAL_LEN);
                        STRNCPY(rtc_config.iceServers[uri_idx + 1].username,
                                srv.username.c_str(), MAX_ICE_CONFIG_USER_NAME_LEN);
                        uri_idx++;
                    }
                }
            }
        }

        PRtcPeerConnection pc = NULL;
        STATUS ret = createPeerConnection(&rtc_config, &pc);
        if (STATUS_FAILED(ret)) {
            release_cell(idx);
            return fail("createPeerConnection", ret);
        }

        ret = addSupportedCodec(pc,
            RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE);
        if (STATUS_FAILED(ret)) {
            freePeerConnection(&pc);
            release_cell(idx);
            return fail("addSupportedCodec (H.264)", ret);
        }
        addSupportedCodec(pc, RTC_CODEC_OPUS);  // non-fatal

        RtcMediaStreamTrack video_track;
        MEMSET(&video_track, 0, SIZEOF(RtcMediaStreamTrack));
        video_track.kind = MEDIA_STREAM_TRACK_KIND_VIDEO;
        video_track.codec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
        STRCPY(video_track.streamId, "myKvsVideoStream");
        STRCPY(video_track.trackId, "myVideoTrack");
        RtcRtpTransceiverInit video_init;
        MEMSET(&video_init, 0, SIZEOF(RtcRtpTransceiverInit));
        video_init.direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;
        PRtcRtpTransceiver video_transceiver = NULL;
        ret = addTransceiver(pc, &video_track, &video_init, &video_transceiver);
        if (STATUS_FAILED(ret)) {
            freePeerConnection(&pc);
            release_cell(idx);
            return fail("addTransceiver (video)", ret);
        }

        RtcMediaStreamTrack audio_track;
        MEMSET(&audio_track, 0, SIZEOF(RtcMediaStreamTrack));
        audio_track.kind = MEDIA_STREAM_TRACK_KIND_AUDIO;
        audio_track.codec = RTC_CODEC_OPUS;
        STRCPY(audio_track.streamId, "myKvsVideoStream");
        STRCPY(audio_track.trackId, "myAudioTrack");
        RtcRtpTransceiverInit audio_init;
        MEMSET(&audio_init, 0, SIZEOF(RtcRtpTransceiverInit));
        audio_init.direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;
        PRtcRtpTransceiver audio_transceiver = NULL;
        ret = addTransceiver(pc, &audio_track, &audio_init, &audio_transceiver);
        if (STATUS_FAILED(ret)) {
            if (logger) logger->warn(
                "addTransceiver (audio) failed, status: {} — continuing without audio",
                media_status_hex(static_cast<uint32_t>(ret)));
        }

        ret = peerConnectionOnIceCandidate(pc, reinterpret_cast<UINT64>(&cell),
                                           on_ice_candidate_tramp);
        if (STATUS_FAILED(ret)) {
            freePeerConnection(&pc);
            release_cell(idx);
            return fail("peerConnectionOnIceCandidate", ret);
        }
        ret = peerConnectionOnConnectionStateChange(pc, reinterpret_cast<UINT64>(&cell),
                                                    on_state_change_tramp);
        if (STATUS_FAILED(ret)) {
            freePeerConnection(&pc);
            release_cell(idx);
            return fail("peerConnectionOnConnectionStateChange", ret);
        }

        auto handle = std::make_unique<ProductionPeerHandle>();
        handle->pc = pc;
        handle->video = video_transceiver;
        handle->cell_index = idx;
        out = std::move(handle);
        return {};
    }

    SdkCallResult negotiate(PeerHandle& h, std::string_view sdp_offer,
                            std::string& answer) override {
        auto& handle = static_cast<ProductionPeerHandle&>(h);
        answer.clear();
        std::string offer_copy(sdp_offer);  // SDK needs a NUL-terminated buffer

        RtcSessionDescriptionInit offer_sdp;
        MEMSET(&offer_sdp, 0, SIZEOF(RtcSessionDescriptionInit));
        STATUS ret = deserializeSessionDescriptionInit(
            const_cast<PCHAR>(offer_copy.c_str()),
            static_cast<UINT32>(offer_copy.size()), &offer_sdp);
        if (STATUS_FAILED(ret)) return fail("deserializeSessionDescriptionInit", ret);

        ret = setRemoteDescription(handle.pc, &offer_sdp);
        if (STATUS_FAILED(ret)) return fail("setRemoteDescription", ret);

        RtcSessionDescriptionInit answer_sdp;
        MEMSET(&answer_sdp, 0x00, SIZEOF(RtcSessionDescriptionInit));
        ret = setLocalDescription(handle.pc, &answer_sdp);
        if (STATUS_FAILED(ret)) return fail("setLocalDescription", ret);

        ret = createAnswer(handle.pc, &answer_sdp);
        if (STATUS_FAILED(ret)) return fail("createAnswer", ret);

        UINT32 serialized_len = MAX_SIGNALING_MESSAGE_LEN;
        CHAR serialized_answer[MAX_SIGNALING_MESSAGE_LEN];
        ret = serializeSessionDescriptionInit(&answer_sdp, serialized_answer, &serialized_len);
        if (STATUS_FAILED(ret)) return fail("serializeSessionDescriptionInit", ret);

        answer.assign(serialized_answer, serialized_len);
        return {};
    }

    SdkCallResult add_ice(PeerHandle& h, std::string_view candidate) override {
        auto& handle = static_cast<ProductionPeerHandle&>(h);
        std::string cand_copy(candidate);
        RtcIceCandidateInit cand_init;
        MEMSET(&cand_init, 0, SIZEOF(RtcIceCandidateInit));
        STATUS ret = deserializeRtcIceCandidateInit(
            const_cast<PCHAR>(cand_copy.c_str()),
            static_cast<UINT32>(cand_copy.size()), &cand_init);
        if (STATUS_FAILED(ret)) return fail("deserializeRtcIceCandidateInit", ret);
        ret = addIceCandidate(handle.pc, cand_init.candidate);
        if (STATUS_FAILED(ret)) return fail("addIceCandidate", ret);
        return {};
    }

    SdkCallResult write_frame(PeerHandle& h, const uint8_t* data,
                              size_t size, uint64_t timestamp_100ns,
                              bool keyframe) override {
        auto& handle = static_cast<ProductionPeerHandle&>(h);
        if (handle.video == nullptr) return {SdkCallStatus::FATAL, 0};
        Frame frame;
        MEMSET(&frame, 0, SIZEOF(Frame));
        frame.version = FRAME_CURRENT_VERSION;
        frame.trackId = DEFAULT_VIDEO_TRACK_ID;
        frame.duration = 0;
        frame.decodingTs = timestamp_100ns;
        frame.presentationTs = timestamp_100ns;
        frame.frameData = const_cast<PBYTE>(data);
        frame.size = static_cast<UINT32>(size);
        frame.flags = keyframe ? FRAME_FLAG_KEY_FRAME : FRAME_FLAG_NONE;
        STATUS ret = writeFrame(handle.video, &frame);
        if (STATUS_SUCCEEDED(ret)) return {};
        if (ret == STATUS_SRTP_NOT_READY_YET) {
            // DTLS handshake still in progress: transient skip, not counted.
            return {SdkCallStatus::RETRYABLE, static_cast<uint32_t>(ret)};
        }
        return {SdkCallStatus::FATAL, static_cast<uint32_t>(ret)};
    }

    SdkCallResult close(PeerHandle& h) override {
        auto& handle = static_cast<ProductionPeerHandle&>(h);
        if (handle.pc != nullptr) {
            STATUS ret = closePeerConnection(handle.pc);
            if (STATUS_FAILED(ret)) return fail("closePeerConnection", ret);
        }
        return {};
    }

    SdkCallResult release(std::unique_ptr<PeerHandle> h) override {
        if (!h) return {};
        auto* handle = static_cast<ProductionPeerHandle*>(h.get());
        if (handle->pc != nullptr) {
            freePeerConnection(&handle->pc);
            handle->pc = nullptr;
        }
        // Cell recycled only AFTER freePeerConnection returned (Task 1:
        // per-handle callbacks are silent after free). Memory itself is
        // never released while the adapter lives.
        if (handle->cell_index < cells_.size()) {
            release_cell(handle->cell_index);
        }
        return {};
    }

private:
    // Fixed callback cell: stable address for SDK customData.
    struct CallbackCell {
        std::atomic<bool> armed{false};
        bool in_use = false;         // guarded by cells_mutex_
        uint64_t generation = 0;     // immutable while armed
        PeerCallbacks cbs;
    };

    size_t acquire_cell() {
        std::lock_guard<std::mutex> lock(cells_mutex_);
        for (size_t i = 0; i < cells_.size(); ++i) {
            if (!cells_[i].in_use) {
                cells_[i].in_use = true;
                return i;
            }
        }
        return cells_.size();
    }

    void release_cell(size_t idx) {
        cells_[idx].armed.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(cells_mutex_);
        cells_[idx].in_use = false;
    }

    SdkCallResult fail(const char* api, STATUS status) {
        auto logger = spdlog::get("webrtc");
        if (logger) logger->error("Peer ops: {} failed, status: {}",
                                  api, media_status_hex(static_cast<uint32_t>(status)));
        return {SdkCallStatus::RETRYABLE, static_cast<uint32_t>(status)};
    }

    // C trampolines: try/catch only, forward to the bridge lambdas with the
    // generation stamped at registration time. Bridge lambdas validate
    // open/generation before any side effect.
    static VOID on_ice_candidate_tramp(UINT64 custom_data, PCHAR candidate_json) {
        auto* cell = reinterpret_cast<CallbackCell*>(custom_data);
        try {
            if (cell == nullptr || !cell->armed.load(std::memory_order_acquire)) return;
            if (candidate_json == NULL) return;  // gathering finished marker
            if (cell->cbs.on_local_ice) {
                cell->cbs.on_local_ice(cell->generation, std::string_view(candidate_json));
            }
        } catch (...) {
        }
    }

    static VOID on_state_change_tramp(UINT64 custom_data,
                                      RTC_PEER_CONNECTION_STATE new_state) {
        auto* cell = reinterpret_cast<CallbackCell*>(custom_data);
        try {
            if (cell == nullptr || !cell->armed.load(std::memory_order_acquire)) return;
            int kind = kPeerStateOther;
            if (new_state == RTC_PEER_CONNECTION_STATE_CONNECTED) {
                kind = kPeerStateConnected;
            } else if (new_state == RTC_PEER_CONNECTION_STATE_FAILED) {
                kind = kPeerStateFailed;
            } else if (new_state == RTC_PEER_CONNECTION_STATE_CLOSED) {
                kind = kPeerStateClosed;
            }
            if (cell->cbs.on_state) cell->cbs.on_state(cell->generation, kind);
        } catch (...) {
        }
    }

    WebRtcSignaling& signaling_;
    std::string region_;
    std::vector<CallbackCell> cells_;  // fixed: never resized/freed
    std::mutex cells_mutex_;
};
#endif  // HAVE_KVS_WEBRTC_SDK

}  // namespace media
}  // namespace internal
}  // namespace webrtc

namespace wi = webrtc::internal;
namespace wm = webrtc::internal::media;

// ============================================================
// WebRtcMediaManager::Impl — shared runtime for both platforms.
// ============================================================

struct WebRtcMediaManager::Impl {
    // Declaration order == reverse destruction order:
    //   pool (bridge slots) declared FIRST => destroyed LAST (decision B);
    //   ops (adapter callback cells) declared second => destroyed just
    //   before the pool and AFTER the runtime token release (if this was
    //   the last token, deinitKvsWebRtc has joined all SDK threads by then).
    wm::HandlePermitPool pool;
    std::shared_ptr<wi::PeerSdkOps> ops;
    std::shared_ptr<wi::KvsRuntimeToken> runtime_token;

    WebRtcSignaling& signaling;
    std::string region;
    std::shared_ptr<wi::RuntimeClock> clock;
    wi::RuntimeOptions options;

    wm::PeerReaper::PeerMap peers;
    mutable std::shared_mutex peers_mutex;
    wm::PendingIceStore pending_candidates;  // guarded by peers_mutex
    wm::MediaCounters counters;

    std::atomic<bool> admission_open{true};
    std::atomic<uint64_t> generation_seed{0};

    // Shutdown waits for in-flight offer creations (bounded by the SDK
    // upper bounds from Task 1) before draining the map and the Reaper.
    std::mutex inflight_mutex;
    std::condition_variable inflight_cv;
    size_t offers_in_flight = 0;  // guarded by inflight_mutex

    GstElement* pipeline_ = nullptr;  // not owned, never unref'ed
    std::atomic<int> writeframe_fail_threshold_{10};

    std::unique_ptr<wm::PeerReaper> reaper;

    Impl(WebRtcSignaling& sig, const wi::RuntimeOptions& opt)
        : pool(opt.handle_permits), signaling(sig), options(opt) {}

    ~Impl() { shutdown(); }

    // RAII guard for in-flight offer accounting. Constructed BEFORE the
    // admission check so shutdown's inflight wait always covers a racing
    // offer (which then observes admission_open == false and backs out).
    class OfferGuard {
    public:
        explicit OfferGuard(Impl& impl) : impl_(impl) {
            std::lock_guard<std::mutex> lock(impl_.inflight_mutex);
            ++impl_.offers_in_flight;
        }
        ~OfferGuard() {
            {
                std::lock_guard<std::mutex> lock(impl_.inflight_mutex);
                --impl_.offers_in_flight;
            }
            impl_.inflight_cv.notify_all();
        }
        OfferGuard(const OfferGuard&) = delete;
        OfferGuard& operator=(const OfferGuard&) = delete;
    private:
        Impl& impl_;
    };

    // ---- offer (two-phase creation, design Component 6) ----
    bool handle_offer(const std::string& peer_id, const std::string& sdp_offer,
                      std::string* error_msg) {
        auto logger = spdlog::get("webrtc");
        if (peer_id.size() > wm::kMaxPeerIdLen) {
            if (logger) logger->warn("Rejecting peer with oversized id ({} bytes, max {})",
                                     peer_id.size(), wm::kMaxPeerIdLen);
            if (error_msg) *error_msg = "peer_id too long";
            return false;
        }
        OfferGuard guard(*this);
        if (!admission_open.load()) {
            if (error_msg) *error_msg = "media manager shutting down";
            return false;
        }

        // Phase 1: permit + session + bridge bind (no map lock; no permit =>
        // reject and keep every existing peer).
        wm::HandlePermit permit = pool.acquire();
        if (!permit.valid()) {
            if (logger) logger->warn(
                "No handle permit available ({} live handles), rejecting peer: {}",
                pool.capacity(), peer_id);
            if (error_msg) *error_msg = "no handle permit available";
            return false;
        }
        const uint64_t gen = generation_seed.fetch_add(1) + 1;
        auto session = std::make_shared<wm::PeerSession>();
        session->peer_id = peer_id;
        session->generation = gen;
        session->created_at = clock->now();
        session->bridge = pool.slot(permit.index());
        session->permit = std::move(permit);
        session->bridge->bind(gen, session.get());

        // Rollback for pre-publish failures: mark, hand over to the Reaper
        // (which also removes the map placeholder), never free in place.
        auto rollback = [&](const char* reason, const std::string& msg) {
            wm::transition_to_disconnecting(*session, reason, clock->now());
            session->in_creation.store(false);
            reaper->submit(session, clock->now());
            if (error_msg) *error_msg = msg;
            return false;
        };

        // Phase 2: map lock — container-only work (active<=10 check,
        // same-peer replacement handover, placeholder insertion).
        std::shared_ptr<wm::PeerSession> replaced;
        {
            std::unique_lock<std::shared_mutex> lock(peers_mutex);
            auto existing = peers.find(peer_id);
            const bool replacing = (existing != peers.end());
            if (!replacing) {
                size_t active = 0;
                for (const auto& entry : peers) {
                    if (entry.second->state.load() != wm::PeerState::DISCONNECTING) ++active;
                }
                if (active >= wm::kMaxActivePeers) {
                    lock.unlock();
                    if (logger) logger->warn("Max peers ({}) reached, rejecting peer: {}",
                                             wm::kMaxActivePeers, peer_id);
                    // Uniform permit return path: the Reaper unbinds the slot
                    // and releases the permit (session never entered the map).
                    return rollback("rejected_max_peers", "Max peer count reached");
                }
            }
            if (replacing) {
                replaced = existing->second;
                peers.erase(existing);
            }
            peers.emplace(peer_id, session);
        }
        if (replaced) {
            if (logger) logger->info("Replacing existing peer: {}", peer_id);
            wm::transition_to_disconnecting(*replaced, "replaced", clock->now());
            // Mid-creation sessions are submitted by their own creator at
            // publish time (it observes the map eviction).
            if (!replaced->in_creation.load()) {
                reaper->submit(replaced, clock->now());
            }
        }

        // Phase 3 (no locks held): create / negotiate / send.
        std::unique_ptr<wi::PeerHandle> handle;
        auto r = ops->create(gen, session->bridge->sdk_callbacks(), handle);
        if (r.status != wi::SdkCallStatus::OK || !handle) {
            if (logger) logger->error("Peer create failed for {}, code: {}",
                                      peer_id, wm::media_status_hex(r.code));
            return rollback("create_failed",
                            "peer create failed: " + wm::media_status_hex(r.code));
        }

        std::string answer;
        {
            // I/O gate: publish the handle and negotiate under the session
            // gate (retirement of this session is deferred until publish,
            // so close/release cannot interleave here).
            std::lock_guard<std::mutex> io(session->io_mutex);
            session->handle = std::move(handle);
            r = ops->negotiate(*session->handle, sdp_offer, answer);
        }
        if (r.status != wi::SdkCallStatus::OK) {
            if (logger) logger->error("Peer negotiate failed for {}, code: {}",
                                      peer_id, wm::media_status_hex(r.code));
            return rollback("negotiate_failed",
                            "peer negotiate failed: " + wm::media_status_hex(r.code));
        }
        if (logger) logger->debug("Offer SDP summary for peer {}: {}",
                                  peer_id, extract_sdp_summary(sdp_offer));

        if (!answer.empty()) {
            if (!signaling.send_answer(peer_id, answer)) {
                if (logger) logger->error("Failed to send SDP answer for peer {}", peer_id);
                return rollback("send_answer_failed", "send_answer failed");
            }
            if (logger) logger->debug("Answer SDP summary for peer {}: {}",
                                      peer_id, extract_sdp_summary(answer));
        }

        // Phase 4: publish by generation (or hand the orphan to the Reaper).
        bool published = false;
        {
            std::unique_lock<std::shared_mutex> lock(peers_mutex);
            auto it = peers.find(peer_id);
            published = (it != peers.end() && it->second.get() == session.get());
        }
        session->in_creation.store(false);
        if (!published) {
            // Evicted while creating (replace/remove/shutdown): this thread
            // owns the submission (eviction skipped it, see handle_remove).
            wm::transition_to_disconnecting(*session, "evicted_during_creation",
                                            clock->now());
            reaper->submit(session, clock->now());
            if (error_msg) *error_msg = "peer evicted during creation";
            return false;
        }
        // Double-check: a state transition during creation deferred its
        // Reaper submission to us (the retired CAS dedups both sides).
        // Terminal-callback reasons keep the grace period; eviction-style
        // reasons (manual_remove/replaced racing publish) retire now.
        if (session->state.load() == wm::PeerState::DISCONNECTING) {
            const char* reason = session->disconnect_reason.load();
            const bool terminal_cb =
                (std::strcmp(reason, "connection_failed") == 0 ||
                 std::strcmp(reason, "connection_closed") == 0);
            reaper->submit(session, terminal_cb
                ? clock->now() + wm::kDisconnectGracePeriod
                : clock->now());
        }

        // CONNECTING-timeout watch (req 5.1): a published session that has
        // not reached CONNECTED yet must not linger past 30s. The watch is
        // harmless when the state changes first (checked at due time).
        if (session->state.load() == wm::PeerState::CONNECTING) {
            reaper->watch_connecting(session,
                                     session->created_at + wm::kConnectingTimeout);
        }

        if (logger) {
            std::shared_lock<std::shared_mutex> lock(peers_mutex);
            logger->info("Created PeerConnection for peer {}, count={}",
                         peer_id, peers.size());
        }

        // Phase 5: flush pending ICE candidates in FIFO order (collect under
        // the map lock, apply outside under the session I/O gate).
        std::vector<std::string> pending;
        {
            std::unique_lock<std::shared_mutex> lock(peers_mutex);
            pending = pending_candidates.take(peer_id, clock->now(), counters);
        }
        if (!pending.empty()) {
            if (logger) logger->info("Flushing {} buffered ICE candidates for peer {}",
                                     pending.size(), peer_id);
            for (const auto& cand : pending) {
                apply_candidate(session, cand, nullptr);
            }
        }
        return true;
    }

    // ---- viewer ICE ----
    bool handle_ice(const std::string& peer_id, const std::string& candidate,
                    std::string* error_msg) {
        auto logger = spdlog::get("webrtc");
        if (!admission_open.load()) return false;
        if (candidate.size() > wm::kMaxCandidateBytes) {
            counters.pending_ice_rejected_oversized.fetch_add(1);
            if (logger) logger->warn(
                "Rejecting oversized ICE candidate for peer {} ({} bytes, max {})",
                peer_id, candidate.size(), wm::kMaxCandidateBytes);
            if (error_msg) *error_msg = "ICE candidate too large";
            return false;
        }
        std::shared_ptr<wm::PeerSession> session;
        {
            std::unique_lock<std::shared_mutex> lock(peers_mutex);
            auto it = peers.find(peer_id);
            const bool buffer = (it == peers.end()) ||
                                it->second->in_creation.load();
            if (buffer) {
                // Bounded early-ICE buffer (req 5.5): per-peer 50 (oldest
                // candidate out), 20 pending peers / 200 global (LRU peer
                // out), TTL 30s. The store classifies every drop.
                pending_candidates.add(peer_id, candidate, clock->now(), counters);
                if (logger) logger->debug(
                    "Buffered early ICE candidate for peer: {} (pending_total={})",
                    peer_id, pending_candidates.total());
                return true;  // buffered — applied after the offer completes
            }
            session = it->second;
        }
        session->received_candidates.fetch_add(1);
        return apply_candidate(session, candidate, error_msg);
    }

    // Apply one candidate under the session I/O gate (never under the map
    // mutex). Dying/handle-less sessions swallow the candidate silently.
    bool apply_candidate(const std::shared_ptr<wm::PeerSession>& session,
                         const std::string& candidate, std::string* error_msg) {
        auto logger = spdlog::get("webrtc");
        std::lock_guard<std::mutex> io(session->io_mutex);
        if (session->io_closed || !session->handle) return true;
        auto r = ops->add_ice(*session->handle, candidate);
        if (r.status != wi::SdkCallStatus::OK) {
            if (logger) logger->warn("addIceCandidate failed for peer {}, status: {}",
                                     session->peer_id, wm::media_status_hex(r.code));
            if (error_msg) *error_msg = "addIceCandidate failed: " + wm::media_status_hex(r.code);
            return false;
        }
        if (logger) logger->debug("Added ICE candidate for peer: {}", session->peer_id);
        return true;
    }

    // ---- remove ----
    void handle_remove(const std::string& peer_id) {
        auto logger = spdlog::get("webrtc");
        std::shared_ptr<wm::PeerSession> victim;
        {
            std::unique_lock<std::shared_mutex> lock(peers_mutex);
            auto it = peers.find(peer_id);
            if (it == peers.end()) return;
            victim = it->second;
            peers.erase(it);
            pending_candidates.erase(peer_id);
        }
        wm::transition_to_disconnecting(*victim, "manual_remove", clock->now());
        // Mid-creation: the creator observes the eviction and submits.
        if (!victim->in_creation.load()) {
            reaper->submit(victim, clock->now());
        }
        if (logger) logger->info("Removed peer {} (reason=manual_remove, queued for reaper)",
                                 peer_id);
    }

    // ---- broadcast (Task 4: map lock only for the snapshot; write under
    // the per-session I/O gate; Task 5 refines further) ----
    void handle_broadcast(const uint8_t* data, size_t size,
                          uint64_t timestamp_100ns, bool is_keyframe) {
        std::vector<std::shared_ptr<wm::PeerSession>> snapshot;
        {
            std::shared_lock<std::shared_mutex> lock(peers_mutex);
            if (peers.empty()) return;
            snapshot.reserve(peers.size());
            for (const auto& entry : peers) {
                if (entry.second->state.load() == wm::PeerState::CONNECTED) {
                    snapshot.push_back(entry.second);
                }
            }
        }
        if (snapshot.empty()) return;
        auto logger = spdlog::get("webrtc");

        // Consume keyframe_pending on the media path (the CONNECTED callback
        // only sets the flag; GStreamer is never touched from SDK callbacks).
        bool want_keyframe = false;
        for (const auto& s : snapshot) {
            if (s->keyframe_pending.exchange(false)) want_keyframe = true;
        }
        if (want_keyframe) request_keyframe();

        const int threshold = writeframe_fail_threshold_.load();
        for (const auto& s : snapshot) {
            std::lock_guard<std::mutex> io(s->io_mutex);
            if (s->io_closed || !s->handle) continue;
            if (s->state.load() != wm::PeerState::CONNECTED) continue;
            if (s->keyframe_only_mode && !is_keyframe) continue;  // spec 26

            auto r = ops->write_frame(*s->handle, data, size, timestamp_100ns, is_keyframe);
            if (r.status == wi::SdkCallStatus::OK) {
                s->consecutive_write_failures = 0;
                if (!s->first_frame_sent) {
                    s->first_frame_sent = true;
                    if (logger) logger->info("First frame sent to peer {} (size={}, keyframe={})",
                                             s->peer_id, size, is_keyframe);
                }
                if (s->keyframe_only_mode) {
                    if (++s->keyframe_mode_success_count >= 10) {
                        s->keyframe_only_mode = false;
                        s->keyframe_mode_success_count = 0;
                        if (logger) logger->info("Peer {} recovered from keyframe-only mode",
                                                 s->peer_id);
                    }
                }
            } else if (r.status == wi::SdkCallStatus::RETRYABLE) {
                // SRTP not ready — transient skip, not counted as a failure.
                if (logger) logger->debug("writeFrame skipped for peer {}: SRTP not ready",
                                          s->peer_id);
            } else {
                s->consecutive_write_failures++;
                s->keyframe_mode_success_count = 0;
                // Milestone-based logging (req 6.3): warn on the first
                // failure and at the halfway mark; per-failure detail stays
                // at debug so a failing peer cannot flood warn-level logs.
                if (logger) {
                    if (s->consecutive_write_failures == 1) {
                        logger->warn("writeFrame failed for peer {}, status: {}",
                                     s->peer_id, wm::media_status_hex(r.code));
                    } else if (s->consecutive_write_failures == wm::kMaxWriteFailures / 2) {
                        logger->warn(
                            "writeFrame failing for peer {}: {}/{} consecutive failures",
                            s->peer_id, s->consecutive_write_failures, wm::kMaxWriteFailures);
                    } else {
                        logger->debug("writeFrame failed for peer {}, status: {}, failures: {}",
                                      s->peer_id, wm::media_status_hex(r.code),
                                      s->consecutive_write_failures);
                    }
                }
                if (s->consecutive_write_failures >= static_cast<uint32_t>(threshold)
                    && !s->keyframe_only_mode) {
                    s->keyframe_only_mode = true;
                    s->keyframe_mode_success_count = 0;
                    if (logger) logger->info(
                        "Peer {} entered keyframe-only mode after {} consecutive failures",
                        s->peer_id, s->consecutive_write_failures);
                }
                if (s->consecutive_write_failures > wm::kMaxWriteFailures) {
                    if (logger) logger->warn(
                        "Peer {} exceeded max write failures ({}), marking DISCONNECTING",
                        s->peer_id, wm::kMaxWriteFailures);
                    if (wm::transition_to_disconnecting(*s, "max_write_failures",
                                                        clock->now())) {
                        // Submit only (the Reaper frees after the grace
                        // period); never free in the media path.
                        reaper->submit(s, clock->now() + wm::kDisconnectGracePeriod);
                    }
                }
            }
        }
    }

    size_t handle_count() const {
        std::shared_lock<std::shared_mutex> lock(peers_mutex);
        size_t count = 0;
        for (const auto& entry : peers) {
            if (entry.second->state.load() == wm::PeerState::CONNECTED) ++count;
        }
        return count;
    }

    // ---- health snapshot (req 6.1/6.4): immutable copy, no payloads ----
    WebRtcMediaManager::HealthSnapshot snapshot() const {
        WebRtcMediaManager::HealthSnapshot s;
        const auto now = clock->now();
        {
            std::shared_lock<std::shared_mutex> lock(peers_mutex);
            for (const auto& entry : peers) {
                const auto st = entry.second->state.load();
                if (st != wm::PeerState::DISCONNECTING) ++s.active_peers;
                if (st == wm::PeerState::CONNECTED) ++s.connected_peers;
                const int64_t age = std::chrono::duration_cast<std::chrono::seconds>(
                    now - entry.second->created_at).count();
                if (age > s.oldest_session_age_sec) s.oldest_session_age_sec = age;
            }
            s.pending_ice_peers = pending_candidates.peer_count();
            s.pending_ice_depth = pending_candidates.total();
        }
        s.live_handles = pool.in_use();
        s.reaper_queue_depth = reaper ? reaper->depth() : 0;
        s.stale_callbacks = pool.total_stale();
        s.callback_exceptions = pool.total_exceptions();
        s.local_ice_posted = counters.local_ice_posted.load();
        s.local_ice_dropped_oversized = counters.local_ice_dropped_oversized.load();
        s.local_ice_rejected = counters.local_ice_rejected.load();
        s.pending_ice_expired = counters.pending_ice_expired.load();
        s.pending_ice_evicted = counters.pending_ice_evicted.load();
        s.pending_ice_rejected_oversized = counters.pending_ice_rejected_oversized.load();
        s.connecting_timeouts = counters.connecting_timeouts.load();
        s.reaped_total = counters.reaped_total.load();
        s.reap_reasons = counters.reap_reasons();
        return s;
    }

    void request_keyframe() {
        auto logger = spdlog::get("webrtc");
        if (pipeline_ == nullptr) {
            if (logger) logger->warn("Cannot force keyframe: pipeline reference not set");
            return;
        }
        GstElement* encoder = gst_bin_get_by_name(GST_BIN(pipeline_), "encoder");
        if (encoder == nullptr) {
            if (logger) logger->warn("Cannot force keyframe: encoder element not found in pipeline");
            return;
        }
        GstEvent* event = gst_video_event_new_upstream_force_key_unit(
            GST_CLOCK_TIME_NONE, TRUE, 0);
        gst_element_send_event(encoder, event);
        gst_object_unref(encoder);
        if (logger) logger->info("Force keyframe requested for newly connected peer(s)");
    }

    // Fixed shutdown order (Task 4): stop peer admission -> wait in-flight
    // creations -> detach every generation from the map -> Reaper drain/join.
    // The bridge slot pool and adapter cells die with the Impl afterwards
    // (after the runtime token release, see member declaration order).
    void shutdown() {
        if (!admission_open.exchange(false)) return;  // idempotent
        {
            std::unique_lock<std::mutex> lock(inflight_mutex);
            inflight_cv.wait(lock, [this] { return offers_in_flight == 0; });
        }
        std::vector<std::shared_ptr<wm::PeerSession>> victims;
        {
            std::unique_lock<std::shared_mutex> lock(peers_mutex);
            victims.reserve(peers.size());
            for (const auto& entry : peers) victims.push_back(entry.second);
            peers.clear();
            pending_candidates.clear();
        }
        auto logger = spdlog::get("webrtc");
        for (const auto& s : victims) {
            if (logger) logger->info("Shutdown: retiring peer {} (state={})",
                                     s->peer_id, wm::peer_state_name(s->state.load()));
            wm::transition_to_disconnecting(*s, "shutdown", clock->now());
            if (reaper) reaper->submit(s, clock->now());
        }
        victims.clear();
        if (reaper) reaper->drain_and_join();
        if (logger) logger->info("WebRtcMediaManager destroyed, all peers released");
    }
};

// ============================================================
// WebRtcMediaManager public interface
// ============================================================

std::unique_ptr<WebRtcMediaManager> WebRtcMediaManager::create(
    WebRtcSignaling& signaling, const std::string& aws_region,
    std::string* error_msg) {
    wi::RuntimeOptions options;  // defaults: handle_permits = 16
    auto obj = std::unique_ptr<WebRtcMediaManager>(new WebRtcMediaManager());
    obj->impl_ = std::make_unique<Impl>(signaling, options);
    auto& impl = *obj->impl_;
    impl.region = aws_region;
    impl.clock = std::make_shared<wm::MediaSteadyClock>();
#ifdef HAVE_KVS_WEBRTC_SDK
    impl.ops = std::make_shared<wm::ProductionPeerSdkOps>(
        signaling, aws_region, options.handle_permits);
#else
    impl.ops = std::make_shared<wm::StubPeerSdkOps>();
#endif

    uint32_t code = 0;
    impl.runtime_token = wi::KvsRuntimeToken::acquire(&code);
    if (!impl.runtime_token) {
        if (error_msg) {
            *error_msg = "Failed to initialize KVS WebRTC runtime, code: " +
                         wm::media_status_hex(code);
        }
        return nullptr;
    }

    impl.reaper = std::make_unique<wm::PeerReaper>(
        impl.pool.capacity(), impl.ops, impl.clock,
        &impl.peers, &impl.peers_mutex, &impl.pending_candidates, &impl.counters);
    // Local ICE goes out through the signaling fire-and-forget path
    // (try_post_ice_candidate): enqueue only, never waits for a reply.
    // The signaling reference outlives the media manager (fixed shutdown
    // order: media/Reaper -> signaling -> global runtime).
    WebRtcSignaling* sig_ptr = &impl.signaling;
    impl.pool.attach_all(
        impl.reaper.get(), impl.clock, &impl.counters,
        [sig_ptr](const std::string& peer, const std::string& cand) {
            return sig_ptr->try_post_ice_candidate(peer, cand);
        });
    impl.reaper->start();

    auto logger = spdlog::get("webrtc");
#ifdef HAVE_KVS_WEBRTC_SDK
    if (logger) logger->info("Created WebRtcMediaManager (KVS WebRTC SDK)");
#else
    if (logger) logger->info("Created WebRtcMediaManager (stub adapter)");
#endif
    return obj;
}

std::unique_ptr<WebRtcMediaManager> WebRtcMediaManager::create_for_test(
    WebRtcSignaling& signaling, const std::string& region,
    std::shared_ptr<wi::PeerSdkOps> ops,
    std::shared_ptr<wi::RuntimeClock> clock,
    const wi::RuntimeOptions& options,
    std::string* error_msg) {
    if (!ops || !clock) {
        if (error_msg) *error_msg = "create_for_test requires ops and clock";
        return nullptr;
    }
    auto obj = std::unique_ptr<WebRtcMediaManager>(new WebRtcMediaManager());
    obj->impl_ = std::make_unique<Impl>(signaling, options);
    auto& impl = *obj->impl_;
    impl.region = region;
    impl.clock = std::move(clock);
    impl.ops = std::move(ops);

    uint32_t code = 0;
    impl.runtime_token = wi::KvsRuntimeToken::acquire(&code);
    if (!impl.runtime_token) {
        if (error_msg) {
            *error_msg = "Failed to initialize KVS WebRTC runtime, code: " +
                         wm::media_status_hex(code);
        }
        return nullptr;
    }

    impl.reaper = std::make_unique<wm::PeerReaper>(
        impl.pool.capacity(), impl.ops, impl.clock,
        &impl.peers, &impl.peers_mutex, &impl.pending_candidates, &impl.counters);
    // Local ICE goes out through the signaling fire-and-forget path
    // (try_post_ice_candidate): enqueue only, never waits for a reply.
    // The signaling reference outlives the media manager (fixed shutdown
    // order: media/Reaper -> signaling -> global runtime).
    WebRtcSignaling* sig_ptr = &impl.signaling;
    impl.pool.attach_all(
        impl.reaper.get(), impl.clock, &impl.counters,
        [sig_ptr](const std::string& peer, const std::string& cand) {
            return sig_ptr->try_post_ice_candidate(peer, cand);
        });
    impl.reaper->start();
    return obj;
}

bool WebRtcMediaManager::on_viewer_offer(
    const std::string& peer_id, const std::string& sdp_offer,
    std::string* error_msg) {
    return impl_->handle_offer(peer_id, sdp_offer, error_msg);
}

bool WebRtcMediaManager::on_viewer_ice_candidate(
    const std::string& peer_id, const std::string& candidate,
    std::string* error_msg) {
    return impl_->handle_ice(peer_id, candidate, error_msg);
}

void WebRtcMediaManager::remove_peer(const std::string& peer_id) {
    impl_->handle_remove(peer_id);
}

void WebRtcMediaManager::broadcast_frame(
    const uint8_t* data, size_t size,
    uint64_t timestamp_100ns, bool is_keyframe) {
    impl_->handle_broadcast(data, size, timestamp_100ns, is_keyframe);
}

size_t WebRtcMediaManager::peer_count() const {
    return impl_->handle_count();
}

void WebRtcMediaManager::set_pipeline(GstElement* pipeline) {
    impl_->pipeline_ = pipeline;
}

void WebRtcMediaManager::set_writeframe_fail_threshold(int threshold) {
    impl_->writeframe_fail_threshold_.store(threshold);
}

WebRtcMediaManager::HealthSnapshot WebRtcMediaManager::health_snapshot() const {
    return impl_->snapshot();
}

void WebRtcMediaManager::log_health_status() const {
    auto logger = spdlog::get("webrtc");
    if (!logger) return;
    auto s = health_snapshot();
    // 0 viewers with connected signaling is a healthy state (req 6.2):
    // this is observability only and never feeds a recovery decision.
    logger->debug(
        "Media health: active={}, connected={}, live_handles={}, reaper_queue={}, "
        "pending_ice={} ({} peers), oldest_age={}s, stale={}, exceptions={}, "
        "local_ice posted={}/dropped={}/rejected={}, pending_ice expired={}/evicted={}/oversized={}, "
        "connecting_timeouts={}, reaped={}",
        s.active_peers, s.connected_peers, s.live_handles, s.reaper_queue_depth,
        s.pending_ice_depth, s.pending_ice_peers, s.oldest_session_age_sec,
        s.stale_callbacks, s.callback_exceptions,
        s.local_ice_posted, s.local_ice_dropped_oversized, s.local_ice_rejected,
        s.pending_ice_expired, s.pending_ice_evicted, s.pending_ice_rejected_oversized,
        s.connecting_timeouts, s.reaped_total);
}

WebRtcMediaManager::WebRtcMediaManager() = default;
WebRtcMediaManager::~WebRtcMediaManager() = default;
