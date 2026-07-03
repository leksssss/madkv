#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <random>
#include <map>
#include <atomic>
#include <endian.h>
#include <iostream>

#include "raft.pb.h"
#include "raft.grpc.pb.h"
#include "kvstore.pb.h"
#include "rocksdb/db.h"
#include "absl/strings/str_format.h"
#include <grpcpp/grpcpp.h>

class RaftServer {
 public:
  enum State { FOLLOWER, CANDIDATE, LEADER };

  struct Config {
    int32_t server_id;
    int32_t replica_id;
    std::vector<std::string> peer_addrs;  // excludes self
    int election_timeout_min_ms = 150;
    int election_timeout_max_ms = 300;
    int heartbeat_interval_ms = 50;
  };

  RaftServer(const Config& config, rocksdb::DB* storage)
      : config_(config),
        storage_(storage),
        current_term_(0),
        voted_for_(-1),
        commit_index_(-1),
        last_applied_(-1),
        state_(FOLLOWER),
        leader_id_(-1) {
    LoadPersistentState();
    for (size_t i = 0; i < config_.peer_addrs.size(); i++) {
      next_index_[i] = GetLastLogIndex() + 1;
      match_index_[i] = -1;
    }
    InitializePeerStubs();
    ResetElectionTimer();
  }

  ~RaftServer() { StopBackgroundThreads(); }

