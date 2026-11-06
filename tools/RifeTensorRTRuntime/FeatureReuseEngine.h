#pragma once

// Rows the fused 4.25 Lite plan works on: the content rounded up to a multiple of 64.
// Rendering surfaces stay padded to 128, a height only the coarse blocks need, and
// the plugin gives them that height itself.
constexpr uint32_t RifeFusedEngineHeight(uint32_t contentHeight, uint32_t paddedHeight) noexcept
{
    const uint32_t aligned = (contentHeight + 63) / 64 * 64;
    return aligned < 128 ? (paddedHeight < 128 ? paddedHeight : 128) : aligned < paddedHeight ? aligned : paddedHeight;
}
static_assert(RifeFusedEngineHeight(1080, 1152) == 1088);
static_assert(RifeFusedEngineHeight(1088, 1152) == 1088);
static_assert(RifeFusedEngineHeight(1100, 1152) == 1152);
static_assert(RifeFusedEngineHeight(720, 768) == 720 / 64 * 64 + 64);
static_assert(RifeFusedEngineHeight(2160, 2176) == 2176);
static_assert(RifeFusedEngineHeight(64, 128) == 128);

// How many of a pair's targets share one flow estimate. Off runs every block per target.
// Coarse runs blocks 0-3 once per pair, at t = 0.5, and block 4 per target. Retime runs
// every block once per pair and only warps and blends per target. Both change the picture.
enum class RifeFlowReuse { Off, Coarse, Retime };

// Included after the runtime's internal TensorRT/file helpers. A separate
// image-only encoder feeds the unchanged synthesis graph. Buffers and graphs
// belong to one execution context, never to recycled D3D texture addresses.
class FeatureReuseEngine final {
public:
    struct Context;

    // The most recent pair's encoded second image, shared by all contexts because
    // the renderer gives consecutive pairs to different contexts.
    struct LatestFeatures {
        std::mutex mutex;
        uint64_t pairId = 0;
        Context* owner = nullptr;
    };

