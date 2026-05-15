// This file Copyright © 2008-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <limits>
#include <utility> // for std::swap()
#include <vector>

#include <fmt/core.h>

#include "transmission.h"

#include "bandwidth.h"
#include "crypto-utils.h"
#include "log.h"
#include "peer-io.h"
#include "session.h"
#include "tr-assert.h"
#include "utils.h" // tr_time_msec()

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
// Value of 3000 bytes chosen so that when using µTP we'll send a full-size
// frame right away and leave enough buffered data for the next frame to go
// out in a timely manner.
auto constexpr PhaseOneIncrement = size_t{ 3000 };
auto constexpr LateAsyncBorrowPercent = size_t{ 20U };
auto constexpr ForceBootstrapReservePercent = size_t{ 5U };

[[nodiscard]] size_t saturatingAdd(size_t lhs, size_t rhs) noexcept
{
    auto constexpr MaxSize = std::numeric_limits<size_t>::max();

    return lhs > MaxSize - rhs ? MaxSize : lhs + rhs;
}

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

void tr_bandwidth::appendPeers(std::vector<std::shared_ptr<tr_peerIo>>& peer_pool) const
{
    if (auto shared = this->peer_.lock(); shared)
    {
        peer_pool.push_back(std::move(shared));
    }

    for (auto const* child : this->children_)
    {
        child->appendPeers(peer_pool);
    }
}

