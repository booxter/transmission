// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <gtest/gtest.h>

#include "libtransmission/strict-bandwidth-curve.h"

using namespace std::literals;

namespace libtransmission::test
{

namespace
{

auto make_pulse()
{
    auto pulse = tr_strict_bandwidth_curve_pulse{};
    pulse.start_msec = 1000U;
    pulse.duration_msec = 500U;
    pulse.up_budget = 6000U;
    return pulse;
}

void feed_pulse_outcome(
    tr_strict_bandwidth_curve_policy& policy,
    tr_strict_bandwidth_curve_pulse_outcome const& outcome,
    size_t n_pulses)
{
    auto const pulse = make_pulse();
    for (size_t i = 0U; i < n_pulses; ++i)
    {
        policy.on_pulse_start(pulse);
        policy.on_pulse_finish(outcome);
    }
}

} // namespace

TEST(StrictBandwidthCurvePolicyTest, highPriorityIsAdmittedImmediately)
{
    auto policy = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Balanced, 3000U, 1024U);

    auto pulse = make_pulse();
    policy->on_pulse_start(pulse);

    auto query = tr_strict_bandwidth_curve_query{};
    query.dir = TR_UP;
    query.priority = TR_PRI_HIGH;
    query.now_msec = 1000U;
    query.applies = true;
    auto const result = policy->admit(query);

    EXPECT_EQ(3000U, result.piece_limit);
    EXPECT_EQ(0U, result.next_wakeup_msec);
}

TEST(StrictBandwidthCurvePolicyTest, normalPriorityIsGatedEarlyInPulse)
{
    auto policy = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Balanced, 3000U, 1024U);

    auto pulse = make_pulse();
    policy->on_pulse_start(pulse);

    auto query = tr_strict_bandwidth_curve_query{};
    query.dir = TR_UP;
    query.priority = TR_PRI_NORMAL;
    query.now_msec = 1000U;
    query.applies = true;
    auto const result = policy->admit(query);

    EXPECT_EQ(0U, result.piece_limit);
    EXPECT_GT(result.next_wakeup_msec, 1000U);
    EXPECT_LT(result.next_wakeup_msec, 1500U);
}

TEST(StrictBandwidthCurvePolicyTest, lowPriorityIsReleasedMoreSlowlyThanNormal)
{
    auto policy = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Balanced, 3000U, 1024U);

    auto pulse = make_pulse();
    policy->on_pulse_start(pulse);

    auto charge = tr_strict_bandwidth_curve_charge{};
    charge.dir = TR_UP;
    charge.priority = TR_PRI_LOW;
    charge.piece_bytes = 1024U;
    charge.applies = true;
    policy->charge(charge);

    auto normal_query = tr_strict_bandwidth_curve_query{};
    normal_query.dir = TR_UP;
    normal_query.priority = TR_PRI_NORMAL;
    normal_query.now_msec = 1325U;
    normal_query.applies = true;
    auto const normal = policy->admit(normal_query);

    auto low_query = tr_strict_bandwidth_curve_query{};
    low_query.dir = TR_UP;
    low_query.priority = TR_PRI_LOW;
    low_query.now_msec = 1325U;
    low_query.applies = true;
    auto const low = policy->admit(low_query);

    EXPECT_GT(normal.piece_limit, 0U);
    EXPECT_EQ(0U, low.piece_limit);
    EXPECT_GT(low.next_wakeup_msec, 1325U);
}

TEST(StrictBandwidthCurvePolicyTest, lowChargeAdvancesBothEnvelopes)
{
    auto policy = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Balanced, 3000U, 1024U);

    auto pulse = make_pulse();
    policy->on_pulse_start(pulse);

    auto charge = tr_strict_bandwidth_curve_charge{};
    charge.dir = TR_UP;
    charge.priority = TR_PRI_LOW;
    charge.piece_bytes = 1024U;
    charge.applies = true;
    policy->charge(charge);

    auto query = tr_strict_bandwidth_curve_query{};
    query.dir = TR_UP;
    query.priority = TR_PRI_LOW;
    query.now_msec = 1499U;
    query.applies = true;
    auto const low = policy->admit(query);

    EXPECT_GT(low.piece_limit, 0U);
    EXPECT_EQ(0U, low.next_wakeup_msec);
}

