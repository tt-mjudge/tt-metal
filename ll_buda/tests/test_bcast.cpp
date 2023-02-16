#include <algorithm>
#include <functional>
#include <random>
#include <vector>
#include <map>

#include "ll_buda/host_api.hpp"
#include "common/bfloat16.hpp"

#include "test_tiles.hpp"
#include "test_gold_impls.hpp"
#include "constants.hpp"

#include "llrt/tt_debug_print_server.hpp"

using namespace tt;
using namespace constants;

using u32 = std::uint32_t;
using u16 = std::uint16_t;
using std::vector;


namespace {
const char* get_reader_name(bool multibank, BcastDim::Enum bcast_dim) {
    if (bcast_dim == BcastDim::H) {
        return multibank ?
            "kernels/dataflow/reader_bcast_h_8bank.cpp" :
            "kernels/dataflow/reader_bcast_h.cpp";
    } else if (bcast_dim == BcastDim::W) {
        return multibank ?
            "kernels/dataflow/reader_bcast_w_8bank.cpp" :
            "kernels/dataflow/reader_bcast_w.cpp";
    } if (bcast_dim == BcastDim::HW) {
        return multibank ?
            "kernels/dataflow/reader_dual_8bank.cpp" :
            "kernels/dataflow/reader_binary_diff_lengths.cpp";
    }
    TT_ASSERT(false && "Unexpected bcast_dim!");
    return "";
}

const char* get_compute_name(BcastDim::Enum bcast_dim) {
    switch (bcast_dim) {
        case BcastDim::H:  return "kernels/compute/bcast_h.cpp";
        case BcastDim::W:  return "kernels/compute/bcast_w.cpp";
        case BcastDim::HW: return "kernels/compute/bcast_hw.cpp";
        default:           TT_ASSERT(false && "Unexpected bcast_dim!");
    }
    return "";
}

}

