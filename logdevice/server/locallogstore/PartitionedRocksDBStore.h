/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/IntrusiveList.h>
#include <folly/ThreadLocal.h>
#include <folly/SharedMutex.h>

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/merge_operator.h>

#include "logdevice/common/AtomicsMap.h"
#include "logdevice/common/MetaDataLog.h"
#include "logdevice/common/Metadata.h"
#include "logdevice/common/RandomAccessQueue.h"
#include "logdevice/common/SingleEvent.h"
#include "logdevice/common/Timestamp.h"
#include "logdevice/common/UpdateableSharedPtr.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/util.h"
#include "logdevice/server/FixedKeysMap.h"

#include "logdevice/common/configuration/InternalLogs.h"

#include "logdevice/server/locallogstore/NodeDirtyData.h"
#include "logdevice/server/locallogstore/RocksDBLogStoreBase.h"
#include "logdevice/server/locallogstore/RocksDBWriter.h"

#include "logdevice/server/storage_tasks/StorageTask.h"

namespace facebook { namespace logdevice {

/**
 * @file  PartitionedRocksDBStore (aka LogsDB) is a LocalLogStore implementation
 *        based on RocksDB, further partitioned by time.  This allows us to run
 *        with compactions disabled, therefore keeping write amplification
 *        minimal. Individual partitions are small in order to keep reads
 *        efficient (read performance depends on the number of L0 files).
 *
 *        Partitioning by time makes trimming efficient -- it amounts to just
 *        dropping a partition. To reclaim space faster when logs have different
 *        backlog durations, compactions of individual partitions are also
 *        supported.
 *
 *        Internally each partition is a separate column family. There's also
 *        a special metadata column family that, in addition to log metadata
 *        such as trim points, stores a directory that's used to determine
 *        which partition some (logid, lsn) pair maps to.
 *
 *        Partitions have non-overlapping ranges. Therefore, read iterators
 *        always visit one partition at a time.
 */

class StatsHolder;
class ServerProcessor;

class PartitionedRocksDBStore : public RocksDBLogStoreBase {
 public:
  static const char* METADATA_CF_NAME;
  static const char* UNPARTITIONED_CF_NAME;
  static const char* SNAPSHOTS_CF_NAME;
  static constexpr partition_id_t INITIAL_PARTITION_ID = 1e6;

  enum class BackgroundThreadType {
    HI_PRI = 0,
    LO_PRI,
    COUNT,
  };

  // An update-to-max merge operator used by the metadata column family.
  // Ensures that values of LastRecordMetadata entries updated using Merge()
  // result in the largest value stored in the database.
  class MetadataMergeOperator : public rocksdb::AssociativeMergeOperator {
   public:
    bool Merge(const rocksdb::Slice& key,
               const rocksdb::Slice* existing_value,
               const rocksdb::Slice& value,
               std::string* new_value,
               rocksdb::Logger* logger) const override;
    const char* Name() const override {
      return "LogsDBMetadataMerge";
    }
  };

  // Dirty state tracking.
  struct DirtyState {
    DirtyState() = default;
    DirtyState(const DirtyState& src)
        : dirtied_by_nodes(src.dirtied_by_nodes),
          max_flush_token(src.max_flush_token.load(std::memory_order_relaxed)),
          min_flush_token(src.min_flush_token.load(std::memory_order_relaxed)),
          append_dirtied_wal_token(
              src.append_dirtied_wal_token.load(std::memory_order_relaxed)),
          latest_dirty_time(src.latest_dirty_time.timePoint()),
          oldest_dirty_time(src.oldest_dirty_time.timePoint()),
          under_replicated(false) {}

    const DirtyState& operator=(const DirtyState& rhs) {
      dirtied_by_nodes = rhs.dirtied_by_nodes;
      max_flush_token = rhs.max_flush_token.load(std::memory_order_relaxed);
      min_flush_token = rhs.min_flush_token.load(std::memory_order_relaxed);
      append_dirtied_wal_token =
          rhs.append_dirtied_wal_token.load(std::memory_order_relaxed);
      latest_dirty_time = rhs.latest_dirty_time.timePoint();
      oldest_dirty_time = rhs.oldest_dirty_time.timePoint();
      under_replicated = false;
      return *this;
    }

    void noteDirtied(FlushToken new_max_token, SteadyTimestamp ts) {
      atomic_fetch_max(max_flush_token, new_max_token);
      atomic_fetch_min(min_flush_token, new_max_token);
      latest_dirty_time.storeMax(ts);
      oldest_dirty_time.storeMin(ts);
    }

    // @return true if processing this FlushToken has caused some or
    //         all data tracked by this dirty state to be retired.
    bool noteMemtableFlushed(FlushToken flushed_up_through,
                             SteadyTimestamp oldest_unflushed_timestamp,
                             SteadyTimestamp now) {
      bool data_retired = false;
      if (flushed_up_through >= min_flush_token.load()) {
        data_retired = true;
        if (max_flush_token.load() <= flushed_up_through) {
          // Partition is now clean.
          min_flush_token.store(FlushToken_MAX);
          latest_dirty_time = SteadyTimestamp::min();
          oldest_dirty_time = SteadyTimestamp::max();
          auto prev_token = atomic_conditional_store(max_flush_token,
                                                     FlushToken_INVALID,
                                                     flushed_up_through,
                                                     std::less_equal<>());
          if (prev_token > flushed_up_through) {
            // If we lose the race, ensure all dirty state reflects that the
            // partition is still dirty.
            //
            // The correct min_flush_token isn't known because the thread(s)
            // we lost the race to could have seen min_flush_token before we
            // set it to FlushToken_MAX, and so never applied their value. We
            // do know that it cannot be lower than than flushed_up_through + 1,
            // so we use that value here. Overestimating is fine since the only
            // side effect is to process some unnecessary window slides for
            // this partition.
            //
            // Only this thread takes action based on dirty state, so the short
            // period here where the data is inconsistent is benign.
            noteDirtied(flushed_up_through + 1, now);
          }
        } else {
          // Both updates represent the worst case lower bound of dirty
          // data still left in this partition.
          oldest_dirty_time.store(oldest_unflushed_timestamp);
          min_flush_token.store(flushed_up_through + 1);
        }
      }
      return data_retired;
    }

    PartitionDirtyMetadata metadata() const {
      return PartitionDirtyMetadata(dirtied_by_nodes, under_replicated);
    }

    // Key used for the special DirtiedByMap entry signifying that a
    // partition is being held dirty regarless of write or flush activity.
    static constexpr std::pair<node_index_t, DataClass> SENTINEL_KEY =
        std::make_pair(NODE_INDEX_INVALID, DataClass::APPEND);

    // Insertion/Removal requires mutex_ to be held in
    // write mode. The use of atomic operations within NodeDirtyState
    // allows mutation of an existing element while only holding
    // mutex_ in Shared mode.
    DirtiedByMap dirtied_by_nodes;

    // The flush token for the most recent, unflushed memtable holding
    // data associated with this DirtyState. A value of FlushToken_INVALID
    // indicates no oustanding data (i.e. we are clean).
    std::atomic<FlushToken> max_flush_token{FlushToken_INVALID};

    // The flush token for the oldest, unflushed memtable holding
    // data associated with this DirtyState. A value of FlushToken_INVALID
    // indicates no oustanding data (i.e. we are clean).
    //
    // Cleaning of 'dirtied_by_nodes' cannot make progress (and is thus
    // deferred) until memtables are flushed at least up to this point.
    std::atomic<FlushToken> min_flush_token{FlushToken_MAX};

    // Set to the WAL token that must be waited for when dirtying the
    // partition for appends. FlushToken_MAX indicates that the partition
    // is clean for appends (there are no appending nodes in
    // dirtied_by_nodes).
    std::atomic<FlushToken> append_dirtied_wal_token{FlushToken_MAX};

