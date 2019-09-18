/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef GLOW_BACKENDS_CPU_CPUDEVICEMANAGER_H
#define GLOW_BACKENDS_CPU_CPUDEVICEMANAGER_H

#include "glow/Backends/QueueBackedDeviceManager.h"
#include "glow/Runtime/StatsExporter.h"

#include <atomic>
#include <mutex>

namespace glow {
namespace runtime {

/// A class that contains a CPU device buffer. It frees the buffer when it
/// is destroyed. A CPU device buffer is associated with a specific function
/// name. It containts \p kMaxRequestsSupported specific buffers to handle
/// multiple requests.
class CPUBuffer {

  struct SingleRequestBuffer {
    /// The buffers being stored.
    uint8_t *activationsBuffer_;
    uint8_t *weightsBuffer_;
  };
  std::vector<SingleRequestBuffer> reqBuffers_;
  std::unordered_map<RunIdentifierTy, SingleRequestBuffer> busyRequestBuffers_;

  static constexpr int kMaxRequestsSupported = 1;

  /// Size of the buffer in bytes.
  const size_t activationsSize_{0};
  const size_t weightsSize_{0};

  std::mutex mutex_;

public:
  CPUBuffer(size_t activationsSize, size_t activations_alignment,
            size_t weightsSize, size_t weights_alignment);

  ~CPUBuffer();

  void getUnownedRequestBuffer(RunIdentifierTy id, uint8_t **activationsBuffer,
                               uint8_t **weightsBuffer);
  void freeRequestBuffer(RunIdentifierTy id);

  /// Get size of buffer in bytes.
  size_t getActivationsSize() { return activationsSize_; }
  size_t getWeightsSize() { return weightsSize_; }

  size_t getNumAvailableBuffers() { return reqBuffers_.size(); }
};

/// A class controlling a single CPU thread of execution driving the JIT
/// backend. Many CPUFunctions may be added, but only one inference is executed
/// at a time.
class CPUDeviceManager : public QueueBackedDeviceManager {
  /// Compiled function list by name.
  FunctionMapTy functions_;

  /// A map from function name to its memory buffer
  std::map<std::string, std::unique_ptr<CPUBuffer>> buffers_;

  /// String constant for logging number of in-use devices.
  static constexpr const char *kDevicesUsedCPU = "glow.devices_used.cpu";

public:
  explicit CPUDeviceManager(const DeviceConfig &config)
      : QueueBackedDeviceManager(config) {
    Stats()->incrementCounter(kDevicesUsedCPU);
    exportMemoryCounters();
  }

  ~CPUDeviceManager() override {
    Stats()->incrementCounter(kDevicesUsedCPU, -1);
    zeroMemoryCounters();
  }

  /// Returns the amount of memory in bytes available on the device when no
  /// models are loaded.
  uint64_t getMaximumMemory() const override;

  /// Returns the amount of memory in bytes currently availbe on the device.
  uint64_t getAvailableMemory() const override;

  /// Returns true if a function requiring the \p estimate size will fit on the
  /// device. This is not a promise as memory cost could vary due to alignment,
  /// etc.
  bool isMemoryAvailable(uint64_t estimate) const override;

  /// Returns the DeviceInfo for this device containing peak limits for
  /// compute and bandwidths (used in partitioning).
  DeviceInfo getDeviceInfo() const override;

protected:
  void addNetworkImpl(const Module *module, FunctionMapTy functions,
                      ReadyCBTy cb) override;
  void evictNetworkImpl(std::string functionName,
                        EvictFunctionCBTy evictCb) override;
  void runFunctionImpl(runtime::RunIdentifierTy id, std::string functionName,
                       std::unique_ptr<ExecutionContext> context,
                       ResultCBTy cb) override;
};

DeviceManager *createCPUDeviceManager(const DeviceConfig &config);

} // namespace runtime
} // namespace glow

#endif // GLOW_BACKENDS_CPU_CPUDEVICEMANAGER_H
