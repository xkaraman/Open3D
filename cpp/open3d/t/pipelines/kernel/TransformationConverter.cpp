// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EposePRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/t/pipelines/kernel/TransformationConverter.h"

#include <cmath>

#include "open3d/core/CoreUtil.h"
#include "open3d/core/Tensor.h"
#include "open3d/t/pipelines/kernel/TransformationConverterImpl.h"
#include "open3d/utility/Console.h"

namespace open3d {
namespace t {
namespace pipelines {
namespace kernel {

core::Tensor RtToTransformation(const core::Tensor &R, const core::Tensor &t) {
    core::Device device = R.GetDevice();
    core::Dtype dtype = R.GetDtype();

    if (dtype != core::Dtype::Float32 && dtype != core::Dtype::Float64) {
        utility::LogError(
                " [RtToTransformation]: Only Float32 abd Float64 supported, "
                "but got {} ",
                dtype.ToString());
    }

    core::Tensor transformation = core::Tensor::Zeros({4, 4}, dtype, device);
    R.AssertShape({3, 3});
    R.AssertDtype(dtype);
    t.AssertShape({3});
    t.AssertDevice(device);
    t.AssertDtype(dtype);

    // Rotation.
    transformation.SetItem(
            {core::TensorKey::Slice(0, 3, 1), core::TensorKey::Slice(0, 3, 1)},
            R);
    // Translation.
    transformation.SetItem(
            {core::TensorKey::Slice(0, 3, 1), core::TensorKey::Slice(3, 4, 1)},
            t.Reshape({3, 1}));
    // Scale [assumed to be 1].
    transformation[3][3] = 1;
    return transformation;
}

template <typename scalar_t>
static void PoseToTransformationDevice(
        core::Tensor &transformation,
        const core::Tensor &pose,
        const core::Device::DeviceType &device_type) {
    scalar_t *transformation_ptr = transformation.GetDataPtr<scalar_t>();
    const scalar_t *pose_ptr = pose.GetDataPtr<scalar_t>();

    if (device_type == core::Device::DeviceType::CPU) {
        PoseToTransformationImpl<scalar_t>(transformation_ptr, pose_ptr);
    } else if (device_type == core::Device::DeviceType::CUDA) {
#ifdef BUILD_CUDA_MODULE
        PoseToTransformationCUDA<scalar_t>(transformation_ptr, pose_ptr);
#else
        utility::LogError("Not compiled with CUDA, but CUDA device is used.");
#endif
    } else {
        utility::LogError("Unimplemented device.");
    }
}

core::Tensor PoseToTransformation(const core::Tensor &pose) {
    core::Device device = pose.GetDevice();
    core::Dtype dtype = pose.GetDtype();

    if (dtype != core::Dtype::Float32 && dtype != core::Dtype::Float64) {
        utility::LogError(
                " [PoseToTransformation]: Only Float32 abd Float64 supported, "
                "but got {} ",
                dtype.ToString());
    }

    pose.AssertShape({6});
    core::Tensor transformation = core::Tensor::Zeros({4, 4}, dtype, device);
    transformation = transformation.Contiguous();
    core::Tensor pose_ = pose.Contiguous();

    DISPATCH_FLOAT32_FLOAT64_DTYPE(dtype, [&]() {
        core::Device::DeviceType device_type = device.GetType();
        PoseToTransformationDevice<scalar_t>(transformation, pose_,
                                             device_type);
    });

    // Translation from pose.
    transformation.SetItem(
            {core::TensorKey::Slice(0, 3, 1), core::TensorKey::Slice(3, 4, 1)},
            pose_.GetItem({core::TensorKey::Slice(3, 6, 1)}).Reshape({3, 1}));
    // Scale [assumed to be 1].
    transformation[3][3] = 1;
    return transformation;
}

void DecodeAndSolve6x6(const core::Tensor &A_reduction,
                       core::Tensor &delta,
                       core::Tensor &residual) {
    const core::Device host(core::Device("CPU:0"));
    core::Tensor A_1x29_host = A_reduction.To(host, core::Dtype::Float64);
    double *A_1x29_ptr = A_1x29_host.GetDataPtr<double>();

    core::Tensor AtA = core::Tensor::Empty({6, 6}, core::Dtype::Float64, host);
    core::Tensor Atb = core::Tensor::Empty({6}, core::Dtype::Float64, host);

    double *AtA_local_ptr = AtA.GetDataPtr<double>();
    double *Atb_local_ptr = Atb.GetDataPtr<double>();

    for (int j = 0; j < 6; j++) {
        Atb_local_ptr[j] = A_1x29_ptr[21 + j];
        const int64_t reduction_idx = ((j * (j + 1)) / 2);
        for (int k = 0; k <= j; k++) {
            AtA_local_ptr[j * 6 + k] = A_1x29_ptr[reduction_idx + k];
            AtA_local_ptr[k * 6 + j] = A_1x29_ptr[reduction_idx + k];
        }
    }

    residual = core::Tensor::Init<double>({A_1x29_ptr[27]}, host);

    // Solve on CPU with double to ensure precision.
    delta = AtA.Solve(Atb.Neg());

    const int count = static_cast<int>(A_1x29_ptr[28]);
    utility::LogDebug("avg loss = {}, residual = {}, count = {}",
                      residual.Item<double>() / count, residual.Item<double>(),
                      count);
}

}  // namespace kernel
}  // namespace pipelines
}  // namespace t
}  // namespace open3d