    // Time range covering when outstanding writes (those not yet committed
    // to stable storage) were written to this partition.  This time range
    // is based on "steady time" of writing, not record timestamp, so writing
    // an old record due to rebuilding and writing a new record due to an
    // append have the same impact on these values.
    //
    // The time range is used to implement time based MemTable flush triggers.
    //
    // The range is expanded during the normal write path.
    //
    // The range is closed periodically from the hi-pri background thread as
    // MemTable flush events are processed.
    //
    // A clean partition is represented by latest_dirty_time set to min(),
    // and oldest_dirty_time set to max(). This simplified the logic required
    // to first mark a partition dirty.
    AtomicSteadyTimestamp latest_dirty_time{SteadyTimestamp::min()};
    AtomicSteadyTimestamp oldest_dirty_time{SteadyTimestamp::max()};

    // Report that this partition has lost records that have not yet been
    // restored by rebuilding.
    bool under_replicated{false};
  };

  struct Partition;
  using PartitionPtr = std::shared_ptr<Partition>;
  using PartitionList = RandomAccessQueue<PartitionPtr>::VersionPtr;

  struct Partition {
    class TimestampUpdateTask;

    // id of the partition.
    const partition_id_t id_;

    // Underlying column family.
    const std::unique_ptr<rocksdb::ColumnFamilyHandle> cf_;

    // Locked for writing when cf_ is being dropped.
    // Locked for reading when data is being written to cf_.
    // Also locked when reading/updating the non-atomic part of DirtyState.
    // Note that it's not necessarily locked for directory updates.
    folly::SharedMutex mutex_;

    // This partition covers time range between its starting_timestamp and
    // starting_timestamp of the next partition. PartitionedRocksDBStore
    // will almost always put record in partition that covers its timestamp.
    // starting_timestamp is usually the time when partition was created.
    // Never changes after Partition becomes visible, consider it const.
    RecordTimestamp starting_timestamp;

    // Minimum and maximum record timestamps.
    // These are approximate: when writing a record with a timestamp outside
    // this range, we over-extend the range by partition_timestamp_granularity_,
    // so that we don't have to update the range too often.
    AtomicRecordTimestamp min_timestamp{RecordTimestamp::max()};
    AtomicRecordTimestamp max_timestamp{RecordTimestamp::min()};

    // Minimum and maximum record timestamps as safely recoreced to
    // stable storage.
    AtomicRecordTimestamp min_durable_timestamp{RecordTimestamp::max()};
    AtomicRecordTimestamp max_durable_timestamp{RecordTimestamp::min()};

    AtomicRecordTimestamp last_compaction_time{RecordTimestamp::min()};
    // See PartitionCompactedRetentionMetadata.
    std::atomic<std::chrono::seconds> compacted_retention{
        std::chrono::seconds::min()};

    DirtyState dirty_state_;

    // Whether cf_ is dropped. A dropped column family is readable but not
    // writable. Partition drops and updates to this field are done with
    // exclusively locked mutex_.
    bool is_dropped{false};

    Partition(partition_id_t id,
              rocksdb::ColumnFamilyHandle* cf,
              RecordTimestamp starting_timestamp,
              const DirtyState* pre_dirty_state = nullptr)
        : id_(id), cf_(cf), starting_timestamp(starting_timestamp) {
      if (pre_dirty_state != nullptr) {
        dirty_state_ = *pre_dirty_state;
      }
    }

    // Compute the effective dirty time interval given the currently
    // durable values of min/max timestamp.
    //
    // Partition timestamp updates are always written to the WAL, but
    // write operations only block if their dependent timestamp update
    // is more than partition_timestamp_granularity_ beyond the currently
    // durable min/max value for the partition.  Additionally, for the
    // latest partition, we consider a dirty partition to be dirty for the
    // entire partition_duration_.  This allows timestamp updates to be
    // written infrequently and only when needed, while still providing
    // ample time for timestamp updates to be flushed to persistent storage
    // before causing appends to block.
    //
    // Due to the special handling of the latest partition, blocking is
    // avoided even in cases where the node hasn't taken writes for some
    // time (node startup, new node to the cluster, change to non-zero
    // storage weight, returning from repair or drain, etc).  The expansion
    // on non-latest partitions aims to prevent blocking in most cases during
    // rebuilding where writes can slightly expand the ranges of existing
    // partitions.
    RecordTimeInterval dirtyTimeInterval(const RocksDBSettings& settings,
                                         partition_id_t latest_id) const;
  };

  struct DirectoryEntry {
    partition_id_t id = PARTITION_INVALID;
    lsn_t first_lsn;
    lsn_t max_lsn;
    PartitionDirectoryValue::flags_t flags;
    size_t approximate_size_bytes = 0;

    int fromIterator(const RocksDBIterator* it, logid_t log_id);

    // Put()s this entry in rocksdb_batch.
    void doPut(logid_t log_id,
               Durability durability,
               rocksdb::ColumnFamilyHandle* cf,
               rocksdb::WriteBatch& rocksdb_batch);

    // Delete()s this entry in rocksdb_batch.
    void doDelete(logid_t log_id,
                  rocksdb::ColumnFamilyHandle* cf,
                  rocksdb::WriteBatch& rocksdb_batch);

    std::string toString() const;
    std::string flagsToString() const;
  };

  class MemtableFlushCallback : public FlushCallback {
   public:
    void operator()(LocalLogStore* store, FlushToken token) const override;
  };

  struct DirtyOp {
    DirtyOp(PartitionPtr p,
            RecordTimestamp ts,
            const WriteOp* op,
            node_index_t idx,
            DataClass dc)
        : partition(p),
          timestamp(ts),
          write_op(op),
          node_idx(idx),
          data_class(dc) {}

    bool operator<(const DirtyOp& rhs) const {
      // Important to sort by partition ID first, so that Partition::mutex_'es
      // are locked in order of increasing partition ID.
      return std::forward_as_tuple(partition->id_, node_idx, data_class) <
          std::forward_as_tuple(
                 rhs.partition->id_, rhs.node_idx, rhs.data_class);
    }

    // Merging is allowed for the operations that apply to the same
    // partition, coordinating node, and class of data.
    bool canMergeWith(const DirtyOp& rhs) const {
      return std::forward_as_tuple(partition->id_, node_idx, data_class) ==
          std::forward_as_tuple(
                 rhs.partition->id_, rhs.node_idx, rhs.data_class);
    }

    PartitionPtr partition;
    RecordTimestamp timestamp;
    const WriteOp* write_op;
    node_index_t node_idx;
    DataClass data_class;
    bool newly_dirtied = false;
  };

  // @param config is only guaranteed to be alive during the call,
  //        don't hold on to it.
  explicit PartitionedRocksDBStore(uint32_t shard_idx,
                                   const std::string& path,
                                   RocksDBLogStoreConfig rocksdb_config,
                                   const Configuration* config = nullptr,
                                   StatsHolder* stats = nullptr)
      : PartitionedRocksDBStore(shard_idx,
                                path,
                                std::move(rocksdb_config),
                                config,
                                stats,
                                DeferInit::NO) {}
  ~PartitionedRocksDBStore() override;

  void setProcessor(Processor* processor) override;

  std::unique_ptr<LocalLogStore::ReadIterator>
  read(logid_t log_id, const LocalLogStore::ReadOptions&) const override;

  std::unique_ptr<LocalLogStore::AllLogsIterator>
  readAllLogs(const LocalLogStore::ReadOptions&) const override;

  // See LocalLogStore.h for details.

  int writeMulti(const std::vector<const WriteOp*>& writes,
                 const WriteOptions& write_options) override;

