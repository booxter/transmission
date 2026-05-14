// This file Copyright © 2026 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <utility>
#include <vector>

#include <event2/util.h>

#include <libtransmission/peer-io.h>
#include <libtransmission/session.h>
#include <libtransmission/transmission.h>

#include "test-fixtures.h"

using namespace std::literals;

#ifdef _WIN32
#define LOCAL_SOCKETPAIR_AF AF_INET
#else
#include <unistd.h>
#define LOCAL_SOCKETPAIR_AF AF_UNIX
#endif

namespace libtransmission::test
{

TEST(BandwidthMechanism, asyncUploadSpilloverClampBypassesForce)
{
    auto root = tr_bandwidth{};

    root.setAsyncUploadPieceSpilloverBudget(1024U);

    EXPECT_EQ(2048U, root.clampAsyncUploadPieceBytes(2048U, TR_PRI_FORCE));
    EXPECT_EQ(1024U, root.clampAsyncUploadPieceBytes(2048U, TR_PRI_HIGH));
}

TEST(BandwidthMechanism, asyncUploadSpilloverBudgetDecrementsForNonForcePieceUploads)
{
    auto root = tr_bandwidth{};
    auto child = tr_bandwidth{ &root };
    auto constexpr Now = uint64_t{ 1U };

    child.setPriority(TR_PRI_HIGH);
    root.setAsyncUploadPieceSpilloverBudget(1024U);

    EXPECT_EQ(1024U, child.clampAsyncUploadPieceBytes(2048U, child.getPriority()));

    child.notifyBandwidthConsumed(TR_UP, 256U, true, Now);
    EXPECT_EQ(768U, child.clampAsyncUploadPieceBytes(2048U, child.getPriority()));
}

TEST(BandwidthMechanism, asyncUploadSpilloverBudgetIgnoresProtocolBytes)
{
    auto root = tr_bandwidth{};
    auto child = tr_bandwidth{ &root };
    auto constexpr Now = uint64_t{ 1U };

    child.setPriority(TR_PRI_HIGH);
    root.setAsyncUploadPieceSpilloverBudget(1024U);

    child.notifyBandwidthConsumed(TR_UP, 256U, false, Now);
    EXPECT_EQ(1024U, child.clampAsyncUploadPieceBytes(2048U, child.getPriority()));
}

TEST(BandwidthMechanism, asyncUploadSpilloverBudgetIgnoresDownloadTraffic)
{
    auto root = tr_bandwidth{};
    auto child = tr_bandwidth{ &root };
    auto constexpr Now = uint64_t{ 1U };

    child.setPriority(TR_PRI_HIGH);
    root.setAsyncUploadPieceSpilloverBudget(1024U);

    child.notifyBandwidthConsumed(TR_DOWN, 256U, true, Now);
    EXPECT_EQ(1024U, child.clampAsyncUploadPieceBytes(2048U, child.getPriority()));
}

TEST(BandwidthMechanism, asyncUploadSpilloverBudgetIsNotConsumedByForceUploads)
{
    auto root = tr_bandwidth{};
    auto child = tr_bandwidth{ &root };
    auto constexpr Now = uint64_t{ 1U };

    child.setPriority(TR_PRI_FORCE);
    root.setAsyncUploadPieceSpilloverBudget(1024U);

    child.notifyBandwidthConsumed(TR_UP, 256U, true, Now);
    EXPECT_EQ(1024U, root.clampAsyncUploadPieceBytes(2048U, TR_PRI_HIGH));
}

TEST(BandwidthMechanism, asyncUploadSpilloverBudgetCanBeCleared)
{
    auto root = tr_bandwidth{};

    root.setAsyncUploadPieceSpilloverBudget(1024U);
    EXPECT_EQ(1024U, root.clampAsyncUploadPieceBytes(2048U, TR_PRI_HIGH));

    root.clearAsyncUploadPieceSpilloverBudget();
    EXPECT_EQ(2048U, root.clampAsyncUploadPieceBytes(2048U, TR_PRI_HIGH));
}

class BandwidthTest : public SessionTest
{
protected:
    struct TransferStats
    {
        size_t piece_bytes = 0U;
        size_t non_piece_bytes = 0U;
    };

