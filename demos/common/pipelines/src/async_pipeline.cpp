/*
// Copyright (C) 2018-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "pipelines/async_pipeline.h"
#include <cldnn/cldnn_config.hpp>
#include <samples/common.hpp>
#include <samples/slog.hpp>

using namespace InferenceEngine;

AsyncPipeline::AsyncPipeline(std::unique_ptr<ModelBase>&& modelInstance, const CnnConfig& cnnConfig, InferenceEngine::Core& engine) :
    model(std::move(modelInstance)) {

    // --------------------------- 1. Load inference engine ------------------------------------------------
    slog::info << "Loading Inference Engine" << slog::endl;

    slog::info << "Device info: " << slog::endl;
    slog::info<< printable(engine.GetVersions(cnnConfig.devices));

    /** Load extensions for the plugin **/
    if (!cnnConfig.cpuExtensionsPath.empty()) {
        // CPU(MKLDNN) extensions are loaded as a shared library and passed as a pointer to base extension
        IExtensionPtr extension_ptr = make_so_pointer<IExtension>(cnnConfig.cpuExtensionsPath.c_str());
        engine.AddExtension(extension_ptr, "CPU");
    }
    if (!cnnConfig.clKernelsConfigPath.empty()) {
        // clDNN Extensions are loaded from an .xml description and OpenCL kernel files
        engine.SetConfig({ {PluginConfigParams::KEY_CONFIG_FILE, cnnConfig.clKernelsConfigPath} }, "GPU");
    }

    // --------------------------- 2. Read IR Generated by ModelOptimizer (.xml and .bin files) ------------
    slog::info << "Loading network files" << slog::endl;
    /** Read network model **/
    InferenceEngine::CNNNetwork cnnNetwork = engine.ReadNetwork(model->getModelFileName());
    /** Set batch size to 1 **/
    slog::info << "Batch size is forced to 1." << slog::endl;

    auto shapes = cnnNetwork.getInputShapes();
    for (auto& shape : shapes) {
        shape.second[0] = 1;
    }
    cnnNetwork.reshape(shapes);

    // -------------------------- Reading all outputs names and customizing I/O blobs (in inherited classes)
    model->prepareInputsOutputs(cnnNetwork);

    // --------------------------- 4. Loading model to the device ------------------------------------------
    slog::info << "Loading model to the device" << slog::endl;
    execNetwork = engine.LoadNetwork(cnnNetwork, cnnConfig.devices, cnnConfig.execNetworkConfig);

    // --------------------------- 5. Create infer requests ------------------------------------------------
    requestsPool.reset(new RequestsPool(execNetwork, cnnConfig.maxAsyncRequests));

    // --------------------------- 6. Call onLoadCompleted to complete initialization of model -------------
    model->onLoadCompleted(&execNetwork, requestsPool->getInferRequestsList());
}

AsyncPipeline::~AsyncPipeline(){
    waitForTotalCompletion();
}

void AsyncPipeline::waitForData(){
    std::unique_lock<std::mutex> lock(mtx);

    condVar.wait(lock, [&] {return callbackException != nullptr ||
        requestsPool->isIdleRequestAvailable() ||
        completedInferenceResults.find(outputFrameId) != completedInferenceResults.end();
    });

    if (callbackException)
        std::rethrow_exception(callbackException);
}

int64_t AsyncPipeline::submitData(const InputData& inputData, const std::shared_ptr<MetaData>& metaData){
    auto frameID = inputFrameId;

    auto request = requestsPool->getIdleRequest();
    if (!request)
        return -1;

    auto internalModelData = model->preprocess(inputData, request);

    request->SetCompletionCallback([this,
        frameID,
        request,
        internalModelData,
        metaData] {
            {
                std::lock_guard<std::mutex> lock(mtx);

                try {
                    InferenceResult result;

                    result.frameId = frameID;
                    result.metaData = std::move(metaData);
                    result.internalModelData = std::move(internalModelData);

                    for (const auto& outName : model->getOutputsNames())
                        result.outputsData.emplace(outName, std::make_shared<TBlob<float>>(*as<TBlob<float>>(request->GetBlob(outName))));

                    completedInferenceResults.emplace(frameID, result);
                    this->requestsPool->setRequestIdle(request);
                }
                catch (...) {
                    if (!this->callbackException) {
                        this->callbackException = std::move(std::current_exception());
                    }
                }
            }
            condVar.notify_one();
    });

    inputFrameId++;
    if (inputFrameId < 0)
        inputFrameId = 0;

    request->StartAsync();
    return frameID;
}

std::unique_ptr<ResultBase> AsyncPipeline::getResult() {
    auto infResult = AsyncPipeline::getInferenceResult();
    if (infResult.IsEmpty()) {
        return std::unique_ptr<ResultBase>();
    }

    auto result = model->postprocess(infResult);
    *result = static_cast<ResultBase&>(infResult);

    return result;
}

InferenceResult AsyncPipeline::getInferenceResult() {
    InferenceResult retVal;
    {
        std::lock_guard<std::mutex> lock(mtx);

        const auto& it = completedInferenceResults.find(outputFrameId);

        if (it != completedInferenceResults.end()) {
            retVal = std::move(it->second);
            completedInferenceResults.erase(it);
        }
    }

    if(!retVal.IsEmpty()) {
        outputFrameId = retVal.frameId;
        outputFrameId++;
        if (outputFrameId < 0)
            outputFrameId = 0;
    }

    return retVal;
}