void tr_bandwidth::phaseOne(std::vector<tr_peerIo*>& peers, tr_direction dir)
{
    // First phase of IO. Tries to distribute bandwidth fairly to keep faster
    // peers from starving the others.
    tr_logAddTrace(fmt::format("{} peers to go round-robin for {}", peers.size(), dir == TR_UP ? "upload" : "download"));

    // Shuffle the peers so they all have equal chance to be first in line.
    thread_local auto urbg = tr_urbg<size_t>{};
    std::shuffle(std::begin(peers), std::end(peers), urbg);

    // Give each peer `Increment` bandwidth bytes to use. Repeat this
    // process until we run out of bandwidth and/or peers that can use it.
    for (size_t n_unfinished = std::size(peers); n_unfinished > 0U;)
    {
        for (size_t i = 0; i < n_unfinished;)
        {
            auto const bytes_used = peers[i]->flush(dir, PhaseOneIncrement);
            tr_logAddTrace(fmt::format("peer #{} of {} used {} bytes in this pass", i, n_unfinished, bytes_used));

            if (bytes_used != PhaseOneIncrement)
            {
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
}

void tr_bandwidth::phaseOneForce(std::vector<tr_peerIo*>& peers, tr_direction dir)
{
    // Give FORCE peers first crack at the sync upload pulse, and keep them in
    // the force-only loop as long as they are still making forward progress.
    tr_logAddTrace(fmt::format("{} force peers to go round-robin for {}", peers.size(), dir == TR_UP ? "upload" : "download"));

    thread_local auto urbg = tr_urbg<size_t>{};
    std::shuffle(std::begin(peers), std::end(peers), urbg);

    for (size_t n_unfinished = std::size(peers); n_unfinished > 0U;)
    {
        auto const bytes_used = peers[0]->flush(dir, PhaseOneIncrement);
        tr_logAddTrace(fmt::format("force peer of {} used {} bytes in this pass", n_unfinished, bytes_used));

        if (bytes_used == 0U)
        {
            std::swap(peers[0], peers[n_unfinished - 1]);
            --n_unfinished;
        }
        else if (n_unfinished > 1U)
        {
            std::rotate(std::begin(peers), std::next(std::begin(peers)), std::next(std::begin(peers), n_unfinished));
        }
    }
}

void tr_bandwidth::allocate(unsigned int period_msec)
{
    static auto constexpr AsyncUploadSpilloverPercent = size_t{ 100U };
    static auto constexpr ForceUploadPressureReserveExtraDivisor = size_t{ 2U };

    // keep these peers alive for the scope of this function
    auto refs = std::vector<std::shared_ptr<tr_peerIo>>{};

    auto force = std::vector<tr_peerIo*>{};
    auto unforced_upload_arrays = std::array<std::vector<tr_peerIo*>, 3>{};
    auto download_peer_arrays = std::array<std::vector<tr_peerIo*>, 3>{};
    auto& unforced_upload_high = unforced_upload_arrays[0];
    auto& unforced_upload_normal = unforced_upload_arrays[1];
    auto& unforced_upload_low = unforced_upload_arrays[2];
    auto& download_high = download_peer_arrays[0];
    auto& download_normal = download_peer_arrays[1];
    auto& download_low = download_peer_arrays[2];
    auto const now = tr_time_msec();

    current_pulse_deadline_msec_ = now + period_msec;
    current_pulse_upload_limit_bytes_ = 0U;
    late_nonforce_async_borrow_opened_ = false;
    tail_nonforce_async_floodgate_opened_ = false;
    tail_nonforce_async_floodgate_budget_granted_ = 0U;

    // allocateBandwidth () is a helper function with two purposes:
    // 1. allocate bandwidth to b and its subtree
    // 2. accumulate an array of all the peerIos from b and its subtree.
    clearAsyncUploadPieceSpilloverBudget();
    this->allocateBandwidth(TR_PRI_LOW, period_msec, refs);
    current_pulse_upload_limit_bytes_ = this->isLimited(TR_UP) ? this->band_[TR_UP].bytes_left_ : 0U;

    for (auto const& io : refs)
    {
        io->flush_outgoing_protocol_msgs();

        switch (io->priority())
        {
        case TR_PRI_FORCE:
            force.push_back(io.get());
            download_normal.push_back(io.get());
            download_low.push_back(io.get());
            break;

        case TR_PRI_HIGH:
            unforced_upload_high.push_back(io.get());
            unforced_upload_normal.push_back(io.get());
            unforced_upload_low.push_back(io.get());
            download_high.push_back(io.get());
            download_normal.push_back(io.get());
            download_low.push_back(io.get());
            break;

        case TR_PRI_NORMAL:
            unforced_upload_normal.push_back(io.get());
            unforced_upload_low.push_back(io.get());
            download_normal.push_back(io.get());
            download_low.push_back(io.get());
            break;

        default:
            unforced_upload_low.push_back(io.get());
            download_low.push_back(io.get());
        }
    }

    auto force_recent_up_bps = uint64_t{};
    for (auto const* io : force)
    {
        force_recent_up_bps += io->get_piece_speed_bytes_per_second(now, TR_UP);
    }

    // Give FORCE uploads a dedicated first drain so already-queued FORCE
    // demand gets as much of the sync upload phase as the current socket and
    // bandwidth state allow before mixed-priority upload scheduling begins.
    phaseOneForce(force, TR_UP);
    phaseOne(force, TR_DOWN);

    for (auto* peers : { &download_high, &download_normal, &download_low })
    {
        phaseOne(*peers, TR_DOWN);
    }

    auto queued_force_piece_bytes = size_t{};
    for (auto const* io : force)
    {
        queued_force_piece_bytes += io->queued_outgoing_bytes().piece_bytes;
    }

    auto const force_recent_up_pulse_bytes = size_t{ force_recent_up_bps * uint64_t{ period_msec } / 1000U };
    auto const force_upload_pressure_bytes = std::max(queued_force_piece_bytes, force_recent_up_pulse_bytes);
    auto const optimistic_force_upload_pressure_bytes =
        saturatingAdd(force_upload_pressure_bytes, force_upload_pressure_bytes / ForceUploadPressureReserveExtraDivisor);
    auto const bootstrap_force_upload_bytes =
        !std::empty(force) ? current_pulse_upload_limit_bytes_ * ForceBootstrapReservePercent / 100U : 0U;
    auto const effective_force_upload_target_bytes =
        std::max(optimistic_force_upload_pressure_bytes, bootstrap_force_upload_bytes);

    auto reserved_force_upload_bytes = size_t{};
    if (this->isLimited(TR_UP) && effective_force_upload_target_bytes > 0U)
    {
        // Keep some aspirational runway for small FORCE swarms so they can
        // grow into the pulse instead of only getting what current queue
        // depth or recent history already proved.
        reserved_force_upload_bytes = std::min(this->band_[TR_UP].bytes_left_, effective_force_upload_target_bytes);
        this->band_[TR_UP].bytes_left_ -= reserved_force_upload_bytes;
    }

    for (auto* peers : { &unforced_upload_high, &unforced_upload_normal, &unforced_upload_low })
    {
        phaseOne(*peers, TR_UP);
    }

    if (reserved_force_upload_bytes > 0U)
    {
        this->band_[TR_UP].bytes_left_ += reserved_force_upload_bytes;
    }

    if (this->isLimited(TR_UP) && effective_force_upload_target_bytes > 0U)
    {
        auto const unforced_async_upload_spillover_budget =
            this->band_[TR_UP].bytes_left_ > effective_force_upload_target_bytes ?
                this->band_[TR_UP].bytes_left_ - effective_force_upload_target_bytes :
                0U;
        setAsyncUploadPieceSpilloverBudget(
            unforced_async_upload_spillover_budget * AsyncUploadSpilloverPercent / 100U);
    }

    // Second phase of IO. To help us scale in high bandwidth situations,
    // enable on-demand IO for peers with bandwidth left to burn.
    // This on-demand IO is enabled until (1) the peer runs out of bandwidth,
    // or (2) the next tr_bandwidth::allocate () call, when we start over again.
    for (auto const& io : refs)
    {
        auto const queued = io->queued_outgoing_bytes();
        auto const has_queued_upload =
            queued.piece_bytes > 0U || queued.protocol_bytes > 0U;

        io->set_enabled(TR_UP, has_queued_upload && io->has_bandwidth_left(TR_UP));
        io->set_enabled(TR_DOWN, io->has_bandwidth_left(TR_DOWN));
    }

    maybeOpenLateNonForceAsyncBorrow();
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

    if (this->parent_ == nullptr && enforce_async_upload_piece_spillover_budget_)
    {
        byte_count = std::min(byte_count, async_upload_piece_spillover_budget_left_);
    }

    if (this->parent_ != nullptr && this->band_[TR_UP].honor_parent_limits_ && byte_count > 0U)
    {
        byte_count = this->parent_->clampAsyncUploadPieceBytes(byte_count, peer_priority);
    }

    return byte_count;
}

void tr_bandwidth::maybeOpenLateNonForceAsyncBorrow() noexcept
{
    if (this->parent_ != nullptr)
    {
        if (this->band_[TR_UP].honor_parent_limits_)
        {
            this->parent_->maybeOpenLateNonForceAsyncBorrow();
        }

        return;
    }

    if (!enforce_async_upload_piece_spillover_budget_ || late_nonforce_async_borrow_opened_ ||
        tail_nonforce_async_floodgate_opened_ || this->band_[TR_UP].bytes_left_ == 0U || current_pulse_upload_limit_bytes_ == 0U)
    {
        return;
    }

    auto peers = std::vector<std::shared_ptr<tr_peerIo>>{};
    appendPeers(peers);

    auto nonforce_piece_peers = std::vector<tr_peerIo*>{};
    auto any_force_write_polling = false;

    for (auto const& io : peers)
    {
        if (io->priority() == TR_PRI_FORCE)
        {
            any_force_write_polling |= io->is_write_polling_enabled();
            continue;
        }

        if (io->queued_outgoing_bytes().piece_bytes > 0U)
        {
            nonforce_piece_peers.push_back(io.get());
        }
    }

    if (any_force_write_polling || std::empty(nonforce_piece_peers))
    {
        return;
    }

    auto const late_borrow_budget = std::min(
        this->band_[TR_UP].bytes_left_,
        current_pulse_upload_limit_bytes_ * LateAsyncBorrowPercent / 100U);

    if (late_borrow_budget == 0U)
    {
        return;
    }

    setAsyncUploadPieceSpilloverBudget(late_borrow_budget);
    late_nonforce_async_borrow_opened_ = true;

    for (auto* io : nonforce_piece_peers)
    {
        if (io->has_bandwidth_left(TR_UP))
        {
            io->set_enabled(TR_UP, true);
        }
    }
}

void tr_bandwidth::maybeOpenTailNonForceAsyncFloodgate(size_t cumulative_percent) noexcept
{
    if (this->parent_ != nullptr)
    {
        if (this->band_[TR_UP].honor_parent_limits_)
        {
            this->parent_->maybeOpenTailNonForceAsyncFloodgate(cumulative_percent);
        }

        return;
    }

    if (!enforce_async_upload_piece_spillover_budget_ || late_nonforce_async_borrow_opened_ ||
        this->band_[TR_UP].bytes_left_ == 0U || current_pulse_upload_limit_bytes_ == 0U)
    {
        return;
    }

    auto const target_total_budget =
        current_pulse_upload_limit_bytes_ * std::min(cumulative_percent, size_t{ 100U }) / 100U;
    if (target_total_budget == 0U || tail_nonforce_async_floodgate_budget_granted_ >= target_total_budget)
    {
        return;
    }

    auto peers = std::vector<std::shared_ptr<tr_peerIo>>{};
    appendPeers(peers);

    auto nonforce_piece_peers = std::vector<tr_peerIo*>{};
    for (auto const& io : peers)
    {
        if (io->priority() != TR_PRI_FORCE && io->queued_outgoing_bytes().piece_bytes > 0U)
        {
            nonforce_piece_peers.push_back(io.get());
        }
    }

    if (std::empty(nonforce_piece_peers))
    {
        return;
    }

    auto const current_spillover_budget_left = async_upload_piece_spillover_budget_left_;
    auto const additional_budget = target_total_budget - tail_nonforce_async_floodgate_budget_granted_;
    auto const tail_floodgate_budget =
        std::min(this->band_[TR_UP].bytes_left_, current_spillover_budget_left + additional_budget);
    if (tail_floodgate_budget <= current_spillover_budget_left)
    {
        return;
    }

    late_nonforce_async_borrow_opened_ = false;
    tail_nonforce_async_floodgate_opened_ = true;
    tail_nonforce_async_floodgate_budget_granted_ += tail_floodgate_budget - current_spillover_budget_left;
    setAsyncUploadPieceSpilloverBudget(tail_floodgate_budget);

    for (auto* io : nonforce_piece_peers)
    {
        if (io->has_bandwidth_left(TR_UP))
        {
            io->set_enabled(TR_UP, true);
        }
    }
}

void tr_bandwidth::revokeLateNonForceAsyncBorrow(tr_priority_t peer_priority) noexcept
{
    if (peer_priority != TR_PRI_FORCE)
    {
        return;
    }

    if (this->parent_ != nullptr)
    {
        if (this->band_[TR_UP].honor_parent_limits_)
        {
            this->parent_->revokeLateNonForceAsyncBorrow(peer_priority);
        }

        return;
    }

    if (!late_nonforce_async_borrow_opened_ || !enforce_async_upload_piece_spillover_budget_)
    {
        return;
    }

    late_nonforce_async_borrow_opened_ = false;
    clearAsyncUploadPieceSpilloverBudget();

    auto peers = std::vector<std::shared_ptr<tr_peerIo>>{};
    appendPeers(peers);

    for (auto const& io : peers)
    {
        if (io->priority() != TR_PRI_FORCE)
        {
            io->set_enabled(TR_UP, false);
        }
    }
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

        if (dir == TR_UP && this->parent_ == nullptr && enforce_async_upload_piece_spillover_budget_ &&
            peer_priority != TR_PRI_FORCE)
        {
            async_upload_piece_spillover_budget_left_ -=
                std::min(async_upload_piece_spillover_budget_left_, byte_count);
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