    static auto constexpr DefaultPeerPort = tr_port::fromHost(51413);
    static auto constexpr BytesPerPulse = size_t{ 3000 };
    static auto constexpr HalfPulseBytes = BytesPerPulse / 2U;
    static auto constexpr QuarterPulseBytes = BytesPerPulse / 4U;
    static constexpr unsigned int PeriodMsec = 500U;

    tr_address const DefaultPeerAddr = *tr_address::from_string("127.0.0.1"sv);

    [[nodiscard]] auto createIncomingIo()
    {
        auto sockpair = std::array<evutil_socket_t, 2>{ -1, -1 };
        EXPECT_EQ(0, evutil_socketpair(LOCAL_SOCKETPAIR_AF, SOCK_STREAM, 0, std::data(sockpair))) << tr_strerror(errno);
        EXPECT_EQ(0, evutil_make_socket_nonblocking(sockpair[0])) << tr_strerror(errno);

        return std::make_pair(
            tr_peerIo::new_incoming(
                session_,
                &session_->top_bandwidth_,
                tr_peer_socket(session_, DefaultPeerAddr, DefaultPeerPort, sockpair[0])),
            sockpair[1]);
    }

    void setSinglePulseLimit(tr_direction dir)
    {
        session_->top_bandwidth_.setDesiredSpeedBytesPerSecond(dir, BytesPerPulse * 1000U / PeriodMsec);
        session_->top_bandwidth_.setLimited(dir, true);
    }

    void allocateSinglePulse()
    {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();

        session_->runInSessionThread(
            [this, promise]()
            {
                session_->top_bandwidth_.allocate(PeriodMsec);
                promise->set_value();
            });

        EXPECT_EQ(std::future_status::ready, future.wait_for(20s));
    }

    void flushUploads(std::vector<std::shared_ptr<tr_peerIo>> ios)
    {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        auto shared_ios = std::make_shared<std::vector<std::shared_ptr<tr_peerIo>>>(std::move(ios));

        session_->runInSessionThread(
            [promise, shared_ios]()
            {
                for (auto const& io : *shared_ios)
                {
                    io->flush(TR_UP, SIZE_MAX);
                }

                for (auto const& io : *shared_ios)
                {
                    io->set_enabled(TR_UP, false);
                    io->set_enabled(TR_DOWN, false);
                }

                promise->set_value();
            });

        EXPECT_EQ(std::future_status::ready, future.wait_for(20s));
    }

    static void didWriteCounter(tr_peerIo* /*io*/, size_t bytes_written, bool was_piece_data, void* user_data)
    {
        auto& stats = *static_cast<TransferStats*>(user_data);
        if (was_piece_data)
        {
            stats.piece_bytes += bytes_written;
        }
        else
        {
            stats.non_piece_bytes += bytes_written;
        }
    }

    static ReadState didReadCounter(tr_peerIo* io, void* user_data, size_t* setme_piece_byte_count)
    {
        auto& stats = *static_cast<TransferStats*>(user_data);
        auto const byte_count = io->read_buffer_size();
        io->read_buffer_drain(byte_count);
        stats.piece_bytes += byte_count;
        *setme_piece_byte_count = byte_count;
        return READ_NOW;
    }

    template<size_t N>
    static void sendBytes(evutil_socket_t sock, std::array<char, N> const& payload)
    {
        auto const* walk = std::data(payload);
        auto len = std::size(payload);

        while (len > 0U)
        {
#ifdef _WIN32
            auto const n = send(sock, reinterpret_cast<char const*>(walk), static_cast<int>(len), 0);
#else
            auto const n = write(sock, walk, len);
#endif
            ASSERT_GE(n, 0);
            len -= static_cast<size_t>(n);
            walk += n;
        }
    }

