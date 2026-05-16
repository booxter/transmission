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

    static bool setSocketBufferSize(tr_socket_t sock, int size)
    {
        return setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&size), sizeof(size)) == 0 &&
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&size), sizeof(size)) == 0;
    }

    static ReadState keepReadsBuffered([[maybe_unused]] tr_peerIo* io, [[maybe_unused]] void* user_data, size_t* piece)
    {
        *piece = 0U;
        return READ_LATER;
    }

    static ReadState clearIoAndPauseRead(tr_peerIo* io, void* user_data, size_t* piece)
    {
        if (auto* const called = static_cast<std::atomic_bool*>(user_data); called != nullptr)
        {
            *called = true;
        }

        *piece = 0U;
        io->clear();
        return READ_LATER;
    }

    struct ReadToWriteCapture
    {
        std::string_view trigger;
        std::string_view response;
        size_t bytes = 0U;
        bool matches = true;
        bool wrote = false;
    };

    static ReadState drainReadsAndQueueWrite(tr_peerIo* io, void* user_data, size_t* piece)
    {
        auto* const capture = static_cast<ReadToWriteCapture*>(user_data);
        auto const n_available = io->read_buffer_size();

        if (capture != nullptr)
        {
            auto const offset = std::min(capture->bytes, std::size(capture->trigger));
            auto const expected = capture->trigger.substr(offset, n_available);
            capture->matches = capture->matches && io->read_buffer_starts_with(expected);
            capture->bytes += n_available;
        }

        io->read_buffer_drain(n_available);
        *piece = 0U;

        if (capture != nullptr && !capture->wrote)
        {
            io->write_bytes(std::data(capture->response), std::size(capture->response), true);
            capture->wrote = true;
        }

        return READ_LATER;
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

TEST_F(StrictBandwidthSchedulerTest, pulsePreservesLowPriorityPeers)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.setPriority(TR_PRI_HIGH);

    auto normal_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    normal_parent.setPriority(TR_PRI_NORMAL);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [normal_io, normal_sock] = createIncomingIo(&normal_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);

    auto refs = std::vector<std::shared_ptr<tr_peerIo>>{};

    runInSessionThreadAndWait(
        [&]()
        {
            session_->top_bandwidth_.allocatePulse(500U, refs);
            ASSERT_EQ(TR_PRI_HIGH, high_io->priority());
            ASSERT_EQ(TR_PRI_NORMAL, normal_io->priority());
            ASSERT_EQ(TR_PRI_LOW, low_io->priority());

            high_io->clear();
            normal_io->clear();
            low_io->clear();
        });

    tr_net_close_socket(high_sock);
    tr_net_close_socket(normal_sock);
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

    session_->runInSessionThread(
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

TEST_F(StrictBandwidthSchedulerTest, writableCallbackPreemptsQueuedLowPriorityWriter)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.setPriority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);

    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr LargePayloadSize = size_t{ 450000U };
    static auto constexpr SmallPayloadSize = size_t{ 3000U };

    auto const high_payload = std::string(SmallPayloadSize, 'H');
    auto const low_payload = std::string(LargePayloadSize, 'L');
    auto done = std::atomic_bool{ false };
    auto low_before_high = std::atomic_size_t{ 0U };
    auto low_total = std::atomic_size_t{ 0U };
    auto high_total = std::atomic_size_t{ 0U };

    session_->runInSessionThread(
        [&]()
        {
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            low_io->write_bytes(std::data(low_payload), std::size(low_payload), true);
            low_before_high = std::size(readAvailable(low_sock));
            EXPECT_TRUE(readAvailable(high_sock).empty());

            high_io->write_bytes(std::data(high_payload), std::size(high_payload), true);

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
    high_parent.setPriority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);

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

    session_->runInSessionThread(
        [&]()
        {
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

TEST_F(StrictBandwidthSchedulerTest, readableCallbackQueuesWriterImmediately)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.setPriority(TR_PRI_HIGH);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);

    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr PayloadSize = size_t{ 3000U };

    auto const high_trigger = std::string(PayloadSize, 'R');
    auto const high_payload = std::string(PayloadSize, 'H');
    auto capture = ReadToWriteCapture{ high_trigger, high_payload };
    auto done = std::atomic_bool{ false };
    auto high_total = std::atomic_size_t{ 0U };
    auto high_matches = std::atomic_bool{ false };

    session_->runInSessionThread(
        [&]()
        {
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            high_io->set_callbacks(&drainReadsAndQueueWrite, nullptr, nullptr, &capture);

            EXPECT_TRUE(readAvailable(high_sock).empty());
            EXPECT_TRUE(writeAll(high_sock, high_trigger));
            session_->bandwidthScheduler().on_can_read(*high_io);

            auto const received = readAvailable(high_sock);
            high_total = std::size(received);
            high_matches = received == high_payload && capture.matches && capture.wrote;

            high_io->clear();
            done = true;
        });

    EXPECT_TRUE(waitFor([&]() { return done.load(); }, 200));
    EXPECT_EQ(std::size(high_payload), high_total.load());
    EXPECT_TRUE(high_matches.load());

    tr_net_close_socket(high_sock);
}

TEST_F(StrictBandwidthSchedulerTest, limitedWriterRetentionPreservesBudgetForLateHighWriter)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.setPriority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);
    low_io->bandwidth().setPriority(TR_PRI_LOW);

    static auto constexpr UploadBytesPerSecond = uint64_t{ 6000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr PayloadSize = size_t{ 3000U };

    auto const high_payload = std::string(PayloadSize, 'H');
    auto const low_payload = std::string(PayloadSize, 'L');
    auto done = std::atomic_bool{ false };
    auto low_before_high = std::atomic_size_t{ 0U };
    auto low_total = std::atomic_size_t{ 0U };
    auto high_total = std::atomic_size_t{ 0U };

    session_->runInSessionThread(
        [&]()
        {
            session_->top_bandwidth_.setLimited(TR_UP, true);
            session_->top_bandwidth_.setDesiredSpeedBytesPerSecond(
                TR_UP,
                static_cast<tr_bytes_per_second_t>(UploadBytesPerSecond));
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            low_io->write_bytes(std::data(low_payload), std::size(low_payload), true);
            session_->bandwidthScheduler().on_outbuf_ready(*low_io);
            low_before_high = std::size(readAvailable(low_sock));

            high_io->write_bytes(std::data(high_payload), std::size(high_payload), true);
            session_->bandwidthScheduler().on_outbuf_ready(*high_io);

            high_total = std::size(readAvailable(high_sock));
            low_total = low_before_high.load() + std::size(readAvailable(low_sock));

            high_io->clear();
            low_io->clear();
            done = true;
        });

    EXPECT_TRUE(waitFor([&]() { return done.load(); }, 200));
    EXPECT_EQ(0U, low_before_high.load());
    EXPECT_EQ(std::size(high_payload), high_total.load());
    EXPECT_EQ(0U, low_total.load());

    tr_net_close_socket(high_sock);
    tr_net_close_socket(low_sock);
}

TEST_F(StrictBandwidthSchedulerTest, clearedRetainedWriterIsReleasedBeforeDelayedWakeup)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.setPriority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);
    low_io->bandwidth().setPriority(TR_PRI_LOW);

    static auto constexpr UploadBytesPerSecond = uint64_t{ 6000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr PayloadSize = size_t{ 3000U };

    auto const payload = std::string(PayloadSize, 'L');
    auto weak_low = std::weak_ptr<tr_peerIo>{};

    runInSessionThreadAndWait(
        [&]()
        {
            session_->top_bandwidth_.setLimited(TR_UP, true);
            session_->top_bandwidth_.setDesiredSpeedBytesPerSecond(
                TR_UP,
                static_cast<tr_bytes_per_second_t>(UploadBytesPerSecond));
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            low_io->write_bytes(std::data(payload), std::size(payload), true);
            session_->bandwidthScheduler().on_outbuf_ready(*low_io);
            EXPECT_TRUE(readAvailable(low_sock).empty());

            weak_low = low_io;
            low_io->clear();
            low_io.reset();
        });

    EXPECT_TRUE(weak_low.expired());
    EXPECT_TRUE(readAvailable(low_sock).empty());

    runInSessionThreadAndWait(
        [&]()
        {
            high_io->clear();
            high_io.reset();
        });

    tr_net_close_socket(high_sock);
    tr_net_close_socket(low_sock);
}