TEST(StrictBandwidthCurvePolicyTest, highChargeDelaysNormalRelease)
{
    auto policy = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Balanced, 3000U, 1024U);

    auto pulse = make_pulse();
    policy->on_pulse_start(pulse);

    auto charge = tr_strict_bandwidth_curve_charge{};
    charge.dir = TR_UP;
    charge.priority = TR_PRI_HIGH;
    charge.piece_bytes = 2048U;
    charge.applies = true;
    policy->charge(charge);

    auto query = tr_strict_bandwidth_curve_query{};
    query.dir = TR_UP;
    query.priority = TR_PRI_NORMAL;
    query.now_msec = 1325U;
    query.applies = true;
    auto const normal = policy->admit(query);

    EXPECT_EQ(0U, normal.piece_limit);
    EXPECT_GT(normal.next_wakeup_msec, 1325U);
}

TEST(StrictBandwidthCurvePolicyTest, highChargeDelaysLowRelease)
{
    auto policy = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Balanced, 3000U, 1024U);

    auto pulse = make_pulse();
    policy->on_pulse_start(pulse);

    auto charge = tr_strict_bandwidth_curve_charge{};
    charge.dir = TR_UP;
    charge.priority = TR_PRI_HIGH;
    charge.piece_bytes = 3072U;
    charge.applies = true;
    policy->charge(charge);

    auto query = tr_strict_bandwidth_curve_query{};
    query.dir = TR_UP;
    query.priority = TR_PRI_LOW;
    query.now_msec = 1450U;
    query.applies = true;
    auto const low = policy->admit(query);

    EXPECT_EQ(0U, low.piece_limit);
    EXPECT_GT(low.next_wakeup_msec, 1450U);
}

TEST(StrictBandwidthCurvePolicyTest, NonApplyingWorkIsNeverCurveGated)
{
    auto policy = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Aggressive, 3000U, 1024U);

    auto pulse = make_pulse();
    policy->on_pulse_start(pulse);

    auto query = tr_strict_bandwidth_curve_query{};
    query.dir = TR_UP;
    query.priority = TR_PRI_LOW;
    query.now_msec = 1000U;
    query.applies = false;
    auto const result = policy->admit(query);

    EXPECT_EQ(3000U, result.piece_limit);
    EXPECT_EQ(0U, result.next_wakeup_msec);
}

TEST(StrictBandwidthCurvePolicyTest, fixedSnapshotReportsPresetParameters)
{
    auto policy = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Aggressive, 3000U, 1024U);

    auto const snapshot = policy->snapshot();

    EXPECT_FALSE(snapshot.is_dynamic);
    EXPECT_DOUBLE_EQ(3.0, snapshot.by_direction[0].normal_low_exponent);
    EXPECT_DOUBLE_EQ(6.0, snapshot.by_direction[0].low_exponent);
    EXPECT_EQ(tr_strict_bandwidth_curve_adjustment::Hold, snapshot.by_direction[0].last_adjustment);
}

TEST(StrictBandwidthCurvePolicyTest, snapshotCapturesLowerReleaseProgress)
{
    auto policy = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Balanced, 3000U, 1024U);

    auto const pulse = make_pulse();
    policy->on_pulse_start(pulse);

    auto query = tr_strict_bandwidth_curve_query{};
    query.dir = TR_UP;
    query.priority = TR_PRI_NORMAL;
    query.now_msec = 1325U;
    query.applies = true;
    auto const admit = policy->admit(query);

    auto charge = tr_strict_bandwidth_curve_charge{};
    charge.dir = TR_UP;
    charge.priority = TR_PRI_NORMAL;
    charge.piece_bytes = std::min<size_t>(1024U, admit.piece_limit);
    charge.applies = true;
    policy->charge(charge);

    auto const snapshot = policy->snapshot();
    auto const& state = snapshot.by_direction[0];

    EXPECT_EQ(6000U, state.pulse_budget);
    EXPECT_EQ(charge.piece_bytes, state.lower_piece_bytes);
    EXPECT_GT(state.normal_low_max_allowed, 0U);
    EXPECT_GT(state.normal_low_max_remaining, 0U);
    EXPECT_EQ(325U, state.first_normal_grant_msec);
    EXPECT_EQ(0U, state.first_low_grant_msec);
}

