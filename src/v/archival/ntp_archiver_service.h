/*
 * Copyright 2021 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */

#pragma once
#include "archival/archival_policy.h"
#include "archival/probe.h"
#include "archival/types.h"
#include "cloud_storage/partition_manifest.h"
#include "cloud_storage/remote.h"
#include "cloud_storage/types.h"
#include "cloud_storage_clients/client.h"
#include "cluster/fwd.h"
#include "cluster/partition.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "storage/fwd.h"
#include "storage/segment.h"
#include "utils/intrusive_list_helpers.h"
#include "utils/retry_chain_node.h"

#include <seastar/core/abort_source.hh>
#include <seastar/core/io_priority_class.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/util/noncopyable_function.hh>

namespace archival {

using namespace std::chrono_literals;

enum class segment_upload_kind { compacted, non_compacted };

std::ostream& operator<<(std::ostream& os, segment_upload_kind upload_kind);

/// This class performs per-ntp arhcival workload. Every ntp can be
/// processed independently, without the knowledge about others. All
/// 'ntp_archiver' instances that the shard posesses are supposed to be
/// aggregated on a higher level in the 'archiver_service'.
///
/// The 'ntp_archiver' is responsible for manifest manitpulations and
/// generation of per-ntp candidate set. The actual file uploads are
/// handled by 'archiver_service'.
///
/// Note that archiver uses initial revision of the partition, not the
/// current one. The revision of the partition can change when the partition
/// is moved between the nodes. To make all object names stable inside
/// the S3 bucket we're using initial revision. The revision that the
/// topic was assigned when it was just created.
class ntp_archiver {
public:
    /// Iterator type used to retrieve candidates for upload
    using back_insert_iterator
      = std::back_insert_iterator<std::vector<segment_name>>;

    /// Create new instance
    ///
    /// \param ntp is an ntp that archiver is responsible for
    /// \param conf is an S3 client configuration
    /// \param remote is an object used to send/recv data
    /// \param svc_probe is a service level probe (optional)
    ntp_archiver(
      const storage::ntp_config& ntp,
      ss::lw_shared_ptr<const configuration> conf,
      cloud_storage::remote& remote,
      cluster::partition& parent);

    /// Spawn background fibers, which depending on the mode (read replica or
    /// not) will either do uploads, or periodically read back the manifest.
    ss::future<> start();

    /// Stop archiver.
    ///
    /// \return future that will become ready when all async operation will be
    /// completed
    ss::future<> stop();

    /// Get NTP
    const model::ntp& get_ntp() const;

    /// Get revision id
    model::initial_revision_id get_revision_id() const;

    /// Get timestamp
    const ss::lowres_clock::time_point get_last_upload_time() const;

    /// Download manifest from pre-defined S3 locatnewion
    ///
    /// \return future that returns true if the manifest was found in S3
    ss::future<std::pair<
      cloud_storage::partition_manifest,
      cloud_storage::download_result>>
    download_manifest();

    struct upload_group_result {
        size_t num_succeeded;
        size_t num_failed;
        size_t num_cancelled;

        auto operator<=>(const upload_group_result&) const = default;
    };

    struct batch_result {
        upload_group_result non_compacted_upload_result;
        upload_group_result compacted_upload_result;

        auto operator<=>(const batch_result&) const = default;
    };

    /// \brief Upload next set of segments to S3 (if any)
    /// The semaphore is used to track number of parallel uploads. The method
    /// will pick not more than '_concurrency' candidates and start
    /// uploading them.
    ///
    /// \param lso_override last stable offset override
    /// \return future that returns number of uploaded/failed segments
    virtual ss::future<batch_result> upload_next_candidates(
      std::optional<model::offset> last_stable_offset_override = std::nullopt);

    ss::future<cloud_storage::download_result> sync_manifest();

    uint64_t estimate_backlog_size();

    /// \brief Probe remote storage and truncate the manifest if needed
    ss::future<std::optional<cloud_storage::partition_manifest>>
    maybe_truncate_manifest();

    /// \brief Perform housekeeping operations.
    ss::future<> housekeeping();

