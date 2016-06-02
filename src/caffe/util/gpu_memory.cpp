#ifndef CPU_ONLY
#include <algorithm>
#include <vector>
#include "caffe/common.hpp"
#include "caffe/util/gpu_memory.hpp"

#include "cub/util_allocator.cuh"

namespace caffe {
using std::vector;

static cub::CachingDeviceAllocator* cub_allocator = 0;
GPUMemory::Manager GPUMemory::mgr_;

GPUMemory::Manager::Manager(): mode_(CUDA_MALLOC), debug_(false) {}

void GPUMemory::Manager::init(const vector<int>& gpus, Mode m, bool debug) {
  bool debug_env = getenv("DEBUG_GPU_MEM") != 0;
  debug_ = debug || debug_env;
  if (gpus.size() <= 0) {
    m = CUDA_MALLOC;
  }
  switch (m) {
  case CUB_ALLOCATOR:
    try {
      // Just in case someone installed 'no cleanup' arena before
      delete cub_allocator;
      cub_allocator = new cub::CachingDeviceAllocator(2, 6, 22, (size_t) -1,
          false, debug_);
    } catch (...) {
    }
    CHECK(cub_allocator);
    for (int i = 0; i < gpus.size(); ++i) {
      update_dev_info(gpus[i]);
    }
    break;
  default:
    break;
  }
  mode_ = m;
  VLOG_IF(1, debug) << "GPUMemory::Manager initialized with " << pool_name();
}

void GPUMemory::Manager::destroy() {
  switch (mode_) {
  case CUB_ALLOCATOR:
    delete cub_allocator;
    cub_allocator = NULL;
    break;
  default:
    break;
  }
  mode_ = CUDA_MALLOC;
}

bool GPUMemory::Manager::try_allocate(void** ptr, size_t size, cudaStream_t s) {
  CHECK(ptr!= NULL);
  cudaError_t status = cudaSuccess, last_err = cudaSuccess;
  switch (mode_) {
  case CUB_ALLOCATOR:
    // Clean Cache & Retry logic is inside now
    status = cub_allocator->DeviceAllocate(ptr, size, s);
    // If there was a retry and it succeeded we get good status here but
    // we need to clean up last error...
    last_err = cudaGetLastError();
    // ...and update the dev info if something was wrong
    if (status != cudaSuccess || last_err != cudaSuccess) {
      int cur_device;
      CUDA_CHECK(cudaGetDevice(&cur_device));
      // Refresh per-device saved values.
      for (int i = 0; i < dev_info_.size(); ++i) {
        // only query devices that were initialized
        if (dev_info_[i].total_) {
          update_dev_info(i);
          // record which device caused cache flush
          if (i == cur_device) {
            dev_info_[i].flush_count_++;
          }
        }
      }
    }
    break;
  default:
    status = cudaMalloc(ptr, size);
    break;
  }
  return status == cudaSuccess;
}

void GPUMemory::Manager::deallocate(void* ptr, cudaStream_t stream) {
  // allow for null pointer deallocation
  if (!ptr) {
    return;
  }
  switch (mode_) {
  case CUB_ALLOCATOR:
    CUDA_CHECK(cub_allocator->DeviceFree(ptr));
    break;
  default:
    CUDA_CHECK(cudaFree(ptr));
    break;
  }
}

void GPUMemory::Manager::update_dev_info(int device) {
  int initial_device;
  CUDA_CHECK(cudaGetDevice(&initial_device));
  if (device + 1 > dev_info_.size()) {
    dev_info_.resize(device + 1);
  }

  CUDA_CHECK(cudaSetDevice(device));
  cudaDeviceProp props;
  CUDA_CHECK(cudaGetDeviceProperties(&props, device));
  CUDA_CHECK(cudaMemGetInfo(&dev_info_[device].free_,
      &dev_info_[device].total_));

  VLOG_IF(1, debug_) << "cudaGetDeviceProperties: Mem = "
                     << props.totalGlobalMem;
  VLOG_IF(1, debug_) << "cudaMemGetInfo_[" << device << "]: Free="
                     << dev_info_[device].free_ << " Total="
                     << dev_info_[device].total_;

  // Make sure we don't have more that total device memory
  dev_info_[device].total_ = std::min(props.totalGlobalMem,
      dev_info_[device].total_);
  // Here we are adding existing 'busy' allocations to CUDA free memory
  dev_info_[device].free_ = std::min(dev_info_[device].total_,
      dev_info_[device].free_ + cub_allocator->cached_bytes[device].live);
  CUDA_CHECK(cudaSetDevice(initial_device));
}


const char* GPUMemory::Manager::pool_name() const {
  switch (mode_) {
  case CUB_ALLOCATOR:
    return "Caching (CUB) GPU Allocator";
  default:
    return "Plain CUDA GPU Allocator";
  }
}

void GPUMemory::Manager::GetInfo(size_t* free_mem, size_t* total_mem) {
  switch (mode_) {
  case CUB_ALLOCATOR:
    int cur_device;
    CUDA_CHECK(cudaGetDevice(&cur_device));
    *total_mem = dev_info_[cur_device].total_;
    // Free memory is initial free memory minus outstanding allocations.
    // Assuming we only allocate via GPUMemoryManager since its construction.
    *free_mem = dev_info_[cur_device].free_ -
        cub_allocator->cached_bytes[cur_device].live;
    break;
  default:
    CUDA_CHECK(cudaMemGetInfo(free_mem, total_mem));
    break;
  }
}
}  // namespace caffe
#endif  // CPU_ONLY
