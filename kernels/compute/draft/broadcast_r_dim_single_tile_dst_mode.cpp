#include <cstdint>

#include "compute_hlk_api.h"

struct hlk_args_t {
    std::int32_t per_core_num_blocks; // can be interpreted as "num blocks", "num tensors", "z-dim", or "batch" loop
    std::int32_t per_block_r_tiles;
    std::int32_t per_block_c_tiles;
};

// do this for "num blocks" (per_core_num_blocks)
//    In0: an RM block (per_core_r_tile, per_block_c_tiles)
//    In1: an RM block (1, per_block_c_tiles)
//    wait for a row in In0, add bcast to it entire row of In1, push the row to Out0
//    Out0: an RM block (per_block_r_tiles, per_block_c_tiles)
void hlk_main(tt_core *core_ptr, const hlk_args_t *args) {

    for (int b = 0; b < args->per_core_num_blocks; b++) {

        hlk_wait_tiles(core_ptr, HlkOperand::in0, args->per_block_c_tiles);

        for(int i = 0; i < args->per_block_r_tiles ; ++i) {

            for(int j = 0; j < args->per_block_c_tiles ; ++j) {
                hlk_acquire_dst(core_ptr, DstMode::Half);
                hlk_broadcast_tile(core_ptr, (int)Dim::R, HlkOperand::in0, j, 0);

                hlk_wait_for_free_tiles(core_ptr, HlkOperand::out0, 1);
                hlk_pack_tile_to_stream(core_ptr, 0, HlkOperand::out0);
                hlk_push_tiles(core_ptr, HlkOperand::out0, 1);

                hlk_release_dst(core_ptr, DstMode::Half);
            }
        }

        hlk_pop_tiles(core_ptr, HlkOperand::in0, args->per_block_c_tiles);
    }
}
