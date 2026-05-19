// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

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

struct tr_strict_bandwidth_curve_pulse_outcome
{
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

protected:
    tr_strict_bandwidth_curve_policy() = default;
};