  void onBytesWritten(size_t written) override {
    bytes_written_since_flush_.fetch_add(written);
  }

  int writeStoreMetadata(const StoreMetadata& metadata,
                         const WriteOptions& write_options) override;

  void writePartitionDirtyState(PartitionPtr, rocksdb::WriteBatch&);

  // See LocalLogStore
  int readAllLogSnapshotBlobs(LogSnapshotBlobType type,
                              LogSnapshotBlobCallback callback) override;

  int writeLogSnapshotBlobs(
      LogSnapshotBlobType snapshots_type,
      const std::vector<std::pair<logid_t, Slice>>& snapshots) override;

  int deleteAllLogSnapshotBlobs() override;

  int findTime(logid_t log_id,
               std::chrono::milliseconds timestamp,
               lsn_t* lo,
               lsn_t* hi,
               bool approximate = false,
               bool allow_blocking_io = true,
               std::chrono::steady_clock::time_point deadline =
                   std::chrono::steady_clock::time_point::max()) const override;

  // Approximate size of the data in the given log between the two timestamps.
  int dataSize(logid_t log_id,
               RecordTimestamp lo,
               RecordTimestamp hi,
               size_t* out);
  int dataSize(logid_t log_id,
               std::chrono::milliseconds lo,
               std::chrono::milliseconds hi,
               size_t* out);

  /**
   * Checks whether the given log is empty, meaning it has either no records or
   * only pseudorecords, such as bridge records and hole plugs.
   *
   * @param log_id              Log id to check.
   * @return                    true if any directory entries for the log have
   *                            only pseudorecords in them.
   */
  bool isLogEmpty(logid_t log_id);

  void normalizeTimeRanges(RecordTimeIntervals&) const override;

  /**
   * Implement the FindKey API. @see logdevice/common/FindKeyRequest.h.
   *
   * @param log_id              Log id on which to search.
   * @param key                 Target key.
   * @param lo                  The lsn of the newest record such that its key
   *                            is < `key` will be written here.
   * @param hi                  The lsn of the oldest record such that its key
   *                            is >= `key` will be written here.
   * @param approximate         with this flag turned on findKey() will perform
   *                            faster but may underestimate lo and overestimate
   *                            hi.
   * @param allow_blocking_io   Option that specifies if blocking I/O is
   *                            allowed. If false, restricted to non-blocking
   *                            (cache).
   */
  int findKey(logid_t log_id,
              std::string key,
              lsn_t* lo,
              lsn_t* hi,
              bool approximate = false,
              bool allow_blocking_io = true) const override;

  // Starts a new partition.
  // Used in tests, and is exposed through an admin command.
  // @return the new partition or nullptr in case of error.
  PartitionPtr createPartition();

  // Creates the given number of partitions. Unlike createPartition(), the
  // new partitions will be added at the _beginning_ of the list of partitions,
  // i.e. they will cover old time ranges. Normally, instead of using this
  // method, partitions are automatically prepended as needed to accommodate
  // arriving writes.
  // Used in tests, and is exposed through an admin command.
  //
  // @return  the created partitions, in order of increasing ID. May be shorter
  //          that @param count if there aren't enough partition IDs to create
  //          all `count` partitions. Empty if there was an error or if there
  //          already exists a partition with ID 1. Sets err to:
  //           - LOCAL_LOG_STORE_WRITE  if rocksdb reported an error,
  //           - EXISTS  if a partition with ID 1 exists.
  std::vector<PartitionPtr> prependPartitions(size_t count);

  // Retires outstanding MemTables for the specified partition and
  // all unpartitioned column families.
  //
  // Intended for integration tests and operator intervention, this method
  // is only used to implement the "logsdb flush" admin command.
  //
  // @note  Unpartitioned column families are flushed because we assume that
  //        writes to partitions also rely on metadata writes to the
  //        unpartitioned column families. The goal of this method is to
  //        retire all outstanding writes and their dependencies that existed
  //        at the time the method is called.
  //
  // @param partition    The partition to flush.
  //
  // @return true if the partition exists
  //
  // @note  This API blocks until any required MemTable flushes complete.
  //
  bool flushPartitionAndDependencies(PartitionPtr partition);

  // Returns all partitions in chronological order. The returned value must be
  // destroyed before PartitionedRocksDBStore is destroyed.
  PartitionList getPartitionList() const;

  // Wraps partitions_.get(). Returns false if the partition was dropped.
  bool getPartition(partition_id_t id, PartitionPtr* out_partition) const;

  // Wraps partitions_.get().
  //
  // @param offset  a relative offset into the partition id space:
  //                      0 == current partition
  //                     -n == partition n previous to current
  //                     +n == partition n-1 from first partition
  //                           ('1' == first partition).
  PartitionPtr getRelativePartition(ssize_t offset) const;

  PartitionPtr getLatestPartition() const;

  // Tries to find the partition corresponding to the given LSN and return the
  // starting timestamp of the partition. If the information is not available in
  // cache, returns zero.
  RecordTimestamp
  getPartitionTimestampForLSNIfReadilyAvailable(logid_t log_id,
                                                lsn_t lsn) const;

  // Removes partitions with ids up to oldest_to_keep (exclusive).
  // Updates log trim points so that all dropped records are logically trimmed.
  // Dropping the latest partition is not allowed.
  //
  // @return 0 on success, -1 on error. Sets err to:
  //   E::INVALID_PARAM if oldest_to_keep > latest partition id,
  //   E::AGAIN if we failed because the partition is being written to too much,
  //   E::LOCAL_LOG_STORE_READ if we couldn't read from log store,
  //   or any of the errors of dropPartitions().
  int dropPartitionsUpTo(partition_id_t oldest_to_keep);

  // Apply MemTable flush information to a DirtyState.
  // @return true if any data was retired for this partition.
  bool cleanDirtyState(DirtyState& ds, FlushToken flushed_up_through);

  // Update FlushToken and dirty time data for partitions.
  void updateDirtyState(FlushToken flushed_up_through);

  // Update FlushToken and dirty time data for a single partition
  void updatePartitionDirtyState(PartitionPtr partition,
                                 FlushToken flushed_up_through);

  // Returns true if a partition is configured to stay dirty regardless of
  // the presence of outstanding, dirty, MemTables.
  bool isHeldDirty(PartitionPtr) const;

  // Mark a partition dirty and prevent it from being cleaned in response
  // to FlushToken expiration.
  int holdDirty(PartitionPtr partition);

  // Release dirty hold on a partition - allow a partition previously
  // modified by holdDirty() to be cleaned by updatePartitionDirtyState()
  void releaseDirtyHold(PartitionPtr partition);

  // Performs a RocksDB compaction of single partition. Compaction removes
  // trimmed records from it, but only the records that were already behind
  // trim points when compaction got to them
  // (see RocksDBCompactionFilter for details).
  // To remove most records in a single compaction this method approximately
  // updates trim points for all logs beforehand.
  void performCompaction(partition_id_t partition);

  // Perform manual compaction on the metadata column family, performing
  // trimming and updating trim points for per-epoch log metadata.
  void performMetadataCompaction();

  // Size in bytes of column family data.
  uint64_t getApproximatePartitionSize(rocksdb::ColumnFamilyHandle* cf);

  // Number of level 0 files in column family. -1 if something
  // went wrong.
  int getNumL0Files(rocksdb::ColumnFamilyHandle* cf);

  // Approximate total size of records that are older than their backlog
  // duration but are still stored on disk. This is approximately the amount of
  // disk space that a compaction would reclaim. Only takes retention-based
  // trimming into account.
  uint64_t getApproximateObsoleteBytes(partition_id_t partition_id);

  // Returns rocksdb handle of metadata column family.
  rocksdb::ColumnFamilyHandle* getMetadataCFHandle() const override {
    return metadata_cf_.get();
  }