  bool IsLeader() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_ == LEADER;
  }

  int32_t GetLeaderId() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return leader_id_;
  }

  int AppendCommand(const kvstore::KeyValuePair& command) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ != LEADER) return -1;
    raft::LogEntry entry;
    entry.set_term(current_term_);
    entry.mutable_command()->CopyFrom(command);
    int index = WriteLogEntry(entry);
    // Track self replication
    match_index_[config_.peer_addrs.size()] = index;  // use peer_count as self slot
    UpdateCommitIndex();
    return index;
  }

  bool GetLogEntry(int index, raft::LogEntry& entry) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return ReadLogEntry(index, entry);
  }

  int GetCommitIndex() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return commit_index_;
  }

  void SetLastApplied(int index) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_applied_ = index;
  }

  int GetLastApplied() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_applied_;
  }

  void HandleRequestVote(const raft::RequestVoteRequest& req,
                         raft::RequestVoteResponse& res) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    res.set_term(current_term_);
    res.set_vote_granted(false);

    if (req.term() < current_term_) return;

    if (req.term() > current_term_) {
      current_term_ = req.term();
      state_ = FOLLOWER;
      voted_for_ = -1;
      leader_id_ = -1;
      PersistTermAndVotedFor();
      res.set_term(current_term_);
    }

    bool log_ok = req.last_log_term() > GetLastLogTerm() ||
                  (req.last_log_term() == GetLastLogTerm() &&
                   req.last_log_index() >= GetLastLogIndex());

    if ((voted_for_ == -1 || voted_for_ == req.candidate_id()) && log_ok) {
      voted_for_ = req.candidate_id();
      PersistTermAndVotedFor();
      res.set_vote_granted(true);
      ResetElectionTimer();  // reset timer when granting vote
    }
  }

  void HandleAppendEntries(const raft::AppendEntriesRequest& req,
                           raft::AppendEntriesResponse& res) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    res.set_term(current_term_);
    res.set_success(false);
    res.set_match_index(-1);

    if (req.term() < current_term_) return;

    if (req.term() > current_term_) {
      current_term_ = req.term();
      voted_for_ = -1;
      PersistTermAndVotedFor();
      res.set_term(current_term_);
    }

    // Valid leader heartbeat — reset election timer
    state_ = FOLLOWER;
    leader_id_ = req.leader_id();
    ResetElectionTimer();

    int prev_log_index = req.prev_log_index();
    if (prev_log_index >= 0) {
      raft::LogEntry prev_entry;
      if (!ReadLogEntry(prev_log_index, prev_entry) ||
          prev_entry.term() != req.prev_log_term()) {
        return;  // log inconsistency — reject
      }
    }

    int insert_index = prev_log_index + 1;
    for (const auto& entry : req.entries()) {
      raft::LogEntry existing;
      bool exists = ReadLogEntry(insert_index, existing);
      if (exists && existing.term() != entry.term())
        DeleteLogEntriesFrom(insert_index);
      if (!exists || existing.term() != entry.term())
        WriteLogEntry(entry, insert_index);
      insert_index++;
    }

    if (req.leader_commit() > commit_index_)
      commit_index_ = std::min(req.leader_commit(), GetLastLogIndex());

    res.set_success(true);
    res.set_match_index(insert_index - 1);
  }

  void StartBackgroundThreads() {
    election_thread_  = std::thread([this] { ElectionLoop(); });
    heartbeat_thread_ = std::thread([this] { HeartbeatLoop(); });
  }

  void StopBackgroundThreads() {
    should_stop_ = true;
    election_cv_.notify_all();
    if (election_thread_.joinable())  election_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
  }

  std::string GetStatus() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const char* s = state_ == LEADER ? "LEADER" :
                    state_ == CANDIDATE ? "CANDIDATE" : "FOLLOWER";
    return absl::StrFormat(
        "replica=%d state=%s term=%d leader=%d commit=%d applied=%d lastLog=%d",
        config_.replica_id, s, current_term_, leader_id_,
        commit_index_, last_applied_, GetLastLogIndex());
  }

 private:
  Config config_;
  rocksdb::DB* storage_;

  // Persistent state
  int32_t current_term_;
  int32_t voted_for_;

  // Volatile state
  int commit_index_;
  int last_applied_;
  State state_;
  int32_t leader_id_;

  // Leader state: indexed 0..peer_count-1 for peers, peer_count for self
  std::map<int, int> next_index_;
  std::map<int, int> match_index_;

  mutable std::mutex state_mutex_;
  std::thread election_thread_;
  std::thread heartbeat_thread_;
  std::condition_variable election_cv_;
  std::atomic<bool> should_stop_{false};
  std::chrono::steady_clock::time_point election_timeout_;

  // FIX 2: one stub per entry in peer_addrs (no self-skip needed)
  std::map<int, std::unique_ptr<raft::Raft::Stub>> peer_stubs_;

  static constexpr const char* TERM_KEY          = "raft:term";
  static constexpr const char* VOTED_FOR_KEY      = "raft:voted_for";
  static constexpr const char* LOG_ENTRY_PREFIX   = "raft:log:";
  static constexpr const char* LAST_LOG_INDEX_KEY = "raft:last_log_index";

  // ── FIX 2: peer_addrs excludes self, so connect to ALL entries ────────────
  void InitializePeerStubs() {
    for (size_t i = 0; i < config_.peer_addrs.size(); i++) {
      auto ch = grpc::CreateChannel(config_.peer_addrs[i],
                                    grpc::InsecureChannelCredentials());
      peer_stubs_[i] = raft::Raft::NewStub(ch);
    }
  }

  void LoadPersistentState() {
    std::string val;
    if (storage_->Get(rocksdb::ReadOptions(), TERM_KEY, &val).ok() &&
        val.size() == sizeof(int32_t))
      current_term_ = *reinterpret_cast<const int32_t*>(val.data());
    if (storage_->Get(rocksdb::ReadOptions(), VOTED_FOR_KEY, &val).ok() &&
        val.size() == sizeof(int32_t))
      voted_for_ = *reinterpret_cast<const int32_t*>(val.data());
  }

  void PersistTermAndVotedFor() {
    rocksdb::WriteOptions wo; wo.sync = true;
    std::string t(reinterpret_cast<const char*>(&current_term_), sizeof(current_term_));
    std::string v(reinterpret_cast<const char*>(&voted_for_),   sizeof(voted_for_));
    storage_->Put(wo, TERM_KEY,      t);
    storage_->Put(wo, VOTED_FOR_KEY, v);
  }

  int WriteLogEntry(const raft::LogEntry& entry) {
    return WriteLogEntry(entry, GetLastLogIndex() + 1);
  }

  int WriteLogEntry(const raft::LogEntry& entry, int index) {
    std::string val; entry.SerializeToString(&val);
    rocksdb::WriteOptions wo; wo.sync = true;
    storage_->Put(wo, LOG_ENTRY_PREFIX + std::to_string(index), val);
    std::string idx(reinterpret_cast<const char*>(&index), sizeof(index));
    storage_->Put(wo, LAST_LOG_INDEX_KEY, idx);
    return index;
  }

  bool ReadLogEntry(int index, raft::LogEntry& entry) const {
    std::string val;
    if (!storage_->Get(rocksdb::ReadOptions(),
                       LOG_ENTRY_PREFIX + std::to_string(index), &val).ok())
      return false;
    return entry.ParseFromString(val);
  }

  void DeleteLogEntriesFrom(int start_index) {
    int last = GetLastLogIndex();
    for (int i = start_index; i <= last; i++)
      storage_->Delete(rocksdb::WriteOptions(),
                       LOG_ENTRY_PREFIX + std::to_string(i));
    int ni = start_index - 1;
    std::string idx(reinterpret_cast<const char*>(&ni), sizeof(ni));
    storage_->Put(rocksdb::WriteOptions(), LAST_LOG_INDEX_KEY, idx);
  }

  int GetLastLogIndex() const {
    std::string val;
    if (storage_->Get(rocksdb::ReadOptions(), LAST_LOG_INDEX_KEY, &val).ok() &&
        val.size() == sizeof(int32_t))
      return *reinterpret_cast<const int32_t*>(val.data());
    return -1;
  }

  int GetLastLogTerm() const {
    int li = GetLastLogIndex();
    if (li < 0) return 0;
    raft::LogEntry e;
    return ReadLogEntry(li, e) ? e.term() : 0;
  }

  void ResetElectionTimer() {
    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis(config_.election_timeout_min_ms,
                                        config_.election_timeout_max_ms);
    election_timeout_ = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(dis(gen));
    election_cv_.notify_all();
  }

  // ── FIX 1: check clock directly after wait_until ──────────────────────────
  //  FIX 4: exponential backoff after failed election to prevent election storms
  void ElectionLoop() {
    int consecutive_failures = 0;
    while (!should_stop_) {
      bool should_elect = false;
      {
        std::unique_lock<std::mutex> lock(state_mutex_);
        // wait_until returns no_timeout if deadline is already in the past,
        // so we always check the clock directly afterward.
        election_cv_.wait_until(lock, election_timeout_);
        if (!should_stop_ && state_ != LEADER &&
            std::chrono::steady_clock::now() >= election_timeout_) {
          should_elect = true;
          ResetElectionTimer();  // push deadline forward before releasing lock
        }
      }
      if (should_elect) {
        int voted = StartElection();  // returns votes_granted
        if (voted < 0) {
          // Election failed; add backoff
          consecutive_failures++;
          int backoff_ms = std::min(500, 10 * consecutive_failures);
          std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        } else {
          consecutive_failures = 0;
        }
      }
    }
  }

  // ── FIX 3: check state under lock, spawn threads outside ──────────────────
  void HeartbeatLoop() {
    while (!should_stop_) {
      bool is_leader = false;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        is_leader = (state_ == LEADER);
      }
      if (is_leader) {
        // Spawn one thread per peer — no lock held here
        for (size_t i = 0; i < config_.peer_addrs.size(); i++)
          std::thread([this, i] { SendAppendEntries(i); }).detach();
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(config_.heartbeat_interval_ms));
    }
  }

  int StartElection() {
    int term_snapshot, last_log_index, last_log_term;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      state_ = CANDIDATE;
      current_term_++;
      voted_for_ = config_.replica_id;
      PersistTermAndVotedFor();
      // Timer already reset in ElectionLoop before calling us
      term_snapshot  = current_term_;
      last_log_index = GetLastLogIndex();
      last_log_term  = GetLastLogTerm();
    }

    std::atomic<int> votes_granted{1};  // vote for self
    std::vector<std::thread> threads;

    // FIX 2: iterate all peers (peer_addrs excludes self, no skip needed)
    for (size_t i = 0; i < config_.peer_addrs.size(); i++) {
      threads.emplace_back([this, i, &votes_granted,
                            term_snapshot, last_log_index, last_log_term] {
        SendRequestVote(i, votes_granted, term_snapshot,
                        last_log_index, last_log_term);
      });
    }
    for (auto& t : threads) t.join();  // wait for all votes (each has 500ms deadline)

    std::lock_guard<std::mutex> lock(state_mutex_);

    // Bail if term changed or we already became follower
    if (state_ != CANDIDATE || current_term_ != term_snapshot) return -1;

    // total cluster size = peers + self
    int total    = (int)config_.peer_addrs.size() + 1;
    int majority = total / 2 + 1;

    if (votes_granted.load() >= majority) {
      state_    = LEADER;
      leader_id_ = config_.replica_id;
      // Reinitialize leader tracking
      int next = GetLastLogIndex() + 1;
      for (size_t i = 0; i < config_.peer_addrs.size(); i++) {
        next_index_[i]  = next;
        match_index_[i] = -1;
      }
      std::cout << "Replica " << config_.replica_id
                << " elected LEADER term=" << current_term_
                << " votes=" << votes_granted.load() << "\n";
      std::cout.flush();
      return votes_granted.load();
    }
    // If lost election, ElectionLoop will add backoff before next attempt
    return -1;
  }

  void SendRequestVote(size_t peer_idx, std::atomic<int>& votes_granted,
                       int term, int last_log_index, int last_log_term) {
    auto it = peer_stubs_.find(peer_idx);
    if (it == peer_stubs_.end()) return;

    raft::RequestVoteRequest req;
    req.set_term(term);
    req.set_candidate_id(config_.replica_id);
    req.set_last_log_index(last_log_index);
    req.set_last_log_term(last_log_term);

    raft::RequestVoteResponse res;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::milliseconds(500));

    if (it->second->RequestVote(&ctx, req, &res).ok() && res.vote_granted())
      votes_granted++;

    // If they have a higher term, step down
    if (res.term() > term) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (res.term() > current_term_) {
        current_term_ = res.term();
        state_ = FOLLOWER;
        voted_for_ = -1;
        PersistTermAndVotedFor();
      }
    }
  }

  void SendAppendEntries(size_t peer_idx) {
    auto it = peer_stubs_.find(peer_idx);
    if (it == peer_stubs_.end()) return;

    // Snapshot everything needed under lock
    int term, prev_log_index, prev_log_term, commit_idx;
    std::vector<raft::LogEntry> entries;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_ != LEADER) return;  // stepped down since heartbeat was scheduled
      term           = current_term_;
      prev_log_index = next_index_[peer_idx] - 1;
      prev_log_term  = 0;
      raft::LogEntry prev;
      if (prev_log_index >= 0 && ReadLogEntry(prev_log_index, prev))
        prev_log_term = prev.term();
      commit_idx = commit_index_;
      for (int i = next_index_[peer_idx]; i <= GetLastLogIndex(); i++) {
        raft::LogEntry e;
        if (ReadLogEntry(i, e)) entries.push_back(e);
      }
    }

    raft::AppendEntriesRequest req;
    req.set_term(term);
    req.set_leader_id(config_.replica_id);
    req.set_prev_log_index(prev_log_index);
    req.set_prev_log_term(prev_log_term);
    req.set_leader_commit(commit_idx);
    for (const auto& e : entries) req.add_entries()->CopyFrom(e);

    raft::AppendEntriesResponse res;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::milliseconds(500));

    auto status = it->second->AppendEntries(&ctx, req, &res);
    if (!status.ok()) return;

    std::lock_guard<std::mutex> lock(state_mutex_);

    // Step down if we see a higher term
    if (res.term() > current_term_) {
      current_term_ = res.term();
      state_ = FOLLOWER;
      voted_for_ = -1;
      PersistTermAndVotedFor();
      return;
    }

    if (res.success()) {
      match_index_[peer_idx] = res.match_index();
      next_index_[peer_idx]  = res.match_index() + 1;
      UpdateCommitIndex();
    } else {
      if (next_index_[peer_idx] > 0) next_index_[peer_idx]--;
    }
  }

  void UpdateCommitIndex() {
    // Only commit entries from current term (Raft safety rule)
    int last = GetLastLogIndex();
    for (int i = last; i > commit_index_; i--) {
      raft::LogEntry entry;
      if (!ReadLogEntry(i, entry)) continue;
      if (entry.term() != current_term_) continue;  // never commit old-term entries directly

      // Count how many replicas have this entry (peers + self)
      int count = 1;  // self always has it (we wrote it)
      for (size_t j = 0; j < config_.peer_addrs.size(); j++)
        if (match_index_[j] >= i) count++;

      int total    = (int)config_.peer_addrs.size() + 1;
      int majority = total / 2 + 1;
      if (count >= majority) {
        commit_index_ = i;
        break;
      }
    }
  }
};