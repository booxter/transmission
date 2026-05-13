// This file Copyright (C) 2026 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include <event2/util.h>

#include <libtransmission/transmission.h>

#include "test-fixtures.h"

#ifdef _WIN32
#define LOCAL_SOCKETPAIR_AF AF_INET
#else
#include <unistd.h>
#define LOCAL_SOCKETPAIR_AF AF_UNIX
#endif

using namespace std::literals;

namespace libtransmission::test
{

class PeerMgrRechokeTest : public SessionTest
{
protected:
    void SetUp() override
    {
        tr_variantDictAddInt(settings(), TR_KEY_upload_slots_per_torrent, 1);
        SessionTest::SetUp();
    }

    class RemoteSocket
    {
    public:
        explicit RemoteSocket(tr_socket_t sock)
            : sock_{ sock }
        {
        }

        RemoteSocket(RemoteSocket&& that) noexcept
            : sock_{ std::exchange(that.sock_, TR_BAD_SOCKET) }
        {
        }

        RemoteSocket& operator=(RemoteSocket&& that) noexcept
        {
            close();
            sock_ = std::exchange(that.sock_, TR_BAD_SOCKET);
            return *this;
        }

        RemoteSocket(RemoteSocket const&) = delete;
        RemoteSocket& operator=(RemoteSocket const&) = delete;

        ~RemoteSocket()
        {
            close();
        }

        template<typename Container>
        void send(Container const& data) const
        {
            auto const* walk = reinterpret_cast<uint8_t const*>(std::data(data));
            auto left = size_t{ std::size(data) };

            while (left > 0U)
            {
#ifdef _WIN32
                auto const n = ::send(sock_, reinterpret_cast<char const*>(walk), static_cast<int>(left), 0);
#else
                auto const n = ::write(sock_, walk, left);
#endif
                ASSERT_LE(0, n);

                walk += n;
                left -= static_cast<size_t>(n);
            }
        }

    private:
        void close()
        {
            if (sock_ != TR_BAD_SOCKET)
            {
                evutil_closesocket(sock_);
                sock_ = TR_BAD_SOCKET;
            }
        }

        tr_socket_t sock_ = TR_BAD_SOCKET;
    };

    static constexpr auto ProtocolName = "\023BitTorrent protocol"sv;
    static constexpr auto ReservedBytes = std::array<uint8_t, 8>{};
    static constexpr auto InterestedMessage = std::array<uint8_t, 5>{ 0, 0, 0, 1, 2 };

    static auto makePeerId(char suffix)
    {
        auto peer_id = tr_peer_id_t{};
        auto const prefix = "-UTTEST-"sv;
        std::copy(std::begin(prefix), std::end(prefix), std::begin(peer_id));
        std::fill(std::begin(peer_id) + std::size(prefix), std::end(peer_id), suffix);
        return peer_id;
    }

    static auto makeHandshake(tr_torrent const* tor, tr_peer_id_t const& peer_id)
    {
        auto handshake = std::array<uint8_t, 68>{};
        auto out = std::begin(handshake);

        *out++ = std::size(ProtocolName) - 1U;
        out = std::copy(std::begin(ProtocolName) + 1, std::end(ProtocolName), out);
        out = std::copy(std::begin(ReservedBytes), std::end(ReservedBytes), out);
        out = std::transform(
            std::begin(tor->infoHash()),
            std::end(tor->infoHash()),
            out,
            [](auto byte)
            {
                return std::to_integer<uint8_t>(byte);
            });
        out = std::copy(std::begin(peer_id), std::end(peer_id), out);

        EXPECT_EQ(std::end(handshake), out);
        return handshake;
    }

    [[nodiscard]] auto connectIncomingPeer(tr_torrent* tor, tr_address const& addr, tr_port port, tr_peer_id_t const& peer_id)
    {
        auto sockpair = std::array<tr_socket_t, 2>{ TR_BAD_SOCKET, TR_BAD_SOCKET };
        if (evutil_socketpair(LOCAL_SOCKETPAIR_AF, SOCK_STREAM, 0, std::data(sockpair)) != 0)
        {
            ADD_FAILURE() << tr_strerror(errno);
            return RemoteSocket{ TR_BAD_SOCKET };
        }

        addIncomingPeerSocket(tr_peer_socket(session_, addr, port, sockpair[0]));
        flushSessionThread();

        auto remote = RemoteSocket{ sockpair[1] };
        remote.send(makeHandshake(tor, peer_id));
        return remote;
    }

