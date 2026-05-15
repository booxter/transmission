// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <memory>

#include "bandwidth-scheduler.h"

#include "peer-io.h"
#include "session.h"
#include "tr-assert.h"

namespace
{

class tr_legacy_bandwidth_scheduler final : public tr_bandwidth_scheduler
{
public:
    explicit tr_legacy_bandwidth_scheduler(tr_session& session)
        : session_{ session }
    {
    }

    void on_pulse(uint64_t period_msec) override
    {
        session_.top_bandwidth_.allocate(period_msec);
    }

    void on_can_read(tr_peerIo& io) override
    {
        io.execute_can_read();
    }

    void on_can_write(tr_peerIo& io) override
    {
        io.execute_can_write();
    }

    void on_outbuf_ready(tr_peerIo& io) override
    {
        static_cast<void>(io);
    }

    void on_utp_read(tr_peerIo& io, size_t bytes_transferred) override
    {
        io.execute_utp_read(bytes_transferred);
    }

private:
    tr_session& session_;
};

} // namespace

std::unique_ptr<tr_bandwidth_scheduler> tr_bandwidth_scheduler::create(tr_session& session)
{
    switch (session.bandwidthAllocator())
    {
    case tr_bandwidth_allocator_mode::Default:
    case tr_bandwidth_allocator_mode::Strict:
        // Keep both modes on the legacy implementation until the strict
        // scheduler is introduced in a later commit.
        return std::make_unique<tr_legacy_bandwidth_scheduler>(session);
    }

    TR_ASSERT_MSG(false, "invalid bandwidth allocator mode");
    return std::make_unique<tr_legacy_bandwidth_scheduler>(session);
}
