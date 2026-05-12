// This file Copyright © 2008-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <utility> // for std::swap()
#include <vector>

#include <fmt/core.h>

#include "transmission.h"

#include "bandwidth.h"
#include "crypto-utils.h"
#include "log.h"
#include "peer-io.h"
#include "tr-assert.h"
#include "utils.h" // tr_time_msec()

namespace
{

[[nodiscard]] auto directionName(tr_direction dir) noexcept -> char const*
{
    return dir == TR_UP ? "upload" : "download";
}

[[nodiscard]] auto priorityName(size_t bucket) noexcept -> char const*
{
    switch (bucket)
    {
    case 0:
        return "high";

    case 1:
        return "normal";

    default:
        return "low";
    }
}

struct EnabledCounts
{
    size_t force = 0U;
    size_t high = 0U;
    size_t normal = 0U;
    size_t low = 0U;
};

struct QueuedBytesStats
{
    size_t piece_bytes = 0U;
    size_t protocol_bytes = 0U;
    size_t peers_with_piece = 0U;
};

[[nodiscard]] constexpr auto priorityIndex(tr_priority_t priority) noexcept -> size_t
{
    switch (priority)
    {
    case TR_PRI_FORCE:
        return 0U;

    case TR_PRI_HIGH:
        return 1U;

    case TR_PRI_NORMAL:
        return 2U;

    case TR_PRI_LOW:
    default:
        return 3U;
    }
}

void incrementEnabledCount(EnabledCounts& counts, tr_priority_t priority) noexcept
{
    switch (priority)
    {
    case TR_PRI_FORCE:
        ++counts.force;
        break;

    case TR_PRI_HIGH:
        ++counts.high;
        break;

    case TR_PRI_NORMAL:
        ++counts.normal;
        break;

    case TR_PRI_LOW:
    default:
        ++counts.low;
        break;
    }
}

[[nodiscard]] auto priorityBytes(std::array<size_t, 4> const& bytes, tr_priority_t priority) noexcept -> size_t
{
    return bytes[priorityIndex(priority)];
}

[[nodiscard]] auto queuedBytesStats(std::vector<tr_peerIo*> const& peers) noexcept -> QueuedBytesStats
{
    auto stats = QueuedBytesStats{};
    for (auto const* peer : peers)
    {
        auto const [piece_bytes, protocol_bytes] = peer->get_queued_outbound_byte_counts();
        stats.piece_bytes += piece_bytes;
        stats.protocol_bytes += protocol_bytes;
        if (piece_bytes > 0U)
        {
            ++stats.peers_with_piece;
        }
    }

    return stats;
}

auto constexpr ForceUploadHistorySaturationPercent = uint64_t{ 90U };
auto constexpr ForceUploadHistoryReservePercent = uint64_t{ 200U };
auto constexpr ForceUploadAsyncSpilloverPercent = uint64_t{ 25U };
auto constexpr ForceUploadReserveWarmupMultiplier = uint64_t{ 4U };

void pushToBuckets(std::array<std::vector<tr_peerIo*>, 3>& peer_arrays, size_t first_bucket, tr_peerIo* peer)
{
    for (size_t i = first_bucket; i < std::size(peer_arrays); ++i)
    {
        peer_arrays[i].push_back(peer);
    }
}

} // namespace

tr_bytes_per_second_t tr_bandwidth::getSpeedBytesPerSecond(RateControl& r, unsigned int interval_msec, uint64_t now)
{
    if (now == 0)
    {
        now = tr_time_msec();
    }

    if (now != r.cache_time_)
    {
        uint64_t bytes = 0;
        uint64_t const cutoff = now - interval_msec;

        for (int i = r.newest_; r.date_[i] > cutoff;)
        {
            bytes += r.size_[i];

            if (--i == -1)
            {
                i = HistorySize - 1; /* circular history */
            }

            if (i == r.newest_)
            {
                break; /* we've come all the way around */
            }
        }

        r.cache_val_ = static_cast<tr_bytes_per_second_t>(bytes * 1000U / interval_msec);
        r.cache_time_ = now;
    }

    return r.cache_val_;
}

