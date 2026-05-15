// This file Copyright (C) 2026 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
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
#include <libtransmission/peer-socket.h>
#include <libtransmission/session.h>
#include <libtransmission/tr-macros.h>
#include "test-fixtures.h"

using namespace std::literals;

#define LOCAL_SOCKETPAIR_AF TR_IF_WIN32(AF_INET, AF_UNIX)

namespace libtransmission::test
{

class StrictBandwidthSchedulerTest : public SessionTest
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

    struct ReadCapture
    {
        std::string_view expected;
        size_t bytes = 0U;
        bool matches = true;
    };

    static ReadState drainAndCaptureReads(tr_peerIo* io, void* user_data, size_t* piece)
    {
        auto* const capture = static_cast<ReadCapture*>(user_data);
        auto const n_available = io->read_buffer_size();

        if (capture != nullptr)
        {
            auto const offset = std::min(capture->bytes, std::size(capture->expected));
            auto const expected = capture->expected.substr(offset, n_available);
            capture->matches = capture->matches && io->read_buffer_starts_with(expected);
            capture->bytes += n_available;
        }

        io->read_buffer_drain(n_available);
        *piece = n_available;
        return READ_LATER;
    }

    void SetUp() override
    {
        tr_variantDictAddStr(settings(), TR_KEY_bandwidth_allocator, "strict");
        SessionTest::SetUp();
    }

    auto createIncomingIo(tr_bandwidth* parent)
    {
        auto sockpair = std::array<evutil_socket_t, 2>{ -1, -1 };
        EXPECT_EQ(0, evutil_socketpair(LOCAL_SOCKETPAIR_AF, SOCK_STREAM, 0, std::data(sockpair))) << tr_strerror(errno);
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
};

TEST_F(StrictBandwidthSchedulerTest, pulseSeedsHighPriorityWritersFirst)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.setPriority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);

    static auto constexpr UploadBytesPerSecond = uint64_t{ 6000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr PayloadSize = size_t{ 3000U };

    auto const high_payload = std::string(PayloadSize, 'H');
    auto const low_payload = std::string(PayloadSize, 'L');

    runInSessionThreadAndWait(
        [&]()
        {
            session_->top_bandwidth_.setLimited(TR_UP, true);
            session_->top_bandwidth_.setDesiredSpeedBytesPerSecond(
                TR_UP,
                static_cast<tr_bytes_per_second_t>(UploadBytesPerSecond));

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

    runInSessionThreadAndWait(
        [&]()
        {
            high_io->clear();
            low_io->clear();
        });

    tr_net_close_socket(high_sock);
    tr_net_close_socket(low_sock);
}

TEST_F(StrictBandwidthSchedulerTest, pulseSeedsHighPriorityReadersFirst)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.setPriority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);

    static auto constexpr DownloadBytesPerSecond = uint64_t{ 6000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr PayloadSize = size_t{ 3000U };

    auto const high_payload = std::string(PayloadSize, 'H');
    auto const low_payload = std::string(PayloadSize, 'L');
    auto high_capture = ReadCapture{ high_payload };
    auto low_capture = ReadCapture{ low_payload };

    runInSessionThreadAndWait(
        [&]()
        {
            session_->top_bandwidth_.setLimited(TR_DOWN, true);
            session_->top_bandwidth_.setDesiredSpeedBytesPerSecond(
                TR_DOWN,
                static_cast<tr_bytes_per_second_t>(DownloadBytesPerSecond));

            high_io->set_callbacks(&drainAndCaptureReads, nullptr, nullptr, &high_capture);
            low_io->set_callbacks(&drainAndCaptureReads, nullptr, nullptr, &low_capture);
        });

    ASSERT_TRUE(writeAll(high_sock, high_payload));
    ASSERT_TRUE(writeAll(low_sock, low_payload));

    runInSessionThreadAndWait([&]() { session_->bandwidthScheduler().on_pulse(PulseMsec); });

    EXPECT_TRUE(waitFor([&]() { return high_capture.bytes + low_capture.bytes != 0U; }, 200));

    runInSessionThreadAndWait(
        [&]()
        {
            high_io->set_enabled(TR_DOWN, false);
            low_io->set_enabled(TR_DOWN, false);
            high_io->clear_callbacks();
            low_io->clear_callbacks();
            high_io->clear();
            low_io->clear();
        });

    EXPECT_EQ(PayloadSize, high_capture.bytes);
    EXPECT_TRUE(high_capture.matches);
    EXPECT_EQ(0U, low_capture.bytes);

    tr_net_close_socket(high_sock);
    tr_net_close_socket(low_sock);
}

} // namespace libtransmission::test
