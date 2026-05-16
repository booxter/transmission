// This file Copyright (C) 2026 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
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
#include <libtransmission/peer-socket.h>
#include <libtransmission/session.h>
#include <libtransmission/string-utils.h>
#include <libtransmission/tr-macros.h>
#include <libtransmission/values.h>

#include "test-fixtures.h"

using namespace std::literals;

#define LOCAL_SOCKETPAIR_AF TR_IF_WIN32(AF_INET, AF_UNIX)

namespace tr::test
{

class StrictBandwidthSchedulerTest : public SessionTest
{
protected:
    static bool setSocketBufferSize(tr_socket_t sock, int size)
    {
        return setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&size), sizeof(size)) == 0 &&
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&size), sizeof(size)) == 0;
    }

    static ReadState keepReadsBuffered([[maybe_unused]] tr_peerIo* io, [[maybe_unused]] void* user_data, size_t* piece)
    {
        *piece = 0U;
        return ReadState::Later;
    }

    void SetUp() override
    {
        auto* const settings_map = settings()->get_if<tr_variant::Map>();
        ASSERT_NE(settings_map, nullptr);
        settings_map->insert_or_assign(TR_KEY_bandwidth_allocator, "strict"sv);
        SessionTest::SetUp();
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

        auto const socket_address = tr_socket_address{ *tr_address::from_string("127.0.0.1"sv), tr_port::from_host(51413) };
        return std::pair{
            tr_peerIo::new_incoming(session_, parent, tr_peer_socket(session_, socket_address, sockpair[0])),
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
};

TEST_F(StrictBandwidthSchedulerTest, pulseSeedsHighPriorityWritersFirst)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.set_priority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.set_priority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);

    static auto constexpr UploadBytesPerSecond = uint64_t{ 6000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr PayloadSize = size_t{ 3000U };

    auto const high_payload = std::string(PayloadSize, 'H');
    auto const low_payload = std::string(PayloadSize, 'L');

    session_->run_in_session_thread(
        [&]()
        {
            session_->top_bandwidth_.set_limited(tr_direction::Up, true);
            session_->top_bandwidth_.set_desired_speed(
                tr_direction::Up,
                tr::Values::Speed{ UploadBytesPerSecond, tr::Values::Speed::Units::Byps });

            high_io->write_bytes(std::data(high_payload), std::size(high_payload), true);
            low_io->write_bytes(std::data(low_payload), std::size(low_payload), true);

            session_->bandwidthScheduler().on_pulse(PulseMsec);
        });

    auto high_received = std::string{};
    auto low_received = std::string{};

    EXPECT_TRUE(waitFor(
        [&]()
        {
            high_received += readAvailable(high_sock);
            low_received += readAvailable(low_sock);
            return high_received == high_payload && low_received.empty();
        },
        200));

    tr_net_close_socket(high_sock);
    tr_net_close_socket(low_sock);
}

TEST_F(StrictBandwidthSchedulerTest, pulseSeedsHighPriorityReadersFirst)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.set_priority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.set_priority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);

    static auto constexpr DownloadBytesPerSecond = uint64_t{ 6000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr PayloadSize = size_t{ 3000U };

    auto const high_payload = std::string(PayloadSize, 'H');
    auto const low_payload = std::string(PayloadSize, 'L');
    auto done = std::atomic_bool{ false };
    auto write_ok = std::atomic_bool{ true };
    auto high_size = std::atomic_size_t{ 0U };
    auto low_size = std::atomic_size_t{ 0U };
    auto high_matches = std::atomic_bool{ false };

    session_->queue_session_thread(
        [&]()
        {
            session_->top_bandwidth_.set_limited(tr_direction::Down, true);
            session_->top_bandwidth_.set_desired_speed(
                tr_direction::Down,
                tr::Values::Speed{ DownloadBytesPerSecond, tr::Values::Speed::Units::Byps });

            high_io->set_callbacks(&keepReadsBuffered, nullptr, nullptr, nullptr);
            low_io->set_callbacks(&keepReadsBuffered, nullptr, nullptr, nullptr);

            write_ok = writeAll(high_sock, high_payload) && writeAll(low_sock, low_payload);
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            high_size = high_io->read_buffer_size();
            low_size = low_io->read_buffer_size();
            high_matches = high_io->read_buffer_starts_with(high_payload);

            high_io->set_enabled(tr_direction::Down, false);
            low_io->set_enabled(tr_direction::Down, false);
            high_io->clear_callbacks();
            low_io->clear_callbacks();
            done = true;
        });

    EXPECT_TRUE(waitFor([&]() { return done.load(); }, 200));
    EXPECT_TRUE(write_ok.load());
    EXPECT_EQ(PayloadSize, high_size.load());
    EXPECT_TRUE(high_matches.load());
    EXPECT_EQ(0U, low_size.load());

    tr_net_close_socket(high_sock);
    tr_net_close_socket(low_sock);
}

