#include "ll_buda/host_api.hpp"
#include "ll_buda/tensor/tensor.hpp"
#include "ll_buda/op_library/reduce/reduce_op.hpp"
#include "constants.hpp"

#include <algorithm>
#include <functional>
#include <random>

using namespace tt;
using namespace tt::ll_buda;
using namespace constants;


//////////////////////////////////////////////////////////////////////////////////////////
// TODO: explain what test does
//////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv) {
    bool pass = true;

    try {
        ////////////////////////////////////////////////////////////////////////////
        //                      Grayskull Device Setup
        ////////////////////////////////////////////////////////////////////////////
        int pci_express_slot = 0;
        Device *device = CreateDevice(tt::ARCH::GRAYSKULL, pci_express_slot);
        ll_buda::Host *host = ll_buda::GetHost();

        pass &= InitializeDevice(device);

        ////////////////////////////////////////////////////////////////////////////
        //                      Application Setup
        ////////////////////////////////////////////////////////////////////////////
        std::array<uint32_t, 4> shape = {1, 1, TILE_HEIGHT, TILE_WIDTH};
        // Allocates a DRAM buffer on device populated with values specified by initialize
        Tensor a = Tensor(shape, Initialize::RANDOM, tt::DataFormat::Float16_b, Layout::TILE, device);
        Tensor host_a = a.to(host); // Move tensor a to host to validate

        for (auto op_type: ReduceOpMath::all())
        for (auto op_dim: ReduceOpDim::all()) {
            Tensor c = reduce(a, op_type, op_dim);
            Tensor d = c.to(host);
            //pass &= (host_a.data() == d.data()); // TODO(AP)
        }

        ////////////////////////////////////////////////////////////////////////////
        //                      Validation & Teardown
        ////////////////////////////////////////////////////////////////////////////

        pass &= CloseDevice(device);

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
