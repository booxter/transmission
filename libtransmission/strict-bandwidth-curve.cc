// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "strict-bandwidth-curve.h"

namespace
{

[[nodiscard]] constexpr auto direction_index(tr_direction dir) noexcept
{
    return dir == TR_UP ? size_t{ 0U } : size_t{ 1U };
}

struct StrictCurveParameters
{
    double normal_low_exponent;
    double low_exponent;
};

[[nodiscard]] auto make_snapshot_state(
    StrictCurveParameters params,
    size_t window_pulses = 0U,
    size_t fully_utilized_pulses = 0U,
    size_t high_pressure_pulses = 0U,
    size_t underfilled_lower_demand_pulses = 0U,
    tr_strict_bandwidth_curve_adjustment last_adjustment = tr_strict_bandwidth_curve_adjustment::Hold)
{
    auto state = tr_strict_bandwidth_curve_policy_snapshot::DirectionState{};
    state.normal_low_exponent = params.normal_low_exponent;
    state.low_exponent = params.low_exponent;
    state.window_pulses = window_pulses;
    state.fully_utilized_pulses = fully_utilized_pulses;
    state.high_pressure_pulses = high_pressure_pulses;
    state.underfilled_lower_demand_pulses = underfilled_lower_demand_pulses;
    state.last_adjustment = last_adjustment;
    return state;
}

[[nodiscard]] constexpr auto relaxed_curve_parameters() noexcept
{
    return StrictCurveParameters{ 1.5, 3.0 };
}

[[nodiscard]] constexpr auto balanced_curve_parameters() noexcept
{
    return StrictCurveParameters{ 2.0, 4.0 };
}

[[nodiscard]] constexpr auto aggressive_curve_parameters() noexcept
{
    return StrictCurveParameters{ 3.0, 6.0 };
}

[[nodiscard]] constexpr auto preset_curve_parameters(tr_strict_bandwidth_curve curve) noexcept
{
    switch (curve)
    {
    case tr_strict_bandwidth_curve::Relaxed:
        return relaxed_curve_parameters();

    case tr_strict_bandwidth_curve::Aggressive:
        return aggressive_curve_parameters();

    case tr_strict_bandwidth_curve::Balanced:
    case tr_strict_bandwidth_curve::Dynamic:
    default:
        return balanced_curve_parameters();
    }
}

class tr_power_strict_bandwidth_curve_policy : public tr_strict_bandwidth_curve_policy
{
private:
    struct LimitedRetentionState
    {
        size_t pulse_budget = 0U;
        size_t high_piece = 0U;
        size_t normal_low_piece = 0U;
    };

public:
    explicit tr_power_strict_bandwidth_curve_policy(size_t execution_increment, size_t release_quantum)
        : execution_increment_{ execution_increment }
        , release_quantum_{ release_quantum }
    {
    }

    void on_pulse_start(tr_strict_bandwidth_curve_pulse const& pulse) override
    {
        pulse_start_msec_ = pulse.start_msec;
        pulse_duration_msec_ = pulse.duration_msec;
        pulse_deadline_msec_ = pulse.start_msec + pulse.duration_msec;
        retention_ = {};
        retention_[direction_index(TR_UP)].pulse_budget = pulse.up_budget;
        retention_[direction_index(TR_DOWN)].pulse_budget = pulse.down_budget;
    }

    [[nodiscard]] tr_strict_bandwidth_curve_admit_result admit(tr_strict_bandwidth_curve_query const& query) const override
    {
        if (query.priority == TR_PRI_HIGH || !limited_retention_applies(query))
        {
            return { execution_increment_, 0U };
        }

        auto const params = parameters(query.dir);
        auto const& retention = retention_[direction_index(query.dir)];
        auto const normal_low_allowed = released_bytes(query.dir, params.normal_low_exponent, query.now_msec);
        auto const shared_consumed = retained_lower_consumed(retention);
        auto const normal_low_remaining = normal_low_allowed > shared_consumed ? normal_low_allowed - shared_consumed : 0U;
        auto const normal_low_target = next_retention_target(shared_consumed, retention.pulse_budget);
        auto const normal_low_release_increment = normal_low_target - shared_consumed;

        if (query.priority == TR_PRI_NORMAL)
        {
            if (normal_low_remaining < normal_low_release_increment && query.now_msec < pulse_deadline_msec_)
            {
                return {
                    0U,
                    next_release_msec(shared_consumed, retention.pulse_budget, params.normal_low_exponent, query.now_msec)
                };
            }

            return { std::min(execution_increment_, normal_low_remaining), 0U };
        }

        auto const low_allowed = released_bytes(query.dir, params.low_exponent, query.now_msec);
        auto const low_remaining = low_allowed > shared_consumed ? low_allowed - shared_consumed : 0U;
        auto const low_target = next_retention_target(shared_consumed, retention.pulse_budget);
        auto const low_release_increment = low_target - shared_consumed;

        if ((normal_low_remaining < normal_low_release_increment || low_remaining < low_release_increment) &&
            query.now_msec < pulse_deadline_msec_)
        {
            return { 0U,
                     std::max(
                         next_release_msec(shared_consumed, retention.pulse_budget, params.normal_low_exponent, query.now_msec),
                         next_release_msec(shared_consumed, retention.pulse_budget, params.low_exponent, query.now_msec)) };
        }

        return { std::min(execution_increment_, std::min(normal_low_remaining, low_remaining)), 0U };
    }

