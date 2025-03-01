// This file Copyright © 2008-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <initializer_list>
#include <utility> // for std::swap()
#include <vector>

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/bandwidth.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/log.h"
#include "libtransmission/peer-io.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils.h" // tr_time_msec()

tr_bytes_per_second_t tr_bandwidth::get_speed_bytes_per_second(RateControl& r, unsigned int interval_msec, uint64_t now)
{
    if (now == 0)
    {
        now = tr_time_msec();
    }

    if (now != r.cache_time_)
    {
        uint64_t bytes = 0;
        uint64_t const cutoff = now - interval_msec;

        for (int i = r.newest_; r.date_[i] > cutoff;)
        {
            bytes += r.size_[i];

            if (--i == -1)
            {
                i = HistorySize - 1; /* circular history */
            }

            if (i == r.newest_)
            {
                break; /* we've come all the way around */
            }
        }

        r.cache_val_ = static_cast<tr_bytes_per_second_t>(bytes * 1000U / interval_msec);
        r.cache_time_ = now;
    }

    return r.cache_val_;
}

void tr_bandwidth::notify_bandwidth_consumed_bytes(uint64_t const now, RateControl* r, size_t size)
{
    if (r->date_[r->newest_] + GranularityMSec >= now)
    {
        r->size_[r->newest_] += size;
    }
    else
    {
        if (++r->newest_ == HistorySize)
        {
            r->newest_ = 0;
        }

        r->date_[r->newest_] = now;
        r->size_[r->newest_] = size;
    }

    /* invalidate cache_val*/
    r->cache_time_ = 0;
}

// ---

tr_bandwidth::tr_bandwidth(tr_bandwidth* parent)
{
    this->set_parent(parent);
}

// ---

namespace
{
namespace deparent_helpers
{
void remove_child(std::vector<tr_bandwidth*>& v, tr_bandwidth* remove_me) noexcept
{
    // the list isn't sorted -- so instead of erase()ing `it`,
    // do the cheaper option of overwriting it with the final item
    if (auto it = std::find(std::begin(v), std::end(v), remove_me); it != std::end(v))
    {
        *it = v.back();
        v.resize(v.size() - 1);
    }
}
} // namespace deparent_helpers
} // namespace

void tr_bandwidth::deparent() noexcept
{
    using namespace deparent_helpers;

    if (parent_ == nullptr)
    {
        return;
    }

    remove_child(parent_->children_, this);
    parent_ = nullptr;
}

void tr_bandwidth::set_parent(tr_bandwidth* new_parent)
{
    TR_ASSERT(this != new_parent);

    deparent();

    if (new_parent != nullptr)
    {
#ifdef TR_ENABLE_ASSERTS
        TR_ASSERT(new_parent->parent_ != this);
        auto& children = new_parent->children_;
        TR_ASSERT(std::find(std::begin(children), std::end(children), this) == std::end(children)); // not already there
#endif

        new_parent->children_.push_back(this);
        this->parent_ = new_parent;
    }
}

// ---

void tr_bandwidth::allocate_bandwidth(
    tr_priority_t parent_priority,
    unsigned int period_msec,
    std::vector<std::shared_ptr<tr_peerIo>>& peer_pool)
{
    auto const priority = std::max(parent_priority, this->priority_);

    // set the available bandwidth
    for (auto const dir : { TR_UP, TR_DOWN })
    {
        if (auto& bandwidth = band_[dir]; bandwidth.is_limited_)
        {
            auto const next_pulse_speed = bandwidth.desired_speed_bps_;
            bandwidth.bytes_left_ = next_pulse_speed * period_msec / 1000U;
        }
    }

    // add this bandwidth's peer, if any, to the peer pool
    if (auto shared = this->peer_.lock(); shared)
    {
        shared->set_priority(priority);
        peer_pool.push_back(std::move(shared));
    }

    // traverse & repeat for the subtree
    for (auto* child : this->children_)
    {
        child->allocate_bandwidth(priority, period_msec, peer_pool);
    }
}

void tr_bandwidth::phase_one(std::vector<tr_peerIo*>& peers, tr_direction dir)
{
    // First phase of IO. Tries to distribute bandwidth fairly to keep faster
    // peers from starving the others.
    tr_logAddTrace(fmt::format("{} peers to go round-robin for {}", peers.size(), dir == TR_UP ? "upload" : "download"));

    // Shuffle the peers so they all have equal chance to be first in line.
    thread_local auto urbg = tr_urbg<size_t>{};
    std::shuffle(std::begin(peers), std::end(peers), urbg);

    // Give each peer `Increment` bandwidth bytes to use. Repeat this
    // process until we run out of bandwidth and/or peers that can use it.
    for (size_t n_unfinished = std::size(peers); n_unfinished > 0U;)
    {
        for (size_t i = 0; i < n_unfinished;)
        {
            // Value of 3000 bytes chosen so that when using µTP we'll send a full-size
            // frame right away and leave enough buffered data for the next frame to go
            // out in a timely manner.
            static auto constexpr Increment = size_t{ 3000 };

            auto const bytes_used = peers[i]->flush(dir, Increment);
            tr_logAddTrace(fmt::format("peer #{} of {} used {} bytes in this pass", i, n_unfinished, bytes_used));

            if (bytes_used != Increment)
            {
                // peer is done writing for now; move it to the end of the list
                std::swap(peers[i], peers[n_unfinished - 1]);
                --n_unfinished;
            }
            else
            {
                ++i;
            }
        }
    }
}