void tr_bandwidth::notifyBandwidthConsumedBytes(uint64_t const now, RateControl* r, size_t size)
{
    if (r->date_[r->newest_] + GranularityMSec >= now)
    {
        r->size_[r->newest_] += size;
    }
    else
    {
        if (++r->newest_ == HistorySize)
        {
            r->newest_ = 0;
        }

        r->date_[r->newest_] = now;
        r->size_[r->newest_] = size;
    }

    /* invalidate cache_val*/
    r->cache_time_ = 0;
}

// ---

tr_bandwidth::tr_bandwidth(tr_bandwidth* parent)
{
    this->setParent(parent);
}

// ---

namespace
{
namespace deparent_helpers
{
void remove_child(std::vector<tr_bandwidth*>& v, tr_bandwidth* remove_me) noexcept
{
    // the list isn't sorted -- so instead of erase()ing `it`,
    // do the cheaper option of overwriting it with the final item
    if (auto it = std::find(std::begin(v), std::end(v), remove_me); it != std::end(v))
    {
        *it = v.back();
        v.resize(v.size() - 1);
    }
}
} // namespace deparent_helpers
} // namespace

void tr_bandwidth::deparent() noexcept
{
    using namespace deparent_helpers;

    if (parent_ == nullptr)
    {
        return;
    }

    remove_child(parent_->children_, this);
    parent_ = nullptr;
}

void tr_bandwidth::setParent(tr_bandwidth* new_parent)
{
    TR_ASSERT(this != new_parent);

    deparent();

    if (new_parent != nullptr)
    {
#ifdef TR_ENABLE_ASSERTS
        TR_ASSERT(new_parent->parent_ != this);
        auto& children = new_parent->children_;
        TR_ASSERT(std::find(std::begin(children), std::end(children), this) == std::end(children)); // not already there
#endif

        new_parent->children_.push_back(this);
        this->parent_ = new_parent;
    }
}

// ---

void tr_bandwidth::allocateBandwidth(
    tr_priority_t parent_priority,
    unsigned int period_msec,
    std::vector<std::shared_ptr<tr_peerIo>>& peer_pool)
{
    auto const priority = std::max(parent_priority, this->priority_);

    // set the available bandwidth
    for (auto const dir : { TR_UP, TR_DOWN })
    {
        if (auto& bandwidth = band_[dir]; bandwidth.is_limited_)
        {
            auto const next_pulse_speed = bandwidth.desired_speed_bps_;
            bandwidth.bytes_left_ = next_pulse_speed * period_msec / 1000U;
        }
    }

    // add this bandwidth's peer, if any, to the peer pool
    if (auto shared = this->peer_.lock(); shared)
    {
        shared->set_priority(priority);
        peer_pool.push_back(std::move(shared));
    }

    // traverse & repeat for the subtree
    for (auto* child : this->children_)
    {
        child->allocateBandwidth(priority, period_msec, peer_pool);
    }
}

auto tr_bandwidth::phaseOne(std::vector<tr_peerIo*>& peers, tr_direction dir) -> PhaseStats
{
    auto stats = PhaseStats{ .peer_count = std::size(peers) };

    // First phase of IO. Tries to distribute bandwidth fairly to keep faster
    // peers from starving the others.
    tr_logAddTrace(fmt::format("{} peers to go round-robin for {}", peers.size(), directionName(dir)));

    // Shuffle the peers so they all have equal chance to be first in line.
    thread_local auto urbg = tr_urbg<size_t>{};
    std::shuffle(std::begin(peers), std::end(peers), urbg);

    // Give each peer `Increment` bandwidth bytes to use. Repeat this
    // process until we run out of bandwidth and/or peers that can use it.
    for (size_t n_unfinished = std::size(peers); n_unfinished > 0U;)
    {
        for (size_t i = 0; i < n_unfinished;)
        {
            // Value of 3000 bytes chosen so that when using µTP we'll send a full-size
            // frame right away and leave enough buffered data for the next frame to go
            // out in a timely manner.
            static auto constexpr Increment = size_t{ 3000 };

            auto const bytes_used = peers[i]->flush(dir, Increment);
            ++stats.peers_processed;
            stats.bytes_used += bytes_used;
            stats.bytes_used_by_priority[priorityIndex(peers[i]->priority())] += bytes_used;
            if (bytes_used > 0U)
            {
                ++stats.peers_made_progress;
            }
            tr_logAddTrace(fmt::format("peer #{} of {} used {} bytes in this pass", i, n_unfinished, bytes_used));

            if (bytes_used != Increment)
            {
                ++stats.peers_stalled;
                // peer is done writing for now; move it to the end of the list
                std::swap(peers[i], peers[n_unfinished - 1]);
                --n_unfinished;
            }
            else
            {
                ++i;
            }
        }
    }

    return stats;
}