    /// \brief Advance the start offest for the remote partition
    /// according to the retention policy specified by the partition
    /// configuration. This function does *not* delete any data.
    ss::future<> apply_retention();

    /// \brief Remove segments that are no longer queriable by:
    /// segments that are below the current start offset and segments
    /// that have been replaced with their compacted equivalent.
    ss::future<> garbage_collect();

    virtual ~ntp_archiver() = default;

    /**
     * Partition 0 carries a copy of the topic configuration, updated by
     * the controller, so that its archiver can make updates to the topic
     * manifest in cloud storage.
     *
     * When that changes, we are notified, so that we may re-upload the
     * manifest if needed.
     */
    void notify_topic_config() { _topic_manifest_dirty = true; }

    /**
     * If the group has a leader (non-null argument), and it is ourselves,
     * then signal _leader_cond to prompt the upload loop to stop waiting.
     */
    void notify_leadership(std::optional<model::node_id>);

    /**
     * Get list of all housekeeping jobs for the ntp
     *
     * The list includes adjacent segment merging but may be extended in
     * the future. The references are guaranteed to have the same lifetime
     * as ntp_archiver_service object itself.
     */
    std::vector<std::reference_wrapper<housekeeping_job>>
    get_housekeeping_jobs();

    /// The user supplied function that can be used to scan the
    /// state of the archiver and return an adjacent_segment_run.
    ///
    /// \param local_start_offset is a start offset of the raft group of
    ///        the partition
    /// \param manifest is a manifest instance stored in the archival
    ///        STM
    /// \return nullopt or initialized adjacent_segment_run
    using manifest_scanner_t
      = ss::noncopyable_function<std::optional<adjacent_segment_run>(
        model::offset local_start_offset,
        const cloud_storage::partition_manifest& manifest)>;

    /// Find upload candidate
    ///
    /// Depending on the output of the 'scanner' the upload candidate
    /// might be local (it will contain a list of segments in candidate.segments
    /// and a list of locks) or remote (it will contain a list of paths in
    /// candidate.remote_segments).
    ///
    /// \param scanner is a user provided function used to find upload candidate
    /// \return nullopt or the upload candidate
    ss::future<std::optional<upload_candidate_with_locks>>
    find_reupload_candidate(manifest_scanner_t scanner);

    /**
     * Upload segment provided from the outside of the ntp_archiver
     *
     * The method can be used to upload segments stored locally in the
     * redpanda data directory or remotely in cloud storage.
     *
     * \param candidate is an upload candidate
     * \param source_rtc is used to pass retry_chain_node that belongs
     *        to the caller. This way the caller can use its own abort_source
     *        and also filter out the notifications generated by the 'upload'
     *        call that it triggers.
     * \return true on success and false otherwise
     */
    ss::future<bool> upload(
      upload_candidate_with_locks candidate,
      std::optional<std::reference_wrapper<retry_chain_node>> source_rtc);

    /// Return reference to partition manifest from archival STM
    const cloud_storage::partition_manifest& manifest() const;

    /// Get segment size for the partition
    size_t get_local_segment_size() const;

    /// Ahead of a leadership transfer, finish any pending uploads and stop
    /// the upload loop, so that we do not leave orphan objects behind if
    /// a leadership transfer happens between writing a segment and writing
    /// the manifest.
    /// \param timeout: block for this long waiting for uploads to finish
    ///                 before returning.  Return false if timeout expires.
    /// \return true if uploads have cleanly quiesced within timeout.
    ss::future<bool> prepare_transfer_leadership(ss::lowres_clock::duration);