TEST(StrictBandwidthCurvePolicyTest, dynamicStartsFromBalancedCurve)
{
    auto dynamic = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Dynamic, 3000U, 1024U);
    auto balanced = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Balanced, 3000U, 1024U);

    auto const pulse = make_pulse();
    dynamic->on_pulse_start(pulse);
    balanced->on_pulse_start(pulse);

    auto query = tr_strict_bandwidth_curve_query{};
    query.dir = TR_UP;
    query.priority = TR_PRI_NORMAL;
    query.now_msec = 1000U;
    query.applies = true;

    auto const dynamic_result = dynamic->admit(query);
    auto const balanced_result = balanced->admit(query);

    EXPECT_EQ(balanced_result.piece_limit, dynamic_result.piece_limit);
    EXPECT_EQ(balanced_result.next_wakeup_msec, dynamic_result.next_wakeup_msec);
}

TEST(StrictBandwidthCurvePolicyTest, dynamicSnapshotReportsWindowStateAndAdjustment)
{
    auto dynamic = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Dynamic, 3000U, 1024U);

    auto outcome = tr_strict_bandwidth_curve_pulse_outcome{};
    outcome.note_piece_bytes(TR_UP, TR_PRI_HIGH, 5000U);
    outcome.note_piece_bytes(TR_UP, TR_PRI_NORMAL, 1000U);
    outcome.note_pending(TR_UP, TR_PRI_HIGH);
    feed_pulse_outcome(*dynamic, outcome, 4U);

    auto const snapshot = dynamic->snapshot();

    EXPECT_TRUE(snapshot.is_dynamic);
    EXPECT_GT(snapshot.by_direction[0].normal_low_exponent, 2.0);
    EXPECT_EQ(0U, snapshot.by_direction[0].window_pulses);
    EXPECT_EQ(tr_strict_bandwidth_curve_adjustment::Tighten, snapshot.by_direction[0].last_adjustment);
}

TEST(StrictBandwidthCurvePolicyTest, dynamicTightensAfterSustainedHighPressure)
{
    auto dynamic = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Dynamic, 3000U, 1024U);
    auto balanced = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Balanced, 3000U, 1024U);

    auto outcome = tr_strict_bandwidth_curve_pulse_outcome{};
    outcome.note_piece_bytes(TR_UP, TR_PRI_HIGH, 5000U);
    outcome.note_piece_bytes(TR_UP, TR_PRI_NORMAL, 1000U);
    outcome.note_pending(TR_UP, TR_PRI_HIGH);
    feed_pulse_outcome(*dynamic, outcome, 4U);

    auto const pulse = make_pulse();
    dynamic->on_pulse_start(pulse);
    balanced->on_pulse_start(pulse);

    auto query = tr_strict_bandwidth_curve_query{};
    query.dir = TR_UP;
    query.priority = TR_PRI_NORMAL;
    query.now_msec = 1000U;
    query.applies = true;

    auto const dynamic_result = dynamic->admit(query);
    auto const balanced_result = balanced->admit(query);

    EXPECT_EQ(0U, dynamic_result.piece_limit);
    EXPECT_EQ(0U, balanced_result.piece_limit);
    EXPECT_GT(dynamic_result.next_wakeup_msec, balanced_result.next_wakeup_msec);
}

