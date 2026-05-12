// This file Copyright © 2008-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <numeric>
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
    tr_torrent_priority_t parent_priority,
    unsigned int period_msec,
    std::vector<std::shared_ptr<tr_peerIo>>& peer_pool)
{
    auto const priority = std::max(parent_priority, this->priority_);

    // set the available bandwidth
    for (auto const dir : { TR_UP, TR_DOWN })
    {
        auto& bandwidth = band_[dir];
        if (bandwidth.is_limited_)
        {
            auto const next_pulse_speed = bandwidth.desired_speed_bps_;
            bandwidth.bytes_left_ = next_pulse_speed * period_msec / 1000U;
        }

        bandwidth.non_force_bytes_left_ = 0U;
        bandwidth.pulse_budget_ = bandwidth.bytes_left_;
        bandwidth.reserved_force_bytes_ = 0U;
        bandwidth.force_phase_one_bytes_ = 0U;
        bandwidth.force_piece_bytes_used_ = 0U;
        bandwidth.non_force_piece_bytes_used_ = 0U;
        bandwidth.force_peer_count_ = 0U;
        bandwidth.other_peer_count_ = 0U;
        bandwidth.is_non_force_limited_ = false;
        bandwidth.force_has_more_demand_ = false;
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

size_t tr_bandwidth::phaseOne(std::vector<tr_peerIo*>& peers, tr_direction dir)
{
    // First phase of IO. Tries to distribute bandwidth fairly to keep faster
    // peers from starving the others.
    tr_logAddTrace(fmt::format("{} peers to go round-robin for {}", peers.size(), dir == TR_UP ? "upload" : "download"));

    // Shuffle the peers so they all have equal chance to be first in line.
    thread_local auto urbg = tr_urbg<size_t>{};
    std::shuffle(std::begin(peers), std::end(peers), urbg);

    // Give each peer `Increment` bandwidth bytes to use. Repeat this
    // process until we run out of bandwidth and/or peers that can use it.
    auto total_bytes_used = size_t{};
    for (size_t n_unfinished = std::size(peers); n_unfinished > 0U;)
    {
        for (size_t i = 0; i < n_unfinished;)
        {
            // Value of 3000 bytes chosen so that when using µTP we'll send a full-size
            // frame right away and leave enough buffered data for the next frame to go
            // out in a timely manner.
            static auto constexpr Increment = size_t{ 3000 };

            auto const bytes_used = peers[i]->flush(dir, Increment);
            total_bytes_used += bytes_used;
            tr_logAddTrace(fmt::format("peer #{} of {} used {} bytes in this pass", i, n_unfinished, bytes_used));

            if (bytes_used != Increment)
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

    return total_bytes_used;
}

void tr_bandwidth::allocate(unsigned int period_msec)
{
    auto log_previous_force_pulse = [&](tr_direction dir)
    {
        auto const& bandwidth = band_[dir];
        if (!bandwidth.is_limited_ || bandwidth.pulse_budget_ == 0U)
        {
            return;
        }

        if (bandwidth.force_peer_count_ == 0U && bandwidth.reserved_force_bytes_ == 0U)
        {
            return;
        }

        auto const reserved_unused = bandwidth.reserved_force_bytes_ > bandwidth.force_piece_bytes_used_ ?
            bandwidth.reserved_force_bytes_ - bandwidth.force_piece_bytes_used_ :
            0U;

        tr_logAddTrace(fmt::format(
            "force bandwidth pulse {}: budget={} reserved={} force_phase1={} force_used={} other_used={} unused={} "
            "reserved_unused={} next_floor={} force_peers={} other_peers={} force_has_more_demand={}",
            dir == TR_UP ? "up" : "down",
            bandwidth.pulse_budget_,
            bandwidth.reserved_force_bytes_,
            bandwidth.force_phase_one_bytes_,
            bandwidth.force_piece_bytes_used_,
            bandwidth.non_force_piece_bytes_used_,
            bandwidth.bytes_left_,
            reserved_unused,
            bandwidth.force_reservation_floor_,
            bandwidth.force_peer_count_,
            bandwidth.other_peer_count_,
            bandwidth.force_has_more_demand_));
    };

    log_previous_force_pulse(TR_UP);
    log_previous_force_pulse(TR_DOWN);

    // keep these peers alive for the scope of this function
    auto refs = std::vector<std::shared_ptr<tr_peerIo>>{};

    auto force_upload_peers = std::vector<tr_peerIo*>{};
    auto other_upload_peer_arrays = std::array<std::vector<tr_peerIo*>, 3>{};
    auto force_download_peers = std::vector<tr_peerIo*>{};
    auto other_download_peer_arrays = std::array<std::vector<tr_peerIo*>, 3>{};

    auto add_other_upload_peer = [](std::array<std::vector<tr_peerIo*>, 3>& peer_arrays, tr_torrent_priority_t priority, tr_peerIo* io)
    {
        switch (priority)
        {
        case TR_TOR_PRI_HIGH:
            peer_arrays[0].push_back(io);
            [[fallthrough]];

        case TR_TOR_PRI_NORMAL:
            peer_arrays[1].push_back(io);
            [[fallthrough]];

        default:
            peer_arrays[2].push_back(io);
        }
    };

    auto add_other_download_peer = [](std::array<std::vector<tr_peerIo*>, 3>& peer_arrays, tr_torrent_priority_t priority, tr_peerIo* io)
    {
        switch (priority)
        {
        case TR_TOR_PRI_HIGH:
            peer_arrays[0].push_back(io);
            [[fallthrough]];

        case TR_TOR_PRI_NORMAL:
            peer_arrays[1].push_back(io);
            [[fallthrough]];

        default:
            peer_arrays[2].push_back(io);
        }
    };

    auto run_force_first_pass = [&](std::vector<tr_peerIo*>& force_peers,
                                    auto& other_peer_arrays,
                                    tr_direction dir,
                                    size_t pulse_budget)
    {
        auto const force_bytes = phaseOne(force_peers, dir);

        auto& bandwidth = band_[dir];
        auto& recent_force_bytes = recent_force_bytes_[dir];
        auto const expected_force_bytes = std::min(*std::max_element(std::begin(recent_force_bytes), std::end(recent_force_bytes)), pulse_budget);
        auto const observed_force_bytes = std::max(expected_force_bytes, force_bytes);
        auto const probe_budget = pulse_budget > expected_force_bytes ? pulse_budget - expected_force_bytes : 0U;
        auto const active_probe_bytes = std::min(bandwidth.force_probe_bytes_, probe_budget);
        auto const reserved_force_bytes = expected_force_bytes + active_probe_bytes;

        std::move_backward(std::begin(recent_force_bytes), std::end(recent_force_bytes) - 1, std::end(recent_force_bytes));
        recent_force_bytes[0] = force_bytes;

        bandwidth.force_phase_one_bytes_ = force_bytes;
        bandwidth.force_peer_count_ = std::size(force_peers);
        bandwidth.other_peer_count_ = std::accumulate(
            std::begin(other_peer_arrays),
            std::end(other_peer_arrays),
            size_t{ 0U },
            [](size_t n, auto const& peers) { return n + std::size(peers); });
        bandwidth.force_has_more_demand_ = std::any_of(
            std::begin(force_peers),
            std::end(force_peers),
            [dir](auto const* io)
            {
                return dir == TR_UP ?
                    ((io->has_pending_piece_requests() || io->has_pending_piece_data()) && io->has_bandwidth_left(TR_UP)) :
                    (io->has_pending_download_requests() && io->has_bandwidth_left(TR_DOWN));
            });

        auto const probe_hold_active = bandwidth.force_probe_hold_pulses_ > 0U;

        if (bandwidth.force_has_more_demand_)
        {
            bandwidth.force_probe_hold_pulses_ = ForceProbeHoldPulses;
        }
        else if (probe_hold_active)
        {
            --bandwidth.force_probe_hold_pulses_;
        }

        auto const current_reserved_force_bytes = std::max(
            observed_force_bytes,
            bandwidth.force_has_more_demand_ || probe_hold_active ? reserved_force_bytes : expected_force_bytes);

        bandwidth.force_reservation_floor_ = std::empty(force_peers) ? 0U : observed_force_bytes;

        if (bandwidth.force_has_more_demand_ && pulse_budget > 0U)
        {
            auto const step = std::max(size_t{ 1U }, pulse_budget / ForceRampStepDivisor);
            auto const growth_step = std::max(size_t{ 1U }, step / 2U);
            auto const used_probe_bytes =
                force_bytes > expected_force_bytes ? std::min(force_bytes - expected_force_bytes, active_probe_bytes) : 0U;
            auto const next_probe_step =
                active_probe_bytes == 0U || used_probe_bytes * 2U >= active_probe_bytes ? step : growth_step;
            auto const next_probe_bytes = std::min(probe_budget, active_probe_bytes + next_probe_step);

            bandwidth.force_probe_bytes_ = next_probe_bytes;
        }
        else if ((probe_hold_active || force_bytes > 0U) && pulse_budget > 0U)
        {
            auto const step = std::max(size_t{ 1U }, pulse_budget / ForceRampStepDivisor);
            auto const decay_step = std::max(size_t{ 1U }, step / 4U);
            bandwidth.force_probe_bytes_ = active_probe_bytes > decay_step ? active_probe_bytes - decay_step : 0U;
        }
        else
        {
            bandwidth.force_probe_bytes_ = 0U;
            bandwidth.force_probe_hold_pulses_ = 0U;
        }

        if (bandwidth.is_limited_ && !std::empty(force_peers))
        {
            bandwidth.reserved_force_bytes_ = std::min(current_reserved_force_bytes, pulse_budget);
            bandwidth.non_force_bytes_left_ =
                pulse_budget > bandwidth.reserved_force_bytes_ ? pulse_budget - bandwidth.reserved_force_bytes_ : 0U;
            bandwidth.is_non_force_limited_ = true;
        }
        else
        {
            bandwidth.reserved_force_bytes_ = 0U;
        }

        bool const allow_other_peers = !bandwidth.is_non_force_limited_ || bandwidth.non_force_bytes_left_ > 0U;
        if (allow_other_peers)
        {
            for (auto& peers : other_peer_arrays)
            {
                (void)phaseOne(peers, dir);
            }
        }
    };

    // allocateBandwidth () is a helper function with two purposes:
    // 1. allocate bandwidth to b and its subtree
    // 2. accumulate an array of all the peerIos from b and its subtree.
    this->allocateBandwidth(TR_TOR_PRI_LOW, period_msec, refs);

    for (auto const& io : refs)
    {
        io->flush_outgoing_protocol_msgs();

        if (io->priority() == TR_TOR_PRI_FORCE)
        {
            force_upload_peers.push_back(io.get());
            force_download_peers.push_back(io.get());
        }
        else
        {
            add_other_upload_peer(other_upload_peer_arrays, io->priority(), io.get());
            add_other_download_peer(other_download_peer_arrays, io->priority(), io.get());
        }
    }

    auto const upload_pulse_budget = band_[TR_UP].bytes_left_;
    auto const download_pulse_budget = band_[TR_DOWN].bytes_left_;

    run_force_first_pass(force_upload_peers, other_upload_peer_arrays, TR_UP, upload_pulse_budget);
    run_force_first_pass(force_download_peers, other_download_peer_arrays, TR_DOWN, download_pulse_budget);

    // Second phase of IO. To help us scale in high bandwidth situations,
    // enable on-demand IO for peers with bandwidth left to burn.
    // This on-demand IO is enabled until (1) the peer runs out of bandwidth,
    // or (2) the next tr_bandwidth::allocate () call, when we start over again.
    for (auto const& io : refs)
    {
        io->set_enabled(TR_UP, io->has_bandwidth_left(TR_UP));
        io->set_enabled(TR_DOWN, io->has_bandwidth_left(TR_DOWN));
    }
}

// ---

size_t tr_bandwidth::clamp(tr_direction const dir, size_t byte_count, tr_torrent_priority_t const priority) const noexcept
{
    TR_ASSERT(tr_isDirection(dir));

    if (auto const& band = this->band_[dir]; band.is_limited_)
    {
        byte_count = std::min(byte_count, band.bytes_left_);
    }

    if (auto const& band = this->band_[dir]; priority != TR_TOR_PRI_FORCE && band.is_non_force_limited_)
    {
        byte_count = std::min(byte_count, band.non_force_bytes_left_);
    }

    if (this->parent_ != nullptr && this->band_[dir].honor_parent_limits_ && byte_count > 0)
    {
        byte_count = this->parent_->clamp(dir, byte_count, priority);
    }

    return byte_count;
}

void tr_bandwidth::notifyBandwidthConsumed(
    tr_direction dir,
    size_t byte_count,
    bool is_piece_data,
    uint64_t now,
    tr_torrent_priority_t const priority)
{
    TR_ASSERT(tr_isDirection(dir));

    Band* band = &this->band_[dir];

    if (band->is_limited_ && is_piece_data)
    {
        band->bytes_left_ -= std::min(size_t{ band->bytes_left_ }, byte_count);
    }

    if (priority != TR_TOR_PRI_FORCE && band->is_non_force_limited_ && is_piece_data)
    {
        band->non_force_bytes_left_ -= std::min(band->non_force_bytes_left_, byte_count);
    }

    if (is_piece_data)
    {
        if (priority == TR_TOR_PRI_FORCE)
        {
            band->force_piece_bytes_used_ += byte_count;
        }
        else
        {
            band->non_force_piece_bytes_used_ += byte_count;
        }
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
    }

    if (this->parent_ != nullptr)
    {
        this->parent_->notifyBandwidthConsumed(dir, byte_count, is_piece_data, now, priority);
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
