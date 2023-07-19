#include <cstdint>

#define REDUCE_OP PoolType::SUM
#define REDUCE_DIM ReduceDim::REDUCE_ROW

#define BCAST_LLKOP EltwiseBinaryType::ELWMUL
#define BCAST_DIM BroadcastType::COL

#include "compute_kernel_api.h"



ALWI void ACQ() { acquire_dst(tt::DstMode::Half); }
ALWI void REL() { release_dst(tt::DstMode::Half); }


namespace NAMESPACE {
void MAIN {
    constexpr uint32_t NCHt = get_compile_time_arg_val(0);
    constexpr uint32_t Wt = get_compile_time_arg_val(1);
    constexpr uint32_t blk = get_compile_time_arg_val(2);
    constexpr uint32_t do_gamma = get_compile_time_arg_val(3);
    constexpr uint32_t do_beta = get_compile_time_arg_val(4);


    #ifdef FUSE_PRE_ADD
        binary_op_init_common(tt::CB::c_in0, tt::CB::c_in1);
    #else
        binary_op_init_common(tt::CB::c_in0, tt::CB::c_in0);
    #endif

    constexpr uint32_t onetile = 1;
    // reserve one tile for zeros on cb_in2
    // TODO(AP): check that if DST is indeed zeroed by release_dst (and initially), we can use it as zeroes

    // Note that the entire W dimension must fit in the intermed0 CB for this kernel to be correct
    constexpr auto cb_scaler = tt::CB::c_in2; // single tile generated by the reader
    constexpr auto cb_eps = tt::CB::c_in3; // single tile generated by the reader
    constexpr auto cb_ex = tt::CB::c_intermed1; // E[x]
    constexpr auto cb_ex2 = tt::CB::c_intermed2; // E[(x-E[x])^2]
    constexpr auto cb_x2 = tt::CB::c_intermed3; // x^2
    constexpr auto cb_ex2pe = tt::CB::c_intermed4; // E[(x-E[x])^2]+eps
    constexpr auto cb_in = tt::CB::c_in0; // input x or a for fused pre-add (x=a+b)
    constexpr auto cb_inb = tt::CB::c_in1; // input b for fused pre-add
    constexpr auto cb_out = tt::CB::c_out0; // output
    constexpr auto cb_gamma = tt::CB::c_in5;
    constexpr auto cb_beta = tt::CB::c_in6;
    constexpr auto cb_fusion = tt::CB::c_intermed5; // stream gamma/beta
    constexpr auto scaler0 = 0;
    #ifdef FUSE_PRE_ADD
    constexpr auto cb_x = tt::CB::c_intermed6;
    #else
    constexpr auto cb_x = tt::CB::c_in0;
    #endif

    cb_wait_front(cb_scaler, 1); // comes from the reader
    cb_wait_front(cb_eps, 1); // comes from the reader


    constexpr int cb_im_or_out = (do_gamma|do_beta) ? cb_fusion : tt::CB::c_out0;


    for (uint32_t ncht = 0; ncht < NCHt; ncht++) {

        constexpr int onetile = 1;
        constexpr int dst0 = 0;

        /*
         * X + Y
         */
        #ifdef FUSE_PRE_ADD
            add_tiles_init();
            for (uint32_t wt = 0; wt < Wt; wt += blk) {
                ACQ();
                        //UNPACK(( { DPRINT  << "Waiting on cb_x" << ENDL(); } ));
                cb_wait_front(cb_in, blk);
                        //UNPACK(( { DPRINT  << "Waiting on cb_inb" << ENDL(); } ));
                cb_wait_front(cb_inb, blk);
                        //UNPACK(( { DPRINT  << "Done Waiting on cb_inb" << ENDL(); } ));
                cb_reserve_back(cb_x, blk);
                for (uint32_t j = 0; j < blk; j++) {
                    add_tiles(cb_in, cb_inb, j, j, j);
                    pack_tile(j, cb_x);
                }
                REL();
                cb_push_back(cb_x, blk); // push the sum into the same buffer
                cb_pop_front(cb_in, blk);
                cb_pop_front(cb_inb, blk);
            }
            // by the end of this loop we should end up with Wt tiles in cb_x
        #endif

        /* (x)^2
         * compute temp = x^2
         */
        mul_tiles_init();
        for (uint32_t wt = 0; wt < Wt; wt += blk) {
            cb_wait_front(cb_x, wt+blk);
            cb_reserve_back(cb_x2, blk); // can probably use less space for this if we block
            ACQ();
            for (uint32_t wtr = 0; wtr<blk; wtr++) {
                mul_tiles(cb_x, cb_x, wt+wtr, wt+wtr, wtr);
                //mul_tiles(cb_xmm, cb_col1, wt+wtr, wt+wtr, wtr);
                pack_tile(wtr, cb_x2);
            }
            cb_push_back(cb_x2, blk);
            REL();
        }

        /* Var(x)
         * compute E[(x)^2]
         */
        cb_reserve_back(cb_ex2, 1);
        reduce_init_delta_v2<false>(REDUCE_OP, REDUCE_DIM);
        ACQ();
        cb_wait_front(cb_x2, Wt);
        //cb_wait_front(cb_xmm, Wt);
        for (uint32_t wt = 0; wt < Wt; wt += blk) {
            // reduce
            for (uint32_t wtr = 0; wtr<blk; wtr++)
                reduce_tile_v2(REDUCE_OP, REDUCE_DIM, cb_x2, cb_scaler, wt+wtr, scaler0, dst0);
                //reduce_tile_v2(REDUCE_OP, REDUCE_DIM, cb_xmm, cb_scaler, wt+wtr, scaler0, dst0);
        }
        cb_pop_front(cb_x2, Wt);
        reduce_revert_delta_v2();
        pack_tile(dst0, cb_ex2);
        REL();

        cb_push_back(cb_ex2, 1);
        cb_wait_front(cb_ex2, 1);

        /* Var(x) + eps
         * add epsilon E[(x-E[x])^2]+eps
         */
        ACQ();
        add_tiles_init();
        add_tiles(cb_ex2, cb_eps, 0, 0, dst0);

        cb_reserve_back(cb_ex2pe, 1); // 1
        sqrt_tile_init();
        sqrt_tile(dst0);
        recip_tile_init();
        recip_tile(dst0);
        pack_tile(dst0, cb_ex2pe);
        cb_push_back(cb_ex2pe, 1);
        REL();

        /* ln(x) * gamma + beta (gamma and beta are optional)
         * now xmm = (x-E[x])
         * we have 1.0/sqrt( E[(x-E[x])^2] + eps) in cb_ex2pe
         * just need to bcast_mul xmm with cb_ex2pe
         */
        cb_reserve_back(cb_ex2pe, 1); // 2
        cb_wait_front(cb_ex2pe, 1);
        for (uint32_t wt = 0; wt < Wt; wt += blk) {
                        //if (ht == 1) UNPACK(( DPRINT << "wt_2=" << wt << " " ));
                        //if (ht == 1) UNPACK(( DPRINT << "rem_2=" << rem << ENDL() ));
            cb_reserve_back(cb_im_or_out, blk);

            ACQ();
            mul_bcast_cols_init_short();
            for (uint32_t wtr = 0; wtr < blk; wtr++) {
                // cb_xmm[wt+wtr] since we pop Wt from cb_xmm after the entire loop
                mul_tiles_bcast_cols(cb_x, cb_ex2pe, wt+wtr, 0, wtr); // tile *= 1/(sum(exp(x)))
                pack_tile(wtr, cb_im_or_out); // pack either to intermediate (cb_fusion or out0)
            }
            cb_push_back(cb_im_or_out, blk); // if no gamma/beta are provided, this will be passed on to the writer
            REL();

            if (do_gamma) {
                ACQ();
                uint32_t cb_outg = do_beta ? cb_fusion : tt::CB::c_out0;
                mul_bcast_rows_init_short();
                cb_reserve_back(cb_outg, blk);
                cb_wait_front(cb_gamma, wt+blk); // we don't pop, TODO: only wait on first ht
                cb_wait_front(cb_fusion, blk);
                for (uint32_t wtr = 0; wtr < blk; wtr++) {
                    mul_tiles_bcast_rows(cb_fusion, cb_gamma, wtr, wt+wtr, wtr); // tile *= 1/(sum(exp(x)))
                    pack_tile(wtr, cb_outg); // pack either to intermediate (cb_fusion or out0)
                }
                cb_pop_front(cb_fusion, blk);
                // we don't pop gamma
                cb_push_back(cb_outg, blk);
                // We don't pop gamma since it's 1,1,1,Wt and we reuse it for all NCHt
                REL();
            }
            if (do_beta) {
                ACQ();
                add_bcast_rows_init_short();
                cb_reserve_back(tt::CB::c_out0, blk);
                cb_wait_front(cb_beta, wt+blk); // TODO: optimization - only wait on first ht
                cb_wait_front(cb_fusion, blk);
                for (uint32_t wtr = 0; wtr < blk; wtr++) {
                    add_tiles_bcast_rows(cb_fusion, cb_beta, wtr, wt+wtr, wtr); // tile *= 1/(sum(exp(x)))
                    pack_tile(wtr, tt::CB::c_out0); // pack either to intermediate (cb_fusion or out0)
                }
                cb_pop_front(cb_fusion, blk);
                // We don't pop beta since it's 1,1,1,Wt and we reuse it for all NCHt
                cb_push_back(tt::CB::c_out0, blk);
                REL();
            }
        }
        cb_pop_front(cb_ex2pe, 1);
        cb_pop_front(cb_x, Wt);

    } // NCHt loop
    //cb_pop_front(cb_scaler, 1); // optional for correctness
    //cb_pop_front(cb_eps, 1); // optional for correctness
    //cb_pop_front(cb_col1, 1); // optional for correctness
}
}
