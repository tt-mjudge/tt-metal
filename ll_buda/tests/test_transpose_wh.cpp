#include <algorithm>
#include <functional>
#include <random>
#include <vector>

#include "ll_buda/host_api.hpp"
#include "common/bfloat16.hpp"

#include "test_tiles.hpp"
#include "test_gold_impls.hpp"

#include "llrt/tt_debug_print_server.hpp"

using namespace tt;

using u32 = std::uint32_t;
using u16 = std::uint16_t;
using std::vector;

namespace hlk_transpose_wh {
// clone of hlk args from "kernels/compute/eltwise_copy.cpp"
// FIXME:copy pasted the args here from the blank kernel file,  we could refactor the HLK file
struct hlk_args_t {
    std::int32_t NHtWt; // can be interpreted as "num blocks", "num tensors", "z-dim", or "batch" loop
};
}

//////////////////////////////////////////////////////////////////////////////////////////
// Tests transpose kernel in HW dimensions
//////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv) {
    bool pass = true;

    try {
        ////////////////////////////////////////////////////////////////////////////
        //                      Grayskull Device Setup
        ////////////////////////////////////////////////////////////////////////////
        int pci_express_slot = 0;
        ll_buda::Device *device = ll_buda::CreateDevice(tt::ARCH::GRAYSKULL, pci_express_slot);

        pass &= ll_buda::InitializeDevice(device);;

        // Also tests that the debug print server terminates cleanly with new ll_buda APIs
        // (it was previously crashing due to different termination sequence)
        //tt_start_debug_print_server(device->cluster(), {0}, {{1, 1}});

        ////////////////////////////////////////////////////////////////////////////
        //                      Application Setup
        ////////////////////////////////////////////////////////////////////////////
        ll_buda::Program *program = new ll_buda::Program();

        tt_xy_pair core = {0, 0};

        vector<uint32_t> shape = {1, 3, 3*32*1, 4*32*1};
        u32 W = shape[3], H = shape[2], NC = shape[1]*shape[0];
        u32 HW = H*W;
        TT_ASSERT(W % 32 == 0 && H % 32 == 0);
        TT_ASSERT(H > 0 && W > 0 && NC > 0);
        u32 Wt = W/32;
        // TODO(AP): temporary limitation
        // size of DST register, with unary r/w this currently only works if the entire Wt fits into DST for reduce
        TT_ASSERT(Wt <= 16);
        u32 Ht = H/32;
        float scaler = 1.0f/W;
        uint32_t num_tensor_tiles = NC*H*W / (32*32);

        uint32_t single_tile_bytes = 2 * 1024;
        uint32_t dram_buffer_bytes = single_tile_bytes * num_tensor_tiles; // num_tiles of FP16_B, hard-coded in the reader/writer kernels

        uint32_t dram_buffer_src0_addr = 0;
        int dram_src0_channel_id = 0;
        uint32_t dram_buffer_dst_addr = 512 * 1024 * 1024; // 512 MB (upper half)
        int dram_dst_channel_id = 0;

        auto src0_dram_buffer = ll_buda::CreateDramBuffer(device, dram_src0_channel_id, dram_buffer_bytes, dram_buffer_src0_addr);
        auto dst_dram_buffer = ll_buda::CreateDramBuffer(device, dram_dst_channel_id, dram_buffer_bytes, dram_buffer_dst_addr);
        auto dram_src0_noc_xy = src0_dram_buffer->noc_coordinates();
        auto dram_dst_noc_xy = dst_dram_buffer->noc_coordinates();

        uint32_t src0_cb_index = 0;
        uint32_t src0_cb_addr = 200 * 1024;
        uint32_t num_buffer_tiles = 32;
        // this buffer is used in transpose_hc.cpp NCRISC kernel
        auto cb_src0 = ll_buda::CreateCircularBuffer(
            program,
            device,
            src0_cb_index,
            core,
            num_buffer_tiles,
            num_buffer_tiles * single_tile_bytes,
            src0_cb_addr,
            tt::DataFormat::Float16_b
        );

        uint32_t ouput_cb_index = 16; // output operands start at index 16
        uint32_t output_cb_addr = 300 * 1024;
        uint32_t num_output_buffer_tiles = 32;
        // this buffer is used in writer_unary.cpp BRISC kernel
        auto cb_output = ll_buda::CreateCircularBuffer(
            program,
            device,
            ouput_cb_index,
            core,
            num_output_buffer_tiles,
            num_output_buffer_tiles * single_tile_bytes,
            output_cb_addr,
            tt::DataFormat::Float16_b
        );

        auto reader_kernel = ll_buda::CreateDataMovementKernel(
            program,
            //"kernels/dataflow/reader_unary_transpose_wh.cpp",
            "kernels/dataflow/reader_unary_transpose_wh_8bank.cpp",
            core,
            ll_buda::DataMovementProcessor::RISCV_1,
            ll_buda::NOC::RISCV_1_default);

        auto unary_writer_kernel = ll_buda::CreateDataMovementKernel(
            program,
            //"kernels/dataflow/writer_unary.cpp",
            "kernels/dataflow/writer_unary_8bank.cpp",
            core,
            ll_buda::DataMovementProcessor::RISCV_0,
            ll_buda::NOC::RISCV_0_default);

        void *hlk_args = new hlk_transpose_wh::hlk_args_t{ .NHtWt = int(Ht*Wt*NC) };
        ll_buda::ComputeKernelArgs *compute_args = ll_buda::InitializeCompileTimeComputeKernelArgs(core, hlk_args, sizeof(reduce_args::hlk_args_t));
        bool fp32_dest_acc_en = false;
        bool math_approx_mode = false;
        auto reduce_w_compute_kernel = ll_buda::CreateComputeKernel(
            program,
            "kernels/compute/transpose_wh.cpp",
            core,
            compute_args,
            MathFidelity::HiFi4,
            fp32_dest_acc_en,
            math_approx_mode
        );

        ////////////////////////////////////////////////////////////////////////////
        //                      Compile Application
        ////////////////////////////////////////////////////////////////////////////
        bool skip_hlkc = false;
        pass &= ll_buda::CompileProgram(device, program, skip_hlkc);

        ////////////////////////////////////////////////////////////////////////////
        //                      Execute Application
        ////////////////////////////////////////////////////////////////////////////
        auto seed = std::chrono::system_clock::now().time_since_epoch().count();
        vector<uint32_t> src0_vec = create_random_vector_of_bfloat16(dram_buffer_bytes, 100.0f, 0x1234);
        //pass &= ll_buda::WriteToDeviceDRAMChannel(device, dram_src0_channel_id, src0_vec, src0_dram_buffer->address());
        pass &= ll_buda::WriteToDeviceDRAMChannelsInterleavedTiles(device, src0_vec, src0_dram_buffer->address());

        pass &= ll_buda::ConfigureDeviceWithProgram(device, program);

        ll_buda::WriteRuntimeArgsToDevice(
            device,
            reader_kernel,
            core,
            {
                dram_buffer_src0_addr,
                (std::uint32_t)dram_src0_noc_xy.x,
                (std::uint32_t)dram_src0_noc_xy.y,
                num_tensor_tiles, NC, Ht, Wt, Ht*Wt
            }
        );

        ll_buda::WriteRuntimeArgsToDevice(
            device,
            unary_writer_kernel,
            core,
            {
                dram_buffer_dst_addr,
                (std::uint32_t)dram_dst_noc_xy.x,
                (std::uint32_t)dram_dst_noc_xy.y,
                num_tensor_tiles
            }
        );

        pass &= ll_buda::LaunchKernels(device, program);

        // The kernel will view the input as TILED32_4FACES
        vector<uint32_t> result_vec;
        //ll_buda::ReadFromDeviceDRAMChannel(
        //    device, dram_dst_channel_id, dst_dram_buffer->address(), result_vec, dst_dram_buffer->size());
        ll_buda::ReadFromDeviceDRAMChannelsInterleavedTiles(
            device, dst_dram_buffer->address(), result_vec, dst_dram_buffer->size());
        TT_ASSERT(result_vec.size() == NC*H*W/2); // we are expecting one tile in H, and half the elements since the vector packs 2 uint16_ts

        ////////////////////////////////////////////////////////////////////////////
        //                      Validation & Teardown
        ////////////////////////////////////////////////////////////////////////////

        int argfail = -1;
        auto comparison_function = [](float a, float b) {
            const float rtol = 0.02f;
            const float atol = 1e-3f;
            float maxabs = fmaxf(fabsf(a), fabsf(b));
            float absdiff = fabsf(a - b);
            auto result = (absdiff <= atol) || absdiff < rtol * maxabs;
            if (!result)
                absdiff *= 1.0f; // breakpoint spot
            return result;
        };

        // recover a linear view of input vector for consumption by gold_ function
        auto u16_src0_vec = u16_from_u32_vector(src0_vec);
        vector<u16> src_linear = convert_layout<u16>(u16_src0_vec, shape, TensorLayout::TILED32_4FACES, TensorLayout::LIN_ROW_MAJOR);
        vector<u16> gold_reduced = gold_transpose_wh(src_linear, shape); // result is u16 untilized

        // Tilize from row major and convert to pairs (u32)
        vector<uint32_t> shapeR{shape[0], shape[1], shape[3], shape[2]};
        auto gold_4f_u32 = u32_from_u16_vector(convert_layout<u16>(gold_reduced, shapeR, TensorLayout::LIN_ROW_MAJOR, TensorLayout::TILED32_4FACES));

        pass &= packed_uint32_t_vector_comparison(result_vec, gold_4f_u32, comparison_function, &argfail);
        if (!pass)
            log_error(LogTest, "Failure position={}", argfail);

        pass &= ll_buda::CloseDevice(device);;

    } catch (const std::exception &e) {
        pass = false;
        // Capture the exception error message
        log_error(LogTest, "{}", e.what());
        // Capture system call errors that may have returned from driver/kernel
        log_error(LogTest, "System error message: {}", std::strerror(errno));
    }

    if (pass) {
        log_info(LogTest, "Test Passed");
    } else {
        log_fatal(LogTest, "Test Failed");
    }

    TT_ASSERT(pass);

    return 0;
}