    struct EncoderStage {
        TrtPtr<nvinfer1::IExecutionContext> context;
        cudaGraph_t graph = nullptr;
        cudaGraphExec_t graphExec = nullptr;
        void Reset() {
            if (graphExec) cudaGraphExecDestroy(graphExec);
            if (graph) cudaGraphDestroy(graph);
            graphExec = nullptr;
            graph = nullptr;
            context.reset();
        }
        bool Enqueue(cudaStream_t stream) {
            return graphExec ? cudaGraphLaunch(graphExec, stream) == cudaSuccess : context->enqueueV3(stream);
        }
        bool Prime(cudaStream_t stream) {
            if (!context->enqueueV3(stream) || cudaStreamSynchronize(stream) != cudaSuccess) return false;
            if (cudaStreamBeginCapture(stream, cudaStreamCaptureModeGlobal) != cudaSuccess) return true;
            const bool enqueued = context->enqueueV3(stream);
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

    struct Context final {
        // The whole pair; with secondImage, only the first image.
        EncoderStage encoder;
        EncoderStage secondImage;
        // Blocks 0-3 at t = 0.5, once per pair, with their result and what each target
        // makes of it. Only with RifeFlowReuse::Coarse.
        EncoderStage anchor;
        void* anchorLevel = nullptr;
        void* level = nullptr;
        void* features = nullptr;
        size_t imageFeatureBytes = 0;
        // On the fused path, what the synthesis kernels read: each image and its
        // features interleaved. Features then only pass through their own buffer.
        void* warpSource = nullptr;
        size_t imageSourceBytes = 0;
        const void* images = nullptr;
        int width = 0, height = 0;
        LatestFeatures* latest = nullptr;
        cudaEvent_t encodedEvent = nullptr;
        cudaEvent_t copiedEvent = nullptr;
        // A later context's copy from this buffer, which may not have run yet.
        cudaEvent_t pendingReader = nullptr;
        // TensorRT scratch shared by the encoder and synthesis contexts. Both
        // run in order on one stream, so one allocation serves both; separate
        // allocations made reuse exceed the original engine's memory at 4K.
        // The synthesis context must be destroyed before this Context.
        void* scratch = nullptr;
        size_t scratchClearBytes = 0;
        size_t anchorClearBytes = 0;
        ~Context() {
            anchor.Reset();
            secondImage.Reset();
            encoder.Reset();
            if (encodedEvent) cudaEventDestroy(encodedEvent);
            if (copiedEvent) cudaEventDestroy(copiedEvent);
            if (features) cudaFree(features);
            if (warpSource) cudaFree(warpSource);
            if (anchorLevel) cudaFree(anchorLevel);
            if (level) cudaFree(level);
            if (scratch) cudaFree(scratch);
        }
        bool Enqueue(cudaStream_t stream, uint64_t pairId, uint64_t previousPairId, bool& firstKept) {
            firstKept = false;
            bool encoded = secondImage.context
                ? EnqueueImages(stream, pairId, previousPairId, firstKept) : encoder.Enqueue(stream);
            // A fused plan without the one-image encoder has both images' features here.
            if (encoded && warpSource && !secondImage.context) encoded = Interleave(stream, 0) && Interleave(stream, 1);
            // TensorRT 11.4's RIFE 4.25 Lite plans (whole and synthesis) read
            // part of their scratch before writing it; their own leftovers from
            // the previous frame have negligible effect, but encoder data could
            // decode as NaN there. Clear what both can touch once per pair, after
            // the encoder: features live in their own buffer.
            if (!encoded || (scratchClearBytes && cudaMemsetAsync(scratch, 0, scratchClearBytes, stream) != cudaSuccess)) return false;
            return !anchor.context || (anchor.Enqueue(stream)
                && (!anchorClearBytes || cudaMemsetAsync(scratch, 0, anchorClearBytes, stream) == cudaSuccess));
        }
        bool Prime(cudaStream_t stream) {
            return encoder.Prime(stream) && (!secondImage.context || secondImage.Prime(stream))
                && (!anchor.context || anchor.Prime(stream));
        }
    private:
        bool Interleave(cudaStream_t stream, int image) {
            const size_t imageBytes = size_t{3} * height * width * sizeof(uint16_t);
            return MpcvrRifeInterleaveImage(static_cast<const char*>(images) + image * imageBytes,
                static_cast<const char*>(features) + image * imageFeatureBytes,
                static_cast<char*>(warpSource) + image * imageSourceBytes, width, height, stream) == cudaSuccess;
        }
        bool EnqueueImages(cudaStream_t stream, uint64_t pairId, uint64_t previousPairId, bool& firstKept) {
            std::lock_guard lock(latest->mutex);
            Context* source = previousPairId && latest->pairId == previousPairId ? latest->owner : nullptr;
            // Nothing may read this buffer from here until it is published again,
            // and whoever already planned to must finish before it changes.
            latest->pairId = 0;
            latest->owner = nullptr;
            if (pendingReader) {
                if (cudaStreamWaitEvent(stream, pendingReader, 0) != cudaSuccess) return false;
                pendingReader = nullptr;
            }
            if (source) {
                if (source != this && cudaStreamWaitEvent(stream, source->encodedEvent, 0) != cudaSuccess) return false;
                if (cudaMemcpyAsync(warpSource, static_cast<char*>(source->warpSource) + imageSourceBytes,
                        imageSourceBytes, cudaMemcpyDeviceToDevice, stream) != cudaSuccess) return false;
                if (source != this) {
                    if (cudaEventRecord(copiedEvent, stream) != cudaSuccess) return false;
                    source->pendingReader = copiedEvent;
                }
                firstKept = true;
            } else if (!encoder.Enqueue(stream) || !Interleave(stream, 0)) {
                return false;
            }
            if (!secondImage.Enqueue(stream) || !Interleave(stream, 1)
                    || cudaEventRecord(encodedEvent, stream) != cudaSuccess) return false;
            if (pairId) {
                latest->pairId = pairId;
                latest->owner = this;
            }
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
        const std::string& modelHash, const std::string& cacheKey, uint32_t width, uint32_t height, uint32_t contentHeight,
        RifeFlowReuse flowReuse) {
        if (!IsValidatedModel(modelHash)) return false;
        // Only the measured SM75/4.25 Lite path uses the fused warp. Keep the
        // native feature engine as a fallback, with its original cache identity.
        int device = 0;
        cudaDeviceProp props{};
        const bool useWarp = modelHash == "15e6177e5469924c1b3a70d360d0d807bd87f0a2be4c94188184b47f886a0563"
            && width % 128 == 0 && height % 128 == 0
            && cudaGetDevice(&device) == cudaSuccess && cudaGetDeviceProperties(&props, device) == cudaSuccess
            && props.major == 7 && props.minor == 5 && RegisterRifeWarpPlugin();
        if (useWarp) {
            const uint32_t fusedHeight = RifeFusedEngineHeight(contentHeight, height);
            if (flowReuse != RifeFlowReuse::Off && InitializeSize(model, cache, modelHash, cacheKey, width, fusedHeight, true, flowReuse)) return true;
            if (InitializeSize(model, cache, modelHash, cacheKey, width, fusedHeight, true, RifeFlowReuse::Off)) return true;
        }
        return InitializeSize(model, cache, modelHash, cacheKey, width, height, false, RifeFlowReuse::Off);
    }

    RifeFlowReuse FlowReuse() const { return m_flowReuse; }
    bool Fused() const { return m_fused; }
    int LevelRows() const { return static_cast<int>((m_height + 127) / 128 * 128 / 4); }
    int LevelCols() const { return static_cast<int>(m_width / 4); }
    // The field plan's output doesn't fit the frame's three planes.
    size_t OutputBytes() const { return size_t{16} * m_height * m_width; }

    // Rows every tensor has; on the fused path, fewer than the padded surface's.
    uint32_t Height() const { return m_height; }

    // Its scratch memory is assigned by CreateContext().
    nvinfer1::IExecutionContext* CreateCoreContext() const {
        return m_core->createExecutionContext(nvinfer1::ExecutionContextAllocationStrategy::kUSER_MANAGED);
    }
    uint64_t EngineBytes() const { return m_engineBytes; }

    std::unique_ptr<Context> CreateContext(void* images, nvinfer1::IExecutionContext* core) {
        auto result = std::make_unique<Context>();
        const auto& encoderEngine = m_oneImage ? m_oneImage : m_encoder;
        const auto create = [&] {
            return encoderEngine->createExecutionContext(nvinfer1::ExecutionContextAllocationStrategy::kUSER_MANAGED);
        };
        result->encoder.context.reset(create());
        if (m_oneImage) result->secondImage.context.reset(create());
        if (!images || !core || !result->encoder.context || (m_oneImage && !result->secondImage.context)
                || !core->setInputShape(m_featureName.c_str(), m_featureShape)
                || !core->setInputShape(kImageViewName, ImageViewShape())
                // One image at a time passes through this buffer on its way to the
                // warp source, so a one-image encoder needs room for only one.
                || cudaMalloc(&result->features, m_oneImage ? m_featureBytes / 2 : m_featureBytes) != cudaSuccess
                || !core->setTensorAddress(m_featureName.c_str(), result->features)
                // The two images as 2x3xHxW are the first six NCHW input
                // channels unchanged, so the view shares the input buffer.
                || !core->setTensorAddress(kImageViewName, images)
                || cudaEventCreate(&result->encodedEvent) != cudaSuccess
                || cudaEventCreate(&result->copiedEvent) != cudaSuccess) return {};
        result->imageFeatureBytes = m_oneImage ? 0 : m_featureBytes / 2;
        result->latest = &m_latest;
        const auto levelBytes = size_t(LevelRows()) * LevelCols() * 32;
        if (m_anchor && (cudaMalloc(&result->anchorLevel, levelBytes) != cudaSuccess || cudaMalloc(&result->level, levelBytes) != cudaSuccess
                || cudaMemset(result->anchorLevel, 0, levelBytes) != cudaSuccess || cudaMemset(result->level, 0, levelBytes) != cudaSuccess
                || !core->setInputShape(kRifeLevelName, LevelShape()) || !core->setTensorAddress(kRifeLevelName, result->level))) return {};
        if (m_fused) {
            const size_t sourceBytes = size_t{2} * m_height * m_width * 8 * sizeof(uint16_t);
            if (cudaMalloc(&result->warpSource, sourceBytes) != cudaSuccess
                    || cudaMemset(result->warpSource, 0, sourceBytes) != cudaSuccess
                    || !core->setInputShape(kRifeWarpSourceName, WarpSourceShape())
                    || !core->setTensorAddress(kRifeWarpSourceName, result->warpSource)) return {};
            result->imageSourceBytes = sourceBytes / 2;
            result->images = images;
            result->width = static_cast<int>(m_width);
            result->height = static_cast<int>(m_height);
        }
        const auto bind = [&](nvinfer1::IExecutionContext& encoder, int image) {
            auto* pixels = static_cast<char*>(images);
            auto* imageFeatures = static_cast<char*>(result->features);
            const size_t imageBytes = size_t{3} * m_height * m_width * sizeof(uint16_t);
            return encoder.setInputShape(m_imageName.c_str(), ImageShape())
                && encoder.setInputShape(kOneImageName, OneImageShape())
                && encoder.setTensorAddress(m_imageName.c_str(), images)
                && encoder.setTensorAddress(kOneImageName, pixels + image * imageBytes)
                && encoder.setTensorAddress(m_featureName.c_str(), imageFeatures + image * result->imageFeatureBytes);
        };
        const bool bound = m_oneImage
            ? bind(*result->encoder.context, 0) && bind(*result->secondImage.context, 1)
            : result->encoder.context->setInputShape(m_imageName.c_str(), ImageShape())
                && result->encoder.context->setTensorAddress(m_imageName.c_str(), images)
                && result->encoder.context->setTensorAddress(m_featureName.c_str(), result->features);
        if (!bound) return {};
        if (m_anchor) {
            result->anchor.context.reset(m_anchor->createExecutionContext(nvinfer1::ExecutionContextAllocationStrategy::kUSER_MANAGED));
            if (!result->anchor.context) return {};
            for (int i = 0; i < m_anchor->getNbIOTensors(); ++i) {
                const std::string_view name = m_anchor->getIOTensorName(i);
                void* address = images;
                nvinfer1::Dims4 shape{1, 11, static_cast<int>(m_height), static_cast<int>(m_width)};
                if (name == kRifeAnchorLevelName) { result->anchor.context->setTensorAddress(name.data(), result->anchorLevel); continue; }
                if (name == m_featureName) { address = result->features; shape = {m_featureShape.d[0], m_featureShape.d[1], m_featureShape.d[2], m_featureShape.d[3]}; }
                else if (name == kImageViewName) { shape = ImageViewShape(); }
                else if (name == kRifeWarpSourceName) { address = result->warpSource; shape = WarpSourceShape(); }
                if (!result->anchor.context->setInputShape(name.data(), shape) || !result->anchor.context->setTensorAddress(name.data(), address)) return {};
            }
        }
        // Every stage has fixed-shape profiles, so the engine sizes are exact.
        const int64_t encoderScratch = encoderEngine->getDeviceMemorySizeV2();
        const int64_t coreScratch = m_core->getDeviceMemorySizeV2();
        const int64_t anchorScratch = m_anchor ? m_anchor->getDeviceMemorySizeV2() : 0;
        const int64_t scratchBytes = std::max({encoderScratch, coreScratch, anchorScratch});
        if (scratchBytes < 0
                || (scratchBytes && cudaMalloc(&result->scratch, static_cast<size_t>(scratchBytes)) != cudaSuccess)) return {};
        result->scratchClearBytes = static_cast<size_t>(std::min(encoderScratch, m_anchor ? anchorScratch : coreScratch));
        result->anchorClearBytes = m_anchor ? static_cast<size_t>(std::min(anchorScratch, coreScratch)) : 0;
        result->encoder.context->setDeviceMemoryV2(result->scratch, scratchBytes);
        if (m_oneImage) result->secondImage.context->setDeviceMemoryV2(result->scratch, scratchBytes);
        if (m_anchor) result->anchor.context->setDeviceMemoryV2(result->scratch, scratchBytes);
        core->setDeviceMemoryV2(result->scratch, scratchBytes);
        return result;
    }

private:
    bool InitializeSize(const std::filesystem::path& model, const std::filesystem::path& cache,
        const std::string& modelHash, const std::string& cacheKey, uint32_t width, uint32_t height, bool useWarp, RifeFlowReuse flowReuse) {
        m_core.reset();
        m_anchor.reset();
        m_oneImage.reset();
        m_encoder.reset();
        m_engineBytes = 0;
        m_imageName.clear();
        m_featureName.clear();
        m_fused = false;
        m_flowReuse = RifeFlowReuse::Off;
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
        const uint64_t twoImageBytes = m_engineBytes;
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
        // An engine must outlive its contexts, and this one may be released below.
        shape.reset();
        const size_t volume = Volume(m_featureShape);
        if (!volume || volume > (size_t{1} << 29)) return false;
        m_featureBytes = volume * sizeof(uint16_t);
        // Keep these suffixes no longer than the native suffix: the atomic writer
        // also appends a unique temporary filename, subject to MAX_PATH.
        const auto corePlan = flowReuse == RifeFlowReuse::Coarse ? RifeFusedPlan::Tail
            : flowReuse == RifeFlowReuse::Retime ? RifeFusedPlan::Field : RifeFusedPlan::Whole;
        const char* coreSuffix = flowReuse == RifeFlowReuse::Coarse ? "_tail_w6" : flowReuse == RifeFlowReuse::Retime ? "_field_w6" : "_synth_w6";
        const bool warpBuilt = useWarp
            && BuildStage(synthesisPath, cache, cacheKey + suffix + coreSuffix, false, m_core, true, false, corePlan)
            && (flowReuse != RifeFlowReuse::Coarse
                || BuildStage(synthesisPath, cache, cacheKey + suffix + "_anchor_w6", false, m_anchor, true, false, RifeFusedPlan::Anchor));
        // On the same measured path, encoding images one at a time lets consecutive
        // pairs share their common frame. The two-image plan stays the fallback.
        if (warpBuilt && BuildStage(featurePath, cache, cacheKey + suffix + "_encoder1", true, m_oneImage, false, true)
                && m_oneImage->getNbIOTensors() == 3) {
            m_encoder.reset();
            m_engineBytes -= twoImageBytes;
        } else {
            m_oneImage.reset();
        }
        // The fused path has already chosen a size only it can use.
        if (useWarp && !warpBuilt) return false;
        if (!useWarp && !BuildStage(synthesisPath, cache, cacheKey + suffix + "_synthesis", false, m_core)) return false;
        m_fused = warpBuilt;
        m_flowReuse = flowReuse;
        if (m_core->getNbIOTensors() != (!m_fused ? 4 : flowReuse == RifeFlowReuse::Coarse ? 6 : 5)) return false;
        for (const char* name : {m_featureName.c_str(), kImageViewName}) {
            if (m_core->getTensorIOMode(name) != nvinfer1::TensorIOMode::kINPUT
                    || m_core->getTensorDataType(name) != nvinfer1::DataType::kHALF
                    || m_core->getTensorFormat(name) != nvinfer1::TensorFormat::kLINEAR) return false;
        }
        return true;
    }

    static constexpr const char* kImageViewName = "mpcvr_rife_image_view";
    static constexpr const char* kOneImageName = "mpcvr_rife_one_image";

    nvinfer1::Dims4 ImageShape() const {
        return {1, 6, static_cast<int>(m_height), static_cast<int>(m_width)};
    }
    nvinfer1::Dims4 OneImageShape() const {
        return {1, 3, static_cast<int>(m_height), static_cast<int>(m_width)};
    }
    nvinfer1::Dims4 ImageViewShape() const {
        return {2, 3, static_cast<int>(m_height), static_cast<int>(m_width)};
    }
    nvinfer1::Dims4 WarpSourceShape() const {
        return {2, static_cast<int>(m_height), static_cast<int>(m_width), 8};
    }
    nvinfer1::Dims4 LevelShape() const {
        return {1, LevelRows(), LevelCols(), 16};
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
    // Feed one image straight to the convolutions, past the reshape into two. The
    // graph's own input stays declared but unused, which TensorRT retains.
    static bool ExposeOneImage(nvinfer1::INetworkDefinition& network) {
        nvinfer1::ITensor* pair = nullptr;
        for (int i = 0; i < network.getNbLayers() && !pair; ++i) {
            auto* layer = network.getLayer(i);
            if (layer->getType() == nvinfer1::LayerType::kSHUFFLE && std::string_view(layer->getName()) == "/Reshape") {
                pair = layer->getOutput(0);
            }
        }
        if (!pair || pair->getType() != nvinfer1::DataType::kHALF) return false;
        auto* input = network.addInput(kOneImageName, nvinfer1::DataType::kHALF, nvinfer1::Dims4{1, 3, -1, -1});
        if (!input) return false;
        int consumers = 0;
        for (int i = 0; i < network.getNbLayers(); ++i) {
            auto* layer = network.getLayer(i);
            for (int j = 0; j < layer->getNbInputs(); ++j) {
                if (layer->getInput(j) == pair) {
                    layer->setInput(j, *input);
                    ++consumers;
                }
            }
        }
        return consumers > 0;
    }
    bool BuildStage(const std::filesystem::path& path, const std::filesystem::path& cache,
        const std::string& key, bool encoder, TrtPtr<nvinfer1::ICudaEngine>& engine, bool fuseWarp = false, bool oneImage = false,
        RifeFusedPlan fusedPlan = RifeFusedPlan::Whole) {
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
        std::vector<uint16_t> fusedWeights;
        TrtPtr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
        if (!parser || !config || !parser->parse(model.data(), model.size())
                || network->getNbInputs() != (encoder ? 1 : 2) || network->getNbOutputs() != 1
                || (!encoder && !ExposeImageView(*network))
                || (oneImage && !ExposeOneImage(*network))
                || (fuseWarp && !FuseRifeSynthesisWarps(*network, fusedWeights, fusedPlan))) return false;
        auto* profile = builder->createOptimizationProfile();
        if (!profile) return false;
        for (int i = 0; i < network->getNbInputs(); ++i) {
            auto* input = network->getInput(i);
            nvinfer1::Dims dims;
            if (!encoder && input->getName() == m_featureName) dims = m_featureShape;
            else if (!encoder && std::string_view(input->getName()) == kImageViewName) dims = ImageViewShape();
            else if (oneImage && std::string_view(input->getName()) == kOneImageName) dims = OneImageShape();
            else if (fuseWarp && std::string_view(input->getName()) == kRifeWarpSourceName) dims = WarpSourceShape();
            else if (fuseWarp && std::string_view(input->getName()) == kRifeLevelName) dims = LevelShape();
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
    bool m_fused = false;
    RifeFlowReuse m_flowReuse = RifeFlowReuse::Off;
    std::string m_imageName, m_featureName;
    nvinfer1::Dims m_featureShape{};
    TrtPtr<nvinfer1::IRuntime> m_runtime;
    TrtPtr<nvinfer1::ICudaEngine> m_encoder, m_oneImage, m_core, m_anchor;
    LatestFeatures m_latest;
};
