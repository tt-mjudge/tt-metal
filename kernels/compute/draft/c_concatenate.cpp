#include <cstdint>

#include "compute_hlk_api.h"

struct hlk_args_t {
    std::int32_t num_input_buffers;
    std::int32_t num_tiles_per_input_buffer;
};

void hlk_main(tt_core *core_ptr, const hlk_args_t *args) {

    for (int input_index = 0; input_index < args->num_input_buffers; input_index++) {
        for (int tile_index = 0; tile_index < args->num_tiles_per_input_buffer; tile_index++) {
            hlk_acquire_dst(core_ptr, DstMode::Half);

            // wait and pop tile-by-tile -- such that we can use minimally sized buffers for each of the inputs
            hlk_wait_tiles(core_ptr, HlkOperand::in0 + input_index, 1);
            hlk_wait_for_free_tiles(core_ptr, HlkOperand::out0, 1);

            hlk_copy_tile_to_dst(core_ptr, HlkOperand::in0 + input_index, 0, 0);
            hlk_pack_tile_to_stream(core_ptr, 0, HlkOperand::out0);

            hlk_pop_tiles(core_ptr, HlkOperand::in0 + input_index, 1);
            hlk_push_tiles(core_ptr, HlkOperand::out0, 1);

            hlk_release_dst(core_ptr, DstMode::Half);
        }
    }
}
