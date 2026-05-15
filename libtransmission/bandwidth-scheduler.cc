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

#include "bandwidth-scheduler.h"

#include "bandwidth.h"
#include "crypto-utils.h"
#include "peer-io.h"
#include "session.h"
#include "tr-assert.h"

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
        static_cast<void>(io);
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
    using SeedBuckets = std::array<std::vector<PeerRef>, 3>;
    using ReadQueues = std::array<std::deque<PeerRef>, 3>;
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
        session_.top_bandwidth_.allocatePulse(period_msec, refs);

        seed_reads_from_pulse(refs);
        drain_reads();

        for (auto const& io : refs)
        {
            io->set_enabled(TR_DOWN, io->has_bandwidth_left(TR_DOWN));
        }

        seed_writes_from_pulse(refs);
        drain_writes();
    }

    void on_can_read(tr_peerIo& io) override
    {
        enqueue_read(io);
        drain_reads();
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
    [[nodiscard]] static auto shuffle_seed_bucket(SeedBuckets bucket)
    {
        static thread_local auto urbg = tr_urbg<size_t>{};

        std::shuffle(std::begin(bucket[0]), std::end(bucket[0]), urbg);
        std::shuffle(std::begin(bucket[1]), std::end(bucket[1]), urbg);
        std::shuffle(std::begin(bucket[2]), std::end(bucket[2]), urbg);
        return bucket;
    }

    void seed_reads_from_pulse(std::vector<PeerRef> const& refs)
    {
        auto buckets = SeedBuckets{};
        for (auto const& io : refs)
        {
            if (!io->is_utp() && io->has_bandwidth_left(TR_DOWN))
            {
                buckets[priority_index(io->priority())].push_back(io);
            }
        }

        for (auto& bucket : shuffle_seed_bucket(std::move(buckets)))
        {
            for (auto& io : bucket)
            {
                enqueue_read(std::move(io));
            }
        }
    }

    void seed_writes_from_pulse(std::vector<PeerRef> const& refs)
    {
        auto buckets = SeedBuckets{};
        for (auto const& io : refs)
        {
            if (io->has_output_buffered() && io->has_bandwidth_left(TR_UP))
            {
                buckets[priority_index(io->priority())].push_back(io);
            }
        }

        for (auto& bucket : shuffle_seed_bucket(std::move(buckets)))
        {
            for (auto& io : bucket)
            {
                enqueue_write(std::move(io));
            }
        }
    }

    void enqueue_read(tr_peerIo& io)
    {
        if (!io.is_utp() && io.has_bandwidth_left(TR_DOWN))
        {
            enqueue_read(io.shared_from_this());
        }
    }

    void enqueue_read(PeerRef io)
    {
        TR_ASSERT(io != nullptr);

        if (!queued_reads_.emplace(io.get()).second)
        {
            return;
        }

        read_queues_[priority_index(io->priority())].push_back(std::move(io));
    }

    void enqueue_write(tr_peerIo& io)
    {
        if (io.has_output_buffered() && io.has_bandwidth_left(TR_UP))
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

    [[nodiscard]] auto* next_read_queue() noexcept
    {
        for (auto& queue : read_queues_)
        {
            if (!std::empty(queue))
            {
                return &queue;
            }
        }

        return static_cast<std::deque<PeerRef>*>(nullptr);
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

    void drain_reads()
    {
        if (is_draining_reads_)
        {
            return;
        }

        is_draining_reads_ = true;

        for (;;)
        {
            auto* const queue = next_read_queue();
            if (queue == nullptr)
            {
                break;
            }

            auto io = std::move(queue->front());
            queue->pop_front();
            queued_reads_.erase(io.get());

            auto const bytes_read = io->flush(TR_DOWN, Increment);

            if (bytes_read == Increment && io->has_bandwidth_left(TR_DOWN))
            {
                enqueue_read(std::move(io));
            }
        }

        is_draining_reads_ = false;
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
            auto const piece_bytes = io->flush(TR_UP, Increment);

            if (piece_bytes == Increment && io->has_output_buffered() && io->has_bandwidth_left(TR_UP))
            {
                enqueue_write(std::move(io));
            }
        }

        is_draining_writes_ = false;
    }

private:
    tr_session& session_;
    ReadQueues read_queues_ = {};
    WriteQueues write_queues_ = {};
    std::unordered_set<tr_peerIo*> queued_reads_;
    std::unordered_set<tr_peerIo*> queued_writes_;
    bool is_draining_reads_ = false;
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