    void charge(tr_strict_bandwidth_curve_charge const& charge_info) override
    {
        if (charge_info.piece_bytes == 0U || !charge_info.applies)
        {
            return;
        }

        auto& retention = retention_[direction_index(charge_info.dir)];
        if (charge_info.priority == TR_PRI_HIGH)
        {
            retention.high_piece = std::min(retention.pulse_budget, retention.high_piece + charge_info.piece_bytes);
        }
        else
        {
            retention.normal_low_piece = std::min(retention.pulse_budget, retention.normal_low_piece + charge_info.piece_bytes);
        }
    }

protected:
    [[nodiscard]] virtual StrictCurveParameters parameters(tr_direction dir) const noexcept = 0;

    [[nodiscard]] auto pulse_budget(tr_direction dir) const noexcept
    {
        return retention_[direction_index(dir)].pulse_budget;
    }

private:
    [[nodiscard]] static size_t retained_lower_consumed(LimitedRetentionState const& retention) noexcept
    {
        return std::min(retention.pulse_budget, retention.high_piece + retention.normal_low_piece);
    }

    [[nodiscard]] size_t next_retention_target(size_t consumed_piece, size_t pulse_budget) const noexcept
    {
        if (consumed_piece >= pulse_budget)
        {
            return pulse_budget;
        }

        auto const remaining_piece = pulse_budget - consumed_piece;
        auto const release_increment = std::min(remaining_piece, release_quantum_);
        return std::min(pulse_budget, consumed_piece + release_increment);
    }

    [[nodiscard]] bool limited_retention_applies(tr_strict_bandwidth_curve_query const& query) const noexcept
    {
        return pulse_duration_msec_ != 0U && retention_[direction_index(query.dir)].pulse_budget != 0U && query.applies;
    }

    [[nodiscard]] size_t released_bytes(tr_direction dir, double exponent, uint64_t now_msec) const noexcept
    {
        auto const& retention = retention_[direction_index(dir)];

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

    [[nodiscard]] uint64_t next_release_msec(size_t consumed_piece, size_t pulse_budget, double exponent, uint64_t now_msec)
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

private:
    size_t execution_increment_;
    size_t release_quantum_;
    uint64_t pulse_start_msec_ = 0U;
    uint64_t pulse_duration_msec_ = 0U;
    uint64_t pulse_deadline_msec_ = 0U;
    std::array<LimitedRetentionState, 2> retention_ = {};
};

class tr_fixed_strict_bandwidth_curve_policy final : public tr_power_strict_bandwidth_curve_policy
{
public:
    explicit tr_fixed_strict_bandwidth_curve_policy(
        StrictCurveParameters params,
        size_t execution_increment,
        size_t release_quantum)
        : tr_power_strict_bandwidth_curve_policy{ execution_increment, release_quantum }
        , params_{ params }
    {
    }

    void on_pulse_finish(tr_strict_bandwidth_curve_pulse_outcome const&) override
    {
    }

    [[nodiscard]] tr_strict_bandwidth_curve_policy_snapshot snapshot() const override
    {
        auto snapshot = tr_strict_bandwidth_curve_policy_snapshot{};
        snapshot.by_direction[direction_index(TR_UP)] = make_snapshot_state(params_);
        snapshot.by_direction[direction_index(TR_DOWN)] = make_snapshot_state(params_);
        return snapshot;
    }

protected:
    [[nodiscard]] StrictCurveParameters parameters(tr_direction) const noexcept override
    {
        return params_;
    }

private:
    StrictCurveParameters params_;
};

class tr_dynamic_strict_bandwidth_curve_policy final : public tr_power_strict_bandwidth_curve_policy
{
private:
    struct DynamicDirectionState
    {
        StrictCurveParameters params = balanced_curve_parameters();
        size_t window_pulses = 0U;
        size_t fully_utilized_pulses = 0U;
        size_t high_pressure_pulses = 0U;
        size_t underfilled_lower_demand_pulses = 0U;
        tr_strict_bandwidth_curve_adjustment last_adjustment = tr_strict_bandwidth_curve_adjustment::Hold;
    };

    static auto constexpr DynamicWindowPulses = size_t{ 4U };
    static constexpr auto DynamicStep = StrictCurveParameters{ 0.25, 0.5 };
    static constexpr auto DynamicMax = StrictCurveParameters{ 8.0, 16.0 };

public:
    explicit tr_dynamic_strict_bandwidth_curve_policy(size_t execution_increment, size_t release_quantum)
        : tr_power_strict_bandwidth_curve_policy{ execution_increment, release_quantum }
    {
    }

