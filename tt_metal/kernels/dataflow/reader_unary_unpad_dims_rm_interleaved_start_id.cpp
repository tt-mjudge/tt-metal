// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>
#include "dataflow_api.h"

void kernel_main() {

    const uint32_t src_addr                 = get_arg_val<uint32_t>(0);
    const uint32_t padded_stick_size        = get_arg_val<uint32_t>(1);
    const uint32_t unpadded_stick_size      = get_arg_val<uint32_t>(2);
    const uint32_t num_dims                 = get_arg_val<uint32_t>(3);
    const uint32_t start_id                 = get_arg_val<uint32_t>(4);
    const uint32_t num_sticks               = get_arg_val<uint32_t>(5);

    volatile tt_l1_ptr uint32_t * num_unpadded_sticks = (volatile tt_l1_ptr uint32_t*)(get_arg_addr(6));
    volatile tt_l1_ptr uint32_t * num_padded_sticks = num_unpadded_sticks + num_dims;
    volatile tt_l1_ptr uint32_t * id_per_dim = num_padded_sticks + num_dims;

    constexpr bool src0_is_dram          = get_compile_time_arg_val(0) == 1;
    #define src_stick_size_is_pow2 get_compile_time_arg_val(1) == 1
    #if (src_stick_size_is_pow2)
    constexpr uint32_t src_log_base_2_of_page_size = get_compile_time_arg_val(2);
    const InterleavedPow2AddrGen<src0_is_dram> s0 = {
        .bank_base_address = src_addr,
        .log_base_2_of_page_size = src_log_base_2_of_page_size // TODO(AP): refactor
    };
    #else
    const InterleavedAddrGen<src0_is_dram> s0 = {
        .bank_base_address = src_addr,
        .page_size = padded_stick_size
    };
    #endif

    constexpr uint32_t cb_id_in0 = 0;

    uint32_t src_stick_id = start_id;

    for(uint32_t i = 0; i < num_sticks; i++) {
        // Copy Input
        cb_reserve_back(cb_id_in0, 1);
        uint32_t src_buffer_l1_addr = get_write_ptr(cb_id_in0);
        uint64_t src_noc_addr = get_noc_addr(src_stick_id, s0);
        noc_async_read(src_noc_addr, src_buffer_l1_addr, unpadded_stick_size);
        noc_async_read_barrier();
        cb_push_back(cb_id_in0, 1);
        src_stick_id++;
        for(uint32_t j = 0; j < num_dims; j++) {
            id_per_dim[j]++;
            if (id_per_dim[j] == num_unpadded_sticks[j]) {
                id_per_dim[j] = 0;
                src_stick_id += num_padded_sticks[j];
            } else {
                break;
            }
        }
    }
}
