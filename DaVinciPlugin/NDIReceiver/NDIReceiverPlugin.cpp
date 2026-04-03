// NDI Receiver + AI LUT OFX Generator Plugin for DaVinci Resolve
// Receives NDI video from UE LookScopes, optionally applies a .cube 3D LUT,
// and presents the result as a source in Resolve.

#include "NDIReceiverPlugin.h"
#include "ofxsProcessing.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>

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
// 3D LUT Engine
// ============================================================

struct LUT3D
{
    std::vector<float> data;   // R,G,B interleaved — size^3 * 3 floats
    int size = 0;              // entries per axis
    std::string filePath;
    FILETIME lastWriteTime{};

    bool empty() const { return size == 0 || data.empty(); }

    bool loadCube(const std::string& path)
    {
        std::ifstream f(path);
        if (!f.is_open()) return false;

        std::vector<float> newData;
        int newSize = 0;
        std::string line;

        while (std::getline(f, line))
        {
            if (line.empty() || line[0] == '#') continue;

            if (line.rfind("LUT_3D_SIZE", 0) == 0)
            {
                std::istringstream ss(line.substr(11));
                ss >> newSize;
                if (newSize < 2 || newSize > 256) return false;
                newData.reserve((size_t)newSize * newSize * newSize * 3);
                continue;
            }

            if (line.rfind("TITLE", 0) == 0 ||
                line.rfind("DOMAIN_MIN", 0) == 0 ||
                line.rfind("DOMAIN_MAX", 0) == 0 ||
                line.rfind("LUT_1D_SIZE", 0) == 0)
                continue;

            if (newSize == 0) continue;

            float r, g, b;
            std::istringstream ss(line);
            if (ss >> r >> g >> b)
            {
                newData.push_back(r);
                newData.push_back(g);
                newData.push_back(b);
            }
        }

        size_t expected = (size_t)newSize * newSize * newSize * 3;
        if (newData.size() != expected) return false;

        data = std::move(newData);
        size = newSize;
        filePath = path;
        return true;
    }

    // Trilinear interpolation lookup
    inline void apply(float& r, float& g, float& b) const
    {
        if (empty()) return;

        float maxIdx = (float)(size - 1);
        float rr = std::max(0.0f, std::min(r * maxIdx, maxIdx));
        float gg = std::max(0.0f, std::min(g * maxIdx, maxIdx));
        float bb = std::max(0.0f, std::min(b * maxIdx, maxIdx));

        int r0 = (int)rr, g0 = (int)gg, b0 = (int)bb;
        int r1 = std::min(r0 + 1, size - 1);
        int g1 = std::min(g0 + 1, size - 1);
        int b1 = std::min(b0 + 1, size - 1);

        float fr = rr - r0, fg = gg - g0, fb = bb - b0;

        // .cube ordering: R inner, G middle, B outer
        auto idx = [&](int ri, int gi, int bi) -> size_t {
            return ((size_t)bi * size * size + (size_t)gi * size + ri) * 3;
        };

        // 8 corner samples
        const float* c000 = &data[idx(r0, g0, b0)];
        const float* c100 = &data[idx(r1, g0, b0)];
        const float* c010 = &data[idx(r0, g1, b0)];
        const float* c110 = &data[idx(r1, g1, b0)];
        const float* c001 = &data[idx(r0, g0, b1)];
        const float* c101 = &data[idx(r1, g0, b1)];
        const float* c011 = &data[idx(r0, g1, b1)];
        const float* c111 = &data[idx(r1, g1, b1)];

        for (int ch = 0; ch < 3; ++ch)
        {
            float c00 = c000[ch] * (1 - fr) + c100[ch] * fr;
            float c10 = c010[ch] * (1 - fr) + c110[ch] * fr;
            float c01 = c001[ch] * (1 - fr) + c101[ch] * fr;
            float c11 = c011[ch] * (1 - fr) + c111[ch] * fr;

            float c0 = c00 * (1 - fg) + c10 * fg;
            float c1 = c01 * (1 - fg) + c11 * fg;

            float val = c0 * (1 - fb) + c1 * fb;
            if (ch == 0) r = val;
            else if (ch == 1) g = val;
            else b = val;
        }
    }
};

// Global LUT state — shared across render calls, protected by mutex
static LUT3D g_LUT;
static std::mutex g_LUTMutex;

static bool CheckFileChanged(const std::string& path, FILETIME& lastTime)
{
    if (path.empty()) return false;
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    FILETIME ft{};
    GetFileTime(hFile, nullptr, nullptr, &ft);
    CloseHandle(hFile);
    if (ft.dwHighDateTime != lastTime.dwHighDateTime || ft.dwLowDateTime != lastTime.dwLowDateTime)
    {
        lastTime = ft;
        return true;
    }
    return false;
}

static void TryReloadLUT(const std::string& path)
{
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(g_LUTMutex);
    if (path != g_LUT.filePath || CheckFileChanged(path, g_LUT.lastWriteTime))
    {
        LUT3D newLut;
        if (newLut.loadCube(path))
        {
            newLut.filePath = path;
            newLut.lastWriteTime = g_LUT.lastWriteTime;
            g_LUT = std::move(newLut);
            fprintf(stderr, "[NDIReceiver] LUT loaded: %s (size=%d)\n", path.c_str(), g_LUT.size);
        }
        else
        {
            fprintf(stderr, "[NDIReceiver] Failed to load LUT: %s\n", path.c_str());
        }
    }
}

// ============================================================
// Plugin identity
// ============================================================

