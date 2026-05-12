// This file Copyright © 2026 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <utility>

#include <event2/util.h>

#include <libtransmission/peer-io.h>
#include <libtransmission/session.h>
#include <libtransmission/transmission.h>

#include "test-fixtures.h"

using namespace std::literals;

#ifdef _WIN32
#define LOCAL_SOCKETPAIR_AF AF_INET
#else
#define LOCAL_SOCKETPAIR_AF AF_UNIX
#endif

namespace libtransmission::test
{

class BandwidthTest : public SessionTest
{
protected:
    struct TransferStats
    {
        size_t piece_bytes = 0U;
        size_t non_piece_bytes = 0U;
    };

    struct ReadAndQueueUploadStats
    {
        TransferStats read_stats;
        TransferStats write_stats;
        size_t upload_bytes = 0U;
    };

    static auto constexpr DefaultPeerPort = tr_port::fromHost(51413);
    static auto constexpr BytesPerPulse = size_t{ 3000 };
    static auto constexpr HalfPulseBytes = BytesPerPulse / 2U;
    static constexpr unsigned int PeriodMsec = 500U;

    tr_address const DefaultPeerAddr = *tr_address::from_string("127.0.0.1"sv);

    [[nodiscard]] auto createIncomingIo()
    {
        auto sockpair = std::array<evutil_socket_t, 2>{ -1, -1 };
        EXPECT_EQ(0, evutil_socketpair(LOCAL_SOCKETPAIR_AF, SOCK_STREAM, 0, std::data(sockpair))) << tr_strerror(errno);

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

    static void didWriteReadAndQueueCounter(tr_peerIo* /*io*/, size_t bytes_written, bool was_piece_data, void* user_data)
    {
        auto& stats = static_cast<ReadAndQueueUploadStats*>(user_data)->write_stats;
        if (was_piece_data)
        {
            stats.piece_bytes += bytes_written;
        }
        else
        {
            stats.non_piece_bytes += bytes_written;
        }
    }

    static ReadState didReadAndQueueUpload(tr_peerIo* io, void* user_data, size_t* setme_piece_byte_count)
    {
        auto& stats = *static_cast<ReadAndQueueUploadStats*>(user_data);
        auto const byte_count = io->read_buffer_size();
        io->read_buffer_drain(byte_count);
        stats.read_stats.piece_bytes += byte_count;
        *setme_piece_byte_count = byte_count;

        if (auto const upload_bytes = std::exchange(stats.upload_bytes, 0U); upload_bytes != 0U)
        {
            io->write_bytes(std::array<char, BytesPerPulse>{}.data(), upload_bytes, true);
        }

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
            auto const n = send(sock, reinterpret_cast<char const*>(walk), len, 0);
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

TEST_F(BandwidthTest, forceUploadPeerUsesRemainingPulseBandwidth)
{
    setSinglePulseLimit(TR_UP);

    auto [force_io, force_sock] = createIncomingIo();
    auto [normal_io, normal_sock] = createIncomingIo();

    auto force_stats = TransferStats{};
    auto normal_stats = TransferStats{};
    force_io->set_callbacks(nullptr, didWriteCounter, nullptr, &force_stats);
    normal_io->set_callbacks(nullptr, didWriteCounter, nullptr, &normal_stats);

    force_io->bandwidth().setPriority(TR_PRI_FORCE);
    force_io->write_bytes(std::array<char, HalfPulseBytes>{}.data(), HalfPulseBytes, true);
    normal_io->write_bytes(std::array<char, BytesPerPulse>{}.data(), BytesPerPulse, true);

    session_->top_bandwidth_.allocate(PeriodMsec);
    EXPECT_EQ(HalfPulseBytes, force_stats.piece_bytes);
    EXPECT_EQ(HalfPulseBytes, normal_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse, force_stats.piece_bytes + normal_stats.piece_bytes);

    destroyIo(force_io, force_sock);
    destroyIo(normal_io, normal_sock);
}

TEST_F(BandwidthTest, forceUploadPeerCanConsumeEntirePulse)
{
    setSinglePulseLimit(TR_UP);

    auto [force_io, force_sock] = createIncomingIo();
    auto [normal_io, normal_sock] = createIncomingIo();

    auto force_stats = TransferStats{};
    auto normal_stats = TransferStats{};
    force_io->set_callbacks(nullptr, didWriteCounter, nullptr, &force_stats);
    normal_io->set_callbacks(nullptr, didWriteCounter, nullptr, &normal_stats);

    force_io->bandwidth().setPriority(TR_PRI_FORCE);
    force_io->write_bytes(std::array<char, BytesPerPulse>{}.data(), BytesPerPulse, true);
    normal_io->write_bytes(std::array<char, BytesPerPulse>{}.data(), BytesPerPulse, true);

    session_->top_bandwidth_.allocate(PeriodMsec);
    EXPECT_EQ(BytesPerPulse, force_stats.piece_bytes);
    EXPECT_EQ(0U, normal_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse, force_stats.piece_bytes + normal_stats.piece_bytes);

    destroyIo(force_io, force_sock);
    destroyIo(normal_io, normal_sock);
}

TEST_F(BandwidthTest, forceUploadPeerCanConsumePulseAfterDownloadPass)
{
    setSinglePulseLimit(TR_UP);

    auto [force_io, force_sock] = createIncomingIo();
    auto [normal_io, normal_sock] = createIncomingIo();

    auto force_stats = ReadAndQueueUploadStats{};
    auto normal_stats = TransferStats{};
    force_stats.upload_bytes = BytesPerPulse;
    force_io->set_callbacks(didReadAndQueueUpload, didWriteReadAndQueueCounter, nullptr, &force_stats);
    normal_io->set_callbacks(nullptr, didWriteCounter, nullptr, &normal_stats);

    force_io->bandwidth().setPriority(TR_PRI_FORCE);
    normal_io->write_bytes(std::array<char, HalfPulseBytes>{}.data(), HalfPulseBytes, true);

    sendBytes(force_sock, std::array<char, 1>{});

    session_->top_bandwidth_.allocate(PeriodMsec);
    EXPECT_EQ(BytesPerPulse, force_stats.write_stats.piece_bytes);
    EXPECT_EQ(0U, normal_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse, force_stats.write_stats.piece_bytes + normal_stats.piece_bytes);

    destroyIo(force_io, force_sock);
    destroyIo(normal_io, normal_sock);
}

TEST_F(BandwidthTest, forceDownloadPeerDoesNotBeatHighPriorityDownload)
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

    session_->top_bandwidth_.allocate(PeriodMsec);
    EXPECT_EQ(0U, force_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse, high_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse, force_stats.piece_bytes + high_stats.piece_bytes);

    destroyIo(force_io, force_sock);
    destroyIo(high_io, high_sock);
}

TEST_F(BandwidthTest, forceDownloadPeerCanConsumeEntirePulseWhenAlone)
{
    setSinglePulseLimit(TR_DOWN);

    auto [force_io, force_sock] = createIncomingIo();

    auto force_stats = TransferStats{};
    force_io->set_callbacks(didReadCounter, nullptr, nullptr, &force_stats);

    force_io->bandwidth().setPriority(TR_PRI_FORCE);

    sendBytes(force_sock, std::array<char, BytesPerPulse>{});

    session_->top_bandwidth_.allocate(PeriodMsec);
    EXPECT_EQ(BytesPerPulse, force_stats.piece_bytes);

    destroyIo(force_io, force_sock);
}

} // namespace libtransmission::test
