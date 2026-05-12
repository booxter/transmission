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

    static auto constexpr DefaultPeerPort = tr_port::fromHost(51413);
    static auto constexpr BytesPerPulse = size_t{ 3000 };
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
};

TEST_F(BandwidthTest, forceUploadPeerReservesRecentSliceAndLeavesRemainder)
{
    setSinglePulseLimit(TR_UP);

    auto [force_io, force_sock] = createIncomingIo();
    auto [normal_io, normal_sock] = createIncomingIo();

    auto force_stats = TransferStats{};
    auto normal_stats = TransferStats{};
    force_io->set_callbacks(nullptr, didWriteCounter, nullptr, &force_stats);
    normal_io->set_callbacks(nullptr, didWriteCounter, nullptr, &normal_stats);

    force_io->bandwidth().setPriority(TR_TOR_PRI_FORCE);

    auto full_payload = std::array<char, BytesPerPulse>{};
    auto reserved_payload = std::array<char, BytesPerPulse / 3U>{};

    force_io->write_bytes(reserved_payload.data(), std::size(reserved_payload), true);
    normal_io->write_bytes(full_payload.data(), std::size(full_payload), true);

    session_->top_bandwidth_.allocate(PeriodMsec);
    EXPECT_EQ(std::size(reserved_payload), force_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse - force_stats.piece_bytes, normal_stats.piece_bytes);

    force_stats = {};
    normal_stats = {};

    normal_io->write_bytes(full_payload.data(), std::size(full_payload), true);

    session_->top_bandwidth_.allocate(PeriodMsec);
    EXPECT_EQ(0U, force_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse - std::size(reserved_payload), normal_stats.piece_bytes);

    evutil_closesocket(force_sock);
    evutil_closesocket(normal_sock);
}

TEST_F(BandwidthTest, persistentForceUploadDemandRampsReservedSlice)
{
    setSinglePulseLimit(TR_UP);

    auto [force_io, force_sock] = createIncomingIo();
    auto [normal_io, normal_sock] = createIncomingIo();

    auto force_stats = TransferStats{};
    auto normal_stats = TransferStats{};
    force_io->set_callbacks(nullptr, didWriteCounter, nullptr, &force_stats);
    normal_io->set_callbacks(nullptr, didWriteCounter, nullptr, &normal_stats);

    force_io->bandwidth().setPriority(TR_TOR_PRI_FORCE);
    force_io->set_has_pending_piece_requests(true);

    auto full_payload = std::array<char, BytesPerPulse>{};
    auto force_payload = std::array<char, BytesPerPulse / 3U>{};

    force_io->write_bytes(force_payload.data(), std::size(force_payload), true);
    normal_io->write_bytes(full_payload.data(), std::size(full_payload), true);

    session_->top_bandwidth_.allocate(PeriodMsec);
    ASSERT_EQ(std::size(force_payload), force_stats.piece_bytes);
    ASSERT_EQ(BytesPerPulse - force_stats.piece_bytes, normal_stats.piece_bytes);

    force_stats = {};
    normal_stats = {};

    force_io->write_bytes(force_payload.data(), std::size(force_payload), true);
    normal_io->write_bytes(full_payload.data(), std::size(full_payload), true);

    session_->top_bandwidth_.allocate(PeriodMsec);

    auto const expected_reserved = std::size(force_payload) + BytesPerPulse / 8U;
    auto const expected_reserved_after_decay =
        std::size(force_payload) + (BytesPerPulse / 8U - std::max(size_t{ 1U }, (BytesPerPulse / 8U) / 4U));
    EXPECT_EQ(std::size(force_payload), force_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse - expected_reserved, normal_stats.piece_bytes);

    force_stats = {};
    normal_stats = {};
    force_io->set_has_pending_piece_requests(false);

    force_io->write_bytes(force_payload.data(), std::size(force_payload), true);
    normal_io->write_bytes(full_payload.data(), std::size(full_payload), true);

    session_->top_bandwidth_.allocate(PeriodMsec);

    EXPECT_EQ(std::size(force_payload), force_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse - expected_reserved_after_decay, normal_stats.piece_bytes);

    evutil_closesocket(force_sock);
    evutil_closesocket(normal_sock);
}

TEST_F(BandwidthTest, forceDownloadPeerReservesRecentSliceAndLeavesRemainder)
{
    setSinglePulseLimit(TR_DOWN);

    auto [force_io, force_sock] = createIncomingIo();
    auto [normal_io, normal_sock] = createIncomingIo();

    auto force_stats = TransferStats{};
    auto normal_stats = TransferStats{};
    force_io->set_callbacks(didReadCounter, nullptr, nullptr, &force_stats);
    normal_io->set_callbacks(didReadCounter, nullptr, nullptr, &normal_stats);

    force_io->bandwidth().setPriority(TR_TOR_PRI_FORCE);
    force_io->set_has_pending_download_requests(true);

    auto full_payload = std::array<char, BytesPerPulse>{};
    auto reserved_payload = std::array<char, BytesPerPulse / 3U>{};
    sendBytes(force_sock, reserved_payload);
    sendBytes(normal_sock, full_payload);

    session_->top_bandwidth_.allocate(PeriodMsec);
    EXPECT_EQ(std::size(reserved_payload), force_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse - force_stats.piece_bytes, normal_stats.piece_bytes);

    force_io->set_has_pending_download_requests(false);
    force_stats = {};
    normal_stats = {};
    sendBytes(normal_sock, full_payload);

    session_->top_bandwidth_.allocate(PeriodMsec);
    EXPECT_EQ(0U, force_stats.piece_bytes);
    EXPECT_EQ(BytesPerPulse - std::size(reserved_payload), normal_stats.piece_bytes);

    evutil_closesocket(force_sock);
    evutil_closesocket(normal_sock);
}

} // namespace libtransmission::test
