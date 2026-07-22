/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "common/def.h"
#include "common/log.h"
#include "common/sys.h"
#include "common/base.h"

#include "dpcp_base.h"

using namespace dpcp;

class dpcp_err_cqe : public dpcp_base {};

/**
 * @test dpcp_err_cqe.ti_01_ib_known_code
 * @brief
 *    Check decode_syndrome handles a known IB syndrome
 * @details
 *    Validates that a known IB code returns the proper expected string
 */
TEST_F(dpcp_err_cqe, ti_01_ib_known_code)
{
    std::string result = err_cqe::decode_syndrome(0x01);
    EXPECT_NE(result.find("local length error"), std::string::npos) 
        << "IB syndrome 0x01 should contain 'local length error', got: " << result;
}

/**
 * @test dpcp_err_cqe.ti_02_ib_unknown_code
 * @brief
 *    Check decode_syndrome returns "unknown" for undefined codes
 * @details
 *    Tests that an undefined IB syndrome code returns "unknown"
 */
TEST_F(dpcp_err_cqe, ti_02_ib_unknown_code)
{
    std::string result = err_cqe::decode_syndrome(0xFF);
    EXPECT_EQ(result, "unknown") << "Undefined IB syndrome should return 'unknown'";
}

/**
 * @test dpcp_err_cqe.ti_03_vendor_discrete_code
 * @brief
 *    Check decode_vendor_error_syndrome handles a discrete vendor syndrome
 * @details
 *    Validates a single discrete vendor syndrome returns the proper expected string
 */
TEST_F(dpcp_err_cqe, ti_03_vendor_discrete_code)
{
    std::string result = err_cqe::decode_vendor_error_syndrome(0x80);
    EXPECT_NE(result.find("timeout"), std::string::npos)
        << "Vendor syndrome 0x80 should contain 'timeout', got: " << result;
}

/**
 * @test dpcp_err_cqe.ti_04_vendor_unknown_code
 * @brief
 *    Check decode_vendor_error_syndrome returns "unknown" for undefined codes
 * @details
 *    Tests that an undefined vendor syndrome returns "unknown"
 */
TEST_F(dpcp_err_cqe, ti_04_vendor_unknown_code)
{
    std::string result = err_cqe::decode_vendor_error_syndrome(0x01);
    EXPECT_EQ(result, "unknown") << "Undefined vendor syndrome should return 'unknown'";
}

/**
 * @test dpcp_err_cqe.ti_05_vendor_tpt_with_detail
 * @brief
 *    Check decode_vendor_error_syndrome handles TPT syndrome with detail
 * @details
 *    Tests that a TPT base code with 3-bit detail is properly decoded
 */
TEST_F(dpcp_err_cqe, ti_05_vendor_tpt_with_detail)
{
    std::string result = err_cqe::decode_vendor_error_syndrome(0x32);
    EXPECT_NE(result.find("[TPT]"), std::string::npos) 
        << "Should contain '[TPT]', got: " << result;
    EXPECT_NE(result.find("protection domain violation"), std::string::npos) 
        << "Should contain 'protection domain violation', got: " << result;
}

/**
 * @test dpcp_err_cqe.ti_06_vendor_rnr_nak
 * @brief
 *    Check decode_vendor_error_syndrome handles RNR NAK range
 * @details
 *    Tests that RNR NAK syndrome (0xA0-0xBF) is properly decoded with timer value
 */
TEST_F(dpcp_err_cqe, ti_06_vendor_rnr_nak)
{
    std::string result = err_cqe::decode_vendor_error_syndrome(0xA5);
    EXPECT_NE(result.find("receiver not ready"), std::string::npos)
        << "RNR syndrome should contain 'receiver not ready', got: " << result;
    EXPECT_NE(result.find("RNR NAK"), std::string::npos)
        << "RNR syndrome should contain 'RNR NAK', got: " << result;
    EXPECT_NE(result.find("timer=5"), std::string::npos)
        << "RNR syndrome 0xA5 should have 'timer=5', got: " << result;
}
