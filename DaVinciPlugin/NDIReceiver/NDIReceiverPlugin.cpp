// NDI Receiver OFX Generator Plugin for DaVinci Resolve
// Receives NDI video from UE LookScopes and presents it as a source in Resolve's Color page.

#include "NDIReceiverPlugin.h"
#include "ofxsProcessing.h"
#include <cstring>
#include <cstdio>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>

// NDI SDK - dynamic loading (no link-time dependency)
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

// We must manually LoadLibrary since no import .lib is shipped with UE's NDI SDK.
// Only include the types and struct definitions, not the dllimport declarations.
#define PROCESSINGNDILIB_STATIC 1
#define NDILIB_CPP_DEFAULT_CONSTRUCTORS 1
#include <Processing.NDI.Lib.h>
#include <Processing.NDI.DynamicLoad.h>

static const NDIlib_v5* g_NDILib = nullptr;
static HMODULE g_NDIDll = nullptr;

static std::string GetPluginDirectory()
{
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&GetPluginDirectory,
        &hSelf);
    if (!hSelf) return {};
    char path[MAX_PATH] = {};
    GetModuleFileNameA(hSelf, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    return (pos != std::string::npos) ? dir.substr(0, pos) : dir;
}

static const NDIlib_v5* ManualLoadNDI()
{
    // 1) Try loading from the same directory as this OFX plugin
    std::string pluginDir = GetPluginDirectory();
    if (!pluginDir.empty())
    {
        std::string fullPath = pluginDir + "\\Processing.NDI.Lib.x64.dll";
        g_NDIDll = LoadLibraryA(fullPath.c_str());
    }

    // 2) Fallback: system PATH / NDI Tools install
    if (!g_NDIDll)
        g_NDIDll = LoadLibraryA("Processing.NDI.Lib.x64.dll");

    if (!g_NDIDll)
    {
        fprintf(stderr, "[NDIReceiver] Could not load Processing.NDI.Lib.x64.dll (searched: %s)\n",
                pluginDir.c_str());
        return nullptr;
    }

    typedef const NDIlib_v5* (*PFN_NDIlib_v5_load)(void);
    auto fn = (PFN_NDIlib_v5_load)GetProcAddress(g_NDIDll, "NDIlib_v5_load");
    if (!fn)
    {
        fprintf(stderr, "[NDIReceiver] Could not find NDIlib_v5_load export\n");
        FreeLibrary(g_NDIDll);
        g_NDIDll = nullptr;
        return nullptr;
    }
    return fn();
}

// ============================================================
// Plugin identity
// ============================================================

static const char* kPluginName = "NDI Receiver (UE Bridge)";
static const char* kPluginGrouping = "LookScopes";
static const char* kPluginDescription =
    "Receives NDI video stream from Unreal Engine.\n"
    "Use as a Generator source in DaVinci Resolve to preview UE viewport in real-time.";
static const char* kPluginIdentifier = "com.kuoyu.lookscopes.ndireceiver";
static const int kPluginVersionMajor = 1;
static const int kPluginVersionMinor = 0;

static const char* kParamSourceName = "sourceName";
static const char* kParamRefresh = "refreshBtn";
static const char* kParamRefreshCounter = "refreshCounter";

// ============================================================
// NDI Receiver state (shared across render calls)
// ============================================================

struct NDIReceiverState
{
    NDIlib_recv_instance_t recv = nullptr;
    NDIlib_find_instance_t finder = nullptr;

    std::mutex frameMutex;
    std::vector<uint8_t> frameBuffer;
    int frameWidth = 0;
    int frameHeight = 0;
    std::atomic<bool> hasNewFrame{false};
    std::atomic<bool> isConnected{false};
    std::string targetSourceName = "UE_LookScopes";

    std::thread receiveThread;
    std::atomic<bool> shouldRun{false};

    void StartReceiving()
    {
        if (!g_NDILib || shouldRun.load()) return;
        shouldRun.store(true);
        receiveThread = std::thread(&NDIReceiverState::ReceiveLoop, this);
    }

    void StopReceiving()
    {
        shouldRun.store(false);
        if (receiveThread.joinable())
            receiveThread.join();
        if (recv && g_NDILib)
        {
            g_NDILib->recv_destroy(recv);
            recv = nullptr;
        }
        if (finder && g_NDILib)
        {
            g_NDILib->find_destroy(finder);
            finder = nullptr;
        }
        isConnected.store(false);
    }

