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

class tr_fixed_strict_bandwidth_curve_policy final : public tr_strict_bandwidth_curve_policy
{
private:
    struct LimitedRetentionState
    {
        size_t pulse_budget = 0U;
        size_t normal_low_piece = 0U;
        size_t low_piece = 0U;
    };

public:
    explicit tr_fixed_strict_bandwidth_curve_policy(
        tr_strict_bandwidth_curve curve,
        size_t execution_increment,
        size_t release_quantum)
        : params_{ strict_curve_parameters(curve) }
        , execution_increment_{ execution_increment }
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

        auto const& retention = retention_[direction_index(query.dir)];
        auto const normal_low_allowed = released_bytes(query.dir, params_.normal_low_exponent, query.now_msec);
        auto const normal_low_remaining = normal_low_allowed > retention.normal_low_piece ?
            normal_low_allowed - retention.normal_low_piece :
            0U;
        auto const normal_low_target = next_retention_target(retention.normal_low_piece, retention.pulse_budget);
        auto const normal_low_release_increment = normal_low_target - retention.normal_low_piece;

        if (query.priority == TR_PRI_NORMAL)
        {
            if (normal_low_remaining < normal_low_release_increment && query.now_msec < pulse_deadline_msec_)
            {
                return { 0U,
                         next_release_msec(
                             retention.normal_low_piece,
                             retention.pulse_budget,
                             params_.normal_low_exponent,
                             query.now_msec) };
            }

            return { std::min(execution_increment_, normal_low_remaining), 0U };
        }

        auto const low_allowed = released_bytes(query.dir, params_.low_exponent, query.now_msec);
        auto const low_remaining = low_allowed > retention.low_piece ? low_allowed - retention.low_piece : 0U;
        auto const low_target = next_retention_target(retention.low_piece, retention.pulse_budget);
        auto const low_release_increment = low_target - retention.low_piece;

        if ((normal_low_remaining < normal_low_release_increment || low_remaining < low_release_increment) &&
            query.now_msec < pulse_deadline_msec_)
        {
            return {
                0U,
                std::max(
                    next_release_msec(
                        retention.normal_low_piece,
                        retention.pulse_budget,
                        params_.normal_low_exponent,
                        query.now_msec),
                    next_release_msec(retention.low_piece, retention.pulse_budget, params_.low_exponent, query.now_msec))
            };
        }

        return { std::min(execution_increment_, std::min(normal_low_remaining, low_remaining)), 0U };
    }

    void charge(tr_strict_bandwidth_curve_charge const& charge_info) override
    {
        if (charge_info.piece_bytes == 0U || charge_info.priority == TR_PRI_HIGH || !charge_info.applies)
        {
            return;
        }

        auto& retention = retention_[direction_index(charge_info.dir)];
        retention.normal_low_piece = std::min(retention.pulse_budget, retention.normal_low_piece + charge_info.piece_bytes);
        if (charge_info.priority == TR_PRI_LOW)
        {
            retention.low_piece = std::min(retention.pulse_budget, retention.low_piece + charge_info.piece_bytes);
        }
    }

    void on_pulse_finish(tr_strict_bandwidth_curve_pulse_outcome const&) override
    {
    }

private:
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
    StrictCurveParameters params_;
    size_t execution_increment_;
    size_t release_quantum_;
    uint64_t pulse_start_msec_ = 0U;
    uint64_t pulse_duration_msec_ = 0U;
    uint64_t pulse_deadline_msec_ = 0U;
    std::array<LimitedRetentionState, 2> retention_ = {};
};

} // namespace

std::unique_ptr<tr_strict_bandwidth_curve_policy> tr_strict_bandwidth_curve_policy::create(
    tr_strict_bandwidth_curve curve,
    size_t execution_increment,
    size_t release_quantum)
{
    return std::make_unique<tr_fixed_strict_bandwidth_curve_policy>(curve, execution_increment, release_quantum);
}
