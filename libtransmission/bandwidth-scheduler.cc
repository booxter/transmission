// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

[[nodiscard]] constexpr auto priority_from_index(size_t index) noexcept
{
    switch (index)
    {
    case 0U:
        return TR_PRI_HIGH;

    case 1U:
        return TR_PRI_NORMAL;

    default:
        return TR_PRI_LOW;
    }
}

[[nodiscard]] constexpr auto direction_index(tr_direction dir) noexcept
{
    return dir == TR_UP ? size_t{ 0U } : size_t{ 1U };
}

struct StrictCurveParameters
{
    double normal_low_exponent;
    double low_exponent;
};

[[nodiscard]] constexpr auto strict_curve_parameters(tr_strict_bandwidth_curve curve) noexcept
{
    switch (curve)
    {
    case tr_strict_bandwidth_curve::Relaxed:
        return StrictCurveParameters{ 1.5, 3.0 };

    case tr_strict_bandwidth_curve::Aggressive:
        return StrictCurveParameters{ 3.0, 6.0 };

    case tr_strict_bandwidth_curve::Balanced:
    default:
        return StrictCurveParameters{ 2.0, 4.0 };
    }
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
    struct LimitedRetentionState
    {
        size_t pulse_budget = 0U;
        size_t normal_low_piece = 0U;
        size_t low_piece = 0U;
    };

    static auto constexpr Increment = size_t{ 3000U };
    static auto constexpr RetentionIncrement = size_t{ 1024U };
    static auto constexpr MaxItemsPerDrain = size_t{ 64U };

public:
    explicit tr_strict_bandwidth_scheduler(tr_session& session)
        : session_{ session }
        , continue_drain_timer_{ session.timerMaker().create([this]() { on_continue_drain(); }) }
    {
    }

    void on_pulse(uint64_t period_msec) override
    {
        reset_limited_retention(period_msec);
        stop_scheduled_wakeup();

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

    [[nodiscard]] static auto min_nonzero(uint64_t a, uint64_t b) noexcept
    {
        if (a == 0U)
        {
            return b;
        }

        if (b == 0U)
        {
            return a;
        }

        return std::min(a, b);
    }

    [[nodiscard]] static auto next_retention_target(size_t consumed_piece, size_t pulse_budget) noexcept
    {
        if (consumed_piece >= pulse_budget)
        {
            return pulse_budget;
        }

        auto const remaining_piece = pulse_budget - consumed_piece;
        auto const release_increment = std::min(RetentionIncrement, remaining_piece);
        return std::min(pulse_budget, consumed_piece + release_increment);
    }

    void reset_limited_retention(uint64_t period_msec)
    {
        pulse_start_msec_ = tr_time_msec();
        pulse_duration_msec_ = period_msec;
        pulse_deadline_msec_ = pulse_start_msec_ + period_msec;
        limited_retention_ = {};

        for (auto const dir : { TR_UP, TR_DOWN })
        {
            if (session_.top_bandwidth_.isLimited(dir))
            {
                limited_retention_[direction_index(dir)].pulse_budget = static_cast<size_t>(
                    session_.top_bandwidth_.getDesiredSpeedBytesPerSecond(dir) * period_msec / 1000U);
            }
        }
    }

    [[nodiscard]] auto limited_retention_applies(tr_direction dir, tr_peerIo const& io) const noexcept
    {
        return pulse_duration_msec_ != 0U && limited_retention_[direction_index(dir)].pulse_budget != 0U &&
            io.bandwidth().honorsAncestor(dir, &session_.top_bandwidth_);
    }

    [[nodiscard]] auto released_bytes(tr_direction dir, double exponent, uint64_t now_msec) const noexcept
    {
        auto const& retention = limited_retention_[direction_index(dir)];

        if (retention.pulse_budget == 0U)
        {
            return size_t{ 0U };
        }

        if (now_msec + 1U >= pulse_deadline_msec_ || pulse_duration_msec_ == 0U)
        {
            return retention.pulse_budget;
        }

        auto const elapsed = std::min(now_msec - pulse_start_msec_, pulse_duration_msec_);
        auto const progress = static_cast<double>(elapsed) / static_cast<double>(pulse_duration_msec_);
        auto const released = std::pow(progress, exponent);
        return std::min(retention.pulse_budget, static_cast<size_t>(released * retention.pulse_budget));
    }

    [[nodiscard]] auto retained_piece_limit(tr_direction dir, tr_priority_t priority, tr_peerIo const& io) const noexcept
    {
        if (priority == TR_PRI_HIGH || !limited_retention_applies(dir, io))
        {
            return Increment;
        }

        auto const now_msec = tr_time_msec();
        auto const params = strict_curve_parameters(session_.strictBandwidthCurve());
        auto const& retention = limited_retention_[direction_index(dir)];
        auto const normal_low_allowed = released_bytes(dir, params.normal_low_exponent, now_msec);
        auto const normal_low_remaining = normal_low_allowed > retention.normal_low_piece ?
            normal_low_allowed - retention.normal_low_piece :
            0U;
        auto const normal_low_target = next_retention_target(retention.normal_low_piece, retention.pulse_budget);
        auto const normal_low_release_increment = normal_low_target - retention.normal_low_piece;

        if (priority == TR_PRI_NORMAL)
        {
            if (normal_low_remaining < normal_low_release_increment && now_msec < pulse_deadline_msec_)
            {
                return size_t{ 0U };
            }

            return std::min(Increment, normal_low_remaining);
        }

        auto const low_allowed = released_bytes(dir, params.low_exponent, now_msec);
        auto const low_remaining = low_allowed > retention.low_piece ? low_allowed - retention.low_piece : 0U;
        auto const low_target = next_retention_target(retention.low_piece, retention.pulse_budget);
        auto const low_release_increment = low_target - retention.low_piece;

        if ((normal_low_remaining < normal_low_release_increment || low_remaining < low_release_increment) &&
            now_msec < pulse_deadline_msec_)
        {
            return size_t{ 0U };
        }

        return std::min(Increment, std::min(normal_low_remaining, low_remaining));
    }

    [[nodiscard]] auto next_release_msec(size_t consumed_piece, size_t pulse_budget, double exponent, uint64_t now_msec)
        const noexcept
    {
        if (pulse_budget == 0U || consumed_piece >= pulse_budget || pulse_duration_msec_ == 0U)
        {
            return uint64_t{ 0U };
        }

        auto const ratio = std::clamp(
            static_cast<double>(next_retention_target(consumed_piece, pulse_budget)) / static_cast<double>(pulse_budget),
            0.0,
            1.0);
        auto const progress = std::pow(ratio, 1.0 / exponent);
        auto wakeup_msec = pulse_start_msec_ + static_cast<uint64_t>(std::ceil(progress * pulse_duration_msec_));
        if (wakeup_msec <= now_msec)
        {
            wakeup_msec = now_msec + 1U;
        }

        auto const final_wakeup_msec = pulse_deadline_msec_ > pulse_start_msec_ ? pulse_deadline_msec_ - 1U :
                                                                                  pulse_deadline_msec_;
        return std::min(wakeup_msec, final_wakeup_msec);
    }

    [[nodiscard]] auto next_retained_wakeup_msec(tr_direction dir, tr_priority_t priority, tr_peerIo const& io) const noexcept
    {
        if (priority == TR_PRI_HIGH || !limited_retention_applies(dir, io))
        {
            return uint64_t{ 0U };
        }

        auto const now_msec = tr_time_msec();
        auto const params = strict_curve_parameters(session_.strictBandwidthCurve());
        auto const& retention = limited_retention_[direction_index(dir)];
        auto wakeup_msec = next_release_msec(
            retention.normal_low_piece,
            retention.pulse_budget,
            params.normal_low_exponent,
            now_msec);

        if (priority == TR_PRI_NORMAL)
        {
            return wakeup_msec;
        }

        return std::max(
            wakeup_msec,
            next_release_msec(retention.low_piece, retention.pulse_budget, params.low_exponent, now_msec));
    }

    void charge_retained_piece_bytes(tr_direction dir, tr_priority_t priority, tr_peerIo const& io, size_t piece_bytes)
    {
        if (piece_bytes == 0U || priority == TR_PRI_HIGH || !limited_retention_applies(dir, io))
        {
            return;
        }

        auto& retention = limited_retention_[direction_index(dir)];
        retention.normal_low_piece = std::min(retention.pulse_budget, retention.normal_low_piece + piece_bytes);
        if (priority == TR_PRI_LOW)
        {
            retention.low_piece = std::min(retention.pulse_budget, retention.low_piece + piece_bytes);
        }
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
            if (auto shared = io.self(); shared != nullptr)
            {
                enqueue_read(std::move(shared));
            }
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
            if (auto shared = io.self(); shared != nullptr)
            {
                enqueue_write(std::move(shared));
            }
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

    void on_continue_drain()
    {
        scheduled_wakeup_msec_ = 0U;
        drain_queues();
    }

    void stop_scheduled_wakeup()
    {
        if (scheduled_wakeup_msec_ != 0U)
        {
            continue_drain_timer_->stop();
            scheduled_wakeup_msec_ = 0U;
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

    void arm_wakeup(uint64_t wakeup_msec)
    {
        auto const now_msec = tr_time_msec();
        auto const delay = wakeup_msec <= now_msec ? std::chrono::milliseconds::zero() :
                                                     std::chrono::milliseconds(wakeup_msec - now_msec);

        if (scheduled_wakeup_msec_ == wakeup_msec)
        {
            return;
        }

        stop_scheduled_wakeup();
        scheduled_wakeup_msec_ = wakeup_msec;
        continue_drain_timer_->startSingleShot(delay);
    }

    void schedule_next_wakeup(uint64_t gated_wakeup_msec)
    {
        if (!has_queued_work() || !can_continue_this_pulse())
        {
            stop_scheduled_wakeup();
            return;
        }

        arm_wakeup(gated_wakeup_msec != 0U ? gated_wakeup_msec : tr_time_msec());
    }

    [[nodiscard]] auto try_drain_read_queue(std::deque<PeerRef>& queue, tr_priority_t priority, uint64_t& gated_wakeup_msec)
    {
        auto const n_items = std::size(queue);
        for (size_t i = 0U; i < n_items; ++i)
        {
            TR_ASSERT(queue.front() != nullptr);
            TR_ASSERT(!queue.front()->is_cleared());

            auto const piece_limit = retained_piece_limit(TR_DOWN, priority, *queue.front());
            if (piece_limit == 0U)
            {
                gated_wakeup_msec = min_nonzero(
                    gated_wakeup_msec,
                    next_retained_wakeup_msec(TR_DOWN, priority, *queue.front()));
                queue.push_back(std::move(queue.front()));
                queue.pop_front();
                continue;
            }

            auto io = std::move(queue.front());
            queue.pop_front();
            queued_reads_.erase(io.get());

            auto const result = io->flush_with_result(TR_DOWN, piece_limit);
            charge_retained_piece_bytes(TR_DOWN, priority, *io, result.piece_bytes);

            if (result.bytes_transferred == piece_limit && io->has_bandwidth_left(TR_DOWN))
            {
                enqueue_read(std::move(io));
            }

            return true;
        }

        return false;
    }

    [[nodiscard]] auto try_drain_write_queue(std::deque<PeerRef>& queue, tr_priority_t priority, uint64_t& gated_wakeup_msec)
    {
        auto const n_items = std::size(queue);
        for (size_t i = 0U; i < n_items; ++i)
        {
            TR_ASSERT(queue.front() != nullptr);
            TR_ASSERT(!queue.front()->is_cleared());

            auto const piece_limit = retained_piece_limit(TR_UP, priority, *queue.front());
            if (piece_limit == 0U && !queue.front()->has_pending_protocol_output())
            {
                gated_wakeup_msec = min_nonzero(gated_wakeup_msec, next_retained_wakeup_msec(TR_UP, priority, *queue.front()));
                queue.push_back(std::move(queue.front()));
                queue.pop_front();
                continue;
            }

            auto io = std::move(queue.front());
            queue.pop_front();
            queued_writes_.erase(io.get());

            auto const protocol_limit = io->pending_protocol_output_size();
            auto const protocol_bytes = protocol_limit != 0U ? io->flush_outgoing_protocol_msgs() : 0U;
            auto const flushed_all_protocol = protocol_limit == protocol_bytes;

            auto piece_result = tr_peerIo::FlushResult{};
            if (flushed_all_protocol && io->has_output_buffered() && piece_limit != 0U)
            {
                piece_result = io->flush_with_result(TR_UP, piece_limit);
                charge_retained_piece_bytes(TR_UP, priority, *io, piece_result.piece_bytes);
            }

            if (io->has_output_buffered() && io->has_bandwidth_left(TR_UP))
            {
                if ((piece_limit == 0U && flushed_all_protocol) ||
                    (piece_limit != 0U && flushed_all_protocol && piece_result.bytes_transferred == piece_limit))
                {
                    enqueue_write(std::move(io));
                }
            }

            return true;
        }

        return false;
    }

    [[nodiscard]] auto drain_one_item(uint64_t& gated_wakeup_msec) -> bool
    {
        for (size_t i = 0U; i < std::size(read_queues_); ++i)
        {
            auto const priority = priority_from_index(i);

            if (!std::empty(read_queues_[i]) && try_drain_read_queue(read_queues_[i], priority, gated_wakeup_msec))
            {
                return true;
            }

            if (!std::empty(write_queues_[i]) && try_drain_write_queue(write_queues_[i], priority, gated_wakeup_msec))
            {
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
        auto gated_wakeup_msec = uint64_t{ 0U };

        for (auto n_drained = size_t{ 0U }; n_drained < MaxItemsPerDrain && can_continue_this_pulse(); ++n_drained)
        {
            if (!drain_one_item(gated_wakeup_msec))
            {
                break;
            }
        }

        is_draining_ = false;
        schedule_next_wakeup(gated_wakeup_msec);
    }

private:
    tr_session& session_;
    std::unique_ptr<libtransmission::Timer> continue_drain_timer_;
    ReadQueues read_queues_ = {};
    WriteQueues write_queues_ = {};
    std::array<LimitedRetentionState, 2> limited_retention_ = {};
    std::unordered_set<tr_peerIo*> queued_reads_;
    std::unordered_set<tr_peerIo*> queued_writes_;
    uint64_t pulse_start_msec_ = 0U;
    uint64_t pulse_duration_msec_ = 0U;
    uint64_t pulse_deadline_msec_ = 0U;
    uint64_t scheduled_wakeup_msec_ = 0U;
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
