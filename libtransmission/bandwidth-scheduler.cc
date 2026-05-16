// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <chrono>
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
#include "timer.h"
#include "tr-assert.h"
#include "utils.h"

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

    void on_peer_cleared(tr_peerIo& io) override
    {
        static_cast<void>(io);
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
    static auto constexpr MaxItemsPerDrain = size_t{ 64U };

public:
    explicit tr_strict_bandwidth_scheduler(tr_session& session)
        : session_{ session }
        , continue_drain_timer_{ session.timerMaker().create([this]() { on_continue_drain(); }) }
    {
    }

    void on_pulse(uint64_t period_msec) override
    {
        pulse_deadline_msec_ = tr_time_msec() + period_msec;

        if (deferred_drain_scheduled_)
        {
            continue_drain_timer_->stop();
            deferred_drain_scheduled_ = false;
        }

        auto refs = std::vector<PeerRef>{};
        session_.top_bandwidth_.allocatePulse(period_msec, refs);

        seed_reads_from_pulse(refs);
        seed_writes_from_pulse(refs);
        drain_queues();

        for (auto const& io : refs)
        {
            io->set_enabled(TR_DOWN, io->has_bandwidth_left(TR_DOWN));
        }
    }

    void on_can_read(tr_peerIo& io) override
    {
        enqueue_read(io);
        drain_queues();
    }

    void on_can_write(tr_peerIo& io) override
    {
        enqueue_write(io);
        drain_queues();
    }

    void on_outbuf_ready(tr_peerIo& io) override
    {
        enqueue_write(io);
        drain_queues();
    }

    void on_utp_read(tr_peerIo& io, size_t bytes_transferred) override
    {
        io.execute_utp_read(bytes_transferred);
    }

    void on_peer_cleared(tr_peerIo& io) override
    {
        dequeue_peer(io);
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
        if (!io.is_cleared() && !io.is_utp() && io.has_bandwidth_left(TR_DOWN))
        {
            enqueue_read(io.shared_from_this());
        }
    }

    void enqueue_read(PeerRef io)
    {
        TR_ASSERT(io != nullptr);
        TR_ASSERT(!io->is_cleared());

        if (!queued_reads_.emplace(io.get()).second)
        {
            return;
        }

        read_queues_[priority_index(io->priority())].push_back(std::move(io));
    }

    void enqueue_write(tr_peerIo& io)
    {
        if (!io.is_cleared() && io.has_output_buffered() && io.has_bandwidth_left(TR_UP))
        {
            enqueue_write(io.shared_from_this());
        }
    }

    void enqueue_write(PeerRef io)
    {
        TR_ASSERT(io != nullptr);
        TR_ASSERT(!io->is_cleared());

        if (!queued_writes_.emplace(io.get()).second)
        {
            return;
        }

        write_queues_[priority_index(io->priority())].push_back(std::move(io));
    }

    [[nodiscard]] auto has_queued_work() const noexcept
    {
        return std::any_of(
                   std::begin(read_queues_),
                   std::end(read_queues_),
                   [](auto const& queue) { return !std::empty(queue); }) ||
            std::any_of(
                   std::begin(write_queues_),
                   std::end(write_queues_),
                   [](auto const& queue) { return !std::empty(queue); });
    }

    template<typename QueueContainer>
    static void erase_peer_from_queues(QueueContainer& queues, tr_peerIo* raw)
    {
        for (auto& queue : queues)
        {
            queue.erase(
                std::remove_if(
                    std::begin(queue),
                    std::end(queue),
                    [raw](auto const& io) { return io == nullptr || io.get() == raw; }),
                std::end(queue));
        }
    }

    [[nodiscard]] auto can_continue_this_pulse() const
    {
        return pulse_deadline_msec_ == 0U || tr_time_msec() < pulse_deadline_msec_;
    }

    void stop_scheduled_wakeup()
    {
        if (deferred_drain_scheduled_)
        {
            continue_drain_timer_->stop();
            deferred_drain_scheduled_ = false;
        }
    }

    void dequeue_peer(tr_peerIo& io)
    {
        auto* const raw = &io;

        queued_reads_.erase(raw);
        queued_writes_.erase(raw);
        erase_peer_from_queues(read_queues_, raw);
        erase_peer_from_queues(write_queues_, raw);

        if (!has_queued_work())
        {
            stop_scheduled_wakeup();
        }
    }

    void on_continue_drain()
    {
        deferred_drain_scheduled_ = false;
        drain_queues();
    }

    void schedule_deferred_drain()
    {
        if (deferred_drain_scheduled_ || !has_queued_work() || !can_continue_this_pulse())
        {
            return;
        }

        deferred_drain_scheduled_ = true;
        continue_drain_timer_->startSingleShot(std::chrono::milliseconds::zero());
    }

    void drain_read(std::deque<PeerRef>& queue)
    {
        TR_ASSERT(queue.front() != nullptr);
        TR_ASSERT(!queue.front()->is_cleared());

        auto io = std::move(queue.front());
        queue.pop_front();
        queued_reads_.erase(io.get());

        auto const bytes_read = io->flush(TR_DOWN, Increment);

        if (bytes_read == Increment && io->has_bandwidth_left(TR_DOWN))
        {
            enqueue_read(std::move(io));
        }
    }

    void drain_write(std::deque<PeerRef>& queue)
    {
        TR_ASSERT(queue.front() != nullptr);
        TR_ASSERT(!queue.front()->is_cleared());

        auto io = std::move(queue.front());
        queue.pop_front();
        queued_writes_.erase(io.get());

        [[maybe_unused]] auto const protocol_bytes = io->flush_outgoing_protocol_msgs();
        auto const piece_bytes = io->flush(TR_UP, Increment);

        if (piece_bytes == Increment && io->has_output_buffered() && io->has_bandwidth_left(TR_UP))
        {
            enqueue_write(std::move(io));
        }
    }

    [[nodiscard]] auto drain_one_item() -> bool
    {
        for (size_t i = 0U; i < std::size(read_queues_); ++i)
        {
            if (!std::empty(read_queues_[i]))
            {
                drain_read(read_queues_[i]);
                return true;
            }

            if (!std::empty(write_queues_[i]))
            {
                drain_write(write_queues_[i]);
                return true;
            }
        }

        return false;
    }

    void drain_queues()
    {
        if (is_draining_)
        {
            return;
        }

        is_draining_ = true;

        for (auto n_drained = size_t{ 0U }; n_drained < MaxItemsPerDrain && can_continue_this_pulse(); ++n_drained)
        {
            if (!drain_one_item())
            {
                break;
            }
        }

        is_draining_ = false;
        schedule_deferred_drain();
    }

private:
    tr_session& session_;
    std::unique_ptr<libtransmission::Timer> continue_drain_timer_;
    ReadQueues read_queues_ = {};
    WriteQueues write_queues_ = {};
    std::unordered_set<tr_peerIo*> queued_reads_;
    std::unordered_set<tr_peerIo*> queued_writes_;
    uint64_t pulse_deadline_msec_ = 0U;
    bool deferred_drain_scheduled_ = false;
    bool is_draining_ = false;
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