  // Returns rocksdb handle of unpartitioned column family.
  rocksdb::ColumnFamilyHandle* getUnpartitionedCFHandle() const {
    return unpartitioned_cf_.get();
  }

  int isEmpty() const override;

  int getHighestInsertedLSN(logid_t log_id, lsn_t* highestLSN) override {
    ld_check(highestLSN);
    if (!isLogPartitioned(log_id)) {
      err = E::NOTSUPPORTEDLOG;
      return -1;
    }
    auto it = logs_.find(log_id.val_);
    if (it != logs_.cend()) {
      *highestLSN = it->second.get()->latest_partition.max_lsn_in_latest.load();
    } else {
      *highestLSN = LSN_INVALID;
    }
    return 0;
  }

  int getApproximateTimestamp(
      logid_t log_id,
      lsn_t lsn,
      bool allow_blocking_io,
      std::chrono::milliseconds* timestamp_out) override;

  // Calls `cb` for every log that has at least one record stored. Note that
  // this method doesn't check trim points, so it's possible that some of the
  // returned logs are already fully behind trim points but compactions/drops
  // haven't removed the records yet.
  //
  // highest_timestamp_approx is calculated as the starting timestamp of the
  // next partition after highest_partition.
  //
  // highest_lsn doesn't necessarily correspond to a record in
  // highest_partition: the two are loaded from two separate atomic variables,
  // so there's a possibility of a "torn" read if another thread writes a record
  // during the listLogs() call.
  //
  // @param only_log_ids  if true, only the first argument of cb will have the
  //                      correct value, others will be zeros. Slightly cheaper.
  void
  listLogs(std::function<void(logid_t,
                              lsn_t highest_lsn,
                              uint64_t highest_partition,
                              RecordTimestamp highest_timestamp_approx)> cb,
           bool only_log_ids = false);

  bool supportsNonBlockingFindTime() const override {
    return true;
  }

  bool supportsNonBlockingFindKey() const override {
    return true;
  }

  // If true, records for this log are stored in partitions.
  // If false, records are stored in 'unpartitioned' column family.
  // Currently only metadata logs and internal logs such as the event log are
  // unpartitioned. If needed, we can put all logs with infinite backlog
  // duration here, or make it configurable.
  // Note that metadata logs are written to "unpartitioned" column family
  // rather than "metadata" column family. This is because it has more
  // appropriate RocksDB settings.
  bool isLogPartitioned(logid_t log_id) const {
    return !MetaDataLog::isMetaDataLog(log_id) &&
        !configuration::InternalLogs::isInternal(log_id);
  }

  // Tells rocksdb to flush all memtables of given partition and optionally
  // wait for the flush to finish.
  bool flushMemtables(PartitionPtr partition, bool wait = true);

  // Flush MemTables associated with unpartitioned column families.
  int flushUnpartitionedMemtables(bool wait = true);

  // Flush all memtables associated with this store.
  int flushAllMemtables(bool wait = true) override;

  // Updates log trim points according to time-based retention policy.
  // Does this at partition granularity: can only move trim point to beginning
  // of some partition.
  // Doesn't trim unpartitioned logs (compaction filter does that).
  //
  // @return 0 on success. On error returns -1 and sets err to:
  //  - LOCAL_LOG_STORE_READ if we couldn't read from log store,
  //  - LOCAL_LOG_STORE_WRITE if we couldn't write to log store,
  //  - NOBUFS if maximum number of logs was reached,
  //  - AGAIN if the store is not fully initialised (no processor),
  int trimLogsBasedOnTime();

  // Used for admin command 'logsdb print_directory'. Puts all directory
  // entries for the given partitions and logs in 'out'. empty 'partitions' or
  // 'logs' is interpreted as 'all'. partitions and logs must be sorted.
  void
  getLogsDBDirectories(std::vector<partition_id_t> partitions,
                       std::vector<logid_t> logs,
                       std::vector<std::pair<logid_t, DirectoryEntry>>& out);

  // Schedules a manual compaction. If hi_pri is true, it will be done before
  // other compactions. Otherwise, lo-pri compactions will only be scheduled
  // when there are no other compactions taking place. If there already is a
  // manual compaction pending for the specified partition_id, removes it from
  // the list before adding the new one.
  void scheduleManualCompaction(partition_id_t partition_id, bool hi_pri);

  // Cancels a manual compaction. If partition_id == 0, cancels all compactions
  // on the shard.
  void cancelManualCompaction(partition_id_t partition_id);

  // Flag a partition as under-replicated expanding, if necessary, the range
  // of under-replicated partitions.
  void setUnderReplicated(PartitionPtr p) {
    p->dirty_state_.under_replicated = true;
    atomic_fetch_min(min_under_replicated_partition_, p->id_);
    atomic_fetch_max(max_under_replicated_partition_, p->id_);
  }

  // Returns a list of pending manual compactions. Compactions currently running
  // on the background thread will not be listed
  std::list<std::pair<partition_id_t, bool>> getManualCompactionList();

  // Exposed for PartialCompactionEvaluator tests:
  struct PartitionToCompact {
    enum class Reason {
      INVALID = 0,
      PARTIAL, // this is first, for lowest priority when deduplicating
      RETENTION,
      PROACTIVE,
      MANUAL,
      MAX,
    };

    static EnumMap<Reason, std::string>& reasonNames();

    PartitionPtr partition;
    Reason reason;
    // If reason == RETENTION, backlog duration that caused us to decide
    // to compact partition.
    std::chrono::seconds retention{std::chrono::seconds::min()};

    // If reason == Reason::Partial, these are the sst files that should be
    // compacted
    std::vector<std::string> partial_compaction_filenames;

    PartitionToCompact(PartitionPtr p, Reason r)
        : partition(std::move(p)), reason(r) {
      ld_check(reason != Reason::RETENTION);
    }
    PartitionToCompact(PartitionPtr p, std::chrono::seconds s)
        : partition(std::move(p)), reason(Reason::RETENTION), retention(s) {}

    static void removeDuplicates(std::vector<PartitionToCompact>* ps) {
      ld_check(ps);
      // save original order
      for (size_t i = 0; i < ps->size(); ++i) {
        (*ps)[i].sort_order = i;
      }
      // If partition is equal, pick maximum reason, then maximum retention.
      std::sort(ps->begin(),
                ps->end(),
                [](const PartitionToCompact& a, const PartitionToCompact& b) {
                  if (a.partition->id_ != b.partition->id_) {
                    return a.partition->id_ < b.partition->id_;
                  }
                  if (a.reason != b.reason) {
                    return a.reason > b.reason;
                  }
                  return a.retention > b.retention;
                });
      // NOTE: std::unique() does guarantee that from each group of equal
      //       elements it will pick the first one, in our case the one with
      //       maximum reason (or retention if reasons are identical).
      ps->erase(std::unique(ps->begin(),
                            ps->end(),
                            [](const PartitionToCompact& a,
                               const PartitionToCompact& b) {
                              if (a.reason == Reason::PARTIAL &&
                                  b.reason == Reason::PARTIAL) {
                                // If both compactions are partial, leave both
                                // (overlapping compactions are already de-duped
                                // in PartialCompactionEvaluator).
                                return false;
                              }
                              return a.partition == b.partition;
                            }),
                ps->end());

      // Restore original order
      std::sort(ps->begin(),
                ps->end(),
                [](const PartitionToCompact& a, const PartitionToCompact& b) {
                  return a.sort_order < b.sort_order;
                });
    }

   private:
    size_t sort_order;
  };

  class PartialCompactionEvaluator {
   public:
    class Deps {
     public:
      // Method to get column family metadata that contains level metadata, and
      // metadata for each SST file on every level
      virtual void
      getColumnFamilyMetaData(rocksdb::ColumnFamilyHandle* column_family,
                              rocksdb::ColumnFamilyMetaData* cf_meta) = 0;
      virtual ~Deps() {}
    };