    [[nodiscard]] static auto peerStats(tr_torrent const* tor)
    {
        auto peer_count = size_t{};
        auto* raw = tr_torrentPeers(tor, &peer_count);
        auto peers = std::vector<tr_peer_stat>{};
        if (raw != nullptr)
        {
            peers.assign(raw, raw + peer_count);
        }
        tr_torrentPeersFree(raw, peer_count);
        return peers;
    }

    [[nodiscard]] static auto countInterested(std::vector<tr_peer_stat> const& peers)
    {
        return std::count_if(
            std::begin(peers),
            std::end(peers),
            [](auto const& peer)
            {
                return peer.peerIsInterested;
            });
    }

    [[nodiscard]] static auto countUnchoked(std::vector<tr_peer_stat> const& peers)
    {
        return std::count_if(
            std::begin(peers),
            std::end(peers),
            [](auto const& peer)
            {
                return !peer.peerIsChoked;
            });
    }

    [[nodiscard]] static auto countOptimistic(std::vector<tr_peer_stat> const& peers)
    {
        return std::count_if(
            std::begin(peers),
            std::end(peers),
            [](auto const& peer)
            {
                return std::string_view{ peer.flagStr }.find('O') != std::string_view::npos;
            });
    }

    [[nodiscard]] static auto describePeers(std::vector<tr_peer_stat> const& peers)
    {
        auto out = std::ostringstream{};
        out << "peers=" << std::size(peers);
        for (auto const& peer : peers)
        {
            out << " [addr=" << peer.addr << " flags=" << peer.flagStr << " peerIsInterested=" << peer.peerIsInterested
                << " peerIsChoked=" << peer.peerIsChoked << ']';
        }
        return out.str();
    }
};

TEST_F(PeerMgrRechokeTest, forcePriorityUnchokesAllInterestedPeers)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);

    tr_torrentSetPriority(tor, TR_PRI_HIGH);
    tr_torrentStartNow(tor);
    flushSessionThread();

    auto const addresses = std::array{ "127.0.0.1"sv, "127.0.0.2"sv, "127.0.0.3"sv };
    auto peers = std::vector<RemoteSocket>{};
    peers.reserve(std::size(addresses));

    for (size_t i = 0; i < std::size(addresses); ++i)
    {
        auto const addr = tr_address::from_string(addresses[i]);
        ASSERT_TRUE(addr);
        peers.emplace_back(
            connectIncomingPeer(tor, *addr, tr_port::fromHost(static_cast<uint16_t>(5000U + i)), makePeerId('0' + i)));
    }

    ASSERT_TRUE(waitFor([tor]() { return std::size(peerStats(tor)) == 3U; }, 5000));

    for (auto const& peer : peers)
    {
        peer.send(InterestedMessage);
    }

    tr_peerMgrRechokeSoon(tor);

    ASSERT_TRUE(waitFor(
                    [tor]()
                    {
                        auto const peers = peerStats(tor);
                        return std::size(peers) == 3U && countInterested(peers) == 3 && countUnchoked(peers) == 2 &&
                            countOptimistic(peers) == 1;
                    },
                    5000))
        << describePeers(peerStats(tor));

    tr_torrentSetPriority(tor, TR_PRI_FORCE);

    ASSERT_TRUE(waitFor(
                    [tor]()
                    {
                        auto const peers = peerStats(tor);
                        return std::size(peers) == 3U && countInterested(peers) == 3 && countUnchoked(peers) == 3 &&
                            countOptimistic(peers) == 0;
                    },
                    5000))
        << describePeers(peerStats(tor));

    tr_torrentRemove(tor, false, nullptr, nullptr);
}

} // namespace libtransmission::test
