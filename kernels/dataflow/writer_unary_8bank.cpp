#include "dataflow_api.h"

void kernel_main() {
    uint32_t dst_addr  = get_arg_val<uint32_t>(0);
    uint32_t num_tiles = get_arg_val<uint32_t>(3); // Index 3 to match with regular writer_unary

    constexpr uint32_t cb_id_out0 = 16;

    // single-tile ublocks
    constexpr uint32_t onetile = 1; 
    uint32_t tile_bytes = get_tile_size(cb_id_out0);

    for (uint32_t i = 0; i<num_tiles; i ++) {
        uint64_t dst_noc_addr = get_noc_addr(i, dst_addr, 8, 3, 11); // TODO(AP): refactor

        cb_wait_front(cb_id_out0, onetile);
        uint32_t l1_read_addr = get_read_ptr(cb_id_out0);

        noc_async_write(l1_read_addr, dst_noc_addr, tile_bytes);

        noc_async_write_barrier();

        cb_pop_front(cb_id_out0, onetile);
    }
}
    