    // A dependency class that forwards the request to a rocksdb::DB instance.
    class DBDeps : public Deps {
     public:
      explicit DBDeps(rocksdb::DB* db) : db_(db) {}

      void
      getColumnFamilyMetaData(rocksdb::ColumnFamilyHandle* column_family,
                              rocksdb::ColumnFamilyMetaData* cf_meta) override {
        return db_->GetColumnFamilyMetaData(column_family, cf_meta);
      }

     private:
      rocksdb::DB* db_;
    };

    PartialCompactionEvaluator(
        std::vector<PartitionedRocksDBStore::PartitionPtr> partitions,
        size_t min_files,
        size_t max_files,
        size_t max_avg_file_size,
        size_t max_file_size,
        double max_largest_file_share,
        std::unique_ptr<Deps> deps)
        : partitions_(std::move(partitions)),
          min_files_(min_files),
          max_files_(max_files),
          max_avg_file_size_(max_avg_file_size),
          max_file_size_(max_file_size),
          max_largest_file_share_(max_largest_file_share),
          deps_(std::move(deps)) {
      if (min_files_ < 2) {
        ld_error("Invalid arg passed to PartialCompactionEvaluator: "
                 "min_files == %lu",
                 min_files_);
        min_files_ = 2;
      }
    }

    // Evaluates all partitions for partial compactions and adds partial
    // compactions to the output vector. Returns true if there are more partial
    // compactions to be done than max_results
    bool evaluateAll(std::vector<PartitionToCompact>* out_to_compact,
                     size_t max_results);

   private:
    // Evaluates ranges of SST files that start at start_idx for partial
    // compaction
    void evaluate(std::shared_ptr<rocksdb::ColumnFamilyMetaData>& metadata,
                  size_t partition,
                  size_t start_idx,
                  size_t min_files,
                  size_t max_files);

    // Calculates how valuable it is to do a partial compaction of files in all
    // ranges from [sst_files, sst_files + min_num) to
    // [sst_files, sst_files + max_num). Returns the highest found value of a
    // compaction and sets res_num to the number of files corresponding to that
    double compactionValue(const rocksdb::SstFileMetaData* sst_files,
                           size_t min_num,
                           size_t max_num,
                           size_t* res_num);

    // Candidates for compaction
    struct CompactionCandidate {
      double value;
      std::shared_ptr<rocksdb::ColumnFamilyMetaData> metadata;
      size_t partition_offset;
      size_t start_idx;
      size_t num_files;

      size_t end_idx() const {
        return start_idx + num_files;
      }
    };

    std::vector<CompactionCandidate> candidates_;

    // partitions to request metadata for
    std::vector<PartitionPtr> partitions_;

    // Minimum number of files in one compaction
    size_t min_files_;

    // Maximum number of files in one compaction
    size_t max_files_;

    // The maximum size of one file at which compacting it makes sense
    // (however, we might choose to compact files higher than this size due to
    // the requirement that files being compacted should be sequential).
    size_t max_avg_file_size_;

    // The maximum size of one file to compact.
    size_t max_file_size_;

    // The largest file in a compaction can't be a bigger proportion of the
    // total file size of files in that compaction than this value.
    double max_largest_file_share_;

    std::unique_ptr<Deps> deps_;
  };

  // Drop partitions up to `partition` on the next iteration of background
  // thread.
  void setSpaceBasedTrimLimit(partition_id_t partition) {
    atomic_fetch_max(space_based_trim_limit, partition);
  }

  // Returns true if a previous LogDevice failure has left one or more
  // partitions potentially under replicated.
  bool isUnderReplicated() const {
    return max_under_replicated_partition_.load(std::memory_order_relaxed) !=
        PARTITION_INVALID;
  }

  bool shouldStallLowPriWrites() override;

  void markImmutable() override {
    joinBackgroundThreads();
  }

  // Marks partitions where data could fall into the given time range as
  // underreplicated
  int markTimeRangeUnderreplicated(DataClass dc, RecordTimeInterval interval);

  void onSettingsUpdated(
      const std::shared_ptr<const RocksDBSettings> settings) override;

 private:
  class Iterator;
  class PartitionedAllLogsIterator;
  class PartitionDirectoryIterator;
  class FindTime;
  class FindKey;

  struct LogState {
    // This struct is poor man's atomic<tuple<partition_id_t, lsn_t, lsn_t>>.
    // It maintains a partition ID and corresponding min and max LSNs, making
    // sure that there are no torn reads.
    // It's fast to read and not necessarily fast to write.
    // Not thread safe for writing: store() must be called with
    // locked LogState::mutex.
    // If you need to read only one of the 3 values, you can just load() it
    // from the corresponding atomic.
    struct LatestPartitionInfo {
      // Invariant: if version is even, latest_partition and first_lsn_in_latest
      // are consistent with each other (correspond to the same partition).
      // version never decreases.
      std::atomic<uint64_t> version{0};

      std::atomic<partition_id_t> latest_partition{PARTITION_INVALID};
      std::atomic<lsn_t> first_lsn_in_latest{LSN_INVALID};
      std::atomic<lsn_t> max_lsn_in_latest{LSN_INVALID};

      void store(partition_id_t id, lsn_t first_lsn, lsn_t max_lsn);
      void load(partition_id_t* out_partition,
                lsn_t* out_first_lsn,
                lsn_t* out_max_lsn) const;

      // A fast path for updating max_lsn_in_latest without the other fields.
      // Just like store(), needs to be called with locked LogState::mutex.
      void updateMaxLSN(lsn_t max_lsn);
    };

    // Locked for all directory updates.
    // Note: If contention on this mutex turns out to be a problem,
    //       can replace it with an rwlock protecting the set of keys in
    //       directory, so that there's no need to lock it for writing
    //       when only updating a value in directory.
    // A note about mutex locking order. To avoid deadlocks, locking
    // multiple mutexes should be done in the following order:
    //  1) {oldest,latest}_partition_mutex_ (they're never locked together),
    //  2) Partition::mutex_ in order of increasing partition ID.
    //  3) LogState::mutex for logs in order of increasing log ID,
    std::mutex mutex;

    // All LogState fields are only modified with locked mutex.

    // ID and first LSN of the highest directory entry for this log. These
    // two need to be atomic because iterators use them without locking
    // LogState::mutex. Moreover, they need to be jointly atomic (i.e. no torn
    // reads) for correctness of iterators.
    LatestPartitionInfo latest_partition;

    // Information about partitions used by this log, keyed by their first_lsn
    std::map<lsn_t, DirectoryEntry> directory;
  };

  using LogStateMap = folly::ConcurrentHashMap<logid_t::raw_type,
                                               std::unique_ptr<LogState>,
                                               Hash64<logid_t::raw_type>>;

  using LogLocks = FixedKeysMap<logid_t, std::unique_lock<std::mutex>>;

  // Opens the RocksDB instance. Called by the constructor.
  bool open(const std::vector<std::string>& column_families,
            const rocksdb::ColumnFamilyOptions& meta_cf_options,
            const Configuration* config);

  // Create partitions to cover time range between timestamp_to_cover and now.
  // Called from constructor.
  bool precreatePartitions(partition_id_t first_id,
                           RecordTimestamp timestamp_to_cover,
                           bool at_least_one);

