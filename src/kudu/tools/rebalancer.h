// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/optional/optional.hpp>

#include "kudu/client/shared_ptr.h"
#include "kudu/tools/ksck_results.h"
#include "kudu/tools/rebalance_algo.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"

namespace kudu {

namespace client {
class KuduClient;
}

namespace tools {

class Ksck;

// Sub-set of fields from KsckResult which are relevant to the rebalancing.
struct ClusterRawInfo {
  std::vector<KsckServerHealthSummary> tserver_summaries;
  std::vector<KsckTableSummary> table_summaries;
  std::vector<KsckTabletSummary> tablet_summaries;
};

// A class implementing logic for Kudu cluster rebalancing.
class Rebalancer {
 public:
  // Configuration parameters for the rebalancer aggregated into a struct.
  struct Config {
    Config(std::vector<std::string> master_addresses = {},
           std::vector<std::string> table_filters = {},
           size_t max_moves_per_server = 5,
           size_t max_staleness_interval_sec = 300,
           int64_t max_run_time_sec = 0,
           bool move_rf1_replicas = false,
           bool output_replica_distribution_details = false);

    // Kudu masters' RPC endpoints.
    std::vector<std::string> master_addresses;

    // Names of tables to balance. If empty, every table and the whole cluster
    // will be balanced.
    std::vector<std::string> table_filters;

    // Maximum number of move operations to run concurrently on one server.
    // An 'operation on a server' means a move operation where either source or
    // destination replica is located on the specified server.
    size_t max_moves_per_server;

    // Maximum duration of the 'staleness' interval, when the rebalancer cannot
    // make any progress in scheduling new moves and no prior scheduled moves
    // are left, even if re-synchronizing against the cluster's state again and
    // again. Such a staleness usually happens in case of a persistent problem
    // with the cluster or when some unexpected concurrent activity is present
    // (such as automatic recovery of failed replicas, etc.).
    size_t max_staleness_interval_sec;

    // Maximum run time, in seconds.
    int64_t max_run_time_sec;

    // Whether to move replicas of tablets with replication factor of one.
    bool move_rf1_replicas;

    // Whether Rebalancer::PrintStats() should output per-table and per-server
    // replica distribution details.
    bool output_replica_distribution_details;
  };

  // Represents a concrete move of a replica from one tablet server to another.
  // Formed logically from a TableReplicaMove by specifying a tablet for the move.
  struct ReplicaMove {
    std::string tablet_uuid;
    std::string ts_uuid_from;
    std::string ts_uuid_to;
    boost::optional<int64_t> config_opid_idx; // for CAS-enabled Raft changes
  };

  enum class RunStatus {
    UNKNOWN,
    CLUSTER_IS_BALANCED,
    TIMED_OUT,
  };

  // A helper type: key is tablet UUID which corresponds to value.tablet_uuid.
  typedef std::unordered_map<std::string, ReplicaMove> MovesInProgress;

  // Create Rebalancer object with the specified configuration.
  explicit Rebalancer(const Config& config);

  // Print the stats on the cluster balance information into the 'out' stream.
  Status PrintStats(std::ostream& out);

  // Run the rebalancing: start the process and return once the balancing
  // criteria are satisfied or if an error occurs. The number of attempted
  // moves is output into the 'moves_count' parameter (if the parameter is
  // not null). The 'result_status' output parameter cannot be null.
  Status Run(RunStatus* result_status, size_t* moves_count = nullptr);

 private:
  // Helper class to find and schedule next available rebalancing move operation
  // and track already scheduled ones.
  class Runner {
   public:
    virtual ~Runner() = default;

    // Initialize instance of Runner so it can run against Kudu cluster with
    // the 'master_addresses' RPC endpoints.
    virtual Status Init(std::vector<std::string> master_addresses) = 0;

    // Load information on prescribed replica movement operations. Also,
    // populate helper containers and other auxiliary run-time structures
    // used by ScheduleNextMove(). This method is called with every batch
    // of move operations output by the rebalancing algorithm once previously
    // loaded moves have been scheduled.
    virtual void LoadMoves(std::vector<ReplicaMove> replica_moves) = 0;

    // Schedule next replica move.
    virtual bool ScheduleNextMove(bool* has_errors, bool* timed_out) = 0;