    void on_pulse_finish(tr_strict_bandwidth_curve_pulse_outcome const& outcome) override
    {
        for (auto const dir : { TR_UP, TR_DOWN })
        {
            update_direction(dir, outcome.by_direction[direction_index(dir)]);
        }
    }

    [[nodiscard]] tr_strict_bandwidth_curve_policy_snapshot snapshot() const override
    {
        auto snapshot = tr_strict_bandwidth_curve_policy_snapshot{};
        snapshot.is_dynamic = true;
        for (auto const dir : { TR_UP, TR_DOWN })
        {
            auto const& state = dynamic_[direction_index(dir)];
            snapshot.by_direction[direction_index(dir)] = make_snapshot_state(
                state.params,
                state.window_pulses,
                state.fully_utilized_pulses,
                state.high_pressure_pulses,
                state.underfilled_lower_demand_pulses,
                state.last_adjustment);
        }

        return snapshot;
    }

protected:
    [[nodiscard]] StrictCurveParameters parameters(tr_direction dir) const noexcept override
    {
        return dynamic_[direction_index(dir)].params;
    }

private:
    static void reset_window(DynamicDirectionState& state) noexcept
    {
        state.window_pulses = 0U;
        state.fully_utilized_pulses = 0U;
        state.high_pressure_pulses = 0U;
        state.underfilled_lower_demand_pulses = 0U;
    }

    static void tighten(DynamicDirectionState& state) noexcept
    {
        state.params.normal_low_exponent = std::min(
            DynamicMax.normal_low_exponent,
            state.params.normal_low_exponent + DynamicStep.normal_low_exponent);
        state.params.low_exponent = std::min(DynamicMax.low_exponent, state.params.low_exponent + DynamicStep.low_exponent);
    }

    static void relax(DynamicDirectionState& state) noexcept
    {
        auto const min_params = relaxed_curve_parameters();
        state.params.normal_low_exponent = std::max(
            min_params.normal_low_exponent,
            state.params.normal_low_exponent - DynamicStep.normal_low_exponent);
        state.params.low_exponent = std::max(min_params.low_exponent, state.params.low_exponent - DynamicStep.low_exponent);
    }

    void update_direction(tr_direction dir, tr_strict_bandwidth_curve_pulse_outcome::DirectionState const& outcome)
    {
        auto& state = dynamic_[direction_index(dir)];
        auto const budget = pulse_budget(dir);

        if (budget == 0U)
        {
            reset_window(state);
            state.params = balanced_curve_parameters();
            state.last_adjustment = tr_strict_bandwidth_curve_adjustment::Hold;
            return;
        }

        auto const total_piece = outcome.piece_bytes[0] + outcome.piece_bytes[1] + outcome.piece_bytes[2];
        auto const lower_demand = outcome.piece_bytes[1] != 0U || outcome.piece_bytes[2] != 0U || outcome.had_blocked_work[1] ||
            outcome.had_blocked_work[2] || outcome.has_pending_work[1] || outcome.has_pending_work[2];
        auto const high_pressure = outcome.had_blocked_work[0] || outcome.has_pending_work[0];
        auto const fully_utilized = lower_demand && total_piece * 100U >= budget * 98U;
        auto const underfilled_with_lower_demand = lower_demand && total_piece * 100U < budget * 95U;

        ++state.window_pulses;
        state.fully_utilized_pulses += fully_utilized ? 1U : 0U;
        state.high_pressure_pulses += high_pressure ? 1U : 0U;
        state.underfilled_lower_demand_pulses += underfilled_with_lower_demand ? 1U : 0U;

        if (state.window_pulses < DynamicWindowPulses)
        {
            return;
        }

        state.last_adjustment = tr_strict_bandwidth_curve_adjustment::Hold;
        if (state.underfilled_lower_demand_pulses * 2U >= state.window_pulses)
        {
            relax(state);
            state.last_adjustment = tr_strict_bandwidth_curve_adjustment::Relax;
        }
        else if (
            state.fully_utilized_pulses * 4U >= state.window_pulses * 3U &&
            state.high_pressure_pulses * 2U >= state.window_pulses)
        {
            tighten(state);
            state.last_adjustment = tr_strict_bandwidth_curve_adjustment::Tighten;
        }

        reset_window(state);
    }

private:
    std::array<DynamicDirectionState, 2> dynamic_ = {};
};

} // namespace

std::unique_ptr<tr_strict_bandwidth_curve_policy> tr_strict_bandwidth_curve_policy::create(
    tr_strict_bandwidth_curve curve,
    size_t execution_increment,
    size_t release_quantum)
{
    switch (curve)
    {
    case tr_strict_bandwidth_curve::Dynamic:
        return std::make_unique<tr_dynamic_strict_bandwidth_curve_policy>(execution_increment, release_quantum);

    case tr_strict_bandwidth_curve::Relaxed:
    case tr_strict_bandwidth_curve::Balanced:
    case tr_strict_bandwidth_curve::Aggressive:
    default:
        return std::make_unique<tr_fixed_strict_bandwidth_curve_policy>(
            preset_curve_parameters(curve),
            execution_increment,
            release_quantum);
    }
}