auto tr_bandwidth::phaseOneForce(std::vector<tr_peerIo*>& peers, tr_direction dir) -> PhaseStats
{
    auto stats = PhaseStats{ .peer_count = std::size(peers) };

    // Give force peers first crack at the pulse, but keep rotating them so
    // one busy peer does not monopolize the force-only pass.
    tr_logAddTrace(fmt::format("{} force peers to go round-robin for {}", peers.size(), directionName(dir)));

    thread_local auto urbg = tr_urbg<size_t>{};
    std::shuffle(std::begin(peers), std::end(peers), urbg);

    static auto constexpr Increment = size_t{ 3000 };

    for (size_t n_unfinished = std::size(peers); n_unfinished > 0U;)
    {
        auto const bytes_used = peers[0]->flush(dir, Increment);
        ++stats.peers_processed;
        stats.bytes_used += bytes_used;
        stats.bytes_used_by_priority[priorityIndex(peers[0]->priority())] += bytes_used;
        if (bytes_used > 0U)
        {
            ++stats.peers_made_progress;
        }
        tr_logAddTrace(fmt::format("force peer of {} used {} bytes in this pass", n_unfinished, bytes_used));

        if (bytes_used == 0U)
        {
            ++stats.peers_stalled;
            std::swap(peers[0], peers[n_unfinished - 1]);
            --n_unfinished;
        }
        else if (n_unfinished > 1U)
        {
            // Any forward progress keeps the peer in the force-only queue.
            // Rotate it to the back and keep draining force peers until they
            // all stall or the pulse limit is exhausted.
            std::rotate(std::begin(peers), std::next(std::begin(peers)), std::next(std::begin(peers), n_unfinished));
        }
    }

    return stats;
}