    void ReceiveLoop()
    {
        if (!g_NDILib) return;

        NDIlib_find_create_t findCreate;
        findCreate.show_local_sources = true;
        findCreate.p_groups = nullptr;
        findCreate.p_extra_ips = nullptr;
        finder = g_NDILib->find_create_v2(&findCreate);
        if (!finder) return;

        while (shouldRun.load())
        {
            if (!recv)
            {
                uint32_t numSources = 0;
                const NDIlib_source_t* sources = g_NDILib->find_get_current_sources(finder, &numSources);

                for (uint32_t i = 0; i < numSources; ++i)
                {
                    if (sources[i].p_ndi_name &&
                        std::string(sources[i].p_ndi_name).find(targetSourceName) != std::string::npos)
                    {
                        NDIlib_recv_create_v3_t recvCreate;
                        recvCreate.source_to_connect_to = sources[i];
                        recvCreate.color_format = NDIlib_recv_color_format_RGBX_RGBA;
                        recvCreate.bandwidth = NDIlib_recv_bandwidth_highest;
                        recvCreate.allow_video_fields = false;
                        recvCreate.p_ndi_recv_name = "DaVinci LookScopes Receiver";

                        recv = g_NDILib->recv_create_v3(&recvCreate);
                        if (recv) isConnected.store(true);
                        break;
                    }
                }

                if (!recv)
                {
                    g_NDILib->find_wait_for_sources(finder, 1000);
                    continue;
                }
            }

            NDIlib_video_frame_v2_t videoFrame;
            NDIlib_audio_frame_v2_t audioFrame;
            NDIlib_metadata_frame_t metadataFrame;

            switch (g_NDILib->recv_capture_v2(recv, &videoFrame, &audioFrame, &metadataFrame, 100))
            {
            case NDIlib_frame_type_video:
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                size_t dataSize = (size_t)videoFrame.xres * videoFrame.yres * 4;
                frameBuffer.resize(dataSize);

                if (videoFrame.line_stride_in_bytes == videoFrame.xres * 4)
                {
                    memcpy(frameBuffer.data(), videoFrame.p_data, dataSize);
                }
                else
                {
                    for (int y = 0; y < videoFrame.yres; ++y)
                    {
                        memcpy(
                            frameBuffer.data() + (size_t)y * videoFrame.xres * 4,
                            videoFrame.p_data + (size_t)y * videoFrame.line_stride_in_bytes,
                            (size_t)videoFrame.xres * 4);
                    }
                }
                frameWidth = videoFrame.xres;
                frameHeight = videoFrame.yres;
                hasNewFrame.store(true);
                g_NDILib->recv_free_video_v2(recv, &videoFrame);
                break;
            }
            case NDIlib_frame_type_audio:
                g_NDILib->recv_free_audio_v2(recv, &audioFrame);
                break;
            case NDIlib_frame_type_metadata:
                g_NDILib->recv_free_metadata(recv, &metadataFrame);
                break;
            case NDIlib_frame_type_error:
                if (recv)
                {
                    g_NDILib->recv_destroy(recv);
                    recv = nullptr;
                }
                isConnected.store(false);
                break;
            default:
                break;
            }
        }
    }
};

static NDIReceiverState* g_NDIState = nullptr;
static int g_InstanceCount = 0;

// ============================================================
// Image Processor (CPU path)
// ============================================================

class NDIFrameWriter : public OFX::ImageProcessor
{
public:
    explicit NDIFrameWriter(OFX::ImageEffect& p_Instance)
        : OFX::ImageProcessor(p_Instance) {}

    void setOutputBounds(int dstW, int dstH) { m_DstW = dstW; m_DstH = dstH; }

