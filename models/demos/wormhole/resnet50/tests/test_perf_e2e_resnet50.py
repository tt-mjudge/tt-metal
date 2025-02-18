# SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.

# SPDX-License-Identifier: Apache-2.0

import pytest

from models.utility_functions import run_for_wormhole_b0
from models.demos.ttnn_resnet.tests.perf_e2e_resnet50 import run_perf_resnet


@run_for_wormhole_b0()
@pytest.mark.models_performance_bare_metal
@pytest.mark.parametrize("device_params", [{"l1_small_size": 24576}], indirect=True)
@pytest.mark.parametrize(
    "batch_size, expected_inference_time, expected_compile_time",
    ((16, 0.0070, 28),),
)
def test_perf(
    device,
    use_program_cache,
    batch_size,
    expected_inference_time,
    expected_compile_time,
    hf_cat_image_sample_input,
    model_location_generator,
):
    run_perf_resnet(
        batch_size,
        expected_inference_time,
        expected_compile_time,
        hf_cat_image_sample_input,
        device,
        "resnet50",
        model_location_generator,
    )


@run_for_wormhole_b0()
@pytest.mark.models_performance_bare_metal
@pytest.mark.parametrize("device_params", [{"l1_small_size": 32768, "trace_region_size": 1500000}], indirect=True)
@pytest.mark.parametrize(
    "batch_size, enable_async_mode, expected_inference_time, expected_compile_time",
    (
        (16, True, 0.005, 25),
        (16, False, 0.0046, 25),
    ),
    indirect=["enable_async_mode"],
)
def test_perf_trace(
    device,
    use_program_cache,
    batch_size,
    expected_inference_time,
    expected_compile_time,
    hf_cat_image_sample_input,
    enable_async_mode,
    model_location_generator,
):
    mode = "async" if enable_async_mode else "sync"
    run_perf_resnet(
        batch_size,
        expected_inference_time,
        expected_compile_time,
        hf_cat_image_sample_input,
        device,
        f"resnet50_trace_{mode}",
        model_location_generator,
    )


@run_for_wormhole_b0()
@pytest.mark.models_performance_bare_metal
@pytest.mark.parametrize("device_params", [{"l1_small_size": 32768, "num_hw_cqs": 2}], indirect=True)
@pytest.mark.parametrize(
    "batch_size, expected_inference_time, expected_compile_time",
    ((16, 0.0070, 26),),
)
def test_perf_2cqs(
    device,
    use_program_cache,
    batch_size,
    expected_inference_time,
    expected_compile_time,
    hf_cat_image_sample_input,
    model_location_generator,
):
    run_perf_resnet(
        batch_size,
        expected_inference_time,
        expected_compile_time,
        hf_cat_image_sample_input,
        device,
        "resnet50_2cqs",
        model_location_generator,
    )


@run_for_wormhole_b0()
@pytest.mark.models_performance_bare_metal
@pytest.mark.parametrize(
    "device_params", [{"l1_small_size": 32768, "num_hw_cqs": 2, "trace_region_size": 1332224}], indirect=True
)
@pytest.mark.parametrize(
    "batch_size, expected_inference_time, expected_compile_time",
    ((16, 0.004, 25),),
)
def test_perf_trace_2cqs(
    device,
    use_program_cache,
    batch_size,
    expected_inference_time,
    expected_compile_time,
    hf_cat_image_sample_input,
    model_location_generator,
):
    run_perf_resnet(
        batch_size,
        expected_inference_time,
        expected_compile_time,
        hf_cat_image_sample_input,
        device,
        "resnet50_trace_2cqs",
        model_location_generator,
    )