void tr_bandwidth::allocate(unsigned int period_msec)
{
    // keep these peers alive for the scope of this function
    auto refs = std::vector<std::shared_ptr<tr_peerIo>>{};

    auto force_peers = std::vector<tr_peerIo*>{};
    auto unforced_upload_arrays = std::array<std::vector<tr_peerIo*>, 3>{};
    auto download_peer_arrays = std::array<std::vector<tr_peerIo*>, 3>{};
    auto& unforced_upload_high = unforced_upload_arrays[0];
    auto& unforced_upload_normal = unforced_upload_arrays[1];
    auto& unforced_upload_low = unforced_upload_arrays[2];
    auto& download_high = download_peer_arrays[0];
    auto& download_normal = download_peer_arrays[1];
    auto& download_low = download_peer_arrays[2];
    auto regular_unforced_up_stats = std::array<PhaseStats, 3>{};
    auto regular_down_stats = std::array<PhaseStats, 3>{};
    auto peer_counts = EnabledCounts{};
    auto const now = tr_time_msec();
    auto const previous_async_up_piece_bytes_by_priority = async_up_piece_bytes_by_priority_;
    async_up_piece_bytes_by_priority_ = {};
    track_async_up_piece_bytes_ = false;
    unforced_async_up_piece_bytes_left_ = 0U;
    enforce_unforced_async_up_budget_ = false;
    auto const trace_active = tr_logLevelIsActive(TR_LOG_TRACE);

    // allocateBandwidth () is a helper function with two purposes:
    // 1. allocate bandwidth to b and its subtree
    // 2. accumulate an array of all the peerIos from b and its subtree.
    this->allocateBandwidth(TR_PRI_LOW, period_msec, refs);

    for (auto const& io : refs)
    {
        io->flush_outgoing_protocol_msgs();
        incrementEnabledCount(peer_counts, io->priority());

        switch (io->priority())
        {
        case TR_PRI_FORCE:
            force_peers.push_back(io.get());
            pushToBuckets(download_peer_arrays, 1U, io.get());
            break;

        case TR_PRI_HIGH:
            pushToBuckets(unforced_upload_arrays, 0U, io.get());
            pushToBuckets(download_peer_arrays, 0U, io.get());
            break;

        case TR_PRI_NORMAL:
            pushToBuckets(unforced_upload_arrays, 1U, io.get());
            pushToBuckets(download_peer_arrays, 1U, io.get());
            break;

        default:
            pushToBuckets(unforced_upload_arrays, 2U, io.get());
            pushToBuckets(download_peer_arrays, 2U, io.get());
            break;
        }
    }

    auto const force_queue_before_up = queuedBytesStats(force_peers);
    auto const force_up_stats_1 = phaseOneForce(force_peers, TR_UP);
    auto const force_queue_after_up = trace_active ? queuedBytesStats(force_peers) : QueuedBytesStats{};

    for (size_t i = 0; i < std::size(download_peer_arrays); ++i)
    {
        regular_down_stats[i] = phaseOne(download_peer_arrays[i], TR_DOWN);
    }

    auto const force_queue_post_sync = trace_active ? queuedBytesStats(force_peers) : QueuedBytesStats{};

    auto force_recent_up_bps = uint64_t{};
    for (auto const* io : force_peers)
    {
        force_recent_up_bps += io->get_piece_speed_bytes_per_second(now, TR_UP);
    }

    // If FORCE already has buffered upload demand, reserve extra runway for it
    // without making the spillover on/off gate itself any stricter.
    auto const reserve_force_up_bps =
        force_queue_before_up.piece_bytes > 0U ? force_recent_up_bps * ForceUploadReserveWarmupMultiplier : force_recent_up_bps;

    auto const upload_target_bps = uint64_t{ this->getDesiredSpeedBytesPerSecond(TR_UP) };
    auto const allow_unforced_upload_spillover = !this->isLimited(TR_UP) || force_peers.empty() ||
        force_recent_up_bps * 100U < upload_target_bps * ForceUploadHistorySaturationPercent;
    auto force_history_reserve_bytes = size_t{};
    auto unforced_upload_spillover_budget = size_t{};

    if (allow_unforced_upload_spillover)
    {
        auto& up_bandwidth = this->band_[TR_UP];

        if (!this->isLimited(TR_UP) || force_peers.empty())
        {
            unforced_upload_spillover_budget = up_bandwidth.bytes_left_;
        }
        else
        {
            auto const recent_force_up_pulse_bytes =
                size_t{ reserve_force_up_bps * uint64_t{ period_msec } / 1000U };
            force_history_reserve_bytes = std::min(
                up_bandwidth.bytes_left_,
                size_t{ recent_force_up_pulse_bytes * ForceUploadHistoryReservePercent / 100U });

            if (up_bandwidth.bytes_left_ > force_history_reserve_bytes)
            {
                unforced_upload_spillover_budget = up_bandwidth.bytes_left_ - force_history_reserve_bytes;
            }
        }

        if (unforced_upload_spillover_budget > 0U)
        {
            auto const reserved_bytes = up_bandwidth.bytes_left_ - unforced_upload_spillover_budget;
            up_bandwidth.bytes_left_ = unforced_upload_spillover_budget;

            for (size_t i = 0; i < std::size(unforced_upload_arrays); ++i)
            {
                regular_unforced_up_stats[i] = phaseOne(unforced_upload_arrays[i], TR_UP);
            }

            up_bandwidth.bytes_left_ += reserved_bytes;
        }
    }

    auto unforced_async_spillover_budget = size_t{};
    if (allow_unforced_upload_spillover)
    {
        auto const up_bytes_left = this->band_[TR_UP].bytes_left_;
        unforced_async_spillover_budget = up_bytes_left * ForceUploadAsyncSpilloverPercent / 100U;
    }

    // Second phase of IO. To help us scale in high bandwidth situations,
    // enable on-demand IO for peers with bandwidth left to burn.
    // This on-demand IO is enabled until (1) the peer runs out of bandwidth,
    // or (2) the next tr_bandwidth::allocate () call, when we start over again.
    auto up_eligible = EnabledCounts{};
    auto up_enabled = EnabledCounts{};
    auto down_enabled = EnabledCounts{};
    auto const any_force_up_bandwidth_left = std::any_of(
        std::begin(refs),
        std::end(refs),
        [](auto const& io) { return io->priority() == TR_PRI_FORCE && io->has_bandwidth_left(TR_UP); });
    auto const enforce_unforced_async_up_budget = any_force_up_bandwidth_left && unforced_async_spillover_budget > 0U;
    unforced_async_up_piece_bytes_left_ = enforce_unforced_async_up_budget ? unforced_async_spillover_budget : 0U;
    enforce_unforced_async_up_budget_ = enforce_unforced_async_up_budget;

    for (auto const& io : refs)
    {
        auto const has_up_bandwidth_left = io->has_bandwidth_left(TR_UP);
        auto const has_down_bandwidth_left = io->has_bandwidth_left(TR_DOWN);
        auto const enable_up = has_up_bandwidth_left &&
            (!any_force_up_bandwidth_left || io->priority() == TR_PRI_FORCE ||
             (enforce_unforced_async_up_budget && io->priority() != TR_PRI_FORCE));
        io->set_enabled(TR_UP, enable_up);
        io->set_enabled(TR_DOWN, has_down_bandwidth_left);

        if (has_up_bandwidth_left)
        {
            incrementEnabledCount(up_eligible, io->priority());
        }

        if (enable_up)
        {
            incrementEnabledCount(up_enabled, io->priority());
        }

        if (has_down_bandwidth_left)
        {
            incrementEnabledCount(down_enabled, io->priority());
        }
    }

    track_async_up_piece_bytes_ = true;

    if (trace_active)
    {
        auto const unforced_bytes = [](PhaseStats const& stats) noexcept
        {
            return priorityBytes(stats.bytes_used_by_priority, TR_PRI_HIGH) +
                priorityBytes(stats.bytes_used_by_priority, TR_PRI_NORMAL) +
                priorityBytes(stats.bytes_used_by_priority, TR_PRI_LOW);
        };

        auto const previous_async_up_total = priorityBytes(previous_async_up_piece_bytes_by_priority, TR_PRI_FORCE) +
            priorityBytes(previous_async_up_piece_bytes_by_priority, TR_PRI_HIGH) +
            priorityBytes(previous_async_up_piece_bytes_by_priority, TR_PRI_NORMAL) +
            priorityBytes(previous_async_up_piece_bytes_by_priority, TR_PRI_LOW);
        auto regular_unforced_up_bytes_by_priority = std::array<size_t, 4>{};
        auto regular_down_bytes_by_priority = std::array<size_t, 4>{};
        for (size_t i = 0; i < std::size(download_peer_arrays); ++i)
        {
            for (size_t j = 0; j < std::size(regular_unforced_up_bytes_by_priority); ++j)
            {
                regular_unforced_up_bytes_by_priority[j] += regular_unforced_up_stats[i].bytes_used_by_priority[j];
                regular_down_bytes_by_priority[j] += regular_down_stats[i].bytes_used_by_priority[j];
            }
        }

        tr_logAddTrace(fmt::format(
            "bandwidth pulse peers by priority: force={} high={} normal={} low={} | bucket sizes: force={} upload unforced high={} normal={} low={} download high={} normal={} low={}",
            peer_counts.force,
            peer_counts.high,
            peer_counts.normal,
            peer_counts.low,
            force_peers.size(),
            unforced_upload_high.size(),
            unforced_upload_normal.size(),
            unforced_upload_low.size(),
            download_high.size(),
            download_normal.size(),
            download_low.size()));

        tr_logAddTrace(fmt::format(
            "previous async upload: total={} force={} high={} normal={} low={}",
            previous_async_up_total,
            priorityBytes(previous_async_up_piece_bytes_by_priority, TR_PRI_FORCE),
            priorityBytes(previous_async_up_piece_bytes_by_priority, TR_PRI_HIGH),
            priorityBytes(previous_async_up_piece_bytes_by_priority, TR_PRI_NORMAL),
            priorityBytes(previous_async_up_piece_bytes_by_priority, TR_PRI_LOW)));

        tr_logAddTrace(fmt::format(
            "upload spillover gate: allow_unforced={} limited={} recent force up bps={} reserve force up bps={} target up bps={} saturation pct={} reserve pct={} reserve bytes={} sync spillover budget={} async spillover pct={} async spillover budget={}",
            allow_unforced_upload_spillover,
            this->isLimited(TR_UP),
            force_recent_up_bps,
            reserve_force_up_bps,
            upload_target_bps,
            ForceUploadHistorySaturationPercent,
            ForceUploadHistoryReservePercent,
            force_history_reserve_bytes,
            unforced_upload_spillover_budget,
            ForceUploadAsyncSpilloverPercent,
            unforced_async_spillover_budget));

        tr_logAddTrace(fmt::format(
            "force-only upload passes: up seen={} flushes={} bytes={} progressed={} stalled={}",
            force_up_stats_1.peer_count,
            force_up_stats_1.peers_processed,
            force_up_stats_1.bytes_used,
            force_up_stats_1.peers_made_progress,
            force_up_stats_1.peers_stalled));

        tr_logAddTrace(fmt::format(
            "force queued outbound: before up piece={} protocol={} peers_with_piece={} | after up piece={} protocol={} peers_with_piece={} | post-sync piece={} protocol={} peers_with_piece={}",
            force_queue_before_up.piece_bytes,
            force_queue_before_up.protocol_bytes,
            force_queue_before_up.peers_with_piece,
            force_queue_after_up.piece_bytes,
            force_queue_after_up.protocol_bytes,
            force_queue_after_up.peers_with_piece,
            force_queue_post_sync.piece_bytes,
            force_queue_post_sync.protocol_bytes,
            force_queue_post_sync.peers_with_piece));

        for (size_t i = 0; i < std::size(download_peer_arrays); ++i)
        {
            tr_logAddTrace(fmt::format(
                "regular {} passes: up unforced seen={} flushes={} bytes={} progressed={} stalled={} | down seen={} flushes={} bytes={} force={} unforced={} progressed={} stalled={}",
                priorityName(i),
                regular_unforced_up_stats[i].peer_count,
                regular_unforced_up_stats[i].peers_processed,
                regular_unforced_up_stats[i].bytes_used,
                regular_unforced_up_stats[i].peers_made_progress,
                regular_unforced_up_stats[i].peers_stalled,
                regular_down_stats[i].peer_count,
                regular_down_stats[i].peers_processed,
                regular_down_stats[i].bytes_used,
                priorityBytes(regular_down_stats[i].bytes_used_by_priority, TR_PRI_FORCE),
                unforced_bytes(regular_down_stats[i]),
                regular_down_stats[i].peers_made_progress,
                regular_down_stats[i].peers_stalled));
        }

        tr_logAddTrace(fmt::format(
            "regular all passes: up unforced total={} high={} normal={} low={} | down total={} force={} high={} normal={} low={}",
            regular_unforced_up_stats[0].bytes_used + regular_unforced_up_stats[1].bytes_used +
                regular_unforced_up_stats[2].bytes_used,
            priorityBytes(regular_unforced_up_bytes_by_priority, TR_PRI_HIGH),
            priorityBytes(regular_unforced_up_bytes_by_priority, TR_PRI_NORMAL),
            priorityBytes(regular_unforced_up_bytes_by_priority, TR_PRI_LOW),
            regular_down_stats[0].bytes_used + regular_down_stats[1].bytes_used + regular_down_stats[2].bytes_used,
            priorityBytes(regular_down_bytes_by_priority, TR_PRI_FORCE),
            priorityBytes(regular_down_bytes_by_priority, TR_PRI_HIGH),
            priorityBytes(regular_down_bytes_by_priority, TR_PRI_NORMAL),
            priorityBytes(regular_down_bytes_by_priority, TR_PRI_LOW)));

        tr_logAddTrace(fmt::format(
            "post-sync bandwidth left: up={} down={} | eligible up: force={} high={} normal={} low={} | enabled up: force={} high={} normal={} low={} | enabled down: force={} high={} normal={} low={}",
            this->band_[TR_UP].bytes_left_,
            this->band_[TR_DOWN].bytes_left_,
            up_eligible.force,
            up_eligible.high,
            up_eligible.normal,
            up_eligible.low,
            up_enabled.force,
            up_enabled.high,
            up_enabled.normal,
            up_enabled.low,
            down_enabled.force,
            down_enabled.high,
            down_enabled.normal,
            down_enabled.low));
    }
}