    virtual void multiThreadProcessImages(OfxRectI p_ProcWindow) override
    {
        if (!g_NDIState || g_NDIState->frameBuffer.empty())
        {
            for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y)
            {
                float* dstPix = static_cast<float*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));
                if (!dstPix) continue;
                memset(dstPix, 0, (size_t)(p_ProcWindow.x2 - p_ProcWindow.x1) * 4 * sizeof(float));
            }
            return;
        }

        std::lock_guard<std::mutex> lock(g_NDIState->frameMutex);
        int srcW = g_NDIState->frameWidth;
        int srcH = g_NDIState->frameHeight;
        const uint8_t* srcData = g_NDIState->frameBuffer.data();
        if (srcW <= 0 || srcH <= 0) return;

        int dstW = m_DstW > 0 ? m_DstW : (p_ProcWindow.x2 - p_ProcWindow.x1);
        int dstH = m_DstH > 0 ? m_DstH : (p_ProcWindow.y2 - p_ProcWindow.y1);

        // Fit source into destination preserving aspect ratio
        float scaleX = (float)dstW / srcW;
        float scaleY = (float)dstH / srcH;
        float scale = std::min(scaleX, scaleY);
        int fitW = (int)(srcW * scale);
        int fitH = (int)(srcH * scale);
        int offsetX = (dstW - fitW) / 2;
        int offsetY = (dstH - fitH) / 2;

        for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y)
        {
            float* dstPix = static_cast<float*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));
            if (!dstPix) continue;

            // OFX Y=0 is bottom, NDI Y=0 is top → flip
            int flippedY = (dstH - 1) - y;

            for (int x = p_ProcWindow.x1; x < p_ProcWindow.x2; ++x)
            {
                int relX = x - offsetX;
                int relY = flippedY - offsetY;

                if (relX >= 0 && relX < fitW && relY >= 0 && relY < fitH)
                {
                    int srcX = (int)((float)relX / scale);
                    int srcY = (int)((float)relY / scale);
                    srcX = std::min(srcX, srcW - 1);
                    srcY = std::min(srcY, srcH - 1);

                    size_t srcIdx = ((size_t)srcY * srcW + srcX) * 4;
                    dstPix[0] = srcData[srcIdx + 0] / 255.0f;
                    dstPix[1] = srcData[srcIdx + 1] / 255.0f;
                    dstPix[2] = srcData[srcIdx + 2] / 255.0f;
                    dstPix[3] = srcData[srcIdx + 3] / 255.0f;
                }
                else
                {
                    dstPix[0] = dstPix[1] = dstPix[2] = 0.0f;
                    dstPix[3] = 1.0f;
                }
                dstPix += 4;
            }
        }
    }

private:
    int m_DstW = 0;
    int m_DstH = 0;
};

// ============================================================
// NDI Receiver Plugin (ImageEffect)
// ============================================================

class NDIReceiverPlugin : public OFX::ImageEffect
{
public:
    explicit NDIReceiverPlugin(OfxImageEffectHandle p_Handle)
        : OFX::ImageEffect(p_Handle)
    {
        m_DstClip = fetchClip(kOfxImageEffectOutputClipName);
        m_SourceName = fetchStringParam(kParamSourceName);
        m_RefreshCounter = fetchIntParam(kParamRefreshCounter);

        g_InstanceCount++;

        if (!g_NDIState)
            g_NDIState = new NDIReceiverState();

        std::string name;
        m_SourceName->getValue(name);
        if (!name.empty())
            g_NDIState->targetSourceName = name;

        g_NDIState->StartReceiving();
    }

    virtual ~NDIReceiverPlugin()
    {
        g_InstanceCount--;
        if (g_InstanceCount <= 0 && g_NDIState)
        {
            g_NDIState->StopReceiving();
            delete g_NDIState;
            g_NDIState = nullptr;
            g_InstanceCount = 0;
        }
    }

    virtual void render(const OFX::RenderArguments& p_Args) override
    {
        if (!m_DstClip) return;

        std::unique_ptr<OFX::Image> dst(m_DstClip->fetchImage(p_Args.time));
        if (!dst) return;

        OfxRectI bounds = dst->getBounds();
        int dstW = bounds.x2 - bounds.x1;
        int dstH = bounds.y2 - bounds.y1;

        NDIFrameWriter processor(*this);
        processor.setDstImg(dst.get());
        processor.setRenderWindow(p_Args.renderWindow);
        processor.setOutputBounds(dstW, dstH);
        processor.process();
    }

    virtual void changedParam(const OFX::InstanceChangedArgs& p_Args, const std::string& p_ParamName) override
    {
        if (p_ParamName == kParamSourceName && g_NDIState)
        {
            std::string name;
            m_SourceName->getValue(name);
            if (!name.empty())
            {
                g_NDIState->StopReceiving();
                g_NDIState->targetSourceName = name;
                g_NDIState->StartReceiving();
            }
        }
        else if (p_ParamName == kParamRefresh && m_RefreshCounter)
        {
            int val = 0;
            m_RefreshCounter->getValue(val);
            m_RefreshCounter->setValue(val + 1);
        }
    }