    static void destroyIo(std::shared_ptr<tr_peerIo>& io, evutil_socket_t sock)
    {
        if (io != nullptr)
        {
            io->set_enabled(TR_UP, false);
            io->set_enabled(TR_DOWN, false);
        }

        io.reset();
        evutil_closesocket(sock);
    }
};

TEST_F(BandwidthTest, forceUploadPeerUsesRemainingPulseBandwidthAheadOfHigh)
{
    setSinglePulseLimit(TR_UP);

    auto [force_io, force_sock] = createIncomingIo();
    auto [high_io, high_sock] = createIncomingIo();

    auto force_stats = TransferStats{};
    auto high_stats = TransferStats{};
    force_io->set_callbacks(nullptr, didWriteCounter, nullptr, &force_stats);
    high_io->set_callbacks(nullptr, didWriteCounter, nullptr, &high_stats);

    force_io->bandwidth().setPriority(TR_PRI_FORCE);
    high_io->bandwidth().setPriority(TR_PRI_HIGH);
    force_io->write_bytes(std::array<char, HalfPulseBytes>{}.data(), HalfPulseBytes, true);
    high_io->write_bytes(std::array<char, BytesPerPulse>{}.data(), BytesPerPulse, true);

    allocateSinglePulse();
    EXPECT_EQ(HalfPulseBytes, force_stats.piece_bytes);
    EXPECT_EQ(HalfPulseBytes, high_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse, force_stats.piece_bytes + high_stats.piece_bytes);

    destroyIo(force_io, force_sock);
    destroyIo(high_io, high_sock);
}

TEST_F(BandwidthTest, forceUploadPeerCanConsumeEntirePulseAheadOfHigh)
{
    setSinglePulseLimit(TR_UP);

    auto [force_io, force_sock] = createIncomingIo();
    auto [high_io, high_sock] = createIncomingIo();

    auto force_stats = TransferStats{};
    auto high_stats = TransferStats{};
    force_io->set_callbacks(nullptr, didWriteCounter, nullptr, &force_stats);
    high_io->set_callbacks(nullptr, didWriteCounter, nullptr, &high_stats);

    force_io->bandwidth().setPriority(TR_PRI_FORCE);
    high_io->bandwidth().setPriority(TR_PRI_HIGH);
    force_io->write_bytes(std::array<char, BytesPerPulse>{}.data(), BytesPerPulse, true);
    high_io->write_bytes(std::array<char, BytesPerPulse>{}.data(), BytesPerPulse, true);

    allocateSinglePulse();
    EXPECT_EQ(BytesPerPulse, force_stats.piece_bytes);
    EXPECT_EQ(0U, high_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse, force_stats.piece_bytes + high_stats.piece_bytes);

    destroyIo(force_io, force_sock);
    destroyIo(high_io, high_sock);
}

TEST_F(BandwidthTest, forceDownloadPeerUsesRemainingPulseBandwidthAheadOfHigh)
{
    setSinglePulseLimit(TR_DOWN);

    auto [force_io, force_sock] = createIncomingIo();
    auto [high_io, high_sock] = createIncomingIo();

    auto force_stats = TransferStats{};
    auto high_stats = TransferStats{};
    force_io->set_callbacks(didReadCounter, nullptr, nullptr, &force_stats);
    high_io->set_callbacks(didReadCounter, nullptr, nullptr, &high_stats);

    force_io->bandwidth().setPriority(TR_PRI_FORCE);
    high_io->bandwidth().setPriority(TR_PRI_HIGH);

    sendBytes(force_sock, std::array<char, HalfPulseBytes>{});
    sendBytes(high_sock, std::array<char, BytesPerPulse>{});

    allocateSinglePulse();
    EXPECT_EQ(HalfPulseBytes, force_stats.piece_bytes);
    EXPECT_EQ(HalfPulseBytes, high_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse, force_stats.piece_bytes + high_stats.piece_bytes);

    destroyIo(force_io, force_sock);
    destroyIo(high_io, high_sock);
}

TEST_F(BandwidthTest, forceDownloadPeerCanConsumeEntirePulseAheadOfHigh)
{
    setSinglePulseLimit(TR_DOWN);

    auto [force_io, force_sock] = createIncomingIo();
    auto [high_io, high_sock] = createIncomingIo();

    auto force_stats = TransferStats{};
    auto high_stats = TransferStats{};
    force_io->set_callbacks(didReadCounter, nullptr, nullptr, &force_stats);
    high_io->set_callbacks(didReadCounter, nullptr, nullptr, &high_stats);

    force_io->bandwidth().setPriority(TR_PRI_FORCE);
    high_io->bandwidth().setPriority(TR_PRI_HIGH);

    sendBytes(force_sock, std::array<char, BytesPerPulse>{});
    sendBytes(high_sock, std::array<char, BytesPerPulse>{});

    allocateSinglePulse();
    EXPECT_EQ(BytesPerPulse, force_stats.piece_bytes);
    EXPECT_EQ(0U, high_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse, force_stats.piece_bytes + high_stats.piece_bytes);

    destroyIo(force_io, force_sock);
    destroyIo(high_io, high_sock);
}

TEST_F(BandwidthTest, allocateArmsAsyncUploadSpilloverWhenForceStillHasQueuedUpload)
{
    setSinglePulseLimit(TR_UP);

    auto [force_io, force_sock] = createIncomingIo();
    force_io->bandwidth().setPriority(TR_PRI_FORCE);
    force_io->write_bytes(std::array<char, BytesPerPulse * 2U>{}.data(), BytesPerPulse * 2U, true);

    allocateSinglePulse();

    EXPECT_TRUE(session_->top_bandwidth_.isAsyncUploadPieceSpilloverBudgetEnforced());
    EXPECT_EQ(0U, session_->top_bandwidth_.asyncUploadPieceSpilloverBudgetLeft());
    EXPECT_EQ(BytesPerPulse, force_io->queued_outgoing_bytes().piece_bytes);

    destroyIo(force_io, force_sock);
}

TEST_F(BandwidthTest, historicalForceUploadPressureReservesSyncBudgetFromHigh)
{
    setSinglePulseLimit(TR_UP);

    auto [force_io, force_sock] = createIncomingIo();
    auto [high_io, high_sock] = createIncomingIo();

    auto force_stats = TransferStats{};
    auto high_stats = TransferStats{};
    force_io->set_callbacks(nullptr, didWriteCounter, nullptr, &force_stats);
    high_io->set_callbacks(nullptr, didWriteCounter, nullptr, &high_stats);

    force_io->bandwidth().setPriority(TR_PRI_FORCE);
    high_io->bandwidth().setPriority(TR_PRI_HIGH);
    high_io->write_bytes(std::array<char, BytesPerPulse>{}.data(), BytesPerPulse, true);

    force_io->bandwidth().notifyBandwidthConsumed(TR_UP, BytesPerPulse * 4U, true, tr_time_msec());

    allocateSinglePulse();
    EXPECT_EQ(0U, force_stats.piece_bytes);
    EXPECT_EQ(0U, high_stats.piece_bytes);
    EXPECT_TRUE(session_->top_bandwidth_.isAsyncUploadPieceSpilloverBudgetEnforced());

    destroyIo(force_io, force_sock);
    destroyIo(high_io, high_sock);
}

TEST_F(BandwidthTest, asyncUploadSpilloverCapsHighPriorityPeerIoWrites)
{
    auto [high_io, high_sock] = createIncomingIo();

    auto high_stats = TransferStats{};
    high_io->set_callbacks(nullptr, didWriteCounter, nullptr, &high_stats);
    high_io->set_priority(TR_PRI_HIGH);
    high_io->write_bytes(std::array<char, BytesPerPulse>{}.data(), BytesPerPulse, true);

    session_->top_bandwidth_.setAsyncUploadPieceSpilloverBudget(QuarterPulseBytes);
    flushUploads({ high_io });

    EXPECT_EQ(QuarterPulseBytes, high_stats.piece_bytes);

    destroyIo(high_io, high_sock);
}

TEST_F(BandwidthTest, idleForcePeerDoesNotArmAsyncUploadSpillover)
{
    setSinglePulseLimit(TR_UP);

    auto [force_io, force_sock] = createIncomingIo();

    force_io->bandwidth().setPriority(TR_PRI_FORCE);

    allocateSinglePulse();

    EXPECT_FALSE(session_->top_bandwidth_.isAsyncUploadPieceSpilloverBudgetEnforced());
    EXPECT_EQ(0U, session_->top_bandwidth_.asyncUploadPieceSpilloverBudgetLeft());

    destroyIo(force_io, force_sock);
}

} // namespace libtransmission::test
