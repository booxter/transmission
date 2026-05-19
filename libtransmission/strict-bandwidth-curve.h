// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "bandwidth.h"

struct tr_strict_bandwidth_curve_pulse
{
    uint64_t start_msec = 0U;
    uint64_t duration_msec = 0U;
    size_t up_budget = 0U;
    size_t down_budget = 0U;
};

struct tr_strict_bandwidth_curve_query
{
    tr_direction dir = TR_UP;
    tr_priority_t priority = TR_PRI_NORMAL;
    uint64_t now_msec = 0U;
    bool applies = false;
};

struct tr_strict_bandwidth_curve_charge
{
    tr_direction dir = TR_UP;
    tr_priority_t priority = TR_PRI_NORMAL;
    size_t piece_bytes = 0U;
    bool applies = false;
};

struct tr_strict_bandwidth_curve_admit_result
{
    size_t piece_limit = 0U;
    uint64_t next_wakeup_msec = 0U;
};

enum class tr_strict_bandwidth_curve_adjustment : uint8_t
{
    Hold,
    Tighten,
    Relax,
};

struct tr_strict_bandwidth_curve_pulse_outcome
{
    struct DirectionState
    {
        std::array<size_t, 3> piece_bytes = {};
        std::array<bool, 3> had_blocked_work = {};
        std::array<bool, 3> has_pending_work = {};
    };

    std::array<DirectionState, 2> by_direction = {};

    void note_piece_bytes(tr_direction dir, tr_priority_t priority, size_t n_bytes) noexcept
    {
        by_direction[direction_index(dir)].piece_bytes[priority_index(priority)] += n_bytes;
    }

    void note_blocked(tr_direction dir, tr_priority_t priority) noexcept
    {
        by_direction[direction_index(dir)].had_blocked_work[priority_index(priority)] = true;
    }

    void note_pending(tr_direction dir, tr_priority_t priority) noexcept
    {
        by_direction[direction_index(dir)].has_pending_work[priority_index(priority)] = true;
    }

private:
    [[nodiscard]] static constexpr size_t direction_index(tr_direction dir) noexcept
    {
        return dir == TR_UP ? size_t{ 0U } : size_t{ 1U };
    }

    [[nodiscard]] static constexpr size_t priority_index(tr_priority_t priority) noexcept
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
};

struct tr_strict_bandwidth_curve_policy_snapshot
{
    struct DirectionState
    {
        double normal_low_exponent = 0.0;
        double low_exponent = 0.0;
        size_t window_pulses = 0U;
        size_t fully_utilized_pulses = 0U;
        size_t high_pressure_pulses = 0U;
        size_t underfilled_lower_demand_pulses = 0U;
        tr_strict_bandwidth_curve_adjustment last_adjustment = tr_strict_bandwidth_curve_adjustment::Hold;
    };

    bool is_dynamic = false;
    std::array<DirectionState, 2> by_direction = {};
};

class tr_strict_bandwidth_curve_policy
{
public:
    virtual ~tr_strict_bandwidth_curve_policy() = default;

    tr_strict_bandwidth_curve_policy(tr_strict_bandwidth_curve_policy&&) = delete;
    tr_strict_bandwidth_curve_policy(tr_strict_bandwidth_curve_policy const&) = delete;
    tr_strict_bandwidth_curve_policy& operator=(tr_strict_bandwidth_curve_policy&&) = delete;
    tr_strict_bandwidth_curve_policy& operator=(tr_strict_bandwidth_curve_policy const&) = delete;

    [[nodiscard]] static std::unique_ptr<tr_strict_bandwidth_curve_policy> create(
        tr_strict_bandwidth_curve curve,
        size_t execution_increment,
        size_t release_quantum);

    virtual void on_pulse_start(tr_strict_bandwidth_curve_pulse const& pulse) = 0;
    [[nodiscard]] virtual tr_strict_bandwidth_curve_admit_result admit(tr_strict_bandwidth_curve_query const& query) const = 0;
    virtual void charge(tr_strict_bandwidth_curve_charge const& charge) = 0;
    virtual void on_pulse_finish(tr_strict_bandwidth_curve_pulse_outcome const& outcome) = 0;
    [[nodiscard]] virtual tr_strict_bandwidth_curve_policy_snapshot snapshot() const = 0;

protected:
    tr_strict_bandwidth_curve_policy() = default;
};