void tr_bandwidth::allocate(unsigned int period_msec)
{
    // keep these peers alive for the scope of this function
    auto refs = std::vector<std::shared_ptr<tr_peerIo>>{};

    auto peer_arrays = std::array<std::vector<tr_peerIo*>, 3>{};
    auto& high = peer_arrays[0];
    auto& normal = peer_arrays[1];
    auto& low = peer_arrays[2];

    // allocateBandwidth () is a helper function with two purposes:
    // 1. allocate bandwidth to b and its subtree
    // 2. accumulate an array of all the peerIos from b and its subtree.
    this->allocate_bandwidth(TR_PRI_LOW, period_msec, refs);

    for (auto const& io : refs)
    {
        io->flush_outgoing_protocol_msgs();

        switch (io->priority())
        {
        case TR_PRI_HIGH:
            high.push_back(io.get());
            [[fallthrough]];

        case TR_PRI_NORMAL:
            normal.push_back(io.get());
            [[fallthrough]];

        default:
            low.push_back(io.get());
        }
    }

    // First phase of IO. Tries to distribute bandwidth fairly to keep faster
    // peers from starving the others. Loop through the peers, giving each a
    // small chunk of bandwidth. Keep looping until we run out of bandwidth
    // and/or peers that can use it
    for (auto& peers : peer_arrays)
    {
        phase_one(peers, TR_UP);
        phase_one(peers, TR_DOWN);
    }

    // Second phase of IO. To help us scale in high bandwidth situations,
    // enable on-demand IO for peers with bandwidth left to burn.
    // This on-demand IO is enabled until (1) the peer runs out of bandwidth,
    // or (2) the next tr_bandwidth::allocate () call, when we start over again.
    for (auto const& io : refs)
    {
        io->set_enabled(TR_UP, io->has_bandwidth_left(TR_UP));
        io->set_enabled(TR_DOWN, io->has_bandwidth_left(TR_DOWN));
    }
}

// ---

size_t tr_bandwidth::clamp(uint64_t now, tr_direction dir, size_t byte_count) const
{
    TR_ASSERT(tr_isDirection(dir));

    if (this->band_[dir].is_limited_)
    {
        byte_count = std::min(byte_count, this->band_[dir].bytes_left_);

        /* if we're getting close to exceeding the speed limit,
         * clamp down harder on the bytes available */
        if (byte_count > 0)
        {
            if (now == 0)
            {
                now = tr_time_msec();
            }

            auto const current = this->get_raw_speed_bytes_per_second(now, TR_DOWN);
            auto const desired = this->get_desired_speed_bytes_per_second(TR_DOWN);
            auto const r = desired >= 1 ? static_cast<double>(current) / desired : 0.0;

            if (r > 1.0)
            {
                byte_count = 0; // none left
            }
            else if (r > 0.9)
            {
                byte_count -= (byte_count / 5U); // cap at 80%
            }
            else if (r > 0.8)
            {
                byte_count -= (byte_count / 10U); // cap at 90%
            }
        }
    }

    if (this->parent_ != nullptr && this->band_[dir].honor_parent_limits_ && byte_count > 0)
    {
        byte_count = this->parent_->clamp(now, dir, byte_count);
    }

    return byte_count;
}

void tr_bandwidth::notify_bandwidth_consumed(tr_direction dir, size_t byte_count, bool is_piece_data, uint64_t now)
{
    TR_ASSERT(tr_isDirection(dir));

    Band* band = &this->band_[dir];

    if (band->is_limited_ && is_piece_data)
    {
        band->bytes_left_ -= std::min(size_t{ band->bytes_left_ }, byte_count);
    }

#ifdef DEBUG_DIRECTION

    if (dir == DEBUG_DIRECTION && band_->isLimited)
    {
        fprintf(
            stderr,
            "%p consumed %5zu bytes of %5s data... was %6zu, now %6zu left\n",
            this,
            byte_count,
            is_piece_data ? "piece" : "raw",
            oldBytesLeft,
            band_->bytesLeft);
    }

#endif

    notify_bandwidth_consumed_bytes(now, &band->raw_, byte_count);

    if (is_piece_data)
    {
        notify_bandwidth_consumed_bytes(now, &band->piece_, byte_count);
    }

    if (this->parent_ != nullptr)
    {
        this->parent_->notify_bandwidth_consumed(dir, byte_count, is_piece_data, now);
    }
}

// ---

tr_bandwidth_limits tr_bandwidth::get_limits() const
{
    tr_bandwidth_limits limits;
    limits.up_limit_KBps = tr_toSpeedKBps(this->get_desired_speed_bytes_per_second(TR_UP));
    limits.down_limit_KBps = tr_toSpeedKBps(this->get_desired_speed_bytes_per_second(TR_DOWN));
    limits.up_limited = this->is_limited(TR_UP);
    limits.down_limited = this->is_limited(TR_DOWN);
    return limits;
}

void tr_bandwidth::set_limits(tr_bandwidth_limits const* limits)
{
    this->set_desired_speed_bytes_per_second(TR_UP, tr_toSpeedBytes(limits->up_limit_KBps));
    this->set_desired_speed_bytes_per_second(TR_DOWN, tr_toSpeedBytes(limits->down_limit_KBps));
    this->set_limited(TR_UP, limits->up_limited);
    this->set_limited(TR_DOWN, limits->down_limited);
}
