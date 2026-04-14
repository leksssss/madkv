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
#include <iostream>
#include <algorithm>

#include <grpcpp/grpcpp.h>
#include "raft.pb.h"
#include "raft.grpc.pb.h"
#include "kvstore.pb.h"
#include "rocksdb/db.h"

class RaftServer {
 public:
  enum State { FOLLOWER, CANDIDATE, LEADER };

  struct Config {
    int32_t replica_id;
    std::vector<std::string> peer_addrs;
    int election_timeout_min_ms = 300;
    int election_timeout_max_ms = 1500;
    int heartbeat_interval_ms   = 25;
  };

  RaftServer(const Config& config, rocksdb::DB* storage)
      : config_(config),
        storage_(storage),
        gen_(std::random_device{}()),
        current_term_(0),
        voted_for_(-1),
        commit_index_(-1),
        last_applied_(-1),
        state_(FOLLOWER),
        leader_id_(-1),
        last_quorum_ms_(0),
        should_stop_(false) {

    LoadPersistentState();

    int last = GetLastLogIndex();
    for (size_t i = 0; i < config_.peer_addrs.size(); i++) {
      next_index_[i]  = last + 1;
      match_index_[i] = -1;
    }

    InitializePeerStubs();

    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.replica_id * 200));

    ResetElectionTimer();
  }

  ~RaftServer() {
    StopBackgroundThreads();
  }

  // ================= PUBLIC API =================

  bool IsLeader() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_ == LEADER;
  }

  int GetLeaderId() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return leader_id_;
  }

  int GetCommitIndex() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return commit_index_;
  }

  int GetLastApplied() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_applied_;
  }

  void SetLastApplied(int idx) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_applied_ = idx;
  }

  int AppendCommand(const kvstore::KeyValuePair& cmd) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ != LEADER) return -1;

    raft::LogEntry entry;
    entry.set_term(current_term_);
    entry.mutable_command()->CopyFrom(cmd);

    int index = WriteLogEntry(entry, GetLastLogIndex() + 1);
    UpdateCommitIndex();
    return index;
  }

  bool GetLogEntry(int index, raft::LogEntry& out) {
    return ReadLogEntry(index, out);
  }

  void StartBackgroundThreads() {
    should_stop_ = false;
    election_thread_  = std::thread([this] { ElectionLoop(); });
    heartbeat_thread_ = std::thread([this] { HeartbeatLoop(); });
  }

  void StopBackgroundThreads() {
    should_stop_ = true;
    election_cv_.notify_all();
    if (election_thread_.joinable())  election_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
  }

  bool HasRecentQuorum() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ != LEADER) return false;

    auto last = std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(
            last_quorum_ms_.load(std::memory_order_relaxed)));
    auto elapsed = std::chrono::steady_clock::now() - last;

    return elapsed <=
           std::chrono::milliseconds(config_.heartbeat_interval_ms * 2);
  }

  // ================= RPC HANDLERS =================

  void HandleRequestVote(const raft::RequestVoteRequest& req,
                         raft::RequestVoteResponse& res) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    res.set_term(current_term_);
    res.set_vote_granted(false);

    if (req.term() < current_term_) return;

    if (req.term() > current_term_) {
      current_term_ = req.term();
      voted_for_    = -1;
      state_        = FOLLOWER;
      leader_id_    = -1;
      PersistTermAndVotedFor();
    }

    bool log_ok =
        (req.last_log_term() > GetLastLogTerm()) ||
        (req.last_log_term() == GetLastLogTerm() &&
         req.last_log_index() >= GetLastLogIndex());

    if ((voted_for_ == -1 || voted_for_ == req.candidate_id()) && log_ok) {
      voted_for_ = req.candidate_id();
      res.set_vote_granted(true);
      PersistTermAndVotedFor();
      ResetElectionTimer();
    }

    res.set_term(current_term_);
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
      voted_for_    = -1;
      PersistTermAndVotedFor();
    }

    state_     = FOLLOWER;
    leader_id_ = req.leader_id();
    ResetElectionTimer();

    int prev = req.prev_log_index();

    if (prev >= 0) {
      raft::LogEntry prev_entry;
      if (!ReadLogEntry(prev, prev_entry) ||
          prev_entry.term() != req.prev_log_term()) {
        return;
      }
    }

    int insert = prev + 1;

    for (const auto& e : req.entries()) {
      raft::LogEntry existing;
      bool exists = ReadLogEntry(insert, existing);

      if (exists && existing.term() != e.term()) {
        DeleteLogEntriesFrom(insert);
        exists = false;
      }

      if (!exists) {
        WriteLogEntry(e, insert);
      }

      insert++;
    }

    commit_index_ =
        std::max(commit_index_,
                 std::min(req.leader_commit(), GetLastLogIndex()));

    res.set_success(true);
    res.set_match_index(insert - 1);
  }

 private:
  // ================= STATE =================
  Config config_;
  rocksdb::DB* storage_;

  std::mt19937 gen_;

  int32_t current_term_;
  int32_t voted_for_;

  int commit_index_;
  int last_applied_;
  State state_;
  int32_t leader_id_;

  std::map<int, int> next_index_;
  std::map<int, int> match_index_;

  // In-memory Raft log for fast access (no disk persistence)
  std::map<int, raft::LogEntry> log_;
  int last_log_index_ = -1;

  std::mutex state_mutex_;
  std::thread election_thread_;
  std::thread heartbeat_thread_;
  std::condition_variable election_cv_;
  std::atomic<bool> should_stop_;

  std::chrono::steady_clock::time_point election_timeout_;

  std::atomic<std::chrono::steady_clock::duration::rep> last_quorum_ms_;

  std::map<int, std::unique_ptr<raft::Raft::Stub>> peer_stubs_;

  static constexpr const char* TERM_KEY   = "raft:term";
  static constexpr const char* VOTED_KEY  = "raft:voted";
  static constexpr const char* LOG_PREFIX = "raft:log:";
  static constexpr const char* LAST_INDEX = "raft:last";

  // ================= INIT =================

  void InitializePeerStubs() {
    for (size_t i = 0; i < config_.peer_addrs.size(); i++) {
      auto ch = grpc::CreateChannel(config_.peer_addrs[i],
                                    grpc::InsecureChannelCredentials());
      peer_stubs_[i] = raft::Raft::NewStub(ch);
    }
  }

  void LoadPersistentState() {
    std::string v;
    if (storage_->Get(rocksdb::ReadOptions(), TERM_KEY, &v).ok() &&
        v.size() == sizeof(int32_t)) {
      current_term_ = *reinterpret_cast<const int32_t*>(v.data());
    }

    if (storage_->Get(rocksdb::ReadOptions(), VOTED_KEY, &v).ok() &&
        v.size() == sizeof(int32_t)) {
      voted_for_ = *reinterpret_cast<const int32_t*>(v.data());
    }
  }

  void PersistTermAndVotedFor() {
    rocksdb::WriteOptions wo;
    wo.sync = true;

    std::string t((char*)&current_term_, sizeof(current_term_));
    std::string v((char*)&voted_for_,    sizeof(voted_for_));

    storage_->Put(wo, TERM_KEY,  t);
    storage_->Put(wo, VOTED_KEY, v);
  }

  // ================= LOG =================

  int WriteLogEntry(const raft::LogEntry& e, int idx) {
    log_[idx] = e;
    last_log_index_ = std::max(last_log_index_, idx);
    return idx;
  }

  bool ReadLogEntry(int idx, raft::LogEntry& out) {
    auto it = log_.find(idx);
    if (it == log_.end()) return false;
    out = it->second;
    return true;
  }

  void DeleteLogEntriesFrom(int start) {
    auto it = log_.lower_bound(start);
    while (it != log_.end()) {
      it = log_.erase(it);
    }
    last_log_index_ = start - 1;
  }

  int GetLastLogIndex() {
    return last_log_index_;
  }

  int GetLastLogTerm() {
    int idx = GetLastLogIndex();
    if (idx < 0) return 0;
    raft::LogEntry e;
    return ReadLogEntry(idx, e) ? e.term() : 0;
  }

  // ================= TIMER =================

  void ResetElectionTimer() {
    std::uniform_int_distribution<> dis(
        config_.election_timeout_min_ms,
        config_.election_timeout_max_ms);

    election_timeout_ =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(dis(gen_));

    election_cv_.notify_all();
  }

  // ================= ELECTION =================

  void ElectionLoop() {
    int consecutive_failures = 0;
    while (!should_stop_) {
      bool should_elect = false;
      {
        std::unique_lock<std::mutex> lock(state_mutex_);
        election_cv_.wait_until(lock, election_timeout_);
        if (!should_stop_ && state_ != LEADER &&
            std::chrono::steady_clock::now() >= election_timeout_) {
          should_elect = true;
          ResetElectionTimer();
        }
      }
      if (should_elect) {
        int voted = StartElection();
        if (voted < 0) {
          consecutive_failures++;
          int backoff_ms = std::min(500, 10 * consecutive_failures);
          std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        } else {
          consecutive_failures = 0;
        }
      }
    }
  }

  int StartElection() {
    int term_snapshot, last_log_index, last_log_term;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      state_        = CANDIDATE;
      current_term_++;
      voted_for_    = config_.replica_id;
      PersistTermAndVotedFor();
      term_snapshot  = current_term_;
      last_log_index = GetLastLogIndex();
      last_log_term  = GetLastLogTerm();
    }

    std::atomic<int> votes_granted{1};
    std::vector<std::thread> threads;

    for (size_t i = 0; i < config_.peer_addrs.size(); i++) {
      threads.emplace_back([this, i, &votes_granted, term_snapshot,
                            last_log_index, last_log_term] {
        SendRequestVote(i, votes_granted, term_snapshot,
                        last_log_index, last_log_term);
      });
    }
    for (auto& t : threads) t.join();

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ != CANDIDATE || current_term_ != term_snapshot) return -1;

    int total    = (int)config_.peer_addrs.size() + 1;
    int majority = total / 2 + 1;

    if (votes_granted.load() >= majority) {
      state_     = LEADER;
      leader_id_ = config_.replica_id;
      int next   = GetLastLogIndex() + 1;
      for (size_t i = 0; i < config_.peer_addrs.size(); i++) {
        next_index_[i]  = next;
        match_index_[i] = -1;
      }
      // Reset quorum timestamp — will be updated after first heartbeat round
      last_quorum_ms_.store(0, std::memory_order_relaxed);
      std::cout << "Replica " << config_.replica_id
                << " elected LEADER term=" << current_term_
                << " votes=" << votes_granted.load() << "\n";
      std::cout.flush();
      return votes_granted.load();
    }
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

    if (res.term() > term) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (res.term() > current_term_) {
        current_term_ = res.term();
        state_        = FOLLOWER;
        voted_for_    = -1;
        PersistTermAndVotedFor();
      }
    }
  }

  // ================= HEARTBEAT =================

  void HeartbeatLoop() {
    while (!should_stop_) {
      bool leader;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        leader = (state_ == LEADER);
      }

      if (leader) {
        for (size_t i = 0; i < config_.peer_addrs.size(); i++)
          std::thread([this, i] { SendAppendEntries(i); }).detach();
      }

      std::this_thread::sleep_for(
          std::chrono::milliseconds(config_.heartbeat_interval_ms));
    }
  }

  void SendAppendEntries(size_t peer_idx) {
    auto it = peer_stubs_.find(peer_idx);
    if (it == peer_stubs_.end()) return;

    int term, prev_log_index, prev_log_term, commit_idx;
    std::vector<raft::LogEntry> entries;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_ != LEADER) return;
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

    if (res.term() > current_term_) {
      current_term_ = res.term();
      state_        = FOLLOWER;
      voted_for_    = -1;
      PersistTermAndVotedFor();
      return;
    }

    if (res.success()) {
      next_index_[peer_idx]  = res.match_index() + 1;
      match_index_[peer_idx] = res.match_index();
      UpdateCommitIndex();

      int ack_count = 1;  // count self
      for (size_t j = 0; j < config_.peer_addrs.size(); j++) {
        if (match_index_[j] >= commit_index_) ack_count++;
      }
      int total    = (int)config_.peer_addrs.size() + 1;
      int majority = total / 2 + 1;
      if (ack_count >= majority) {
        last_quorum_ms_.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);
      }
    } else {
      if (next_index_[peer_idx] > 0)
        next_index_[peer_idx]--;
    }
  }

  // ================= COMMIT =================

  void UpdateCommitIndex() {
    int last     = GetLastLogIndex();
    int total    = (int)config_.peer_addrs.size() + 1;
    int majority = total / 2 + 1;

    for (int i = last; i > commit_index_; i--) {
      raft::LogEntry entry;
      if (!ReadLogEntry(i, entry)) continue;
      if (entry.term() != current_term_) continue;

      int count = 1;  // count self
      for (size_t j = 0; j < config_.peer_addrs.size(); j++)
        if (match_index_[j] >= i) count++;

      if (count >= majority) {
        commit_index_ = i;
        break;
      }
    }
  }
};