    // Update statuses and auxiliary information on in-progress replica move
    // operations. The 'timed_out' parameter is set to 'true' if not all
    // in-progress operations were processed by the deadline specified by
    // the 'deadline_' member field. The method returns 'true' if it's necessary
    // to clear the state of the in-progress operations, i.e. 'forget'
    // those, starting from a clean state.
    virtual bool UpdateMovesInProgressStatus(bool* has_errors, bool* timed_out) = 0;

    virtual Status GetNextMoves(bool* has_moves) = 0;

    virtual uint32_t moves_count() const = 0;
  }; // class Runner

  // Common base for a few Runner implementations.
  class BaseRunner : public Runner {
   public:
    BaseRunner(Rebalancer* rebalancer,
               size_t max_moves_per_server,
               boost::optional<MonoTime> deadline);

    Status Init(std::vector<std::string> master_addresses) override;

    Status GetNextMoves(bool* has_moves) override;

    uint32_t moves_count() const override {
      return moves_count_;
    }

   protected:
    // Get next batch of replica moves from the rebalancing algorithm.
    // Essentially, it runs ksck against the cluster and feeds the data into the
    // rebalancing algorithm along with the information on currently pending
    // replica movement operations. The information returned by the high-level
    // rebalancing algorithm is translated into particular replica movement
    // instructions, which are used to populate the 'replica_moves' parameter
    // (the container is cleared first).
    virtual Status GetNextMovesImpl(std::vector<ReplicaMove>* moves) = 0;

    // Update the helper containers once a scheduled operation is complete
    // (i.e. succeeded or failed).
    void UpdateOnMoveCompleted(const std::string& ts_uuid);

    // A pointer to the Rebalancer object.
    Rebalancer* rebalancer_;

    // Maximum allowed number of move operations per server. For a move
    // operation, a source replica adds +1 at the source server and the target
    // replica adds +1 at the destination server.
    const size_t max_moves_per_server_;

    // Deadline for the activity performed by the Runner class in
    // ScheduleNextMoves() and UpadteMovesInProgressStatus() methods.
    const boost::optional<MonoTime> deadline_;

    // Client object to make queries to Kudu masters for various auxiliary info
    // while scheduling move operations and monitoring their status.
    client::sp::shared_ptr<client::KuduClient> client_;

    // Information on scheduled replica movement operations; keys are
    // tablet UUIDs, values are ReplicaMove structures.
    MovesInProgress scheduled_moves_;

    // Number of successfully completed replica moves operations.
    uint32_t moves_count_;

    // Kudu cluster RPC end-points.
    std::vector<std::string> master_addresses_;

    // Mapping 'tserver UUID' --> 'scheduled move operations count'.
    std::unordered_map<std::string, int32_t> op_count_per_ts_;

    // Mapping 'scheduled move operations count' --> 'tserver UUID'. That's
    // just reversed 'op_count_per_ts_'.
    std::multimap<int32_t, std::string> ts_per_op_count_;
  }; // class BaseRunner

  // Runner that leverages RebalancingAlgo interface for rebalancing.
  class AlgoBasedRunner : public BaseRunner {
   public:
    // The 'max_moves_per_server' specifies the maximum number of operations
    // per tablet server (both the source and the destination are counted in).
    // The 'deadline' specifies the deadline for the run, 'boost::none'
    // if no timeout is set.
    AlgoBasedRunner(Rebalancer* rebalancer,
                    size_t max_moves_per_server,
                    boost::optional<MonoTime> deadline);

    Status Init(std::vector<std::string> master_addresses) override;

    void LoadMoves(std::vector<ReplicaMove> replica_moves) override;

    bool ScheduleNextMove(bool* has_errors, bool* timed_out) override;

    bool UpdateMovesInProgressStatus(bool* has_errors, bool* timed_out) override;

    // Rebalancing algorithm that running uses to find replica moves.
    virtual RebalancingAlgo* algorithm() = 0;

   protected:
    Status GetNextMovesImpl(std::vector<ReplicaMove>* replica_moves) override;

    // Given the data in the helper containers, find the index describing
    // the next replica move and output it into the 'op_idx' parameter.
    bool FindNextMove(size_t* op_idx);

    // Update the helper containers once a move operation has been scheduled.
    void UpdateOnMoveScheduled(size_t idx,
                               const std::string& tablet_uuid,
                               const std::string& src_ts_uuid,
                               const std::string& dst_ts_uuid,
                               bool is_success);

    // Auxiliary method used by UpdateOnMoveScheduled() implementation.
    void UpdateOnMoveScheduledImpl(
        size_t idx,
        const std::string& ts_uuid,
        bool is_success,
        std::unordered_map<std::string, std::set<size_t>>* op_indices);

