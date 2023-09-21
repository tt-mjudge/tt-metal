// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>
#include "dataflow_api.h"

void kernel_main() {


    uint32_t dst_addr                 = get_arg_val<uint32_t>(0);
    uint32_t stick_size               = get_arg_val<uint32_t>(1);
    uint32_t num_sticks               = get_arg_val<uint32_t>(2);
    uint32_t start_id                 = get_arg_val<uint32_t>(3);

    constexpr uint32_t cb_id_out0 = get_compile_time_arg_val(0) == 1;
    constexpr bool dst0_is_dram          = get_compile_time_arg_val(1) == 1;


    const InterleavedAddrGen<dst0_is_dram> s0 = {
        .bank_base_address = dst_addr,
        .page_size = stick_size
    };


    for (uint32_t i = start_id; i < start_id + num_sticks; i++) {
        cb_wait_front(cb_id_out0, 1);
        uint32_t l1_read_addr = get_read_ptr(cb_id_out0);
        uint64_t dst_noc_addr = get_noc_addr(i, s0);
        noc_async_write(l1_read_addr, dst_noc_addr, stick_size);
        noc_async_write_barrier();
        cb_pop_front(cb_id_out0, 1);
    }
}