  // Create partitions with IDs [first_id, first_id + count), with timestamps
  // first_timestamp + i * partition_duration.
  // The range of IDs should be either right before or right after the range
  // of existing partition IDs. Updates `partitions_`.
  // Caller is responsible for locking the appropriate mutex
  // (oldest_partition_mutex_ or latest_partition_mutex_), as well as updating
  // all data structures besides `partitions_` (e.g. oldest_partition_id_ and
  // latest_).
  // Returns empty vector on failure. There are no partial failures: the
  // returned vector is either empty or of size `count`.
  // If called with RecordTimestamp, writes metadata and then the CF;
  // if called with lambda to get first timestamp, by default, waits until CF
  // is created before getting and writing timestamp. This allows for more
  // precise timestamps when we're creating a new latest partition.
  std::vector<PartitionPtr>
  createPartitionsImpl(partition_id_t first_id,
                       RecordTimestamp first_timestamp,
                       size_t count,
                       const DirtyState* pre_dirty_state = nullptr) {
    return createPartitionsImpl(first_id,
                                [&] { return first_timestamp; },
                                count,
                                pre_dirty_state,
                                false);
  }
  std::vector<PartitionPtr> createPartitionsImpl(
      partition_id_t first_id,
      std::function<RecordTimestamp()> get_first_timestamp_func,
      size_t count,
      const DirtyState* pre_dirty_state = nullptr,
      bool create_cfs_before_metadata = true);

  // Create `count` partitions at the beginning of partition list. Requires
  // `oldest_partition_mutex_` to be locked. Updates oldest_partition_id_.
  std::vector<PartitionPtr> prependPartitionsInternal(size_t count);

  // If `min_timestamp_to_cover` is smaller than the timestamp of the first
  // partition, creates partitions at the beginning of partition list to
  // accommodate `min_timestamp_to_cover`.
  // @param log and @param lsn are only used for warning messages.
  // @return  true in case of success, false if some rocksdb operation failed.
  //          If covering `min_timestamp_to_cover` requires creating too many
  //          partitions, it won't be covered; this is not considered a failure.
  bool prependPartitionsIfNeeded(RecordTimestamp min_timestamp_to_cover,
                                 logid_t log,
                                 lsn_t lsn);

  // Drops any deprecated and unsupported metadata found in shards;
  // currently just OldestPartitionMetadata.
  bool removeDeprecatedMetadata();

  // Called by the constructor. Populates directory in LogState for each log.
  bool readDirectories();

  // Called by the constructor.
  // Reads timestamps metadata for the given partition.
  bool readPartitionTimestamps(PartitionPtr partition);

  // Called by the constructor.
  // Reads persisted dirty metadata for the given partition.
  bool readPartitionDirtyState(PartitionPtr partition);

  // Called by the constructor.
  // Reads oldest partition ID from metadata and, if it's greater than
  // oldest_partition_id_, calls dropPartitions().
  // This is needed to avoid a tiny probability of seeing directory entries
  // inconsistent with data in partitions.
  bool finishInterruptedDrops();

  // Helper method which creates a new column family with the given name.
  // Returns a handle to the column family or nullptr on failure.
  std::unique_ptr<rocksdb::ColumnFamilyHandle>
  createColumnFamily(std::string name,
                     const rocksdb::ColumnFamilyOptions& options);

  // Helper method which bulk creates column families with given names
  // Returns a vector of handles to created column families or empty vector on
  // failure
  std::vector<std::unique_ptr<rocksdb::ColumnFamilyHandle>>
  createColumnFamilies(const std::vector<std::string>& names,
                       const rocksdb::ColumnFamilyOptions& options);

  // Inserts into partitions_ and updates stats accordingly.
  void addPartitions(std::vector<PartitionPtr> partitions);

  enum class GetWritePartitionResult {
    OK,    // Caller should write the record to *out_partition.
    RETRY, // min_allowed_partition passed in was too big; getWritePartition()
           // decreased it; caller should retry; it's an unlikely situation
           // caused by discrepancy between directory and partition timestamps
    SKIP,  // Throw this delete/amend away because we already know there's no
           // corresponding record; only possible if !timestamp.hasValue().
    ERROR, // Something failed, go to read-only mode.
  };

  // Gets the handle of the CF into which a record for log_id with the
  // given lsn should be written to. This method may also need to write some
  // metadata (i.e. an entry in the directory). It adds the needed metadata
  // updates to rocksdb_batch. If !timestamp.hasValue(), doesn't update
  // metadata and may return SKIP result.
  //
  // NOTE: In some cases the caller may not add the actual record to
  //       rocksdb_batch: the target partition was dropped (see retries
  //       in writeMulti()), or upgrading the directory entry requires a
  //       durable store and the call has requested non-durable durability
  //       (rocksdb_batch and durable_batch reference different batches).
  //       In these cases the caller must still write the rocksdb_batch.
  //
  // INVARIANT: LogState::mutex should be locked for this log.
  //
  // @param min_allowed_partition
  //   partition with this ID is known to not be dropped or in the process of
  //   being dropped (the caller is holding a Partition::mutex for it).
  //   We are not allowed to write directory entries for partitions with smaller
  //   IDs because they might be dropped by the time we're finished, leaving a
  //   dangling directory entry.
  //   (Side note: these dangling entries would be harmless, as far as I can
  //    tell. But it's so hard to reason about all the potential subtle race
  //    conditions and ABA problems involving them that it's better to avoid
  //    dangling entries altogether.)
  //
  // @param rocksdb_batch
  //   the WriteBatch that will be used to perform the user's operation.
  //   Directory updates may or may not be added to this WriteBatch.
  //
  // @param durable_batch
  //   a WriteBatch that is always converted to durable operations on
  //   rocksdb. May or may not reference the same batch as rocksdb_batch.
  //
  // @param payload_size_bytes
  //   the size of the record's payload in bytes; 0 for all operations except
  //   PUT. Used in order to update the byte size in logsdb directory entries.
  //
  GetWritePartitionResult
  getWritePartition(logid_t log_id,
                    lsn_t lsn,
                    Durability durability,
                    folly::Optional<RecordTimestamp> timestamp,
                    partition_id_t* min_allowed_partition, // in and out
                    rocksdb::WriteBatch& rocksdb_batch,
                    rocksdb::WriteBatch& durable_batch,
                    PartitionPtr* out_partition,
                    size_t payload_size_bytes,
                    LocalLogStoreRecordFormat::flags_t flags);

  // Gets the partition that best matches the given timestamp
  // (see Partition::starting_timestamp). Most records are written to their
  // preferred partitions, but may sometimes go to slightly different partitions
  // if it's required for directory consistency (see getWritePartition()).
  partition_id_t getPreferredPartition(RecordTimestamp,
                                       bool warn_if_old = true);

  // An overload that uses a given snapshot of partition list.
  partition_id_t getPreferredPartition(RecordTimestamp timestamp,
                                       bool warn_if_old,
                                       PartitionList partitions,
                                       partition_id_t latest_id,
                                       partition_id_t oldest_id);

  // Implements most of writeMulti(). `log_locks` should be locked and contain
  // all partitioned logs that have a RecordWriteOp in `writes`.
  //
  // @return  0 on success, -1 on failure, sets err to:
  //   E::LOCAL_LOG_STORE_WRITE
  //     There was some non-transient error, and we should give up the write.
  //   E::AGAIN
  //     The min_target_partition passed in wasn't good enough: either too big
  //     (some record needs to go to a smaller-numbered partition) or
  //     too small (it has already been dropped). In this case this method sets
  //     *min_target_partition to an improved estimate, and you should call
  //     it again. Note that it may fail again if the partition was dropped
  //     between the calls, so this method is supposed to be called in a retry
  //     loop. E::AGAIN should be very rare.
  int writeMultiImpl(const std::vector<const WriteOp*>& writes,
                     const WriteOptions& write_options,
                     const LogLocks::Keys* partitioned_logs, // in
                     partition_id_t* min_target_partition    // in and out
  );