//////////////////////////////////////////////////////////////////////////////////////////
// Tests reduce_h kernel in H dimension (NCHW->NC1W)
//////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv) {
    bool pass = true;

    // convert from bcast mode as defined in compute_hlk_api.h Dim enum to kernel name string
    // W = 2, H = 1, HW = 4
    const char* bdim_to_log_string[] = { "", "BCAST_H", "BCAST_W", "", "BCAST_HW" };
    const char* op_id_to_op_define[] = {"add_tiles_bcast", "sub_tiles_bcast", "mul_tiles_bcast"};
    const char* op_id_to_op_name[] = {"ADD", "SUB", "MUL"};
    bool multibank = false;

    auto bdims = BcastDim::all();
    auto ops = BcastOp::all();
    for (auto bcast_op: ops) {
    for (auto bcast_dim: bdims) {

    log_info(LogTest, "=============================================================");
    log_info(LogTest, "======= Running bcast test for bdim={}, op={}", bdim_to_log_string[bcast_dim], op_id_to_op_name[bcast_op]);
    try {
        ////////////////////////////////////////////////////////////////////////////
        //                      Grayskull Device Setup
        ////////////////////////////////////////////////////////////////////////////
        int pci_express_slot = 0;
        ll_buda::Device *device = ll_buda::CreateDevice(tt::ARCH::GRAYSKULL, pci_express_slot);

        pass &= ll_buda::InitializeDevice(device);;

        // Also tests that the debug print server terminates cleanly with new ll_buda APIs
        // (it was previously crashing due to different termination sequence)
        tt_start_debug_print_server(device->cluster(), {0}, {{1, 1}});

        ////////////////////////////////////////////////////////////////////////////
        //                      Application Setup
        ////////////////////////////////////////////////////////////////////////////
        ll_buda::Program *program = new ll_buda::Program();

        tt_xy_pair core = {0, 0};

        vector<uint32_t> shape = {1, 4, 2*TILE_HEIGHT, 3*TILE_WIDTH};
        u32 W = shape[3], H = shape[2], NC = shape[1]*shape[0];
        u32 HW = H*W;
        TT_ASSERT(W % TILE_WIDTH == 0 && H % TILE_HEIGHT == 0);
        TT_ASSERT(H > 0 && W > 0 && NC > 0);
        u32 Wt = W/TILE_WIDTH;
        u32 Ht = H/TILE_HEIGHT;
        uint32_t num_tensor_tiles = NC*H*W / (32*32);

        uint32_t single_tile_bytes = 2 * 1024;
        uint32_t dram_buffer_bytes = single_tile_bytes * num_tensor_tiles; // num_tiles of FP16_B, hard-coded in the reader/writer kernels
        
        uint32_t dram_buffer_src0_addr = 0;
        int dram_src0_channel_id = 0;
        uint32_t dram_buffer_src1_addr = 256 * 1024 * 1024; // needs to be at a different address for multi-bank
        int dram_src1_channel_id = 0;
        uint32_t dram_buffer_dst_addr = 512 * 1024 * 1024; // 512 MB (upper half)
        int dram_dst_channel_id = 0;

        auto src0_dram_buffer = ll_buda::CreateDramBuffer(dram_src0_channel_id, dram_buffer_bytes, dram_buffer_src0_addr);
        auto dst_dram_buffer = ll_buda::CreateDramBuffer(dram_dst_channel_id, dram_buffer_bytes, dram_buffer_dst_addr);
        auto dram_src0_noc_xy = src0_dram_buffer->noc_coordinates(device);
        auto dram_dst_noc_xy = dst_dram_buffer->noc_coordinates(device);

        uint32_t src0_cb_index = 0;
        uint32_t src0_cb_addr = 200 * 1024;
        uint32_t num_buffer_tiles = 2;
        // this buffer is used in transpose_hc.cpp NCRISC kernel
        auto cb_src0 = ll_buda::CreateCircularBuffer(
            program,
            src0_cb_index,
            core,
            num_buffer_tiles,
            num_buffer_tiles * single_tile_bytes,
            src0_cb_addr,
            tt::DataFormat::Float16_b
        );

        uint32_t src1_cb_index = 1;
        uint32_t src1_cb_addr = 300 * 1024;
        auto cb_src1 = ll_buda::CreateCircularBuffer(
            program,
            src1_cb_index,
            core,
            num_buffer_tiles,
            num_buffer_tiles * single_tile_bytes,
            src1_cb_addr,
            tt::DataFormat::Float16_b
        );

        uint32_t ouput_cb_index = 16; // output operands start at index 16
        uint32_t output_cb_addr = 400 * 1024;
        uint32_t num_output_buffer_tiles = 2;
        // this buffer is used in writer_unary.cpp BRISC kernel
        auto cb_output = ll_buda::CreateCircularBuffer(
            program,
            ouput_cb_index,
            core,
            num_output_buffer_tiles,
            num_output_buffer_tiles * single_tile_bytes,
            output_cb_addr,
            tt::DataFormat::Float16_b
        );

        vector<uint16_t> tiled_bcast_values;
        vector<uint16_t> ref_bcast_values;
        vector<uint32_t> ref_bcast_shape = {1,1,1,1};
        float bcast_1value = 10.0f;
        uint16_t bcast_1value16 = bfloat16(bcast_1value).to_uint16();
        unsigned num_bcast_tiles = 0;
        // build the constant tiles to be broadcast
        if (bcast_dim == BcastDim::HW) {
            num_bcast_tiles = 1;
            ref_bcast_values.resize(1, 0);
            ref_bcast_values[0] = bcast_1value16;
            // convert the reference broadcast tensor to tiled format
            tiled_bcast_values = convert_layout<u16>(
                ref_bcast_values, ref_bcast_shape, TensorLayout::LIN_ROW_MAJOR, TensorLayout::TILED32_4FACES);
            TT_ASSERT(tiled_bcast_values[0] == bcast_1value16);
            // restore ref values and shape to 1
            ref_bcast_values.resize(1);
            ref_bcast_shape[3] = 1;
        } else if (bcast_dim == BcastDim::H) {
            // For bcast_h a.k.a. Dim::R we broadcast _over_ H, meaning we take a W vector and += it over each element in the H dimension
            // At least that's the behavior i've seen from a single tile bcast-H
            // So this is why here we create a W-sized vector
            // Same for the if branch for BCAST_W below
            TT_ASSERT(W%32 == 0);
            // pad values and shape with extra 32 values because the reader kernel expects it
            // generate broadcast values along the W axis with one extra tile (needed by the kernel I believe)
            // TODO(AP): need to figure out why the extra tile in broadcast inputs is expected by the kernel
            ref_bcast_values.resize(W, 0);
            ref_bcast_shape[3] = W;
            for (int j = 0; j < W; j++)
                // add something not too large but different between tiles
                ref_bcast_values[j] = bfloat16(bcast_1value+(j%7)).to_uint16();
            tiled_bcast_values = convert_layout<u16>(
                ref_bcast_values, ref_bcast_shape, TensorLayout::LIN_ROW_MAJOR, TensorLayout::TILED32_4FACES);
            num_bcast_tiles = Wt;
            // restore values and shape to W
        } else if (bcast_dim == BcastDim::W) {
            // see the comments above for BCAST_H
            ref_bcast_values.resize(H, 0);
            ref_bcast_shape[2] = H;
            for (int j = 0; j < H; j++)
                // add something not too large but different between tiles
                ref_bcast_values[j] = bfloat16(bcast_1value+(j%7)).to_uint16();
            tiled_bcast_values = convert_layout<u16>(
                ref_bcast_values, ref_bcast_shape, TensorLayout::LIN_ROW_MAJOR, TensorLayout::TILED32_4FACES);
            num_bcast_tiles = Ht;
        }

        auto bcast_tiled_u32 = u32_from_u16_vector(tiled_bcast_values);
        auto bcast_vals_nbytes = bcast_tiled_u32.size()*sizeof(bcast_tiled_u32[0]);
        auto src1_dram_buffer = ll_buda::CreateDramBuffer(
            dram_src1_channel_id, bcast_vals_nbytes, dram_buffer_src1_addr);
        auto dram_src1_noc_xy = src1_dram_buffer->noc_coordinates(device);
        if (multibank)
            pass &= ll_buda::WriteToDeviceDRAMChannelsInterleavedTiles(device, bcast_tiled_u32, src1_dram_buffer->address());
        else
            pass &= ll_buda::WriteToDeviceDRAM(device, src1_dram_buffer, bcast_tiled_u32);

        const char* reader_name = get_reader_name(multibank, bcast_dim);
        auto binary_reader_kernel = ll_buda::CreateDataMovementKernel(
            program,
            reader_name,
            core,
            ll_buda::DataMovementProcessor::RISCV_1,
            ll_buda::NOC::RISCV_1_default);
        
        auto unary_writer_kernel = ll_buda::CreateDataMovementKernel(
            program,
            multibank ? "kernels/dataflow/writer_unary_8bank.cpp"
                      : "kernels/dataflow/writer_unary.cpp",
            core,
            ll_buda::DataMovementProcessor::RISCV_0,
            ll_buda::NOC::RISCV_0_default);


        ll_buda::WriteRuntimeArgsToDevice(
            device,
            binary_reader_kernel,
            core,
            {dram_buffer_src0_addr, // 0
            (std::uint32_t)dram_src0_noc_xy.x, // 1
            (std::uint32_t)dram_src0_noc_xy.y, // 2
            num_tensor_tiles, // 3
            dram_buffer_src1_addr, // 4
            (std::uint32_t)dram_src1_noc_xy.x, // 5
            (std::uint32_t)dram_src1_noc_xy.y, // 6
            num_bcast_tiles, NC*Ht*Wt, NC, Ht, Wt}); // 7 8 9 10 11
        
        ll_buda::WriteRuntimeArgsToDevice(
            device,
            unary_writer_kernel,
            core,
            {dram_buffer_dst_addr,
            (std::uint32_t)dram_dst_noc_xy.x,
            (std::uint32_t)dram_dst_noc_xy.y,
            num_tensor_tiles});

        void *hlk_args = new bcast_op_params::hlk_args_t { .B = NC, .Ht = Ht, .Wt = Wt };
        ll_buda::ComputeKernelArgs *compute_args = ll_buda::InitializeCompileTimeComputeKernelArgs(core, hlk_args, sizeof(bcast_op_params::hlk_args_t));

        bool fp32_dest_acc_en = false;
        bool math_approx_mode = false;
        auto eltwise_binary_kernel = ll_buda::CreateComputeKernel(
            program,
            get_compute_name(bcast_dim),
            core,
            compute_args,
            MathFidelity::HiFi4,
            fp32_dest_acc_en,
            math_approx_mode
        );

        eltwise_binary_kernel->add_define("BCAST_OP", op_id_to_op_define[bcast_op]);

        ////////////////////////////////////////////////////////////////////////////
        //                      Compile Application
        ////////////////////////////////////////////////////////////////////////////
        bool skip_hlkc = false;
        pass &= ll_buda::CompileProgram(device, program, skip_hlkc);


        ////////////////////////////////////////////////////////////////////////////
        //                      Execute Application
        ////////////////////////////////////////////////////////////////////////////
        auto seed = std::chrono::system_clock::now().time_since_epoch().count();
        vector<uint32_t> src0_vec = create_random_vector_of_bfloat16(dram_buffer_bytes, 10.0f, 0x1234);
        if (multibank)
            pass &= ll_buda::WriteToDeviceDRAMChannelsInterleavedTiles(device, src0_vec, src0_dram_buffer->address());
        else
            pass &= ll_buda::WriteToDeviceDRAM(device, src0_dram_buffer, src0_vec);

        pass &= ll_buda::ConfigureDeviceWithProgram(device, program);

        pass &= ll_buda::LaunchKernels(device, program);

        // The kernel will view the input as TILED32_4FACES
        vector<uint32_t> result_vec;
        if (multibank)
            ll_buda::ReadFromDeviceDRAMChannelsInterleavedTiles(
                device, dst_dram_buffer->address(), result_vec, dst_dram_buffer->size());
        else
            ll_buda::ReadFromDeviceDRAMChannel(
                device, dram_dst_channel_id, dst_dram_buffer->address(), result_vec, dst_dram_buffer->size());
        TT_ASSERT(result_vec.size() == NC*W*H/2); // we are expecting one tile in H, and half the elements since the vector packs 2 uint16_ts

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
            return result;
        };

        // recover a linear view of input vector for consumption by gold_ function
        auto u16_src0_vec = u16_from_u32_vector(src0_vec);
        vector<u16> src_linear = convert_layout<u16>(
            u16_src0_vec, shape, TensorLayout::TILED32_4FACES, TensorLayout::LIN_ROW_MAJOR);
        vector<u16> gold_added = gold_bcast_op(
            src_linear, shape, ref_bcast_values, bcast_dim, bcast_op); // result is u16 untilized

        // Tilize from row major and convert to pairs (u32)
        vector<uint32_t> shapeR{shape[0], shape[1], shape[2], shape[3]};
        auto gold_4f_u32 = u32_from_u16_vector(
            convert_layout<u16>(
                gold_added, shapeR, TensorLayout::LIN_ROW_MAJOR, TensorLayout::TILED32_4FACES));

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

    } // for bcast_op loop
    } // for bcast_mode loop

    if (pass) {
        log_info(LogTest, "Test Passed");
    } else {
        log_fatal(LogTest, "Test Failed");
    }

    TT_ASSERT(pass);

    return 0;
}