// ---

size_t tr_bandwidth::clamp(tr_direction const dir, size_t byte_count) const noexcept
{
    TR_ASSERT(tr_isDirection(dir));

    if (this->band_[dir].is_limited_)
    {
        byte_count = std::min(byte_count, this->band_[dir].bytes_left_);
    }

    if (this->parent_ != nullptr && this->band_[dir].honor_parent_limits_ && byte_count > 0)
    {
        byte_count = this->parent_->clamp(dir, byte_count);
    }

    return byte_count;
}

size_t tr_bandwidth::clampAsyncUploadPieceBytes(size_t byte_count, tr_priority_t peer_priority) const noexcept
{
    if (peer_priority == TR_PRI_FORCE || byte_count == 0U)
    {
        return byte_count;
    }

    if (this->parent_ == nullptr && track_async_up_piece_bytes_ && enforce_unforced_async_up_budget_)
    {
        byte_count = std::min(byte_count, unforced_async_up_piece_bytes_left_);
    }

    if (this->parent_ != nullptr && this->band_[TR_UP].honor_parent_limits_ && byte_count > 0U)
    {
        byte_count = this->parent_->clampAsyncUploadPieceBytes(byte_count, peer_priority);
    }

    return byte_count;
}

void tr_bandwidth::notifyBandwidthConsumed(tr_direction dir, size_t byte_count, bool is_piece_data, uint64_t now)
{
    auto peer_priority = this->priority_;
    if (auto const shared = this->peer_.lock(); shared)
    {
        peer_priority = shared->priority();
    }

    notifyBandwidthConsumed(dir, byte_count, is_piece_data, now, peer_priority);
}

