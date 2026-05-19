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
#include <string>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "bandwidth-scheduler.h"
#include "bandwidth.h"
#include "crypto-utils.h"
#include "log.h"
#include "peer-io.h"
#include "session.h"
#include "strict-bandwidth-curve.h"
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

using PriorityCounters = std::array<uint64_t, 3>;

[[nodiscard]] auto format_priority_counters(PriorityCounters const& counters)
{
    return fmt::format("[{},{},{}]", counters[0], counters[1], counters[2]);
}

[[nodiscard]] constexpr auto to_string(tr_strict_bandwidth_curve_adjustment adjustment) noexcept
{
    switch (adjustment)
    {
    case tr_strict_bandwidth_curve_adjustment::Tighten:
        return "tighten";

    case tr_strict_bandwidth_curve_adjustment::Relax:
        return "relax";

    case tr_strict_bandwidth_curve_adjustment::Hold:
    default:
        return "hold";
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

    static auto constexpr Increment = size_t{ 3000U };
    static auto constexpr RetentionIncrement = size_t{ 1024U };
    static auto constexpr MaxItemsPerDrain = size_t{ 64U };

public:
    explicit tr_strict_bandwidth_scheduler(tr_session& session)
        : session_{ session }
        , retention_policy_{ tr_strict_bandwidth_curve_policy::create(
              session.strictBandwidthCurve(),
              Increment,
              RetentionIncrement) }
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

    [[nodiscard]] auto diagnostics_enabled() const noexcept
    {
        return session_.strictBandwidthDiagnosticsEnabled();
    }

    void maybe_log_curve_diagnostics()
    {
        if (!diagnostics_enabled() || pulse_duration_msec_ == 0U)
        {
            return;
        }

        auto const snapshot = retention_policy_->snapshot();
        tr_logAddInfo(
            fmt::format(
                "strict-scheduler pulse up:{} down:{}",
                format_direction_diagnostics(TR_UP, snapshot.by_direction[direction_index(TR_UP)], snapshot.is_dynamic),
                format_direction_diagnostics(TR_DOWN, snapshot.by_direction[direction_index(TR_DOWN)], snapshot.is_dynamic)),
            "bandwidth-scheduler");
    }

    [[nodiscard]] std::string format_direction_diagnostics(
        tr_direction dir,
        tr_strict_bandwidth_curve_policy_snapshot::DirectionState const& state,
        bool is_dynamic) const
    {
        auto const index = direction_index(dir);
        auto const& outcome = current_pulse_outcome_.by_direction[index];
        auto const budget = current_pulse_budget_[index];
        auto const piece_total = outcome.piece_bytes[0] + outcome.piece_bytes[1] + outcome.piece_bytes[2];
        auto const utilization = budget == 0U ? uint64_t{ 0U } : piece_total * 100U / budget;

        auto const piece = PriorityCounters{
            static_cast<uint64_t>(outcome.piece_bytes[0]),
            static_cast<uint64_t>(outcome.piece_bytes[1]),
            static_cast<uint64_t>(outcome.piece_bytes[2]),
        };
        auto const blocked = PriorityCounters{
            outcome.had_blocked_work[0] ? 1U : 0U,
            outcome.had_blocked_work[1] ? 1U : 0U,
            outcome.had_blocked_work[2] ? 1U : 0U,
        };
        auto const pending = PriorityCounters{
            outcome.has_pending_work[0] ? 1U : 0U,
            outcome.has_pending_work[1] ? 1U : 0U,
            outcome.has_pending_work[2] ? 1U : 0U,
        };

        if (budget == 0U && piece_total == 0U && blocked == PriorityCounters{} && pending == PriorityCounters{})
        {
            return fmt::format("off");
        }

        if (!is_dynamic)
        {
            return fmt::format(
                "budget={} util={} piece={} blocked={} pending={} curve={{fixed p=[{:.2f},{:.2f}]}}",
                budget,
                utilization,
                format_priority_counters(piece),
                format_priority_counters(blocked),
                format_priority_counters(pending),
                state.normal_low_exponent,
                state.low_exponent);
        }

        return fmt::format(
            "budget={} util={} piece={} blocked={} pending={} "
            "curve={{dynamic p=[{:.2f},{:.2f}] win=[{},{},{},{}] last={}}}",
            budget,
            utilization,
            format_priority_counters(piece),
            format_priority_counters(blocked),
            format_priority_counters(pending),
            state.normal_low_exponent,
            state.low_exponent,
            state.window_pulses,
            state.fully_utilized_pulses,
            state.high_pressure_pulses,
            state.underfilled_lower_demand_pulses,
            to_string(state.last_adjustment));
    }

    template<typename QueueContainer>
    void note_pending_curve_work(QueueContainer const& queues, tr_direction dir)
    {
        for (size_t i = 0U; i < std::size(queues); ++i)
        {
            if (!std::empty(queues[i]))
            {
                current_pulse_outcome_.note_pending(dir, priority_from_index(i));
            }
        }
    }

    void finish_limited_retention_pulse()
    {
        if (pulse_duration_msec_ == 0U)
        {
            return;
        }

        note_pending_curve_work(read_queues_, TR_DOWN);
        note_pending_curve_work(write_queues_, TR_UP);
        retention_policy_->on_pulse_finish(current_pulse_outcome_);
        maybe_log_curve_diagnostics();
    }

    void reset_limited_retention(uint64_t period_msec)
    {
        finish_limited_retention_pulse();

        pulse_start_msec_ = tr_time_msec();
        pulse_duration_msec_ = period_msec;
        pulse_deadline_msec_ = pulse_start_msec_ + period_msec;
        current_pulse_outcome_ = {};
        current_pulse_budget_ = {};

        auto pulse = tr_strict_bandwidth_curve_pulse{};
        pulse.start_msec = pulse_start_msec_;
        pulse.duration_msec = pulse_duration_msec_;

        for (auto const dir : { TR_UP, TR_DOWN })
        {
            if (!session_.top_bandwidth_.isLimited(dir))
            {
                continue;
            }

            auto const pulse_budget = static_cast<size_t>(
                session_.top_bandwidth_.getDesiredSpeedBytesPerSecond(dir) * period_msec / 1000U);
            current_pulse_budget_[direction_index(dir)] = pulse_budget;
            if (dir == TR_UP)
            {
                pulse.up_budget = pulse_budget;
            }
            else
            {
                pulse.down_budget = pulse_budget;
            }
        }

        retention_policy_->on_pulse_start(pulse);
    }

    [[nodiscard]] auto retention_policy_applies(tr_direction dir, tr_peerIo const& io) const noexcept
    {
        return io.bandwidth().honorsAncestor(dir, &session_.top_bandwidth_);
    }

    [[nodiscard]] auto retained_piece_admit(tr_direction dir, tr_priority_t priority, tr_peerIo const& io) const
    {
        auto query = tr_strict_bandwidth_curve_query{};
        query.dir = dir;
        query.priority = priority;
        query.now_msec = tr_time_msec();
        query.applies = retention_policy_applies(dir, io);
        return retention_policy_->admit(query);
    }

    void charge_retained_piece_bytes(tr_direction dir, tr_priority_t priority, tr_peerIo const& io, size_t piece_bytes)
    {
        current_pulse_outcome_.note_piece_bytes(dir, priority, piece_bytes);

        auto charge = tr_strict_bandwidth_curve_charge{};
        charge.dir = dir;
        charge.priority = priority;
        charge.piece_bytes = piece_bytes;
        charge.applies = retention_policy_applies(dir, io);
        retention_policy_->charge(charge);
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

            auto const admission = retained_piece_admit(TR_DOWN, priority, *queue.front());
            if (admission.piece_limit == 0U)
            {
                current_pulse_outcome_.note_blocked(TR_DOWN, priority);
                gated_wakeup_msec = min_nonzero(gated_wakeup_msec, admission.next_wakeup_msec);
                queue.push_back(std::move(queue.front()));
                queue.pop_front();
                continue;
            }

            auto io = std::move(queue.front());
            queue.pop_front();
            queued_reads_.erase(io.get());

            auto const result = io->flush_with_result(TR_DOWN, admission.piece_limit);
            charge_retained_piece_bytes(TR_DOWN, priority, *io, result.piece_bytes);

            if (result.bytes_transferred == admission.piece_limit && io->has_bandwidth_left(TR_DOWN))
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

            auto const admission = retained_piece_admit(TR_UP, priority, *queue.front());
            if (admission.piece_limit == 0U)
            {
                current_pulse_outcome_.note_blocked(TR_UP, priority);
            }

            if (admission.piece_limit == 0U && !queue.front()->has_pending_protocol_output())
            {
                gated_wakeup_msec = min_nonzero(gated_wakeup_msec, admission.next_wakeup_msec);
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
            if (flushed_all_protocol && io->has_output_buffered() && admission.piece_limit != 0U)
            {
                piece_result = io->flush_with_result(TR_UP, admission.piece_limit);
                charge_retained_piece_bytes(TR_UP, priority, *io, piece_result.piece_bytes);
            }

            if (io->has_output_buffered() && io->has_bandwidth_left(TR_UP))
            {
                if ((admission.piece_limit == 0U && flushed_all_protocol) ||
                    (admission.piece_limit != 0U && flushed_all_protocol &&
                     piece_result.bytes_transferred == admission.piece_limit))
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
    std::unique_ptr<tr_strict_bandwidth_curve_policy> retention_policy_;
    std::unique_ptr<libtransmission::Timer> continue_drain_timer_;
    ReadQueues read_queues_ = {};
    WriteQueues write_queues_ = {};
    std::unordered_set<tr_peerIo*> queued_reads_;
    std::unordered_set<tr_peerIo*> queued_writes_;
    tr_strict_bandwidth_curve_pulse_outcome current_pulse_outcome_ = {};
    std::array<size_t, 2> current_pulse_budget_ = {};
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