  // Searches through the directory in order to find the partition a record
  // with the given sequence number belongs to. The iterator will be moved
  // to point to the record for this partition.
  // If couldn't find partition returns -1 and sets err to
  //   NOTFOUND             if the log is empty
  //   WOULDBLOCK           if iterator gets into incomplete state
  //   LOCAL_LOG_STORE_READ if iterator gets into error state
  int findPartition(RocksDBIterator* it,
                    logid_t log_id,
                    lsn_t lsn,
                    PartitionPtr* out_partition) const;

  // Returns an iterator over the metadata column family.
  RocksDBIterator createMetadataIterator(bool allow_blocking_io = true) const;

  // Checks if min and max timestamp for partition need to be updated
  // to include new_timestamp. Adds the needed updates to rocksdb_batch.
  std::unique_ptr<Partition::TimestampUpdateTask>
  updatePartitionTimestampsIfNeeded(PartitionPtr partition,
                                    RecordTimestamp new_timestamp,
                                    rocksdb::WriteBatch& rocksdb_batch);

  // Looks at partition directory and log trim points. Finds
  // partitions that only have trimmed records, and can be dropped.
  //
  // @return ID of the oldest partition that should _not_ be dropped.
  // Can be PARTITION_INVALID if nothing should be dopped.
  partition_id_t findObsoletePartitions();

  // Gets partitions to compact based on proactive_compaction_enabled.
  void getPartitionsForProactiveCompaction(
      std::vector<PartitionToCompact>* out_to_compact);

  // Gets partitions for partial compaction. Returns true if there are more
  // partial compactions to be done than the results added
  bool getPartitionsForPartialCompaction(
      std::vector<PartitionToCompact>* out_to_compact,
      size_t max_results);

  // Appends at most `count` partitions to compact to the vector pointed to by
  // `out_to_compact`. Returns true if there are more pending manual compactions
  // left in the relevant (hi-pri or lo-pri) list
  bool getPartitionsForManualCompaction(
      std::vector<PartitionToCompact>* out_to_compact,
      size_t count,
      bool hi_pri);

  // Cancels manual compactions for the specified partition_id. Should be done
  // with manual_compactions_mutex_ locked, so expects a reference to the lock
  // as the first arg. If partition_id == PARTITION_INVALID, cancels all
  // compactions pending on this shard.
  void cancelManualCompactionImpl(std::lock_guard<std::mutex>& lock,
                                  partition_id_t partition_id);

  // Partition up to which we should drop to keep space usage below configured
  // value.
  partition_id_t getPartitionToDropUpToBasedOnSpace();

  // Returns effective retention.
  // Also starts and stops grace period when log is removed or re-added to
  // the config. Because of that, this method should be called periodically
  // on all logs for which we have data. Lo-pri background thread does this.
  // If log is removed from config, determines whether we should wait for
  // grace period to expire before attempting to trim it.
  // Otherwise, it is ok for the log to get trimmed if required.
  //
  // If return value is valid, it contains effective retention,
  // which is equal to
  // a) '0', this means grace period expired
  //                   OR
  // b) log retention stored in config(when log is present in config)
  //
  // If return value is 'folly::none', this means don't trim the log.
  //
  // @param out_logs_in_grace_period
  //    incremented if the log was removed from config but grace period
  //    hasn't expired yet.
  folly::Optional<std::chrono::seconds>
  getEffectiveBacklogDuration(logid_t log_id, size_t* out_logs_in_grace_period);

  // Updates log trim points according to time-based retention policy.
  // See parameterless overload of this method for details and error codes.
  // Along the way determines which partitions should be dropped and which
  // should be compacted.
  // E.g. if there are logs with 7-day and 3-day backlog duration,
  // this method will advise compacting a partition when all
  // 3-day records in it are trimmed, and then dropping it when all
  // 3-day and 7-day records in it are trimmed.
  //
  // @param out_oldest_to_keep  output argument;
  //        partitions up to out_oldest_to_keep can be dropped because all
  //        records are trimmed; same as result of findObsoletePartitions().
  //        Note that this is only a guideline: by the time we get around to
  //        dropping these partitions, they may receive some non-obsolete
  //        records. The trim points will be re-checked right before drop,
  //        with locked Partition::mutex.
  // @param out_to_compact  output argument;
  //        a list of partitions that should be compacted because all records
  //        for some backlog duration are trimmed
  // @param out_logs_in_grace_period;
  //        This counts the number of logs that were removed from the config,
  //        but whose data hasn't been trimmed, as they are waiting for
  //        grace period to expire.
  int trimLogsBasedOnTime(partition_id_t* out_oldest_to_keep,
                          std::vector<PartitionToCompact>* out_to_compact,
                          size_t* out_logs_in_grace_period);

  // Updates log trim points so that all records in partitions
  // [1, oldest_to_keep) are trimmed.
  // @return 0 on success. On error returns -1 and sets err to:
  //  - LOCAL_LOG_STORE_READ if we couldn't read from log store,
  //  - LOCAL_LOG_STORE_WRITE if we couldn't write to log store,
  //  - NOBUFS if maximum number of logs was reached,
  //  - AGAIN if the store is not fully initialised (no processor),
  int trimLogsToExcludePartitions(partition_id_t oldest_to_keep);

  // Drops partitions with id less than
  // min(oldest_to_keep_est, get_oldest_to_keep()).
  // To avoid races with writing, calls get_oldest_to_keep() after
  // acquiring partition locks for partitions up to oldest_to_keep.
  // One way to use this is double-checked locking: dropPartitions(f(), f).
  //
  // Doesn't update trim points; the caller should ensure consistency between
  // partition drops and trim point updates.
  // Dropping the latest partition is not allowed.
  //
  // @return 0 on success, -1 on error. Sets err to:
  //   E::INTERNAL if an assertion failed.
  //   E::LOCAL_LOG_STORE_WRITE if some RocksDB operation failed.
  //   E::GAP if there was a gap in column family numbers.
  //   E::NOBUFS if maximum number of logs was reached.
  //   E::SHUTDOWN if the the store is shutting down.
  int dropPartitions(partition_id_t oldest_to_keep_est,
                     std::function<partition_id_t()> get_oldest_to_keep);

  // Performs compaction in a way optimized for situation when most of the logs
  // in the partition are fully trimmed. This is usually the case for retention
  // compactions.
  // Called from performCompactionInternal().
  bool performStronglyFilteredCompactionInternal(PartitionPtr partition);

  // Deletes obsolete directory entries:
  //  - entries with partition id < oldest_partition_id_,
  //  - entries with max_lsn <= trim_point and
  //    partition id in @param compacted_partitions.
  // Requires oldest_partition_mutex_ to be locked (that's probably
  // not necessary but this way it's easier to reason about).
  // Locks LogState::mutex for each log that has something to delete.
  //
  // @param compacted_partitions  partitions that have just been compacted by
  //                              the caller.
  void
  cleanUpDirectory(const std::set<partition_id_t>& compacted_partitions = {});
  // Same for partition metadata.
  void cleanUpPartitionMetadataAfterDrop(partition_id_t oldest_to_keep);

  // Applies RocksDBSettings::metadata_compaction_period.
  void compactMetadataCFIfNeeded();

  typedef std::function<void(logid_t log_id,
                             partition_id_t partition_id,
                             bool& remove_entry,
                             bool& skip_to_next_log)>
      DirectoryFilterCallback;

  // Runs in a background thread.
  // In an infinite loop creates new partitions as needed.
  void hiPriBackgroundThreadRun();

  // Runs in a background thread.
  // In an infinite loop trims logs, drops and compacts partitions as needed.
  void loPriBackgroundThreadRun();

