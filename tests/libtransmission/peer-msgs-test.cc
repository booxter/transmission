// This file Copyright (C) 2026 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#include <event2/util.h>

#include <gtest/gtest.h>

#include <libtransmission/bandwidth-scheduler.h>
#include <libtransmission/bandwidth.h>
#include <libtransmission/net.h>
#include <libtransmission/peer-io.h>
#include <libtransmission/peer-msgs.h>
#include <libtransmission/peer-socket.h>
#include <libtransmission/torrent.h>
#include <libtransmission/tr-buffer.h>
#include <libtransmission/tr-macros.h>
#include <libtransmission/variant.h>
#include "test-fixtures.h"

using namespace std::literals;

#define LOCAL_SOCKETPAIR_AF TR_IF_WIN32(AF_INET, AF_UNIX)

namespace libtransmission::test
{

class PeerMsgsTest : public SessionTest
{
protected:
    template<typename Func>
    void runInSessionThreadAndWait(Func&& func, std::chrono::milliseconds timeout = 200ms)
    {
        auto done = std::atomic_bool{ false };

        session_->runInSessionThread(
            [&, func = std::forward<Func>(func)]()
            {
                func();
                done = true;
            });

        ASSERT_TRUE(waitFor([&]() { return done.load(); }, timeout));
    }

    static bool setSocketBufferSize(tr_socket_t sock, int size)
    {
        return setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&size), sizeof(size)) == 0 &&
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&size), sizeof(size)) == 0;
    }

    auto createIncomingIo(tr_bandwidth* parent)
    {
        auto sockpair = std::array<evutil_socket_t, 2>{ -1, -1 };
        EXPECT_EQ(0, evutil_socketpair(LOCAL_SOCKETPAIR_AF, SOCK_STREAM, 0, std::data(sockpair))) << tr_strerror(errno);
        static auto constexpr SocketBufferSize = int{ 1U << 20U };
        EXPECT_TRUE(setSocketBufferSize(sockpair[0], SocketBufferSize));
        EXPECT_TRUE(setSocketBufferSize(sockpair[1], SocketBufferSize));
        EXPECT_EQ(0, evutil_make_socket_nonblocking(sockpair[0]));
        EXPECT_EQ(0, evutil_make_socket_nonblocking(sockpair[1]));

        auto const address = *tr_address::from_string("127.0.0.1"sv);
        auto const port = tr_port::fromHost(51413);
        return std::pair{
            tr_peerIo::new_incoming(session_, parent, tr_peer_socket(session_, address, port, sockpair[0])),
            static_cast<tr_socket_t>(sockpair[1]),
        };
    }

    static auto readAvailable(tr_socket_t sock)
    {
        auto buf = std::array<char, 4096>{};
        auto payload = std::string{};

        for (;;)
        {
            auto const n_read = recv(sock, std::data(buf), std::size(buf), 0);
            if (n_read > 0)
            {
                payload.append(std::data(buf), n_read);
                continue;
            }

#ifdef _WIN32
            auto const error = WSAGetLastError();
            EXPECT_TRUE(n_read == 0 || error == WSAEWOULDBLOCK) << error;
#else
            EXPECT_TRUE(n_read == 0 || errno == EAGAIN || errno == EWOULDBLOCK) << errno;
#endif
            break;
        }

        return payload;
    }

    static bool writeAll(tr_socket_t sock, std::string_view payload)
    {
        auto const* walk = std::data(payload);
        auto n_left = std::size(payload);

        while (n_left > 0U)
        {
            auto const n_written = send(sock, walk, n_left, 0);
            if (n_written <= 0)
            {
                return false;
            }

            walk += n_written;
            n_left -= static_cast<size_t>(n_written);
        }

        return true;
    }

    static void noopPeerCallback(tr_peer* /*peer*/, tr_peer_event const& /*event*/, void* /*client_data*/)
    {
    }

    static auto makeRequestMessage(tr_piece_index_t piece, uint32_t offset, uint32_t length)
    {
        auto buf = libtransmission::Buffer{};
        buf.add_uint32(sizeof(uint8_t) + 3U * sizeof(uint32_t));
        buf.add_uint8(uint8_t{ 6U });
        buf.add_uint32(piece);
        buf.add_uint32(offset);
        buf.add_uint32(length);
        return buf.to_string();
    }

    static auto makePieceMessage(tr_piece_index_t piece, uint32_t offset, uint32_t length)
    {
        auto buf = libtransmission::Buffer{};
        buf.add_uint32(sizeof(uint8_t) + 2U * sizeof(uint32_t) + length);
        buf.add_uint8(uint8_t{ 7U });
        buf.add_uint32(piece);
        buf.add_uint32(offset);
        buf.add(std::string(length, '\0'));
        return buf.to_string();
    }

    void SetUp() override
    {
        tr_variantDictAddStr(settings(), TR_KEY_bandwidth_allocator, "strict");
        SessionTest::SetUp();
    }
};