TEST_F(StrictBandwidthSchedulerTest, writableCallbackPreemptsQueuedLowPriorityWriter)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.set_priority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.set_priority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);

    static auto constexpr UploadBytesPerSecond = uint64_t{ 900000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr LargePayloadSize = size_t{ 450000U };
    static auto constexpr SmallPayloadSize = size_t{ 3000U };

    auto const high_payload = std::string(SmallPayloadSize, 'H');
    auto const low_payload = std::string(LargePayloadSize, 'L');
    auto done = std::atomic_bool{ false };
    auto low_before_high = std::atomic_size_t{ 0U };
    auto low_total = std::atomic_size_t{ 0U };
    auto high_total = std::atomic_size_t{ 0U };

    session_->queue_session_thread(
        [&]()
        {
            session_->top_bandwidth_.set_limited(tr_direction::Up, true);
            session_->top_bandwidth_.set_desired_speed(
                tr_direction::Up,
                tr::Values::Speed{ UploadBytesPerSecond, tr::Values::Speed::Units::Byps });
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            low_io->write_bytes(std::data(low_payload), std::size(low_payload), true);
            session_->bandwidthScheduler().on_outbuf_ready(*low_io);
            low_before_high = std::size(readAvailable(low_sock));
            EXPECT_TRUE(readAvailable(high_sock).empty());

            high_io->write_bytes(std::data(high_payload), std::size(high_payload), true);
            session_->bandwidthScheduler().on_outbuf_ready(*high_io);

            high_total = std::size(readAvailable(high_sock));
            low_total = low_before_high.load() + std::size(readAvailable(low_sock));

            high_io->clear();
            low_io->clear();
            done = true;
        });

    EXPECT_TRUE(waitFor([&]() { return done.load(); }, 200));
    EXPECT_LT(0U, low_before_high.load());
    EXPECT_LT(low_before_high.load(), std::size(low_payload));
    EXPECT_EQ(std::size(high_payload), high_total.load());
    EXPECT_LT(low_total.load(), std::size(low_payload));

    tr_net_close_socket(high_sock);
    tr_net_close_socket(low_sock);
}

TEST_F(StrictBandwidthSchedulerTest, readableCallbackPreemptsQueuedLowPriorityReader)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.set_priority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.set_priority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);

    static auto constexpr DownloadBytesPerSecond = uint64_t{ 900000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr LargePayloadSize = size_t{ 450000U };
    static auto constexpr SmallPayloadSize = size_t{ 3000U };

    auto const high_payload = std::string(SmallPayloadSize, 'H');
    auto const low_payload = std::string(LargePayloadSize, 'L');
    auto done = std::atomic_bool{ false };
    auto low_before_high = std::atomic_size_t{ 0U };
    auto low_total = std::atomic_size_t{ 0U };
    auto high_total = std::atomic_size_t{ 0U };
    auto high_matches = std::atomic_bool{ false };

    session_->queue_session_thread(
        [&]()
        {
            session_->top_bandwidth_.set_limited(tr_direction::Down, true);
            session_->top_bandwidth_.set_desired_speed(
                tr_direction::Down,
                tr::Values::Speed{ DownloadBytesPerSecond, tr::Values::Speed::Units::Byps });
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            high_io->set_callbacks(&keepReadsBuffered, nullptr, nullptr, nullptr);
            low_io->set_callbacks(&keepReadsBuffered, nullptr, nullptr, nullptr);

            EXPECT_TRUE(writeAll(low_sock, low_payload));
            session_->bandwidthScheduler().on_can_read(*low_io);
            low_before_high = low_io->read_buffer_size();
            EXPECT_EQ(0U, high_io->read_buffer_size());

            EXPECT_TRUE(writeAll(high_sock, high_payload));
            session_->bandwidthScheduler().on_can_read(*high_io);

            high_total = high_io->read_buffer_size();
            high_matches = high_io->read_buffer_starts_with(high_payload);
            low_total = low_io->read_buffer_size();

            high_io->clear();
            low_io->clear();
            done = true;
        });

    EXPECT_TRUE(waitFor([&]() { return done.load(); }, 200));
    EXPECT_LT(0U, low_before_high.load());
    EXPECT_LT(low_before_high.load(), std::size(low_payload));
    EXPECT_EQ(std::size(high_payload), high_total.load());
    EXPECT_TRUE(high_matches.load());
    EXPECT_LT(low_total.load(), std::size(low_payload));

    tr_net_close_socket(high_sock);
    tr_net_close_socket(low_sock);
}

} // namespace tr::test