TEST_F(StrictBandwidthSchedulerTest, clearedRetainedReaderIsReleasedBeforeDelayedWakeup)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.setPriority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);
    low_io->bandwidth().setPriority(TR_PRI_LOW);

    static auto constexpr DownloadBytesPerSecond = uint64_t{ 6000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr PayloadSize = size_t{ 3000U };

    auto const payload = std::string(PayloadSize, 'L');
    auto weak_low = std::weak_ptr<tr_peerIo>{};
    auto callback_called = std::atomic_bool{ false };

    runInSessionThreadAndWait(
        [&]()
        {
            session_->top_bandwidth_.setLimited(TR_DOWN, true);
            session_->top_bandwidth_.setDesiredSpeedBytesPerSecond(
                TR_DOWN,
                static_cast<tr_bytes_per_second_t>(DownloadBytesPerSecond));
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            low_io->set_callbacks(&clearIoAndPauseRead, nullptr, nullptr, &callback_called);

            ASSERT_TRUE(writeAll(low_sock, payload));
            session_->bandwidthScheduler().on_can_read(*low_io);
            EXPECT_EQ(0U, low_io->read_buffer_size());

            weak_low = low_io;
            low_io.reset();
        });

    EXPECT_FALSE(weak_low.expired());
    EXPECT_TRUE(waitFor([&]() { return callback_called.load(); }, 1000));
    EXPECT_TRUE(waitFor([&]() { return weak_low.expired(); }, 1000));

    runInSessionThreadAndWait(
        [&]()
        {
            high_io->clear();
            high_io.reset();
        });

    tr_net_close_socket(high_sock);
    tr_net_close_socket(low_sock);
}

