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
#include "CPUDeviceManager.h"
#include "CPUFunction.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

namespace glow {
namespace runtime {

unsigned GlowCPUMemory = 0;

static llvm::cl::opt<unsigned, /* ExternalStorage */ true> GlowCPUMemoryOpt(
    "cpu-memory",
    llvm::cl::desc("CPU DeviceManager maximum memory in kilobytes."),
    llvm::cl::location(GlowCPUMemory));

DeviceManager *createCPUDeviceManager(const DeviceConfig &config) {
  if (GlowCPUMemory) {
    // Convert command line GlowCPUMemory to bytes from kilobytes.
    auto configNew = config;
    configNew.setDeviceMemory(uint64_t{GlowCPUMemory} * 1024);
    return new CPUDeviceManager(configNew);
  }
  return new CPUDeviceManager(config);
}

CPUBuffer::~CPUBuffer() {
  alignedFree(activationsBuffer_);
  alignedFree(weightsBuffer_);
}

uint64_t CPUDeviceManager::getMaximumMemory() const { return maxMemoryBytes_; }

uint64_t CPUDeviceManager::getAvailableMemory() const {
  return maxMemoryBytes_ - usedMemoryBytes_;
}

bool CPUDeviceManager::isMemoryAvailable(uint64_t estimate) const {
  // No fuzz factor for the CPU device.
  return maxMemoryBytes_ >= (usedMemoryBytes_ + estimate);
}

DeviceInfo CPUDeviceManager::getDeviceInfo() const {
  // TODO: these may need to be tweaked depending on specific CPU.
  DeviceInfo info = DeviceInfo();
  info.sramCapacity = 256 * 1024 * 1024;
  info.peakCompute = 2.2 * 1024 * 1024 * 1024 * 1024;
  info.peakDramBw = 110.0 * 1024 * 1024 * 1024;
  info.peakSramBw = 1024.0 * 1024 * 1024 * 1024;
  info.peakPCIeBw = 16.0 * 1024 * 1024 * 1024;
  return info;
}

void CPUDeviceManager::addNetworkImpl(const Module *module,
                                      FunctionMapTy functions,
                                      ReadyCBTy readyCB) {
  DCHECK(readyCB != nullptr);

  // First check for uniqueness of the function name.
  for (const auto &func : functions) {
    if (functions_.count(func.first) != 0) {
      readyCB(
          module,
          MAKE_ERR(
              llvm::formatv(
                  "Failed to add network: already have a function called {0}",
                  func.first)
                  .str()));
      return;
    }

    if (func.second->getCompileBackendName() != "CPU") {
      readyCB(
          module,
          MAKE_ERR(
              llvm::formatv(
                  "Failed to add network: function {0} is not a CPUFunction",
                  func.first)
                  .str()));
      return;
    }
  }

  if (usedMemoryBytes_ + functionCost_ > maxMemoryBytes_) {
    readyCB(module, MAKE_ERR(GlowErr::ErrorCode::RUNTIME_OUT_OF_DEVICE_MEMORY,
                             "Failed to add network: not enough memory"));
    return;
  }

  // Add to the function name lookup map.
  for (const auto &func : functions) {
    if (func.second->getRuntimeBundle().getConstants() == nullptr) {
      func.second->getRuntimeBundle().collectConstants(module);
    }
    functions_.emplace(func.first, func.second);
    usedMemoryBytes_ += functionCost_; // TODO:: static moduleSize

    auto &bundle = func.second->getRuntimeBundle();
    uint8_t *activationsBuffer =
        (uint8_t *)alignedAlloc(bundle.getActivationsSize(), TensorAlignment);
    uint8_t *weightsBuffer =
        (uint8_t *)alignedAlloc(bundle.getMutableWeightSize(), TensorAlignment);
    if (!activationsBuffer || !weightsBuffer) {
      readyCB(module, MAKE_ERR(GlowErr::ErrorCode::RUNTIME_OUT_OF_DEVICE_MEMORY,
                               "Failed to add network: not enough memory"));
    }
    std::unique_ptr<CPUBuffer> deviceBuffer = llvm::make_unique<CPUBuffer>(
        activationsBuffer, bundle.getActivationsSize(), weightsBuffer,
        bundle.getMutableWeightSize());
    buffers_.emplace(func.first, std::move(deviceBuffer));
    // TODO: check if usedMemoryBytes_ should be updated according to buffers_
  }

  assert(usedMemoryBytes_ <= maxMemoryBytes_);

  // Export change in memory usage.
  exportMemoryCounters();

  // Fire the ready CB.
  readyCB(module, llvm::Error::success());
}

void CPUDeviceManager::evictNetworkImpl(std::string functionName,
                                        EvictFunctionCBTy evictCB) {
  DCHECK(evictCB != nullptr);

  if (functions_.erase(functionName)) {
    usedMemoryBytes_ -= functionCost_; // TODO: static moduleSize

    buffers_.erase(functionName); // Destroying device buffer
  } else {
    evictCB(functionName,
            MAKE_ERR(GlowErr::ErrorCode::RUNTIME_NET_NOT_FOUND,
                     strFormat("Could not find function with name %s to evict",
                               functionName.c_str())));
    return;
  }

  // Export change in memory usage.
  exportMemoryCounters();

  evictCB(functionName, llvm::Error::success());
}

void CPUDeviceManager::runFunctionImpl(
    RunIdentifierTy id, std::string function,
    std::unique_ptr<ExecutionContext> context, ResultCBTy resultCB) {
  DCHECK(resultCB != nullptr);

  TRACE_EVENT_SCOPE_NAMED(context->getTraceContext(), TraceLevel::RUNTIME,
                          "DeviceManager::run", dmRun);
  auto funcIt = functions_.find(function);
  if (funcIt == functions_.end()) {
    dmRun.addArg("reason", "function not found");
    TRACE_EVENT_SCOPE_END_NAMED(dmRun);
    resultCB(id,
             MAKE_ERR(GlowErr::ErrorCode::RUNTIME_NET_NOT_FOUND,
                      llvm::formatv("Function {0} not found", function).str()),
             std::move(context));
    return;
  }

  CompiledFunction *func = funcIt->second;

  auto cpuBindings = llvm::make_unique<CPUDeviceBindings>(
      buffers_[function]->getActivationsBuffer(),
      buffers_[function]->getWeightsBuffer());

  context->setDeviceBindings(std::move(cpuBindings));

  // Run that function.
  auto executeErr = func->execute(context.get());

  // End the TraceEvent early to avoid time in the CB.
  TRACE_EVENT_SCOPE_END_NAMED(dmRun);

  // Fire the resultCB.
  resultCB(id, std::move(executeErr), std::move(context));
}

bool CPUDeviceManager::isPeerToPeerSupported() { return true; }

llvm::Expected<int64_t>
CPUDeviceManager::getRemotePeerToPeerAddress(int64_t channelId,
                                             PlaceholderBindings *bindings) {
  std::string function = "recv_func";
  auto funcIt = functions_.find(function);
  if (funcIt != functions_.end()) {
    auto *baseAddress = buffers_[function]->getWeightsBuffer();
    auto &bundle = funcIt->second->getRuntimeBundle();
    auto &symbolTable = bundle.getSymbolTable();
    auto *PH = bindings->getPlaceholderByName("recv_input");
    auto it = symbolTable.find(PH->getName());
    if (it == symbolTable.end()) {
      llvm::outs() << "Did not find symbol\n";
    }
    auto symbolInfo = it->second;
    auto addr = symbolInfo.offset;
    int64_t res = reinterpret_cast<int64_t>(baseAddress + addr);
    return res;
  }
  return MAKE_ERR(GlowErr::ErrorCode::RUNTIME_ERROR,
                  "Failed to find remote address.");
}

} // namespace runtime
} // namespace glow