    /// After a leadership transfer attempt (whether it proceeded or not),
    /// permit this archiver to proceed as normal: if it is still the leader
    /// it will resume uploads.
    void complete_transfer_leadership();

private:
    ss::future<bool> do_upload_local(
      upload_candidate_with_locks candidate,
      std::optional<std::reference_wrapper<retry_chain_node>> source_rtc);
    ss::future<bool> do_upload_remote(
      upload_candidate_with_locks candidate,
      std::optional<std::reference_wrapper<retry_chain_node>> source_rtc);
    /// Information about started upload
    struct scheduled_upload {
        /// The future that will be ready when the segment will be fully
        /// uploaded
        std::optional<ss::future<cloud_storage::upload_result>> result;
        /// Last offset of the uploaded segment or part
        model::offset inclusive_last_offset;
        /// Segment metadata
        std::optional<cloud_storage::partition_manifest::segment_meta> meta;
        /// Name of the uploaded segment
        std::optional<ss::sstring> name;
        /// Offset range convered by the upload
        std::optional<model::offset> delta;
        /// Contains 'no' if the method can be called another time or 'yes'
        /// if it shouldn't be called (if there is no data to upload).
        /// If the 'stop' is 'no' the 'result' might be 'nullopt'. In this
        /// case the upload is not started but the method might be called
        /// again anyway.
        ss::stop_iteration stop;
        /// Protects the underlying segment(s) from being deleted while the
        /// upload is in flight.
        std::vector<ss::rwlock::holder> segment_read_locks;
        segment_upload_kind upload_kind;
    };

    using allow_reuploads_t = ss::bool_class<struct allow_reupload_tag>;
    /// An upload context represents a range of offsets to be uploaded. It
    /// will search for segments within this range and upload them, it also
    /// carries some context information like whether re-uploads are
    /// allowed, what is the maximum number of in-flight uploads that can be
    /// processed etc.
    struct upload_context {
        /// The kind of segment being uploaded
        segment_upload_kind upload_kind;
        /// The next scheduled upload will start from this offset
        model::offset start_offset;
        /// Uploads will stop at this offset
        model::offset last_offset;
        /// Controls checks for reuploads, compacted segments have this
        /// check disabled
        allow_reuploads_t allow_reuploads;
        /// Collection of uploads scheduled so far
        std::vector<scheduled_upload> uploads{};

        /// Schedules a single upload, adds it to upload collection and
        /// progresses the start offset
        ss::future<ss::stop_iteration>
        schedule_single_upload(ntp_archiver& archiver);

        upload_context(
          segment_upload_kind upload_kind,
          model::offset start_offset,
          model::offset last_offset,
          allow_reuploads_t allow_reuploads);
    };

    /// Start upload without waiting for it to complete
    ss::future<scheduled_upload>
    schedule_single_upload(const upload_context& upload_ctx);

    /// Start all uploads
    ss::future<std::vector<scheduled_upload>>
    schedule_uploads(model::offset last_stable_offset);

    ss::future<std::vector<scheduled_upload>>
    schedule_uploads(std::vector<upload_context> loop_contexts);

    /// Wait until all scheduled uploads will be completed
    ///
    /// Update the probe and manifest
    ss::future<ntp_archiver::batch_result> wait_all_scheduled_uploads(
      std::vector<ntp_archiver::scheduled_upload> scheduled);

    /// Waits for scheduled segment uploads. The uploaded segments could be
    /// compacted or non-compacted, the actions taken are similar in both
    /// cases with the major difference being the probe updates done after
    /// the upload.
    ss::future<ntp_archiver::upload_group_result> wait_uploads(
      std::vector<scheduled_upload> scheduled,
      segment_upload_kind segment_kind);

    /// Upload individual segment to S3.
    ///
    /// \param candidate is an upload candidate
    /// \param source_rtc is a retry_chain_node of the caller, if it's set
    ///        to nullopt own retry chain of the ntp_archiver is used
    /// \return error code
    ss::future<cloud_storage::upload_result> upload_segment(
      upload_candidate candidate,
      std::optional<std::reference_wrapper<retry_chain_node>> source_rtc
      = std::nullopt);

    /// Upload segment's transactions metadata to S3.
    ///
    /// \return error code
    ss::future<cloud_storage::upload_result> upload_tx(
      upload_candidate candidate,
      std::optional<std::reference_wrapper<retry_chain_node>> source_rtc
      = std::nullopt);

