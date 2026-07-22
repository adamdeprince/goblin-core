/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2020-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string>

#include "dcmd/dcmd.h"
#include "utils/os.h"

using namespace dcmd;

compchannel::compchannel(ctx_handle ctx)
    : m_ctx(ctx)
    , m_cq_obj(nullptr)
    , m_bound(false)
{
    constexpr enum mlx5dv_devx_create_event_channel_flags flags =
        MLX5DV_DEVX_CREATE_EVENT_CHANNEL_FLAGS_OMIT_EV_DATA;
    event_channel_handle new_channel = mlx5dv_devx_create_event_channel(m_ctx, flags);
    if (nullptr == new_channel) {
        log_error("create_event_channel failed, error: %s\n", strerror(errno));
        throw DCMD_ENOTSUP;
    }
    m_event_channel = new_channel;
}

int compchannel::bind(cq_handle cq_obj)
{
    if (cq_obj) {
        m_cq_obj = cq_obj;
    } else {
        log_error("Associated CQ object is missing\n");
        return DCMD_EINVAL;
    }

    uint16_t events_num[] = {MLX5_EVENT_TYPE_CODING_COMPLETION_EVENTS};
    int err = mlx5dv_devx_subscribe_devx_event(m_event_channel, cq_obj, sizeof(events_num),
                                               events_num, reinterpret_cast<uint64_t>(cq_obj));

    if (err) {
        log_error("bind: subscribe_devx_event failed, ret= %d error: %s\n", err, strerror(errno));
        return DCMD_EIO;
    }
    m_bound = true;
    return err;
}

int compchannel::unbind()
{
    flush();
    m_bound = false;
    return DCMD_EOK;
}

int compchannel::get_comp_channel(::event_channel*& ch)
{
    ch = &m_event_channel->fd;
    return DCMD_EOK;
}

int compchannel::request(compchannel_ctx& cc_ctx)
{
    if (!m_bound) {
        log_error("request: channel not bound to any CQ\n");
        return DCMD_EINVAL;
    }

    uint32_t num_events = 0;
    mlx5dv_devx_async_event_hdr event_hdr = {};
    ssize_t ret;

    do {
        ret = mlx5dv_devx_get_event(m_event_channel, &event_hdr, sizeof(event_hdr));
        if (ret == sizeof(event_hdr)) {
            void* event_cookie = reinterpret_cast<void*>(event_hdr.cookie);
            if (event_cookie != m_cq_obj) {
                log_error("Mismatch completions. Received for cq=%p but bound to cq=%p\n",
                          event_cookie, m_cq_obj);
                return DCMD_EIO;
            }
            num_events++;
        }
    } while (ret == sizeof(event_hdr));

    if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log_error("request: devx_get_event failed, ret=%zd error: %s\n", ret, strerror(errno));
        return DCMD_EIO;
    }

    cc_ctx.eqe_nums = num_events;

    return DCMD_EOK;
}

int compchannel::query(void*& ctx)
{
    if (!m_bound) {
        log_error("query: channel not bound to any CQ\n");
        return DCMD_EINVAL;
    }

    // Return the CQ object as context
    ctx = m_cq_obj;
    return DCMD_EOK;
}

void compchannel::flush()
{
    if (!m_bound) {
        return;
    }

    mlx5dv_devx_async_event_hdr event_hdr;
    ssize_t ret;
    uint32_t flushed_count = 0;

    // Loop until no more events are available to consume
    do {
        ret = mlx5dv_devx_get_event(m_event_channel, &event_hdr, sizeof(event_hdr));
        if (ret == sizeof(event_hdr)) {
            flushed_count++;
            // Event consumed/flushed - continue to get more
        }
    } while (ret == sizeof(event_hdr));

    if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log_error("flush() devx_get_event failed, ret=%zd error: %s\n", ret, strerror(errno));
    }

    if (flushed_count > 0) {
        log_trace("flush() compchannel flushed %u events\n", flushed_count);
    }
}

compchannel::~compchannel()
{
    unbind();
    mlx5dv_devx_destroy_event_channel(m_event_channel);
    log_trace("DTR compchannel OK\n");
}
