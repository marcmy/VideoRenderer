#pragma once

// Included after the runtime's internal TensorRT/file helpers. A separate
// image-only encoder feeds the unchanged synthesis graph. Buffers and graphs
// belong to one execution context, never to recycled D3D texture addresses.
class FeatureReuseEngine final {
public:
    struct Context final {
        TrtPtr<nvinfer1::IExecutionContext> encoder;
        void* features = nullptr;
        // TensorRT scratch shared by the encoder and synthesis contexts. Both
        // run in order on one stream, so one allocation serves both; separate
        // allocations made reuse exceed the original engine's memory at 4K.
        // The synthesis context must be destroyed before this Context.
        void* scratch = nullptr;
        size_t scratchBytes = 0;
        cudaGraph_t graph = nullptr;
        cudaGraphExec_t graphExec = nullptr;
        ~Context() {
            if (graphExec) cudaGraphExecDestroy(graphExec);
            if (graph) cudaGraphDestroy(graph);
            encoder.reset();
            if (features) cudaFree(features);
            if (scratch) cudaFree(scratch);
        }
        bool Enqueue(cudaStream_t stream) {
            const bool encoded = graphExec ? cudaGraphLaunch(graphExec, stream) == cudaSuccess : encoder->enqueueV3(stream);
            // TensorRT 11.4's RIFE 4.25 Lite plans (whole and synthesis) read
            // part of their scratch before writing it; their own leftovers from
            // the previous frame have negligible effect, but encoder data could
            // decode as NaN there. Clear the shared scratch once per pair, after
            // the encoder: features live in their own buffer.
            return encoded && (!scratchBytes || cudaMemsetAsync(scratch, 0, scratchBytes, stream) == cudaSuccess);
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

    // Exports checked on the native GPU path, as published in this rife-models
    // dependency release. A model package with other hashes keeps its original
    // engine; update both together after validating the new exports.
    static constexpr const char* kValidatedModelsRelease = "mpcvr-deps-rife-models-cc46eaca0cda87fa";
    static bool IsValidatedModel(const std::string& modelHash) {
        for (const char* validated : {
                "f25f9da57fc6e8ecdeac5f9e3ab90b340004b11b9fadb6e805fbecc2431d9ea9",   // RIFE 4.15 Lite
                "a8daec071d3adb1b784647dea111094b939e9feb0a89bb163f34d9c4d017551b",   // RIFE 4.25
                "15e6177e5469924c1b3a70d360d0d807bd87f0a2be4c94188184b47f886a0563"}) { // RIFE 4.25 Lite
            if (modelHash == validated) return true;
        }
        return false;
    }

    bool Initialize(const std::filesystem::path& model, const std::filesystem::path& cache,
        const std::string& modelHash, const std::string& cacheKey, uint32_t width, uint32_t height) {
        if (!IsValidatedModel(modelHash)) return false;
        m_width = width; m_height = height;
        const auto prefix = model.parent_path() / (model.stem().wstring() + L"_"
            + std::wstring(modelHash.begin(), modelHash.begin() + 16));
        const auto featurePath = std::filesystem::path(prefix.wstring() + L".features.onnx");
        const auto synthesisPath = std::filesystem::path(prefix.wstring() + L".synthesis.onnx");
        if (!std::filesystem::is_regular_file(featurePath) || !std::filesystem::is_regular_file(synthesisPath)) return false;
        m_runtime.reset(nvinfer1::createInferRuntime(g_logger));
        if (!m_runtime) return false;
        // v2: the synthesis stage receives the image view as its own input.
        const std::string suffix = "_pairfeatures_v2_" + std::to_string(width) + 'x' + std::to_string(height);
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
        // Only the measured SM75/4.25 Lite path uses the fused warp. Keep the
        // native feature engine as a fallback, with its original cache identity.
        int device = 0;
        cudaDeviceProp props{};
        const bool useWarp = modelHash == "15e6177e5469924c1b3a70d360d0d807bd87f0a2be4c94188184b47f886a0563"
            && cudaGetDevice(&device) == cudaSuccess && cudaGetDeviceProperties(&props, device) == cudaSuccess
            && props.major == 7 && props.minor == 5 && RegisterRifeWarpPlugin();
        // Keep this suffix no longer than the native suffix: the atomic writer
        // also appends a unique temporary filename, subject to MAX_PATH.
        const bool warpBuilt = useWarp
            && BuildStage(synthesisPath, cache, cacheKey + suffix + "_synth_w3", false, m_core, true);
        if (!warpBuilt && !BuildStage(synthesisPath, cache, cacheKey + suffix + "_synthesis", false, m_core)) return false;
        if (m_core->getNbIOTensors() != 4) return false;
        for (const char* name : {m_featureName.c_str(), kImageViewName}) {
            if (m_core->getTensorIOMode(name) != nvinfer1::TensorIOMode::kINPUT
                    || m_core->getTensorDataType(name) != nvinfer1::DataType::kHALF
                    || m_core->getTensorFormat(name) != nvinfer1::TensorFormat::kLINEAR) return false;
        }
        return true;
    }

    // Its scratch memory is assigned by CreateContext().
    nvinfer1::IExecutionContext* CreateCoreContext() const {
        return m_core->createExecutionContext(nvinfer1::ExecutionContextAllocationStrategy::kUSER_MANAGED);
    }
    uint64_t EngineBytes() const { return m_engineBytes; }

    std::unique_ptr<Context> CreateContext(void* images, nvinfer1::IExecutionContext* core) {
        auto result = std::make_unique<Context>();
        result->encoder.reset(m_encoder->createExecutionContext(nvinfer1::ExecutionContextAllocationStrategy::kUSER_MANAGED));
        if (!images || !core || !result->encoder
                || !result->encoder->setInputShape(m_imageName.c_str(), ImageShape())
                || !core->setInputShape(m_featureName.c_str(), m_featureShape)
                || !core->setInputShape(kImageViewName, ImageViewShape())
                || cudaMalloc(&result->features, m_featureBytes) != cudaSuccess
                || !result->encoder->setTensorAddress(m_imageName.c_str(), images)
                || !result->encoder->setTensorAddress(m_featureName.c_str(), result->features)
                || !core->setTensorAddress(m_featureName.c_str(), result->features)
                // The two images as 2x3xHxW are the first six NCHW input
                // channels unchanged, so the view shares the input buffer.
                || !core->setTensorAddress(kImageViewName, images)) return {};
        // Both stages have fixed-shape profiles, so the engine sizes are exact.
        const int64_t scratchBytes = std::max(m_encoder->getDeviceMemorySizeV2(), m_core->getDeviceMemorySizeV2());
        if (scratchBytes < 0
                || (scratchBytes && cudaMalloc(&result->scratch, static_cast<size_t>(scratchBytes)) != cudaSuccess)) return {};
        result->scratchBytes = static_cast<size_t>(scratchBytes);
        result->encoder->setDeviceMemoryV2(result->scratch, scratchBytes);
        core->setDeviceMemoryV2(result->scratch, scratchBytes);
        return result;
    }

private:
    static constexpr const char* kImageViewName = "mpcvr_rife_image_view";

    nvinfer1::Dims4 ImageShape() const {
        return {1, 6, static_cast<int>(m_height), static_cast<int>(m_width)};
    }
    nvinfer1::Dims4 ImageViewShape() const {
        return {2, 3, static_cast<int>(m_height), static_cast<int>(m_width)};
    }
    // The split graph still reshapes the two input images to 2x3xHxW for the
    // first warp, a step the original graph shares with the removed encoder.
    // TensorRT fuses that lone reshape into the first warp kernel, which
    // corrupted the top rows of RIFE 4.25 Lite on SM75. Feed the view as an
    // input instead: the warp then builds as it does in the original engine.
    static bool ExposeImageView(nvinfer1::INetworkDefinition& network) {
        nvinfer1::ITensor* view = nullptr;
        for (int i = 0; i < network.getNbLayers() && !view; ++i) {
            auto* layer = network.getLayer(i);
            if (layer->getType() == nvinfer1::LayerType::kSHUFFLE && std::string_view(layer->getName()) == "/Reshape") {
                view = layer->getOutput(0);
            }
        }
        if (!view || view->getType() != nvinfer1::DataType::kHALF) return false;
        const auto dims = view->getDimensions();
        if (dims.nbDims != 4 || dims.d[0] != 2 || dims.d[1] != 3) return false;
        auto* input = network.addInput(kImageViewName, nvinfer1::DataType::kHALF, nvinfer1::Dims4{2, 3, -1, -1});
        if (!input) return false;
        int consumers = 0;
        for (int i = 0; i < network.getNbLayers(); ++i) {
            auto* layer = network.getLayer(i);
            for (int j = 0; j < layer->getNbInputs(); ++j) {
                if (layer->getInput(j) == view) {
                    layer->setInput(j, *input);
                    ++consumers;
                }
            }
        }
        return consumers > 0;
    }
    bool BuildStage(const std::filesystem::path& path, const std::filesystem::path& cache,
        const std::string& key, bool encoder, TrtPtr<nvinfer1::ICudaEngine>& engine, bool fuseWarp = false) {
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
                || network->getNbInputs() != (encoder ? 1 : 2) || network->getNbOutputs() != 1
                || (!encoder && !ExposeImageView(*network))
                || (fuseWarp && !FuseRifeSynthesisWarps(*network))) return false;
        auto* profile = builder->createOptimizationProfile();
        if (!profile) return false;
        for (int i = 0; i < network->getNbInputs(); ++i) {
            auto* input = network->getInput(i);
            nvinfer1::Dims dims;
            if (!encoder && input->getName() == m_featureName) dims = m_featureShape;
            else if (!encoder && std::string_view(input->getName()) == kImageViewName) dims = ImageViewShape();
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
