#ifndef GTA4_PLAYER_INFO_ALIAS_GATE_H_
#define GTA4_PLAYER_INFO_ALIAS_GATE_H_

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace gta4::multiplayer64 {

// Low-primary guest operations prevent a projection from starting but are not
// host pointer leases: lifecycle work may run on a worker while its parent is
// waiting. Short pointer leases still exclude lifecycle mutation. An extended
// projection is joinable, including a recursive mutation admission for a guest
// worker that must perform lifecycle work inside the published session.
template <typename Session>
class JoinableProjectionGate {
 public:
  enum class AdmissionKind {
    kOrdinaryOperation,
    kOrdinaryReader,
    kProjectionOwner,
    kProjectionParticipant,
    kOpaqueOwnerBypass,
  };

  class ReadAdmission {
   public:
    ReadAdmission() = default;
    ReadAdmission(const ReadAdmission&) = delete;
    ReadAdmission& operator=(const ReadAdmission&) = delete;
    ReadAdmission(ReadAdmission&& other) noexcept { MoveFrom(other); }
    ReadAdmission& operator=(ReadAdmission&& other) noexcept {
      if (this != &other) {
        Release();
        MoveFrom(other);
      }
      return *this;
    }
    ~ReadAdmission() { Release(); }
    AdmissionKind kind() const noexcept { return kind_; }
    const Session* session() const noexcept { return session_ ? &*session_ : nullptr; }

   private:
    friend class JoinableProjectionGate;
    ReadAdmission(JoinableProjectionGate* gate, AdmissionKind kind, std::optional<Session> session,
                  std::thread::id thread)
        : gate_(gate), kind_(kind), session_(std::move(session)), thread_(thread) {}
    void MoveFrom(ReadAdmission& other) noexcept {
      gate_ = std::exchange(other.gate_, nullptr);
      kind_ = other.kind_;
      session_ = std::move(other.session_);
      thread_ = other.thread_;
    }
    void Release() noexcept {
      if (gate_ != nullptr) {
        gate_->LeaveAdmission(kind_, thread_);
        gate_ = nullptr;
      }
    }
    JoinableProjectionGate* gate_ = nullptr;
    AdmissionKind kind_ = AdmissionKind::kOpaqueOwnerBypass;
    std::optional<Session> session_;
    std::thread::id thread_;
  };

  class ProjectionAdmission {
   public:
    ProjectionAdmission() = default;
    ProjectionAdmission(const ProjectionAdmission&) = delete;
    ProjectionAdmission& operator=(const ProjectionAdmission&) = delete;
    ProjectionAdmission(ProjectionAdmission&& other) noexcept { MoveFrom(other); }
    ProjectionAdmission& operator=(ProjectionAdmission&& other) noexcept {
      if (this != &other) {
        Release();
        MoveFrom(other);
      }
      return *this;
    }
    ~ProjectionAdmission() { Release(); }
    AdmissionKind kind() const noexcept { return kind_; }
    bool is_owner() const noexcept { return kind_ == AdmissionKind::kProjectionOwner; }
    const Session* session() const noexcept { return session_ ? &*session_ : nullptr; }
    template <typename Finish>
    void Complete(Finish&& finish) {
      Complete([] {}, std::forward<Finish>(finish));
    }
    template <typename BeginWait, typename Finish>
    void Complete(BeginWait&& begin_wait, Finish&& finish) {
      if (gate_ == nullptr || !is_owner()) {
        return;
      }
      JoinableProjectionGate* gate = std::exchange(gate_, nullptr);
      gate->CompleteProjection(*session_, thread_, std::forward<BeginWait>(begin_wait),
                               std::forward<Finish>(finish));
    }

   private:
    friend class JoinableProjectionGate;
    ProjectionAdmission(JoinableProjectionGate* gate, AdmissionKind kind,
                        std::optional<Session> session, std::thread::id thread)
        : gate_(gate), kind_(kind), session_(std::move(session)), thread_(thread) {}
    void MoveFrom(ProjectionAdmission& other) noexcept {
      gate_ = std::exchange(other.gate_, nullptr);
      kind_ = other.kind_;
      session_ = std::move(other.session_);
      thread_ = other.thread_;
    }
    void Release() noexcept {
      if (gate_ == nullptr) {
        return;
      }
      if (is_owner()) {
        std::terminate();
      }
      gate_->LeaveAdmission(kind_, thread_);
      gate_ = nullptr;
    }
    JoinableProjectionGate* gate_ = nullptr;
    AdmissionKind kind_ = AdmissionKind::kOpaqueOwnerBypass;
    std::optional<Session> session_;
    std::thread::id thread_;
  };

  class OpaqueAdmission {
   public:
    OpaqueAdmission() = default;
    OpaqueAdmission(const OpaqueAdmission&) = delete;
    OpaqueAdmission& operator=(const OpaqueAdmission&) = delete;
    OpaqueAdmission(OpaqueAdmission&& other) noexcept { MoveFrom(other); }
    OpaqueAdmission& operator=(OpaqueAdmission&& other) noexcept {
      if (this != &other) {
        Release();
        MoveFrom(other);
      }
      return *this;
    }
    ~OpaqueAdmission() { Release(); }

   private:
    friend class JoinableProjectionGate;
    enum class Kind { kNone, kOpaque, kProjection };
    OpaqueAdmission(JoinableProjectionGate* gate, Kind kind, std::thread::id thread)
        : gate_(gate), kind_(kind), thread_(thread) {}
    void MoveFrom(OpaqueAdmission& other) noexcept {
      gate_ = std::exchange(other.gate_, nullptr);
      kind_ = std::exchange(other.kind_, Kind::kNone);
      thread_ = other.thread_;
    }
    void Release() noexcept {
      if (gate_ == nullptr) {
        return;
      }
      if (kind_ == Kind::kOpaque) {
        gate_->LeaveOpaque();
      } else if (kind_ == Kind::kProjection) {
        gate_->LeaveProjectionMutation(thread_);
      }
      gate_ = nullptr;
    }
    JoinableProjectionGate* gate_ = nullptr;
    Kind kind_ = Kind::kNone;
    std::thread::id thread_;
  };

  ReadAdmission EnterRead() {
    std::unique_lock lock(mutex_);
    const auto caller = std::this_thread::get_id();
    for (;;) {
      ThreadState& state = State(caller);
      if (mode_ == Mode::kProjection) {
        if (closing_projection_ && state.projection_depth == 0) {
          condition_.wait(lock);
          continue;
        }
        ++state.projection_depth;
        ++projection_participants_;
        return {this, AdmissionKind::kProjectionParticipant, session_, caller};
      }
      if (mode_ == Mode::kOpaque) {
        if (opaque_owner_ == caller) {
          return {this, AdmissionKind::kOpaqueOwnerBypass, std::nullopt, caller};
        }
        condition_.wait(lock);
        continue;
      }
      if (exclusive_waiters_ != 0 && state.reader_depth == 0 && state.operation_depth == 0) {
        condition_.wait(lock);
        continue;
      }
      ++state.reader_depth;
      ++ordinary_readers_;
      return {this, AdmissionKind::kOrdinaryReader, std::nullopt, caller};
    }
  }

  template <typename ReadValue, typename ShouldProject, typename StartProjection>
  ProjectionAdmission EnterProjection(ReadValue&& read_value, ShouldProject&& should_project,
                                      StartProjection&& start_projection) {
    std::unique_lock lock(mutex_);
    const auto caller = std::this_thread::get_id();
    bool waiting = false;
    for (;;) {
      ThreadState& state = State(caller);
      if (mode_ == Mode::kProjection) {
        if (closing_projection_ && state.projection_depth == 0) {
          condition_.wait(lock);
          continue;
        }
        Unregister(waiting);
        ++state.projection_depth;
        ++projection_participants_;
        return {this, AdmissionKind::kProjectionParticipant, session_, caller};
      }
      if (mode_ == Mode::kOpaque) {
        if (opaque_owner_ == caller) {
          Unregister(waiting);
          return {this, AdmissionKind::kOpaqueOwnerBypass, std::nullopt, caller};
        }
        Register(waiting);
        condition_.wait(lock);
        continue;
      }
      if (exclusive_waiters_ != (waiting ? 1U : 0U) && state.operation_depth == 0) {
        condition_.wait(lock);
        continue;
      }
      const auto value = std::invoke(read_value);
      if (!std::invoke(should_project, value)) {
        Unregister(waiting);
        ++state.operation_depth;
        ++ordinary_operations_;
        return {this, AdmissionKind::kOrdinaryOperation, std::nullopt, caller};
      }
      if (ordinary_readers_ != 0 || ordinary_operations_ != 0) {
        Register(waiting);
        condition_.wait(lock);
        continue;
      }
      Unregister(waiting);
      session_ = std::invoke(start_projection, value);
      projection_owner_ = caller;
      projection_participants_ = 1;
      ++state.projection_depth;
      mode_ = Mode::kProjection;
      return {this, AdmissionKind::kProjectionOwner, session_, caller};
    }
  }

  OpaqueAdmission EnterOpaque() {
    std::unique_lock lock(mutex_);
    const auto caller = std::this_thread::get_id();
    bool waiting = false;
    for (;;) {
      ThreadState& state = State(caller);
      if (mode_ == Mode::kProjection && state.projection_depth != 0) {
        Unregister(waiting);
        if (projection_mutator_ == caller) {
          ++projection_mutation_depth_;
          return {this, OpaqueAdmission::Kind::kProjection, caller};
        }
        projection_mutation_contenders_.insert(caller);
        closing_projection_ = true;
        condition_.notify_all();
        if (projection_mutation_depth_ == 0) {
          condition_.wait(lock, [this] {
            return projection_participants_ == RetainedProjectionParticipants() &&
                   projection_mutation_depth_ == 0;
          });
          projection_mutator_ = caller;
          projection_mutation_depth_ = 1;
          return {this, OpaqueAdmission::Kind::kProjection, caller};
        }
        condition_.wait(lock);
        continue;
      }
      if (mode_ == Mode::kOpaque && opaque_owner_ == caller) {
        Unregister(waiting);
        ++opaque_depth_;
        return {this, OpaqueAdmission::Kind::kOpaque, caller};
      }
      const size_t own_readers = state.reader_depth;
      if (mode_ == Mode::kIdle && ordinary_readers_ == own_readers) {
        Unregister(waiting);
        ordinary_readers_ -= own_readers;
        suspended_readers_ = own_readers;
        mode_ = Mode::kOpaque;
        opaque_owner_ = caller;
        opaque_depth_ = 1;
        return {this, OpaqueAdmission::Kind::kOpaque, caller};
      }
      Register(waiting);
      condition_.wait(lock);
    }
  }

 private:
  enum class Mode { kIdle, kProjection, kOpaque };
  struct ThreadState {
    size_t operation_depth = 0;
    size_t reader_depth = 0;
    size_t projection_depth = 0;
  };
  ThreadState& State(std::thread::id thread) { return thread_states_[thread]; }
  void Register(bool& waiting) noexcept {
    if (!waiting) {
      ++exclusive_waiters_;
      waiting = true;
    }
  }
  void Unregister(bool& waiting) noexcept {
    if (waiting) {
      --exclusive_waiters_;
      waiting = false;
    }
  }
  void LeaveAdmission(AdmissionKind kind, std::thread::id thread) noexcept {
    if (kind == AdmissionKind::kOpaqueOwnerBypass) {
      return;
    }
    {
      std::scoped_lock lock(mutex_);
      ThreadState& state = State(thread);
      if (kind == AdmissionKind::kOrdinaryOperation) {
        --ordinary_operations_;
        --state.operation_depth;
      } else if (kind == AdmissionKind::kOrdinaryReader) {
        --ordinary_readers_;
        --state.reader_depth;
      } else {
        --projection_participants_;
        --state.projection_depth;
      }
    }
    condition_.notify_all();
  }
  template <typename BeginWait, typename Finish>
  void CompleteProjection(const Session& session, std::thread::id thread, BeginWait&& begin_wait,
                          Finish&& finish) {
    std::exception_ptr failure;
    std::unique_lock lock(mutex_);
    closing_projection_ = true;
    try {
      std::invoke(std::forward<BeginWait>(begin_wait));
    } catch (...) {
      failure = std::current_exception();
    }
    condition_.wait(
        lock, [this] { return projection_participants_ == 1 && projection_mutation_depth_ == 0; });
    try {
      std::invoke(std::forward<Finish>(finish), session);
    } catch (...) {
      if (failure == nullptr) {
        failure = std::current_exception();
      }
    }
    projection_participants_ = 0;
    --State(thread).projection_depth;
    projection_owner_ = {};
    projection_mutator_ = {};
    session_.reset();
    closing_projection_ = false;
    mode_ = Mode::kIdle;
    lock.unlock();
    condition_.notify_all();
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
  }
  void LeaveOpaque() noexcept {
    {
      std::scoped_lock lock(mutex_);
      if (--opaque_depth_ != 0) {
        return;
      }
      opaque_owner_ = {};
      ordinary_readers_ += suspended_readers_;
      suspended_readers_ = 0;
      mode_ = Mode::kIdle;
    }
    condition_.notify_all();
  }
  void LeaveProjectionMutation(std::thread::id thread) noexcept {
    {
      std::scoped_lock lock(mutex_);
      if (projection_mutator_ != thread) {
        std::terminate();
      }
      if (--projection_mutation_depth_ != 0) {
        return;
      }
      projection_mutator_ = {};
      projection_mutation_contenders_.erase(thread);
      closing_projection_ = !projection_mutation_contenders_.empty();
    }
    condition_.notify_all();
  }

  size_t RetainedProjectionParticipants() {
    size_t retained = State(projection_owner_).projection_depth;
    for (const std::thread::id contender : projection_mutation_contenders_) {
      if (contender != projection_owner_) {
        retained += State(contender).projection_depth;
      }
    }
    return retained;
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  Mode mode_ = Mode::kIdle;
  size_t ordinary_operations_ = 0;
  size_t ordinary_readers_ = 0;
  size_t projection_participants_ = 0;
  size_t opaque_depth_ = 0;
  size_t projection_mutation_depth_ = 0;
  size_t suspended_readers_ = 0;
  size_t exclusive_waiters_ = 0;
  std::thread::id projection_owner_;
  std::thread::id projection_mutator_;
  std::thread::id opaque_owner_;
  bool closing_projection_ = false;
  std::optional<Session> session_;
  std::unordered_map<std::thread::id, ThreadState> thread_states_;
  std::unordered_set<std::thread::id> projection_mutation_contenders_;
};

}  // namespace gta4::multiplayer64
#endif
