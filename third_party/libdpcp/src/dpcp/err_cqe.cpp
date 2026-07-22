/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <array>
#include <string>

#include "dpcp/internal.h"

namespace dpcp {

struct syndrome_entry {
    uint8_t code;
    const char* desc;
};

static constexpr uint8_t MASK_TPT_3BIT = 0x07;
static constexpr uint8_t RNR_NAK_BASE = 0xA0;
static constexpr uint8_t RNR_NAK_END = 0xBF;
static constexpr uint8_t MASK_RNR_TIMER = 0x1F;

static constexpr std::array<syndrome_entry, 8> TPT_3BIT_SYNDROMES = {{
    {0x0, "unknown TPT error"},
    {0x1, "PCIe error"},
    {0x2, "protection domain violation"},
    {0x3, "access rights violation"},
    {0x4, "length violation"},
    {0x5, "encryption error"},
    {0x6, "master abort or page fault"},
    {0x7, "internal TPT error"},
}};

static constexpr std::array<syndrome_entry, 19> IB_SYNDROMES = {{
    {0x00, "success"},
    {0x01, "local length error"},
    {0x02, "local QP operation error"},
    {0x03, "local EE operation error"},
    {0x04, "local protection error"},
    {0x05, "Work Request Flushed Error"},
    {0x06, "memory management operation error"},
    {0x10, "bad response error"},
    {0x11, "local access error"},
    {0x12, "remote invalid request error"},
    {0x13, "remote access error"},
    {0x14, "remote operation error"},
    {0x15, "transport retry counter exceeded"},
    {0x16, "RNR retry counter exceeded"},
    {0x20, "local RDD violation error"},
    {0x21, "remote invalid RD request error"},
    {0x22, "aborted error"},
    {0x23, "invalid EEC number error"},
    {0x24, "invalid EEC state error"},
}};

static constexpr std::array<syndrome_entry, 111> VENDOR_SYNDROMES = {{
    // No error
    {0x00, "no error detected"},

    // TPT syndromes (base values, 3-bit detail appended separately)
    {0x10, "[TPT] memory access error fetching work queue entry"},
    {0x18, "[TPT] memory access error during atomic operation"},
    {0x20, "[TPT] memory access error during RDMA read"},
    {0x28, "[TPT] memory access error in receive data buffer"},
    {0x30, "[TPT] memory access error: data buffer"},
    {0x38, "[TPT] memory access error during RDMA write"},

    // Completion queue errors
    {0x40, "invalid completion queue"},
    {0x41, "completion queue overflow"},
    {0x42, "completion queue memory access error"},
    {0x43, "completion queue network error"},
    {0x44, "completion queue in error state"},
    {0x45, "completion queue locked"},
    {0x48, "invalid completion queue (to error state)"},
    {0x49, "completion queue overflow (to error state)"},
    {0x4a, "completion queue TPT error (to error state)"},
    {0x4b, "completion queue network error (to error state)"},
    {0x4c, "completion queue in error (to error state)"},
    {0x4d, "completion queue locked (to error state)"},

    // Execution engine / LDB syndromes
    {0x50, "[TPT] memory binding/gather error"},
    {0x58, "wait operation failed"},
    {0x59, "PSV validation error"},
    {0x5a, "signature error"},
    {0x5b, "connection resilience error"},
    {0x60, "[TPT] RDB/WQE fetch TPT violation"},
    {0x68, "invalid work queue entry format"},
    {0x69, "message size exceeds maximum"},
    {0x6a, "message size exceeds MTU"},
    {0x6b, "UD protection domain mismatch"},
    {0x6c, "UD port mismatch"},
    {0x6d, "opcode/transport service mismatch"},
    {0x6e, "NDS too short"},
    {0x6f, "immediate segment exceeds WQE"},
    {0x70, "immediate segment in scatter"},
    {0x71, "reserved opcode"},
    {0x72, "RDB/WQE fetch bus error"},
    {0x73, "gather bus error"},
    {0x74, "UD reserved GID"},
    {0x75, "UD LID/GID mismatch"},
    {0x76, "UD reserved key"},
    {0x77, "GLC bigger than WQE length"},
    {0x78, "[TPT] fast register memory error"},

    // TCU requester syndromes
    {0x80, "request timeout - no response"},
    {0x81, "request timeout with counter exceeded"},
    {0x82, "implicit negative acknowledgment"},
    {0x83, "implicit NAK counter exceeded"},
    {0x84, "out of sequence NAK"},
    {0x85, "out of sequence NAK counter exceeded"},
    {0x86, "unexpected operation code sequence (requester)"},
    {0x87, "receiver not ready - retry limit exceeded"},
    {0x88, "remote access error NAK"},
    {0x89, "remote operation error NAK"},
    {0x8a, "invalid request NAK"},
    {0x8b, "bad AETH"},
    {0x8c, "invalid operation code"},
    {0x8d, "operation sequence error (responder)"},
    {0x8e, "packet MTU violation"},
    {0x8f, "shared receive queue not available"},

    // Responder syndromes
    {0x90, "read operation not permitted"},
    {0x91, "atomic operation not permitted"},
    {0x92, "write operation not permitted"},
    {0x93, "on-demand paging fault"},
    {0x94, "packet sequence number error"},
    {0x95, "QP suspended"},
    {0x96, "QP suspended with pending NACK"},
    {0x97, "retry counter exceeded"},
    {0x98, "virtio fetcher QP error"},
    {0x99, "remote abort - incomplete message"},
    {0x9a, "SRCD violation"},
    {0x9b, "SRC SRQ sequence error"},
    {0x9c, "NVMe emulation error"},
    {0x9d, "GGA error"},
    {0x9e, "ACE error"},
    {0x9f, "GTA error"},

    // RDMA write with immediate TPT
    {0xc0, "[TPT] memory access error during RDMA write with immediate"},
    {0xc8, "[TPT] memory invalidate operation error"},

    // RDE syndromes
    {0xd0, "failed to read end-to-end credits"},
    {0xd1, "atomic operation alignment error"},
    {0xd2, "too many outstanding read/atomic operations"},
    {0xd3, "read/atomic message size exceeded"},
    {0xd4, "send/read-response message size exceeded"},
    {0xd5, "RDMA write message size exceeded"},
    {0xd6, "work queue empty - no valid entries"},
    {0xd7, "work queue exhausted"},
    {0xd8, "bus error reading work queue entry"},
    {0xd9, "atomic or write operation failed"},
    {0xda, "read response length mismatch"},
    {0xdb, "RDMA write with immediate exceeds size limit"},
    {0xdc, "bus error reading next work queue address"},
    {0xdd, "ODP hardware error"},

    // Misc syndromes
    {0xe0, "DC cleanup"},
    {0xe2, "packet error SL difference"},
    {0xe3, "WQE exhausted (offloaded QP)"},
    {0xe4, "send/RRS length exceeded (offloaded QP)"},
    {0xe5, "execution response error"},
    {0xf0, "packet discarded"},
    {0xf1, "DC reconnect"},
    {0xf2, "DC CNAK"},
    {0xf3, "DC reconnect ACK"},
    {0xf4, "pair QP error"},
    {0xf5, "command interface error (to error state)"},
    {0xf6, "error to reset (command interface)"},
    {0xf7, "connection terminated"},
    {0xf8, "destroy QP (command interface)"},
    {0xf9, "flushed in error"},
    {0xfa, "terminated (to error state)"},
    {0xfb, "DC CACK"},
    {0xfc, "DC CNAK retry exceeded"},
    {0xfd, "RQ drained"},
    {0xfe, "inner accelerator error"},
    {0xff, "URB error"},
}};

template <size_t N>
static const char* lookup(uint8_t code, const std::array<syndrome_entry, N>& table)
{
    for (const auto& entry : table) {
        if (entry.code == code) {
            return entry.desc;
        }
    }
    return nullptr;
}

static inline bool is_rnr_nak(uint8_t vendor_synd)
{
    return vendor_synd >= RNR_NAK_BASE && vendor_synd <= RNR_NAK_END;
}

static inline bool is_tpt_syndrome(uint8_t vendor_synd)
{
    uint8_t base = vendor_synd & ~MASK_TPT_3BIT;
    return (base == 0x10 || base == 0x18 || base == 0x20 || base == 0x28 || base == 0x30 ||
            base == 0x38 || base == 0x50 || base == 0x60 || base == 0x78 || base == 0xc0 ||
            base == 0xc8);
}

std::string err_cqe::decode_syndrome(uint8_t syndrome)
{
    const char* desc = lookup(syndrome, IB_SYNDROMES);
    return desc ? desc : "unknown";
}

std::string err_cqe::decode_vendor_error_syndrome(uint8_t vendor_syndrome)
{
    if (is_rnr_nak(vendor_syndrome)) {
        uint8_t timer = vendor_syndrome & MASK_RNR_TIMER;
        return "receiver not ready (RNR NAK, timer=" + std::to_string(timer) + ")";
    }

    if (is_tpt_syndrome(vendor_syndrome)) {
        uint8_t base_code = vendor_syndrome & ~MASK_TPT_3BIT;
        uint8_t detail_code = vendor_syndrome & MASK_TPT_3BIT;

        const char* base_desc = lookup(base_code, VENDOR_SYNDROMES);
        const char* detail_desc = lookup(detail_code, TPT_3BIT_SYNDROMES);

        if (base_desc && detail_desc) {
            return std::string(base_desc) + ": " + detail_desc;
        }
        if (base_desc) {
            return base_desc;
        }
        return "unknown";
    }

    const char* desc = lookup(vendor_syndrome, VENDOR_SYNDROMES);
    return desc ? desc : "unknown";
}

} // namespace dpcp
