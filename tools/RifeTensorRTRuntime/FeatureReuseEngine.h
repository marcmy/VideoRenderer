#pragma once

// Included after the runtime's internal TensorRT/file helpers. A separate
// image-only encoder feeds the unchanged synthesis graph. Buffers and graphs
// belong to one execution context, never to recycled D3D texture addresses.
class FeatureReuseEngine final {
public:
    struct Context final {
        TrtPtr<nvinfer1::IExecutionContext> encoder;
        void* features = nullptr;
        cudaGraph_t graph = nullptr;
        cudaGraphExec_t graphExec = nullptr;
        ~Context() {
            if (graphExec) cudaGraphExecDestroy(graphExec);
            if (graph) cudaGraphDestroy(graph);
            if (features) cudaFree(features);
        }
        bool Enqueue(cudaStream_t stream) {
            return graphExec ? cudaGraphLaunch(graphExec, stream) == cudaSuccess : encoder->enqueueV3(stream);
        }
        bool Prime(cudaStream_t stream) {
            if (!encoder->enqueueV3(stream) || cudaStreamSynchronize(stream) != cudaSuccess) return false;
            if (cudaStreamBeginCapture(stream, cudaStreamCaptureModeGlobal) != cudaSuccess) return true;
            const bool enqueued = encoder->enqueueV3(stream);
            cudaGraph_t captured = nullptr;
            const auto result = cudaStreamEndCapture(stream, &captured);
            if (enqueued && result == cudaSuccess && captured) {
                cudaGraphExec_t executable = nullptr;
                if (cudaGraphInstantiate(&executable, captured, 0) == cudaSuccess) {
                    graph = captured; graphExec = executable; return true;
                }
            }
            if (captured) cudaGraphDestroy(captured);
            // Graph capture is optional; synchronous inference remains valid.
            cudaGetLastError();
            return true;
        }
    };

    bool Initialize(const std::filesystem::path& model, const std::filesystem::path& cache,
        const std::string& modelHash, const std::string& cacheKey, uint32_t width, uint32_t height) {
        // Enable only exports checked on the native GPU path. The pinned 4.25
        // Lite split has a reproducible top-edge artifact on SM75; keep its
        // original engine even if experimental sidecars are present.
        if (modelHash != "77b01922e3190277ada3e5172078b99b90a994dd826ccbd4875f6974d0fe592b"
                && modelHash != "e8254b183b95fe7abc3ba357d4137a5178af80bb049bef59b26c1b888d4d0b08") return false;
        m_width = width; m_height = height;
        const auto prefix = model.parent_path() / (model.stem().wstring() + L"_"
            + std::wstring(modelHash.begin(), modelHash.begin() + 16));
        const auto featurePath = std::filesystem::path(prefix.wstring() + L".features.onnx");
        const auto synthesisPath = std::filesystem::path(prefix.wstring() + L".synthesis.onnx");
        if (!std::filesystem::is_regular_file(featurePath) || !std::filesystem::is_regular_file(synthesisPath)) return false;
        m_runtime.reset(nvinfer1::createInferRuntime(g_logger));
        if (!m_runtime) return false;
        const std::string suffix = "_pairfeatures_v1_" + std::to_string(width) + 'x' + std::to_string(height);
        if (!BuildStage(featurePath, cache, cacheKey + suffix + "_encoder", true, m_encoder)) return false;
        if (m_encoder->getNbIOTensors() != 2) return false;
        for (int i = 0; i < 2; ++i) {
            const char* name = m_encoder->getIOTensorName(i);
            if (!name || m_encoder->getTensorDataType(name) != nvinfer1::DataType::kHALF
                    || m_encoder->getTensorFormat(name) != nvinfer1::TensorFormat::kLINEAR) return false;
            if (m_encoder->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT) m_imageName = name;
            else m_featureName = name;
        }
        if (m_imageName.empty() || m_featureName.empty()) return false;
        TrtPtr<nvinfer1::IExecutionContext> shape(m_encoder->createExecutionContext());
        if (!shape || !shape->setInputShape(m_imageName.c_str(), ImageShape())) return false;
        m_featureShape = shape->getTensorShape(m_featureName.c_str());
        const size_t volume = Volume(m_featureShape);
        if (!volume || volume > (size_t{1} << 29)) return false;
        m_featureBytes = volume * sizeof(uint16_t);
        if (!BuildStage(synthesisPath, cache, cacheKey + suffix + "_synthesis", false, m_core)) return false;
        if (m_core->getNbIOTensors() != 3
                || m_core->getTensorIOMode(m_featureName.c_str()) != nvinfer1::TensorIOMode::kINPUT
                || m_core->getTensorDataType(m_featureName.c_str()) != nvinfer1::DataType::kHALF
                || m_core->getTensorFormat(m_featureName.c_str()) != nvinfer1::TensorFormat::kLINEAR) return false;
        return true;
    }

