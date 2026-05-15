// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // size_t
#include <cstdint> // uint64_t
#include <memory>

class tr_peerIo;
struct tr_session;

class tr_bandwidth_scheduler
{
public:
    virtual ~tr_bandwidth_scheduler() = default;

    tr_bandwidth_scheduler(tr_bandwidth_scheduler&&) = delete;
    tr_bandwidth_scheduler(tr_bandwidth_scheduler const&) = delete;
    tr_bandwidth_scheduler& operator=(tr_bandwidth_scheduler&&) = delete;
    tr_bandwidth_scheduler& operator=(tr_bandwidth_scheduler const&) = delete;

    [[nodiscard]] static std::unique_ptr<tr_bandwidth_scheduler> create(tr_session& session);

    virtual void on_pulse(uint64_t period_msec) = 0;
    virtual void on_can_read(tr_peerIo& io) = 0;
    virtual void on_can_write(tr_peerIo& io) = 0;
    virtual void on_outbuf_ready(tr_peerIo& io) = 0;
    virtual void on_utp_read(tr_peerIo& io, size_t bytes_transferred) = 0;

protected:
    tr_bandwidth_scheduler() = default;
};