TEST_F(PeerMsgsTest, respondsImmediatelyToPeerRequestWithoutWaitingForPulse)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    tr_torrentSetPriority(tor, TR_PRI_HIGH);

    auto [io, peer_sock] = createIncomingIo(&tor->bandwidth());
    ASSERT_NE(nullptr, io);

    auto* peer = tr_peerMsgsNew(tor, nullptr, io, &noopPeerCallback, nullptr);
    ASSERT_NE(nullptr, peer);

    static auto constexpr Piece = tr_piece_index_t{ 0U };
    static auto constexpr Offset = uint32_t{ 0U };
    auto const length = static_cast<uint32_t>(tor->blockSize(tr_block_index_t{ 0U }));
    auto const request = makeRequestMessage(Piece, Offset, length);
    auto const expected = makePieceMessage(Piece, Offset, length);

    runInSessionThreadAndWait([&]() { peer->set_choke(false); });
    EXPECT_TRUE(readAvailable(peer_sock).empty());

    ASSERT_TRUE(writeAll(peer_sock, request));
    runInSessionThreadAndWait([&]() { session_->bandwidthScheduler().on_can_read(*io); });

    auto received = std::string{};
    EXPECT_TRUE(waitFor(
        [&]()
        {
            received += readAvailable(peer_sock);
            return received.find(expected) != std::string::npos;
        },
        500ms));

    runInSessionThreadAndWait(
        [&]()
        {
            delete peer;
            io.reset();
        });
    tr_net_close_socket(peer_sock);
}

TEST_F(PeerMsgsTest, tracksPendingProtocolAndPieceOutputSeparately)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);

    auto [io, peer_sock] = createIncomingIo(&tor->bandwidth());
    ASSERT_NE(nullptr, io);

    auto* peer = tr_peerMsgsNew(tor, nullptr, io, &noopPeerCallback, nullptr);
    ASSERT_NE(nullptr, peer);

    auto const protocol_payload = "ab"sv;
    auto const piece_payload = "cde"sv;

    auto base_protocol_bytes = size_t{};
    auto base_piece_bytes = size_t{};
    auto pending_protocol_bytes = size_t{};
    auto pending_piece_bytes = size_t{};
    runInSessionThreadAndWait(
        [&]()
        {
            io->set_defer_immediate_outbuf_ready(true);
            base_protocol_bytes = peer->pending_protocol_output_size();
            base_piece_bytes = peer->pending_piece_output_size();
            io->write_bytes(std::data(protocol_payload), std::size(protocol_payload), false);
            io->write_bytes(std::data(piece_payload), std::size(piece_payload), true);
            pending_protocol_bytes = peer->pending_protocol_output_size();
            pending_piece_bytes = peer->pending_piece_output_size();
        });

    EXPECT_EQ(base_protocol_bytes + std::size(protocol_payload), pending_protocol_bytes);
    EXPECT_EQ(base_piece_bytes + std::size(piece_payload), pending_piece_bytes);

    runInSessionThreadAndWait(
        [&]()
        {
            delete peer;
            io.reset();
        });
    tr_net_close_socket(peer_sock);
}

} // namespace libtransmission::test