  // A helper function used to position the iterator at the last directory entry
  // for a given log that's <= lsn. If no such entry exists, iterator will point
  // to the smallest entry for log_id instead. If next_log_out is not null, it
  // will be set to the log id of the first entry past `it'.
  // `it` must have total_order_seek = true.
  // On error returns -1 and sets err to
  //   NOTFOUND             if the log is empty
  //   WOULDBLOCK           if iterator gets into incomplete state
  //   LOCAL_LOG_STORE_READ if iterator gets into error state
  int seekToLastInDirectory(RocksDBIterator* it,
                            logid_t log_id,
                            lsn_t lsn,
                            logid_t* next_log_out = nullptr) const;

  // Locked when creating/dropping partitions at the beginning of partition
  // list.
  std::mutex oldest_partition_mutex_;

  // Locked when creating partitions at the end of partition list.
  std::mutex latest_partition_mutex_;

  // Pointer to the latest partition.
  struct LatestPtrTag {};
  UpdateableSharedPtr<Partition, LatestPtrTag> latest_;

  // List of all data partitions.
  RandomAccessQueue<PartitionPtr> partitions_;

  // The oldest partition that is not dropped and not being dropped.
  // Increased when partitions are dropped,
  // decreased when partitions are created in the beginning of partition list.
  // Modified with locked oldest_partition_mutex_.
  std::atomic<partition_id_t> oldest_partition_id_{1};

  // Limit to drop partitions up to for space-based trimming
  std::atomic<partition_id_t> space_based_trim_limit{PARTITION_INVALID};

  // No automatic partition drops will happen until this time. This is used
  // to prevent partitions from being created and dropped too often if
  // we keep receiving writes in old partitions, but these writes are quickly
  // trimmed away, or are already below trim point at the time of writing,
  // or have to go to a later partition based on LSN.
  AtomicSteadyTimestamp avoid_drops_until_{};

  // Column family containing metadata
  std::unique_ptr<rocksdb::ColumnFamilyHandle> metadata_cf_;

  // Column family for unpartitioned logs. See isLogPartitioned().
  std::unique_ptr<rocksdb::ColumnFamilyHandle> unpartitioned_cf_;

  // Column family for persisting log snapshot blobs.
  std::unique_ptr<rocksdb::ColumnFamilyHandle> snapshots_cf_;
  // Used to keep the snapshots CF from being created/deleted while we're
  // trying to flush it.
  std::mutex snapshots_cf_lock_;

  // Dirty data tracking for unpartitioned logs.
  DirtyState unpartitioned_dirty_state_;

  // Per-log information.
  LogStateMap logs_;

  // Options used for data partitions
  rocksdb::ColumnFamilyOptions data_cf_options_;

  // Processor is needed to:
  //  - update trim points when dropping partitions,
  //  - get trimming policy from config.
  std::atomic<ServerProcessor*> processor_{nullptr};

  StatsHolder* stats_;

  std::mutex manual_compaction_mutex_;
  std::list<partition_id_t> hi_pri_manual_compactions_;
  std::list<partition_id_t> lo_pri_manual_compactions_;

  // index of partition_id -> (hi_pri, iterator_to_list_entry)
  std::unordered_map<partition_id_t,
                     std::pair<bool, std::list<partition_id_t>::iterator>>
      manual_compaction_index_;

  std::unique_ptr<MemtableFlushCallback> flushCallback_;

  // If true, stall low-pri writes to wait for partial compactions to catch up.
  std::atomic<bool> too_many_partial_compactions_{false};

  // Approximate time when "metadata" CF was last compacted.
  SteadyTimestamp last_metadata_manual_compaction_time_;

  // Approximate time when cleanUpDirectory last verified consistency of all
  // entries in on-disk directory with in-memory directory
  AtomicSteadyTimestamp last_directory_consistency_check_time_{
      SteadyTimestamp::min()};

 protected:
  enum class DeferInit {
    NO,
    YES,
  };

  // Signaled by ~PartitionedRocksDBStore() and markImmutable().
  // After that we don't expect to get any writes, but reads are still
  // possible and must work.
  SingleEvent shutdown_event_;

  // Set to true after adviceWritesShutdown(). Used by asserts.
  std::atomic<bool> immutable_{false};

  std::thread background_threads_[(int)BackgroundThreadType::COUNT];

  // Used by the background partition cleaner. The cleaner defers
  // cleaning partitions after a flush has occurred by a configurable
  // interval. This allows nodes to redirty a partition by writing to
  // the new memtable in the partition without incuring a synchronous
  // write penalty for updating dirty state durably.
  std::deque<std::pair<SteadyTimestamp, FlushToken>> cleaner_work_queue_;
  FlushToken last_broadcast_flush_{FlushToken_INVALID};
  std::atomic<bool> cleaner_pass_requested_{false};

  // For implementing an amount written flush trigger.
  std::atomic<uint64_t> bytes_written_since_flush_{0};
  // For restricting manual MemTable flushes to a maximum frequency.
  SteadyTimestamp last_flush_time_{currentSteadyTime()};
  // For time triggered manual MemTable flush policies.
  SteadyTimestamp min_partition_idle_time_{SteadyTimestamp::max()};

  // Set to valid partition ids if rebuilding has yet to restore data in one
  // or more partitions that were dirty at the time of an un-safe shutdown.
  std::atomic<partition_id_t> min_under_replicated_partition_{PARTITION_MAX};
  std::atomic<partition_id_t> max_under_replicated_partition_{
      PARTITION_INVALID};

  explicit PartitionedRocksDBStore(uint32_t shard_idx,
                                   const std::string& path,
                                   RocksDBLogStoreConfig rocksdb_config,
                                   const Configuration* config,
                                   StatsHolder* stats,
                                   DeferInit defer_init);

  // Part of initialization.
  void startBackgroundThreads();

  // Registers a callback that is called when a Memtable is flushed.
  // The callback is used to send Memtable flush notifications to other
  // storage nodes in the cluster. Rebuilding without WAL uses these
  // notifications to keep track what is stored durably and what is not.
  void createAndRegisterFlushCallback();

  // Does most of the initialization. Called from constructor, unless
  // DeferInit::YES was given to constructor. The latter is used by tests,
  // so that the initialization uses virtual methods overridden in subclass.
  void init(const Configuration* config);

  // Does most of the shutdown: flushes memtables, aborts compactions, stops
  // background threads. Any writes attempted after this call will fail an
  // assert. Reads, on the other hand, will still fully work.
  //
  // Called from destructor and markImmutable(). And also from tests,
  // similarly to init(): background threads use virtual methods,
  // so they need to be destroyed before the subclass is destroyed.
  void joinBackgroundThreads();

  virtual bool shouldFlushMemtables();

  // Decides if it's time to create new partition. It's time when current
  // latest partition is either too old or has too many L0 files.
  virtual bool shouldCreatePartition();

  // Called on each iteration of each background thread. Sleeps between
  // iterations until some timeout expires or until shutdown is requested.
  // Can be mocked to control background thread from outside.
  virtual void backgroundThreadSleep(BackgroundThreadType type);

  // Hook to allow tests to override time.
  virtual SystemTimestamp currentTime();

  // Hook to allow tests to override time.
  virtual SteadyTimestamp currentSteadyTime();

  void onMemTableWindowUpdated() override;

  // Performs compaction of the partition. Removes obsolete partition directory
  // entries afterwards (for logs that were fully removed from partition).
  // The compaction updates trim points to more precise values than trimLogs()
  // can produce. Doesn't update trim points beforehand.
  // Blocks until compaction is complete.
  virtual void performCompactionInternal(PartitionToCompact to_compact);

  // Calls the given callbacks for every partition that might fall into the
  // given time intervals
  void findPartitionsMatchingIntervals(
      RecordTimeIntervals& rtis,
      std::function<void(PartitionPtr, RecordTimeInterval)> cb) const;
};

}} // namespace facebook::logdevice