    virtual bool isIdentity(const OFX::IsIdentityArguments&, OFX::Clip*&, double&) override
    {
        return false;
    }

private:
    OFX::Clip* m_DstClip = nullptr;
    OFX::StringParam* m_SourceName = nullptr;
    OFX::IntParam* m_RefreshCounter = nullptr;
};

// ============================================================
// Factory
// ============================================================

NDIReceiverPluginFactory::NDIReceiverPluginFactory()
    : OFX::PluginFactoryHelper<NDIReceiverPluginFactory>(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor)
{
}

void NDIReceiverPluginFactory::load()
{
    g_NDILib = ManualLoadNDI();
    if (!g_NDILib)
    {
        fprintf(stderr, "[NDIReceiver] Failed to load NDI runtime DLL.\n");
        return;
    }
    if (!g_NDILib->initialize())
    {
        fprintf(stderr, "[NDIReceiver] NDIlib_initialize() failed.\n");
        g_NDILib = nullptr;
    }
}

void NDIReceiverPluginFactory::unload()
{
    if (g_NDIState)
    {
        g_NDIState->StopReceiving();
        delete g_NDIState;
        g_NDIState = nullptr;
    }
    if (g_NDILib)
    {
        g_NDILib->destroy();
        g_NDILib = nullptr;
    }
    if (g_NDIDll)
    {
        FreeLibrary(g_NDIDll);
        g_NDIDll = nullptr;
    }
}

void NDIReceiverPluginFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
{
    p_Desc.setLabels(kPluginName, kPluginName, kPluginName);
    p_Desc.setPluginGrouping(kPluginGrouping);
    p_Desc.setPluginDescription(kPluginDescription);

    p_Desc.addSupportedContext(OFX::eContextGenerator);
    p_Desc.addSupportedContext(OFX::eContextGeneral);
    p_Desc.addSupportedBitDepth(OFX::eBitDepthFloat);

    p_Desc.setSingleInstance(false);
    p_Desc.setHostFrameThreading(false);
    p_Desc.setSupportsMultiResolution(false);
    p_Desc.setSupportsTiles(true);
    p_Desc.setTemporalClipAccess(false);
    p_Desc.setRenderTwiceAlways(false);
    p_Desc.setSupportsMultipleClipPARs(false);
    p_Desc.setNoSpatialAwareness(true);
}

void NDIReceiverPluginFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum p_Context)
{
    OFX::ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(OFX::ePixelComponentRGBA);
    dstClip->setSupportsTiles(true);

    if (p_Context == OFX::eContextGeneral)
    {
        OFX::ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        srcClip->addSupportedComponent(OFX::ePixelComponentRGBA);
        srcClip->setSupportsTiles(true);
        srcClip->setOptional(true);
    }

    OFX::PageParamDescriptor* page = p_Desc.definePageParam("Controls");

    OFX::StringParamDescriptor* sourceNameParam = p_Desc.defineStringParam(kParamSourceName);
    sourceNameParam->setLabel("NDI Source Name");
    sourceNameParam->setHint("Name of the NDI source to receive (must match UE LookScopes source name)");
    sourceNameParam->setDefault("UE_LookScopes");
    sourceNameParam->setAnimates(false);
    if (page) page->addChild(*sourceNameParam);

    OFX::PushButtonParamDescriptor* refreshBtn = p_Desc.definePushButtonParam(kParamRefresh);
    refreshBtn->setLabel("Refresh Frame");
    refreshBtn->setHint("Click to fetch and display the latest NDI frame");
    if (page) page->addChild(*refreshBtn);

    OFX::IntParamDescriptor* refreshCounter = p_Desc.defineIntParam(kParamRefreshCounter);
    refreshCounter->setLabel("  ");
    refreshCounter->setDefault(0);
    refreshCounter->setIsSecret(true);
    refreshCounter->setAnimates(true);
    if (page) page->addChild(*refreshCounter);
}

OFX::ImageEffect* NDIReceiverPluginFactory::createInstance(OfxImageEffectHandle p_Handle, OFX::ContextEnum)
{
    return new NDIReceiverPlugin(p_Handle);
}

// ============================================================
// Plugin registration
// ============================================================

static NDIReceiverPluginFactory g_NDIReceiverPluginFactory;

void OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray& p_FactoryArray)
{
    p_FactoryArray.push_back(&g_NDIReceiverPluginFactory);
}