    nvinfer1::ICudaEngine* Core() const { return m_core.get(); }
    uint64_t EngineBytes() const { return m_engineBytes; }

    std::unique_ptr<Context> CreateContext(void* images, nvinfer1::IExecutionContext* core) {
        auto result = std::make_unique<Context>();
        result->encoder.reset(m_encoder->createExecutionContext());
        if (!images || !core || !result->encoder
                || !result->encoder->setInputShape(m_imageName.c_str(), ImageShape())
                || !core->setInputShape(m_featureName.c_str(), m_featureShape)
                || cudaMalloc(&result->features, m_featureBytes) != cudaSuccess
                || !result->encoder->setTensorAddress(m_imageName.c_str(), images)
                || !result->encoder->setTensorAddress(m_featureName.c_str(), result->features)
                || !core->setTensorAddress(m_featureName.c_str(), result->features)) return {};
        return result;
    }

private:
    nvinfer1::Dims4 ImageShape() const {
        return {1, 6, static_cast<int>(m_height), static_cast<int>(m_width)};
    }
    bool BuildStage(const std::filesystem::path& path, const std::filesystem::path& cache,
        const std::string& key, bool encoder, TrtPtr<nvinfer1::ICudaEngine>& engine) {
        const auto model = ReadFile(path);
        if (model.empty()) return false;
        const auto hash = Sha256Hex(model);
        if (hash.empty()) return false;
        const auto planPath = cache / (key + '_' + hash.substr(0, 16) + ".plan");
        const auto plan = ReadFile(planPath);
        if (!plan.empty()) {
            engine.reset(m_runtime->deserializeCudaEngine(plan.data(), plan.size()));
            if (engine) { m_engineBytes += plan.size(); return true; }
        }
        TrtPtr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(g_logger));
        if (!builder) return false;
        uint32_t flags = 0;
#if NV_TENSORRT_MAJOR >= 10
        flags |= 1u << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kSTRONGLY_TYPED);
#endif
        TrtPtr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(flags));
        if (!network) return false;
        TrtPtr<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network, g_logger));
        TrtPtr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
        if (!parser || !config || !parser->parse(model.data(), model.size())
                || network->getNbInputs() != (encoder ? 1 : 2) || network->getNbOutputs() != 1) return false;
        auto* profile = builder->createOptimizationProfile();
        if (!profile) return false;
        for (int i = 0; i < network->getNbInputs(); ++i) {
            auto* input = network->getInput(i);
            nvinfer1::Dims dims;
            if (!encoder && input->getName() == m_featureName) dims = m_featureShape;
            else dims = nvinfer1::Dims4{1, encoder ? 6 : 11, static_cast<int>(m_height), static_cast<int>(m_width)};
            if (input->getType() != nvinfer1::DataType::kHALF || dims.nbDims != 4) return false;
            input->setAllowedFormats(1u << static_cast<uint32_t>(nvinfer1::TensorFormat::kLINEAR));
            for (auto selector : {nvinfer1::OptProfileSelector::kMIN, nvinfer1::OptProfileSelector::kOPT, nvinfer1::OptProfileSelector::kMAX}) {
                if (!profile->setDimensions(input->getName(), selector, dims)) return false;
            }
        }
        network->getOutput(0)->setAllowedFormats(1u << static_cast<uint32_t>(nvinfer1::TensorFormat::kLINEAR));
        if (config->addOptimizationProfile(profile) < 0) return false;
        config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, size_t{2} << 30);
        TrtPtr<nvinfer1::IHostMemory> serialized(builder->buildSerializedNetwork(*network, *config));
        if (!serialized) return false;
        engine.reset(m_runtime->deserializeCudaEngine(serialized->data(), serialized->size()));
        if (!engine) return false;
        if (!WriteFileAtomically(planPath, serialized->data(), serialized->size())) {
            ReportEngineCacheSaveFailure(planPath);
        }
        m_engineBytes += serialized->size();
        return true;
    }
    uint32_t m_width = 0, m_height = 0;
    uint64_t m_engineBytes = 0;
    size_t m_featureBytes = 0;
    std::string m_imageName, m_featureName;
    nvinfer1::Dims m_featureShape{};
    TrtPtr<nvinfer1::IRuntime> m_runtime;
    TrtPtr<nvinfer1::ICudaEngine> m_encoder, m_core;
};