    /// Upload manifest to the pre-defined S3 location
    ss::future<cloud_storage::upload_result> upload_manifest(
      std::optional<std::reference_wrapper<retry_chain_node>> source_rtc
      = std::nullopt);

    /// While leader, within a particular term, keep trying to upload data
    /// from local storage to remote storage until our term changes or
    /// our abort source fires.
    ss::future<> upload_until_term_change();

    /// Outer loop to keep invoking upload_until_term_change until our
    /// abort source fires.
    ss::future<> upload_until_abort();

    /// Periodically try to download and ingest the remote manifest until
    /// our term changes or abort source fires
    ss::future<> sync_manifest_until_term_change();

    /// Outer loop to keep invoking sync_manifest_until_term_change until our
    /// abort source fires.
    ss::future<> sync_manifest_until_abort();

    /// Attempt to upload topic manifest.  Does not throw on error.  Clears
    /// _topic_manifest_dirty on success.
    ss::future<> upload_topic_manifest();

    /// Delete a segment and its transaction metadata from S3.
    /// The transaction metadata is only deleted if the segment
    /// deletion was successful.
    ///
    /// Throws if an abort was requested.
    ss::future<cloud_storage::upload_result>
    delete_segment(const remote_segment_path& path);

    void update_probe();

    /// Return true if archival metadata can be replicated.
    /// This means that the replica is a leader, the term did not
    /// change and the archiver is not stopping.
    bool can_update_archival_metadata() const;

    /// Return true if it is permitted to start new uploads: this
    /// requires can_update_archival_metadata, plus that we are
    /// not paused.
    bool may_begin_uploads() const;

    /// Helper to generate a segment path from candidate
    remote_segment_path
    segment_path_for_candidate(const upload_candidate& candidate);

    /// Method to use with lazy_abort_source
    std::optional<ss::sstring> upload_should_abort();

    const cloud_storage_clients::bucket_name& get_bucket_name() const;

    // Adjacent segment merging

    std::vector<upload_candidate> get_local_adjacent_small_segments();

    model::ntp _ntp;
    model::initial_revision_id _rev;
    cloud_storage::remote& _remote;
    cluster::partition& _parent;
    model::term_id _start_term;
    archival_policy _policy;
    std::optional<cloud_storage_clients::bucket_name> _bucket_override;
    ss::gate _gate;
    ss::abort_source _as;
    retry_chain_node _rtcnode;
    retry_chain_logger _rtclog;
    ssx::semaphore _mutex{1, "archive/ntp"};
    ss::lw_shared_ptr<const configuration> _conf;
    config::binding<std::chrono::milliseconds> _sync_manifest_timeout;
    config::binding<size_t> _max_segments_pending_deletion;
    simple_time_jitter<ss::lowres_clock> _backoff_jitter{100ms};
    size_t _concurrency{4};
    ss::lowres_clock::time_point _last_upload_time;

    // Used during leadership transfer: instructs the archiver to
    // not proceed with uploads, even if it has leadership.
    bool _paused{false};

    // Held while the inner segment upload/manifest sync loop is running,
    // to enable code that uses _paused to wait until ongoing activity
    // has stopped.
    ss::semaphore _uploads_active{1};

    config::binding<std::chrono::milliseconds> _housekeeping_interval;
    simple_time_jitter<ss::lowres_clock> _housekeeping_jitter;
    ss::lowres_clock::time_point _next_housekeeping;

    // 'dirty' in the sense that we need to do another update to S3 to ensure
    // the object matches our local topic configuration.
    bool _topic_manifest_dirty{false};

    // While waiting for leadership, wait on this condition variable. It will
    // be triggered by notify_leadership.
    ss::condition_variable _leader_cond;

    std::optional<ntp_level_probe> _probe{std::nullopt};

    const cloud_storage_clients::object_tag_formatter _segment_tags;
    const cloud_storage_clients::object_tag_formatter _manifest_tags;
    const cloud_storage_clients::object_tag_formatter _tx_tags;

    // NTP level adjacent segment merging job
    std::unique_ptr<housekeeping_job> _local_segment_merger;
    config::binding<bool> _segment_merging_enabled;
};

} // namespace archival