    // The moves to schedule.
    std::vector<ReplicaMove> replica_moves_;

    // Mapping 'tserver UUID' --> 'indices of move operations having the
    // tserver UUID (i.e. the key) as the source of the move operation'.
    std::unordered_map<std::string, std::set<size_t>> src_op_indices_;

    // Mapping 'tserver UUID' --> 'indices of move operations having the
    // tserver UUID (i.e. the key) as the destination of the move operation'.
    std::unordered_map<std::string, std::set<size_t>> dst_op_indices_;

    // Random device and generator for selecting among multiple choices, when
    // appropriate.
    std::random_device random_device_;
    std::mt19937 random_generator_;
  }; // class AlgoBasedRunner

  class TwoDimensionalGreedyRunner : public AlgoBasedRunner {
   public:
    // The 'max_moves_per_server' specifies the maximum number of operations
    // per tablet server (both the source and the destination are counted in).
    // The 'deadline' specifies the deadline for the run, 'boost::none'
    // if no timeout is set.
    TwoDimensionalGreedyRunner(Rebalancer* rebalancer,
                               size_t max_moves_per_server,
                               boost::optional<MonoTime> deadline);

    RebalancingAlgo* algorithm() override {
      return &algorithm_;
    }

   private:
    // An instance of the balancing algorithm.
    TwoDimensionalGreedyAlgo algorithm_;
  };

  friend class KsckResultsToClusterBalanceInfoTest;

  // Convert ksck results into information relevant to rebalancing the cluster.
  // Basically, 'raw' information is just a sub-set of relevant fields of the
  // KsckResults structure filtered to contain information only for the
  // specified location.
  static Status KsckResultsToClusterRawInfo(
      const KsckResults& ksck_info,
      ClusterRawInfo* raw_info);

  // Given high-level move-some-tablet-replica-for-a-table information from the
  // rebalancing algorithm, find appropriate tablet replicas to move between the
  // specified tablet servers. The set of result tablet UUIDs is output
  // into the 'tablet_ids' container (note: the container is first cleared).
  // The source and destination replicas are determined by the elements of the
  // 'tablet_ids' container and tablet server UUIDs TableReplicaMove::from and
  // TableReplica::to correspondingly. If no suitable tablet replicas are found,
  // 'tablet_ids' will be empty with the result status of Status::OK().
  static Status FindReplicas(const TableReplicaMove& move,
                             const ClusterRawInfo& raw_info,
                             std::vector<std::string>* tablet_ids);

  // Filter move operations in 'replica_moves': remove all operations that would
  // involve moving replicas of tablets which are in 'scheduled_moves'. The
  // 'replica_moves' cannot be null.
  static void FilterMoves(const MovesInProgress& scheduled_moves,
                          std::vector<ReplicaMove>* replica_moves);

  // Convert the 'raw' information about the cluster into information suitable
  // for the input of the high-level rebalancing algorithm.
  // The 'moves_in_progress' parameter contains information on the replica moves
  // which have been scheduled by a caller and still in progress: those are
  // considered as successfully completed and applied to the 'raw_info' when
  // building ClusterInfo for the specified 'raw_info' input. The idea
  // is to prevent the algorithm outputting the same moves again while some
  // of the moves recommended at prior steps are still in progress.
  // The result cluster balance information is output into the 'info' parameter.
  // The 'info' output parameter cannot be null.
  Status BuildClusterInfo(const ClusterRawInfo& raw_info,
                          const MovesInProgress& moves_in_progress,
                          ClusterInfo* info) const;

  // Run rebalancing using the specified runner.
  Status RunWith(Runner* runner, RunStatus* result_status);

  // Refresh the information on the cluster (involves running ksck).
  Status GetClusterRawInfo(ClusterRawInfo* raw_info);

  Status GetNextMoves(Runner* runner,
                      std::vector<ReplicaMove>* replica_moves);

  // Reset ksck-related fields and run ksck against the cluster.
  Status RefreshKsckResults();

  // Configuration for the rebalancer.
  const Config config_;

  // Random device and generator for selecting among multiple choices, when
  // appropriate.
  std::random_device random_device_;
  std::mt19937 random_generator_;

  // An instance of the balancing algorithm.
  TwoDimensionalGreedyAlgo algo_;

  // Auxiliary Ksck object to get information on the cluster.
  std::shared_ptr<Ksck> ksck_;
};

} // namespace tools
} // namespace kudu