TEST_F(StrictBandwidthSchedulerTest, limitedReaderRetentionPreservesBudgetForLateHighReader)
{
    auto high_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    high_parent.setPriority(TR_PRI_HIGH);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);

    auto [high_io, high_sock] = createIncomingIo(&high_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);
    low_io->bandwidth().setPriority(TR_PRI_LOW);

    static auto constexpr DownloadBytesPerSecond = uint64_t{ 6000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr PayloadSize = size_t{ 3000U };

    auto const high_payload = std::string(PayloadSize, 'H');
    auto const low_payload = std::string(PayloadSize, 'L');
    auto done = std::atomic_bool{ false };
    auto low_before_high = std::atomic_size_t{ 0U };
    auto low_total = std::atomic_size_t{ 0U };
    auto high_total = std::atomic_size_t{ 0U };
    auto high_matches = std::atomic_bool{ false };

    session_->runInSessionThread(
        [&]()
        {
            session_->top_bandwidth_.setLimited(TR_DOWN, true);
            session_->top_bandwidth_.setDesiredSpeedBytesPerSecond(
                TR_DOWN,
                static_cast<tr_bytes_per_second_t>(DownloadBytesPerSecond));
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            high_io->set_callbacks(&keepReadsBuffered, nullptr, nullptr, nullptr);
            low_io->set_callbacks(&keepReadsBuffered, nullptr, nullptr, nullptr);

            EXPECT_TRUE(writeAll(low_sock, low_payload));
            session_->bandwidthScheduler().on_can_read(*low_io);
            low_before_high = low_io->read_buffer_size();

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
    EXPECT_EQ(0U, low_before_high.load());
    EXPECT_EQ(std::size(high_payload), high_total.load());
    EXPECT_TRUE(high_matches.load());
    EXPECT_EQ(0U, low_total.load());

    tr_net_close_socket(high_sock);
    tr_net_close_socket(low_sock);
}

TEST_F(StrictBandwidthSchedulerTest, limitedLowPriorityProtocolWritesBypassPieceRetention)
{
    auto normal_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    normal_parent.setPriority(TR_PRI_NORMAL);

    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);

    auto [normal_io, normal_sock] = createIncomingIo(&normal_parent);
    auto [low_io, low_sock] = createIncomingIo(&low_parent);
    low_io->bandwidth().setPriority(TR_PRI_LOW);

    static auto constexpr UploadBytesPerSecond = uint64_t{ 6000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };

    auto const protocol_payload = std::string(64U, 'P');
    auto const piece_payload = std::string(3000U, 'L');
    auto done = std::atomic_bool{ false };
    auto protocol_bytes = std::atomic_size_t{ 0U };
    auto protocol_matches = std::atomic_bool{ false };

    session_->runInSessionThread(
        [&]()
        {
            session_->top_bandwidth_.setLimited(TR_UP, true);
            session_->top_bandwidth_.setDesiredSpeedBytesPerSecond(
                TR_UP,
                static_cast<tr_bytes_per_second_t>(UploadBytesPerSecond));
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            low_io->write_bytes(std::data(protocol_payload), std::size(protocol_payload), false);
            low_io->write_bytes(std::data(piece_payload), std::size(piece_payload), true);
            session_->bandwidthScheduler().on_outbuf_ready(*low_io);

            auto const received = readAvailable(low_sock);
            protocol_bytes = std::size(received);
            protocol_matches = received == protocol_payload;

            low_io->clear();
            done = true;
        });

    EXPECT_TRUE(waitFor([&]() { return done.load(); }, 200));
    EXPECT_EQ(std::size(protocol_payload), protocol_bytes.load());
    EXPECT_TRUE(protocol_matches.load());

    runInSessionThreadAndWait(
        [&]()
        {
            normal_io.reset();
            session_->bandwidthScheduler().on_pulse(PulseMsec);
        });
    static_cast<void>(readAvailable(low_sock));
    runInSessionThreadAndWait([&]() { low_io->clear(); });

    tr_net_close_socket(normal_sock);
    tr_net_close_socket(low_sock);
}

TEST_F(StrictBandwidthSchedulerTest, limitedSessionRetentionSkipsWritersIgnoringSessionLimit)
{
    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);
    low_parent.honorParentLimits(TR_UP, false);
    low_parent.setLimited(TR_UP, true);
    low_parent.setDesiredSpeedBytesPerSecond(TR_UP, static_cast<tr_bytes_per_second_t>(6000U));

    auto [low_io, low_sock] = createIncomingIo(&low_parent);
    low_io->bandwidth().setPriority(TR_PRI_LOW);

    static auto constexpr UploadBytesPerSecond = uint64_t{ 6000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr PayloadSize = size_t{ 3000U };

    auto const payload = std::string(PayloadSize, 'L');
    auto done = std::atomic_bool{ false };
    auto transferred = std::atomic_size_t{ 0U };

    session_->runInSessionThread(
        [&]()
        {
            session_->top_bandwidth_.setLimited(TR_UP, true);
            session_->top_bandwidth_.setDesiredSpeedBytesPerSecond(
                TR_UP,
                static_cast<tr_bytes_per_second_t>(UploadBytesPerSecond));
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            low_io->write_bytes(std::data(payload), std::size(payload), true);
            session_->bandwidthScheduler().on_outbuf_ready(*low_io);
            transferred = std::size(readAvailable(low_sock));

            low_io->clear();
            done = true;
        });

    EXPECT_TRUE(waitFor([&]() { return done.load(); }, 200));
    EXPECT_EQ(std::size(payload), transferred.load());

    tr_net_close_socket(low_sock);
}

TEST_F(StrictBandwidthSchedulerTest, limitedLowPriorityWriterEventuallyDrainsWithinPulse)
{
    auto low_parent = tr_bandwidth{ &session_->top_bandwidth_ };
    low_parent.setPriority(TR_PRI_LOW);

    auto [low_io, low_sock] = createIncomingIo(&low_parent);
    low_io->bandwidth().setPriority(TR_PRI_LOW);

    static auto constexpr UploadBytesPerSecond = uint64_t{ 6000U };
    static auto constexpr PulseMsec = uint64_t{ 500U };
    static auto constexpr PayloadSize = size_t{ 3000U };

    auto const payload = std::string(PayloadSize, 'L');

    runInSessionThreadAndWait(
        [&]()
        {
            session_->top_bandwidth_.setLimited(TR_UP, true);
            session_->top_bandwidth_.setDesiredSpeedBytesPerSecond(
                TR_UP,
                static_cast<tr_bytes_per_second_t>(UploadBytesPerSecond));
            session_->bandwidthScheduler().on_pulse(PulseMsec);

            low_io->write_bytes(std::data(payload), std::size(payload), true);
            session_->bandwidthScheduler().on_outbuf_ready(*low_io);
        });

    auto low_received = std::string{};
    EXPECT_TRUE(waitFor(
        [&]()
        {
            low_received += readAvailable(low_sock);
            return low_received == payload;
        },
        1000));

    runInSessionThreadAndWait([&]() { low_io->clear(); });
    tr_net_close_socket(low_sock);
}

} // namespace libtransmission::test