static const char* kPluginName = "NDI Receiver + AI Grade (UE Bridge)";
static const char* kPluginGrouping = "LookScopes";
static const char* kPluginDescription =
    "Receives NDI video from Unreal Engine and optionally applies a .cube 3D LUT.\n"
    "AI color grading: point LUT Path to the MCP-generated .cube file for live preview.";
static const char* kPluginIdentifier = "com.kuoyu.lookscopes.ndireceiver";
static const int kPluginVersionMajor = 1;
static const int kPluginVersionMinor = 1;

static const char* kParamSourceName = "sourceName";
static const char* kParamRefresh = "refreshBtn";
static const char* kParamRefreshCounter = "refreshCounter";
static const char* kParamLutPath = "lutPath";
static const char* kParamLutEnable = "lutEnable";
static const char* kParamLutReload = "lutReloadBtn";

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
    void setLUTEnabled(bool e) { m_LutEnabled = e; }

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

        float scaleX = (float)dstW / srcW;
        float scaleY = (float)dstH / srcH;
        float scale = std::min(scaleX, scaleY);
        int fitW = (int)(srcW * scale);
        int fitH = (int)(srcH * scale);
        int offsetX = (dstW - fitW) / 2;
        int offsetY = (dstH - fitH) / 2;

        // Snapshot the LUT under its own lock to avoid holding both locks
        bool useLut = false;
        LUT3D lutCopy;
        if (m_LutEnabled)
        {
            std::lock_guard<std::mutex> lutLock(g_LUTMutex);
            if (!g_LUT.empty())
            {
                lutCopy = g_LUT;
                useLut = true;
            }
        }

        for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y)
        {
            float* dstPix = static_cast<float*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));
            if (!dstPix) continue;

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
                    float r = srcData[srcIdx + 0] / 255.0f;
                    float g = srcData[srcIdx + 1] / 255.0f;
                    float b = srcData[srcIdx + 2] / 255.0f;

                    if (useLut)
                        lutCopy.apply(r, g, b);

                    dstPix[0] = r;
                    dstPix[1] = g;
                    dstPix[2] = b;
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
    bool m_LutEnabled = false;
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
        m_LutPath = fetchStringParam(kParamLutPath);
        m_LutEnable = fetchBooleanParam(kParamLutEnable);

        g_InstanceCount++;

        if (!g_NDIState)
            g_NDIState = new NDIReceiverState();

        std::string name;
        m_SourceName->getValue(name);
        if (!name.empty())
            g_NDIState->targetSourceName = name;

        g_NDIState->StartReceiving();

        // Load LUT if path is already set
        std::string lutPath;
        m_LutPath->getValue(lutPath);
        if (!lutPath.empty())
            TryReloadLUT(lutPath);
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

        // Auto-reload: check if LUT file changed on disk
        std::string lutPath;
        m_LutPath->getValue(lutPath);
        if (!lutPath.empty())
            TryReloadLUT(lutPath);

        bool lutEnabled = false;
        m_LutEnable->getValue(lutEnabled);

        OfxRectI bounds = dst->getBounds();
        int dstW = bounds.x2 - bounds.x1;
        int dstH = bounds.y2 - bounds.y1;

        NDIFrameWriter processor(*this);
        processor.setDstImg(dst.get());
        processor.setRenderWindow(p_Args.renderWindow);
        processor.setOutputBounds(dstW, dstH);
        processor.setLUTEnabled(lutEnabled);
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
        else if (p_ParamName == kParamLutPath)
        {
            std::string path;
            m_LutPath->getValue(path);
            if (!path.empty())
            {
                TryReloadLUT(path);
                // Trigger visual refresh
                if (m_RefreshCounter)
                {
                    int val = 0;
                    m_RefreshCounter->getValue(val);
                    m_RefreshCounter->setValue(val + 1);
                }
            }
        }
        else if (p_ParamName == kParamLutReload)
        {
            std::string path;
            m_LutPath->getValue(path);
            if (!path.empty())
            {
                // Force reload by resetting lastWriteTime
                {
                    std::lock_guard<std::mutex> lock(g_LUTMutex);
                    g_LUT.lastWriteTime = {};
                }
                TryReloadLUT(path);
                if (m_RefreshCounter)
                {
                    int val = 0;
                    m_RefreshCounter->getValue(val);
                    m_RefreshCounter->setValue(val + 1);
                }
            }
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
    OFX::StringParam* m_LutPath = nullptr;
    OFX::BooleanParam* m_LutEnable = nullptr;
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

    // --- NDI ---
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

    // --- AI LUT Grading ---
    OFX::BooleanParamDescriptor* lutEnable = p_Desc.defineBooleanParam(kParamLutEnable);
    lutEnable->setLabel("Enable AI LUT");
    lutEnable->setHint("Apply the .cube LUT to the incoming NDI stream");
    lutEnable->setDefault(true);
    lutEnable->setAnimates(false);
    if (page) page->addChild(*lutEnable);

    OFX::StringParamDescriptor* lutPath = p_Desc.defineStringParam(kParamLutPath);
    lutPath->setLabel("LUT Path (.cube)");
    lutPath->setHint("Absolute path to a .cube 3D LUT file. Auto-reloads when file changes on disk.");
    lutPath->setDefault("I:\\Aura\\Plugins\\LookScopes\\DaVinciPlugin\\MCPServer\\output\\ai_grade.cube");
    lutPath->setStringType(OFX::eStringTypeFilePath);
    lutPath->setAnimates(false);
    if (page) page->addChild(*lutPath);

    OFX::PushButtonParamDescriptor* lutReload = p_Desc.definePushButtonParam(kParamLutReload);
    lutReload->setLabel("Reload LUT");
    lutReload->setHint("Force reload the LUT file from disk");
    if (page) page->addChild(*lutReload);
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