void tr_bandwidth::notifyBandwidthConsumed(
    tr_direction dir,
    size_t byte_count,
    bool is_piece_data,
    uint64_t now,
    tr_priority_t peer_priority)
{
    TR_ASSERT(tr_isDirection(dir));

    Band* band = &this->band_[dir];

    if (band->is_limited_ && is_piece_data)
    {
        band->bytes_left_ -= std::min(size_t{ band->bytes_left_ }, byte_count);
    }

#ifdef DEBUG_DIRECTION

    if (dir == DEBUG_DIRECTION && band_->isLimited)
    {
        fprintf(
            stderr,
            "%p consumed %5zu bytes of %5s data... was %6zu, now %6zu left\n",
            this,
            byte_count,
            is_piece_data ? "piece" : "raw",
            oldBytesLeft,
            band_->bytesLeft);
    }

#endif

    notifyBandwidthConsumedBytes(now, &band->raw_, byte_count);

    if (is_piece_data)
    {
        notifyBandwidthConsumedBytes(now, &band->piece_, byte_count);

        if (dir == TR_UP && this->parent_ == nullptr && track_async_up_piece_bytes_)
        {
            async_up_piece_bytes_by_priority_[priorityIndex(peer_priority)] += byte_count;
            if (enforce_unforced_async_up_budget_ && peer_priority != TR_PRI_FORCE)
            {
                unforced_async_up_piece_bytes_left_ -= std::min(unforced_async_up_piece_bytes_left_, byte_count);
            }
        }
    }

    if (this->parent_ != nullptr)
    {
        this->parent_->notifyBandwidthConsumed(dir, byte_count, is_piece_data, now, peer_priority);
    }
}

// ---

tr_bandwidth_limits tr_bandwidth::getLimits() const
{
    tr_bandwidth_limits limits;
    limits.up_limit_KBps = tr_toSpeedKBps(this->getDesiredSpeedBytesPerSecond(TR_UP));
    limits.down_limit_KBps = tr_toSpeedKBps(this->getDesiredSpeedBytesPerSecond(TR_DOWN));
    limits.up_limited = this->isLimited(TR_UP);
    limits.down_limited = this->isLimited(TR_DOWN);
    return limits;
}

void tr_bandwidth::setLimits(tr_bandwidth_limits const* limits)
{
    this->setDesiredSpeedBytesPerSecond(TR_UP, tr_toSpeedBytes(limits->up_limit_KBps));
    this->setDesiredSpeedBytesPerSecond(TR_DOWN, tr_toSpeedBytes(limits->down_limit_KBps));
    this->setLimited(TR_UP, limits->up_limited);
    this->setLimited(TR_DOWN, limits->down_limited);
}
