// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cstddef> // size_t
#include <cstdint> // uint64_t
#include <deque>
#include <limits>
#include <memory>
#include <unordered_set>
#include <vector>

#include "libtransmission/bandwidth-scheduler.h"

#include "libtransmission/bandwidth.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/peer-io.h"
#include "libtransmission/session.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/types.h"

namespace
{

[[nodiscard]] constexpr auto priority_index(tr_priority_t priority) noexcept
{
    switch (priority)
    {
    case TR_PRI_HIGH:
        return size_t{ 0U };

    case TR_PRI_NORMAL:
        return size_t{ 1U };

    case TR_PRI_LOW:
        return size_t{ 2U };
    }

    return size_t{ 1U };
}

class tr_legacy_bandwidth_scheduler final : public tr_bandwidth_scheduler
{
public:
    explicit tr_legacy_bandwidth_scheduler(tr_session& session)
        : session_{ session }
    {
    }

    void on_pulse(uint64_t period_msec) override
    {
        session_.top_bandwidth_.allocate(period_msec);
    }

    void on_can_read(tr_peerIo& io) override
    {
        io.execute_can_read();
    }

    void on_can_write(tr_peerIo& io) override
    {
        io.execute_can_write();
    }

    void on_outbuf_ready(tr_peerIo& io) override
    {
        io.execute_outbuf_ready();
    }

    void on_utp_read(tr_peerIo& io, size_t bytes_transferred) override
    {
        io.execute_utp_read(bytes_transferred);
    }

private:
    tr_session& session_;
};

class tr_strict_bandwidth_scheduler final : public tr_bandwidth_scheduler
{
private:
    using PeerRef = std::shared_ptr<tr_peerIo>;
    using PeerList = std::vector<tr_peerIo*>;
    using PeerPriorityArrays = std::array<PeerList, 3>;
    using SeedBuckets = std::array<std::vector<PeerRef>, 3>;
    using WriteQueues = std::array<std::deque<PeerRef>, 3>;

    static auto constexpr Increment = size_t{ 3000U };

public:
    explicit tr_strict_bandwidth_scheduler(tr_session& session)
        : session_{ session }
    {
    }

    void on_pulse(uint64_t period_msec) override
    {
        auto refs = std::vector<PeerRef>{};
        session_.top_bandwidth_.allocate_pulse(period_msec, refs);

        auto peer_arrays = build_legacy_priority_arrays(refs);
        for (auto& peers : peer_arrays)
        {
            phase_one(peers, tr_direction::Down);
        }

        for (auto const& io : refs)
        {
            io->set_enabled(tr_direction::Down, io->has_bandwidth_left(tr_direction::Down));
        }

        seed_writes_from_pulse(refs);
        drain_writes();
    }

    void on_can_read(tr_peerIo& io) override
    {
        io.execute_can_read();
    }

    void on_can_write(tr_peerIo& io) override
    {
        enqueue_write(io);
        drain_writes();
    }

    void on_outbuf_ready(tr_peerIo& io) override
    {
        enqueue_write(io);
        drain_writes();
    }

    void on_utp_read(tr_peerIo& io, size_t bytes_transferred) override
    {
        io.execute_utp_read(bytes_transferred);
    }

private:
    [[nodiscard]] static PeerPriorityArrays build_legacy_priority_arrays(std::vector<PeerRef> const& refs)
    {
        auto peer_arrays = PeerPriorityArrays{};
        auto& high = peer_arrays[0];
        auto& normal = peer_arrays[1];
        auto& low = peer_arrays[2];

        for (auto const& io : refs)
        {
            switch (io->priority())
            {
            case TR_PRI_HIGH:
                high.push_back(io.get());
                [[fallthrough]];

            case TR_PRI_NORMAL:
                normal.push_back(io.get());
                [[fallthrough]];

            case TR_PRI_LOW:
                low.push_back(io.get());
                break;

            default:
                TR_ASSERT_MSG(false, "invalid priority");
                break;
            }
        }

        return peer_arrays;
    }

    static void phase_one(std::vector<tr_peerIo*>& peers, tr_direction dir)
    {
        static thread_local auto urbg = tr_urbg<size_t>{};
        std::shuffle(std::begin(peers), std::end(peers), urbg);

        for (size_t n_unfinished = std::size(peers); n_unfinished > 0U;)
        {
            for (size_t i = 0U; i < n_unfinished;)
            {
                auto const bytes_used = peers[i]->flush(dir, Increment);
                if (bytes_used != Increment)
                {
                    std::swap(peers[i], peers[n_unfinished - 1U]);
                    --n_unfinished;
                }
                else
                {
                    ++i;
                }
            }
        }
    }

    void seed_writes_from_pulse(std::vector<PeerRef> const& refs)
    {
        auto buckets = SeedBuckets{};
        for (auto const& io : refs)
        {
            if (io->has_output_buffered() && io->has_bandwidth_left(tr_direction::Up))
            {
                buckets[priority_index(io->priority())].push_back(io);
            }
        }

        static thread_local auto urbg = tr_urbg<size_t>{};
        for (auto& bucket : buckets)
        {
            std::shuffle(std::begin(bucket), std::end(bucket), urbg);
            for (auto& io : bucket)
            {
                enqueue_write(std::move(io));
            }
        }
    }

    void enqueue_write(tr_peerIo& io)
    {
        if (io.has_output_buffered() && io.has_bandwidth_left(tr_direction::Up))
        {
            enqueue_write(io.shared_from_this());
        }
    }

    void enqueue_write(PeerRef io)
    {
        TR_ASSERT(io != nullptr);

        if (!queued_writes_.emplace(io.get()).second)
        {
            return;
        }

        write_queues_[priority_index(io->priority())].push_back(std::move(io));
    }

    [[nodiscard]] auto* next_write_queue() noexcept
    {
        for (auto& queue : write_queues_)
        {
            if (!std::empty(queue))
            {
                return &queue;
            }
        }

        return static_cast<std::deque<PeerRef>*>(nullptr);
    }

    void drain_writes()
    {
        if (is_draining_writes_)
        {
            return;
        }

        is_draining_writes_ = true;

        for (;;)
        {
            auto* const queue = next_write_queue();
            if (queue == nullptr)
            {
                break;
            }

            auto io = std::move(queue->front());
            queue->pop_front();
            queued_writes_.erase(io.get());

            [[maybe_unused]] auto const protocol_bytes = io->flush_outgoing_protocol_msgs();
            auto const piece_bytes = io->flush(tr_direction::Up, Increment);

            if (piece_bytes == Increment && io->has_output_buffered() && io->has_bandwidth_left(tr_direction::Up))
            {
                enqueue_write(std::move(io));
            }
        }

        is_draining_writes_ = false;
    }

private:
    tr_session& session_;
    WriteQueues write_queues_ = {};
    std::unordered_set<tr_peerIo*> queued_writes_;
    bool is_draining_writes_ = false;
};

} // namespace

std::unique_ptr<tr_bandwidth_scheduler> tr_bandwidth_scheduler::create(tr_session& session)
{
    switch (session.bandwidthAllocator())
    {
    case tr_bandwidth_allocator_mode::Default:
        return std::make_unique<tr_legacy_bandwidth_scheduler>(session);

    case tr_bandwidth_allocator_mode::Strict:
        return std::make_unique<tr_strict_bandwidth_scheduler>(session);
    }

    TR_ASSERT_MSG(false, "invalid bandwidth allocator mode");
    return std::make_unique<tr_legacy_bandwidth_scheduler>(session);
}