TEST(StrictBandwidthCurvePolicyTest, dynamicRelaxesAfterRepeatedUnderfillWithLowerDemand)
{
    auto dynamic = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Dynamic, 3000U, 1024U);
    auto balanced = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Balanced, 3000U, 1024U);

    auto outcome = tr_strict_bandwidth_curve_pulse_outcome{};
    outcome.note_piece_bytes(TR_UP, TR_PRI_HIGH, 1000U);
    outcome.note_piece_bytes(TR_UP, TR_PRI_NORMAL, 1000U);
    outcome.note_pending(TR_UP, TR_PRI_NORMAL);
    feed_pulse_outcome(*dynamic, outcome, 8U);

    auto const pulse = make_pulse();
    dynamic->on_pulse_start(pulse);
    balanced->on_pulse_start(pulse);

    auto query = tr_strict_bandwidth_curve_query{};
    query.dir = TR_UP;
    query.priority = TR_PRI_NORMAL;
    query.now_msec = 1000U;
    query.applies = true;

    auto const dynamic_result = dynamic->admit(query);
    auto const balanced_result = balanced->admit(query);

    EXPECT_EQ(0U, dynamic_result.piece_limit);
    EXPECT_EQ(0U, balanced_result.piece_limit);
    EXPECT_LT(dynamic_result.next_wakeup_msec, balanced_result.next_wakeup_msec);
}

TEST(StrictBandwidthCurvePolicyTest, dynamicCanTightenBeyondAggressive)
{
    auto dynamic = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Dynamic, 3000U, 1024U);
    auto aggressive = tr_strict_bandwidth_curve_policy::create(tr_strict_bandwidth_curve::Aggressive, 3000U, 1024U);

    auto outcome = tr_strict_bandwidth_curve_pulse_outcome{};
    outcome.note_piece_bytes(TR_UP, TR_PRI_HIGH, 5000U);
    outcome.note_piece_bytes(TR_UP, TR_PRI_NORMAL, 1000U);
    outcome.note_pending(TR_UP, TR_PRI_HIGH);
    feed_pulse_outcome(*dynamic, outcome, 20U);

    auto const pulse = make_pulse();
    dynamic->on_pulse_start(pulse);
    aggressive->on_pulse_start(pulse);

    auto query = tr_strict_bandwidth_curve_query{};
    query.dir = TR_UP;
    query.priority = TR_PRI_NORMAL;
    query.now_msec = 1000U;
    query.applies = true;

    auto const dynamic_result = dynamic->admit(query);
    auto const aggressive_result = aggressive->admit(query);

    EXPECT_EQ(0U, dynamic_result.piece_limit);
    EXPECT_EQ(0U, aggressive_result.piece_limit);
    EXPECT_GT(dynamic_result.next_wakeup_msec, aggressive_result.next_wakeup_msec);
}

TEST(StrictBandwidthCurvePolicyTest, pulseOutcomeTracksPieceBytesByDirectionAndPriority)
{
    auto outcome = tr_strict_bandwidth_curve_pulse_outcome{};

    outcome.note_piece_bytes(TR_UP, TR_PRI_HIGH, 111U);
    outcome.note_piece_bytes(TR_UP, TR_PRI_HIGH, 222U);
    outcome.note_piece_bytes(TR_DOWN, TR_PRI_LOW, 333U);

    EXPECT_EQ(333U, outcome.by_direction[0].piece_bytes[0]);
    EXPECT_EQ(0U, outcome.by_direction[0].piece_bytes[1]);
    EXPECT_EQ(333U, outcome.by_direction[1].piece_bytes[2]);
}

TEST(StrictBandwidthCurvePolicyTest, pulseOutcomeTracksBlockedAndPendingWork)
{
    auto outcome = tr_strict_bandwidth_curve_pulse_outcome{};

    outcome.note_blocked(TR_UP, TR_PRI_NORMAL);
    outcome.note_pending(TR_DOWN, TR_PRI_LOW);

    EXPECT_TRUE(outcome.by_direction[0].had_blocked_work[1]);
    EXPECT_FALSE(outcome.by_direction[0].had_blocked_work[0]);
    EXPECT_TRUE(outcome.by_direction[1].has_pending_work[2]);
    EXPECT_FALSE(outcome.by_direction[1].has_pending_work[1]);
}

} // namespace libtransmission::test
