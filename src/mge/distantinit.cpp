
#include "proxydx/d3d8header.h"
#include "support/log.h"
#include "configuration.h"
#include "distantland.h"
#include "distantshader.h"
#include "dlformat.h"
#include "compositetelemetry.h"   // CompositeTelemetry::write (task 11.2; Req 8.1/8.2/8.4)
#include "postshaders.h"
#include "morrowindbsa.h"
#include "mwbridge.h"
#include "mgeversion.h"
#include "statusoverlay.h"
#include "ipc/dlshare.h"
#include <memory>
#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <vector>

#ifdef MGE_RTX
#include "remix_api_test.h"      // RemixAPITest::getInterface (live Remix interface)
#include "retained_world.h"
#include "dlcull_config.h"       // dlRegisterDistantTexture — per-texture distant suppress
#endif



using std::string;
using std::string_view;
using std::vector;

bool DistantLand::ready = false;
bool DistantLand::isRenderCached = false;
bool DistantLand::isPPLActive = false;
int DistantLand::numWaterVerts, DistantLand::numWaterTris;

IDirect3DDevice9* DistantLand::device;
ID3DXEffect* DistantLand::effect;
ID3DXEffect* DistantLand::effectShadow;
ID3DXEffect* DistantLand::effectDepth;
ID3DXEffectPool* DistantLand::effectPool;
IDirect3DVertexDeclaration9* DistantLand::LandDecl;
IDirect3DVertexDeclaration9* DistantLand::StaticDecl;
IDirect3DVertexDeclaration9* DistantLand::WaterDecl;
IDirect3DVertexDeclaration9* DistantLand::GrassDecl;

VendorSpecificRendering DistantLand::vsr;

IPC::Client DistantLand::ipcClient;
std::vector<DistantLand::DynamicVisGroup> DistantLand::dynamicVisGroups;
void* DistantLand::lastDistantVisCell;
bool DistantLand::isDistantLandLoaded = false;

VisibleSet<StlVector> DistantLand::visLand;
VisibleSet<StlVector> DistantLand::visDistant;
VisibleSet<StlVector> DistantLand::visGrass;

VisibleSet<IpcClientVector> DistantLand::visLandShared;
VisibleSet<IpcClientVector> DistantLand::visDistantShared;
VisibleSet<IpcClientVector> DistantLand::visGrassShared;
VisibleSet<IpcClientVector> DistantLand::visExtraShared;
IPC::VecView<IPC::DynVisFlag> DistantLand::dynVisFlagsShared;

IPC::VecId DistantLand::visLandSharedId = IPC::InvalidVector;
IPC::VecId DistantLand::visDistantSharedId = IPC::InvalidVector;
IPC::VecId DistantLand::visGrassSharedId = IPC::InvalidVector;
IPC::VecId DistantLand::visExtraSharedId = IPC::InvalidVector;
IPC::VecId DistantLand::dynVisFlagsSharedId = IPC::InvalidVector;

vector<DistantLand::RecordedState> DistantLand::recordMW;
vector<DistantLand::RecordedState> DistantLand::recordSky;
vector< std::pair<const RenderMesh*, int> > DistantLand::batchedGrass;

IDirect3DTexture9* DistantLand::texWorldColour, *DistantLand::texWorldNormals, *DistantLand::texWorldDetail;
// NEW (additive; Architecture B). Single IPC-client-owned resident composite cache shared by
// the FFP and shader distant-land binders. Default-constructed empty/inert: until task 8.5
// wires init()/admit()/evictNotVisible() on THIS instance, every lookup() misses and the
// Texture_Binder falls back to texWorldColour (stock Single_Atlas_Path, Req 6.1/6.2).
ResidentCompositeCache DistantLand::compositeCache;
// NEW (additive; Architecture B client streaming, task 8.5). New_Format gate + the three
// StreamVisibleComposites shared-vector channels and their client views. All default to the
// inert Old_Format state (hasCompositeSet=false, InvalidVector ids), so until initLandscapeClient
// activates them the reconcile path is a no-op and the binder takes the Single_Atlas_Path.
bool DistantLand::hasCompositeSet = false;
IPC::VecId DistantLand::compositeDeltaSharedId = IPC::InvalidVector;
IPC::VecId DistantLand::compositeHeadersSharedId = IPC::InvalidVector;
IPC::VecId DistantLand::compositeBytesSharedId = IPC::InvalidVector;
IPC::VecView<CompositeCellId> DistantLand::compositeDeltaView;
IPC::VecView<CompositeChunkMsg> DistantLand::compositeHeadersView;
IPC::VecView<std::uint8_t> DistantLand::compositeBytesView;
std::uint32_t DistantLand::compositeDefaultEdge = 0;

namespace {

struct CompositeInFlightBatch {
    bool active = false;
    bool timeoutReported = false;
    bool waitErrorReported = false;
    std::uint32_t ageFrames = 0;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t channelGeneration = 0;
    std::uint64_t cacheGeneration = 0;
    std::vector<CellId> cells;
};

std::uint64_t s_compositeChannelGeneration = 0;
constexpr std::uint32_t kMaxCompositeStaleVisibilityFrames = 4;
CompositeRequestController s_compositeRequestController;
CompositeTelemetry::Reporter s_compositeTelemetryReporter;
VisibleCellSet s_previousCompositeVisible;
bool s_previousCompositeVisibleValid = false;
std::uint64_t s_observedCompositeCacheGeneration = 0;
std::uint64_t s_observedCompositeSessionGeneration = 0;
std::uint64_t s_observedCompositeChannelGeneration = 0;
CompositeInFlightBatch s_compositeInFlight;
bool s_compositeTransportFailed = false;
bool s_refreshDistantVisibility = true;
std::uint32_t s_distantVisibilityStaleFrames = 0;
const void* s_distantVisibilityCell = nullptr;
bool s_distantVisibilityCellValid = false;
CompositeTelemetry::FrameSample s_compositePreflightSample;
CompositeTelemetry::FrameSample s_preparedCompositeSample;
std::vector<CellId> s_preparedCompositeCells;
bool s_preparedCompositeVisibleSetChanged = false;
bool s_preparedCompositeFrameReady = false;

void clearCompositeInFlight() {
    s_compositeInFlight.active = false;
    s_compositeInFlight.timeoutReported = false;
    s_compositeInFlight.waitErrorReported = false;
    s_compositeInFlight.ageFrames = 0;
    s_compositeInFlight.sessionGeneration = 0;
    s_compositeInFlight.channelGeneration = 0;
    s_compositeInFlight.cacheGeneration = 0;
    s_compositeInFlight.cells.clear();
}

void resetCompositeControllerState() {
    s_compositeRequestController.reset();
    s_compositeTelemetryReporter.reset();
    s_previousCompositeVisible.clear();
    s_previousCompositeVisibleValid = false;
    s_distantVisibilityCell = nullptr;
    s_distantVisibilityCellValid = false;
    s_preparedCompositeCells.clear();
    s_preparedCompositeFrameReady = false;
}

std::uint64_t compositeElapsedMicros(const LARGE_INTEGER& start) {
    static const LARGE_INTEGER frequency = []() {
        LARGE_INTEGER value = {};
        QueryPerformanceFrequency(&value);
        return value;
    }();

    LARGE_INTEGER end = {};
    QueryPerformanceCounter(&end);
    if (frequency.QuadPart <= 0 || end.QuadPart <= start.QuadPart) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        ((end.QuadPart - start.QuadPart) * 1000000ll) / frequency.QuadPart);
}

}  // namespace

IDirect3DTexture9* DistantLand::texDepthFrame;
IDirect3DSurface9* DistantLand::surfDepthDepth;
IDirect3DTexture9* DistantLand::texDistantBlend;
IDirect3DTexture9* DistantLand::texReflection;
IDirect3DSurface9* DistantLand::surfReflectionZ;
IDirect3DVolumeTexture9* DistantLand::texWater;
IDirect3DVertexBuffer9* DistantLand::vbWater;
IDirect3DIndexBuffer9* DistantLand::ibWater;
IDirect3DVertexBuffer9* DistantLand::vbGrassInstances;

#ifdef MGE_RTX
IDirect3DVertexBuffer9* DistantLand::vbWaterFFP = nullptr;
IDirect3DVertexDeclaration9* DistantLand::waterFFPDecl = nullptr;
IDirect3DTexture9* DistantLand::cachedWaterTex = nullptr;
#endif

IDirect3DTexture9* DistantLand::texRain;
IDirect3DTexture9* DistantLand::texRipples;
IDirect3DTexture9* DistantLand::texRippleBuffer;
IDirect3DSurface9* DistantLand::surfRain;
IDirect3DSurface9* DistantLand::surfRipples;
IDirect3DSurface9* DistantLand::surfRippleBuffer;
IDirect3DVertexBuffer9* DistantLand::vbWaveSim;

IDirect3DTexture9* DistantLand::texShadow;
IDirect3DTexture9* DistantLand::texSoftShadow;
IDirect3DSurface9* DistantLand::surfShadowZ;
IDirect3DVertexBuffer9* DistantLand::vbFullFrame;
IDirect3DVertexBuffer9* DistantLand::vbClipCube;

D3DXMATRIX DistantLand::mwView, DistantLand::mwProj;
D3DXMATRIX DistantLand::smView[2], DistantLand::smProj[2];
D3DXMATRIX DistantLand::smViewproj[2];
D3DXVECTOR4 DistantLand::eyeVec, DistantLand::eyePos;
D3DXVECTOR4 DistantLand::sunVec, DistantLand::sunPos;
float DistantLand::sunVis;
RGBVECTOR DistantLand::sunCol, DistantLand::sunAmb, DistantLand::ambCol;
RGBVECTOR DistantLand::nearFogCol, DistantLand::horizonCol;
RGBVECTOR DistantLand::atmOutscatter(0.07, 0.36, 0.76);
RGBVECTOR DistantLand::atmInscatter(0.25, 0.38, 0.48);
D3DXVECTOR4 DistantLand::atmSkylightScatter(0.4456, 0.6194, 1.0, 0.44);
float DistantLand::fogStart, DistantLand::fogEnd;
float DistantLand::fogExpStart, DistantLand::fogExpDivisor;
float DistantLand::fogNearStart, DistantLand::fogNearEnd;
float DistantLand::nearViewRange;
float DistantLand::windScaling, DistantLand::niceWeather;
float DistantLand::lightSunMult, DistantLand::lightAmbMult;

D3DXHANDLE DistantLand::ehRcpRes;
D3DXHANDLE DistantLand::ehShadowRcpRes;
D3DXHANDLE DistantLand::ehWorld;
D3DXHANDLE DistantLand::ehView;
D3DXHANDLE DistantLand::ehProj;
D3DXHANDLE DistantLand::ehShadowViewproj;
D3DXHANDLE DistantLand::ehVertexBlendState;
D3DXHANDLE DistantLand::ehVertexBlendPalette;
D3DXHANDLE DistantLand::ehAlphaRef;
D3DXHANDLE DistantLand::ehMaterialAlpha;
D3DXHANDLE DistantLand::ehHasAlpha;
D3DXHANDLE DistantLand::ehHasBones;
D3DXHANDLE DistantLand::ehHasVCol;
D3DXHANDLE DistantLand::ehTex0;
D3DXHANDLE DistantLand::ehTex1;
D3DXHANDLE DistantLand::ehTex2;
D3DXHANDLE DistantLand::ehTex3;
D3DXHANDLE DistantLand::ehTex4;
D3DXHANDLE DistantLand::ehTex5;
D3DXHANDLE DistantLand::ehEyePos;
D3DXHANDLE DistantLand::ehFootPos;
D3DXHANDLE DistantLand::ehSunCol;
D3DXHANDLE DistantLand::ehSunAmb;
D3DXHANDLE DistantLand::ehSunVec;
D3DXHANDLE DistantLand::ehSunVecView;
D3DXHANDLE DistantLand::ehSunPos;
D3DXHANDLE DistantLand::ehSunVis;
D3DXHANDLE DistantLand::ehOutscatter;
D3DXHANDLE DistantLand::ehInscatter;
D3DXHANDLE DistantLand::ehSkyScatterFar;
D3DXHANDLE DistantLand::ehSkyCol;
D3DXHANDLE DistantLand::ehFogColNear;
D3DXHANDLE DistantLand::ehFogColFar;
D3DXHANDLE DistantLand::ehFogStart;
D3DXHANDLE DistantLand::ehFogRange;
D3DXHANDLE DistantLand::ehFogNearStart;
D3DXHANDLE DistantLand::ehFogNearRange;
D3DXHANDLE DistantLand::ehNearViewRange;
D3DXHANDLE DistantLand::ehWindVec;
D3DXHANDLE DistantLand::ehNiceWeather;
D3DXHANDLE DistantLand::ehTime;
D3DXHANDLE DistantLand::ehRippleOrigin;
D3DXHANDLE DistantLand::ehWaveHeight;

std::function<void(IDirect3DSurface9*)> DistantLand::captureScreenHandler = nullptr;
bool DistantLand::captureScreenWithUI;


struct MeshResources {
    IDirect3DVertexBuffer9* vb;
    IDirect3DIndexBuffer9* ib;
    IDirect3DTexture9* tex;

    MeshResources(IDirect3DVertexBuffer9* _vb, IDirect3DIndexBuffer9* _ib, IDirect3DTexture9* _tex) : vb(_vb), ib(_ib), tex(_tex) {}
};
static vector<MeshResources> meshCollectionLand;
static vector<MeshResources> meshCollectionStatics;




// Water plane vertex declaration
const D3DVERTEXELEMENT9 WaterElem[] = {
    {0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    D3DDECL_END()
};

// World mesh vertex declaration
const D3DVERTEXELEMENT9 LandElem[] = {
    {0, 0,  D3DDECLTYPE_FLOAT3,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 12, D3DDECLTYPE_SHORT2N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    {0, 16, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
    D3DDECL_END()
};

// Distant static vertex declaration
const D3DVERTEXELEMENT9 StaticElem[] = {
    {0, 0,  D3DDECLTYPE_FLOAT16_4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 8,  D3DDECLTYPE_UBYTE4N,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
    {0, 12, D3DDECLTYPE_D3DCOLOR,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0},
    {0, 16, D3DDECLTYPE_FLOAT16_2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    D3DDECL_END()
};

// Instanced grass vertex declaration
const D3DVERTEXELEMENT9 GrassElem[] = {
    {0, 0,  D3DDECLTYPE_FLOAT16_4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 8,  D3DDECLTYPE_UBYTE4N,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
    {0, 12, D3DDECLTYPE_D3DCOLOR,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0},
    {0, 16, D3DDECLTYPE_FLOAT16_2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    {1, 0,  D3DDECLTYPE_FLOAT4,    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
    {1, 16, D3DDECLTYPE_FLOAT4,    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
    {1, 32, D3DDECLTYPE_FLOAT4,    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3},
    D3DDECL_END()
};



bool DistantLand::init() {
    if (ready) {
        return true;
    }
    if (!device) {
        return false;
    }

    LOG::logline(">> Starting Distant Land init");
    vsr.init(device);
    BSA::init();

    if (Configuration.UseSharedMemory && !initIpc()) {
        return false;
    }

    if (!initShader()) {
        return false;
    }

    if (!FixedFunctionShader::init(device, effectPool)) {
        return false;
    }

    if (!PostShaders::init(device)) {
        return false;
    }

    if (!initDepth()) {
        return false;
    }

    if (!initShadow()) {
        return false;
    }

    if (!initWater()) {
        return false;
    }

    if (!initLandscape()) {
        return false;
    }

    if (!initDistantStaticsClient()) {
        return false;
    }

#ifdef MGE_RTX
    if (Configuration.UseSharedMemory) {
        RetainedWorld::initialize(ipcClient, device);
    }
#endif

    if (!initGrass()) {
        return false;
    }

    MWBridge::get()->patchResolveDuringInit(&resolveDynamicVisGroups);

    LOG::logline("<< Completed Distant Land init");
    ready = true;
    isRenderCached = false;
    return true;
}

bool DistantLand::initIpc() {
    if (!IPC::initImports()) {
        LOG::logline("!! Disabling shared memory because required memory mapping APIs are not available");
        Configuration.UseSharedMemory = false;
        // we'll return success so we can continue on the non-IPC path
        return true;
    }

    if (!ipcClient.startServer("mgeHost64.exe")) {
        return false;
    }

    // The prior host is now confirmed stopped and the client has started a
    // fresh IPC session. Detach its vectors and pending metadata before any
    // replacement vector can inherit an old completion.
    resetDynamicVisState();

    // allocate shared vectors that will be reused for the duration of the program
    auto maybeLandVec = ipcClient.allocVecBlocking<RenderMesh>(1, 200000, 1);
    if (!maybeLandVec.has_value()) {
        return false;
    }
    auto& landVec = maybeLandVec.value();
    visLandSharedId = landVec.id();
    visLandShared.SetVector((IpcClientVector(landVec)));

    auto maybeDistantVec = ipcClient.allocVecBlocking<RenderMesh>(1, 200000, 1);
    if (!maybeDistantVec.has_value()) {
        return false;
    }
    auto& distantVec = maybeDistantVec.value();
    visDistantSharedId = distantVec.id();
    visDistantShared.SetVector((IpcClientVector(distantVec)));

    // we force the maximum number of grass elements to always be resident in memory. this currently equates to 704 KiB of
    // grass memory compared to the standard window size of 64 KiB, but it allows us to avoid a bunch of copying when
    // rendering grass.
    auto maybeGrassVec = ipcClient.allocVecBlocking<RenderMesh>(MaxGrassElements, MaxGrassElements, MaxGrassElements);
    if (!maybeGrassVec.has_value()) {
        return false;
    }
    auto& grassVec = maybeGrassVec.value();
    visGrassSharedId = grassVec.id();
    visGrassShared.SetVector((IpcClientVector(grassVec)));

    auto maybeExtraVec = ipcClient.allocVecBlocking<RenderMesh>(1, 200000, 1);
    if (!maybeExtraVec.has_value()) {
        return false;
    }
    auto& extraVec = maybeExtraVec.value();
    visExtraSharedId = extraVec.id();
    visExtraShared.SetVector((IpcClientVector(extraVec)));

    auto maybeDynVisVec = ipcClient.allocVecBlocking<IPC::DynVisFlag>(1, 1000, 1);
    if (!maybeDynVisVec.has_value()) {
        return false;
    }
    auto& dynVisVec = maybeDynVisVec.value();
    dynVisFlagsSharedId = dynVisVec.id();
    dynVisFlagsShared = dynVisVec;

    return true;
}

bool DistantLand::reloadShaders() {
    LOG::logline(">> Distant Land reloading");
    if (!initShader()) {
        return false;
    }

    FixedFunctionShader::release();
    if (!FixedFunctionShader::init(device, effectPool)) {
        return false;
    }

    return true;
}

static const string shaderCoreModPrefix = "XE Mod";
static const string pathCoreShaders = "Data Files\\shaders\\core\\";
static const string pathCoreMods = "Data Files\\shaders\\core-mods\\";

struct CoreModInclude : public ID3DXInclude {
    vector<string> modsFound;
    std::optional<string> testSingleMod;

    STDMETHOD(Open)(D3DXINCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID *ppData, UINT *pBytes) {
        string filename(pFileName), shaderPath = filename;
        bool isMod = false;
        char *buffer = nullptr;
        HANDLE h;

        // Check if it uses the core shader path prefix, if not, add the prefix
        if (filename.compare(0, pathCoreShaders.length(), pathCoreShaders) != 0) {
            shaderPath = pathCoreShaders + filename;
        }

        if (!testSingleMod) {
            // Check if this file is moddable, and if a core-mod exists, use its path
            if (filename.substr(0, shaderCoreModPrefix.length()) == shaderCoreModPrefix) {
                string modShaderPath = pathCoreMods + filename;
                if (GetFileAttributes(modShaderPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    isMod = true;
                    shaderPath = modShaderPath;
                }
            }
        }
        else {
            // Only load the specified mod for testing, ignoring others
            if (testSingleMod.value() == filename) {
                isMod = true;
                shaderPath = pathCoreMods + filename;
            }
        }

        // Read file contents for the effect compiler
        h = CreateFile(shaderPath.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD bytesRead, bufferSize = GetFileSize(h, NULL);

            buffer = new char[bufferSize];
            ReadFile(h, buffer, bufferSize, &bytesRead, 0);
            CloseHandle(h);

            if (isMod) {
                modsFound.push_back(filename);
            }

            *ppData = buffer;
            *pBytes = bufferSize;
            return S_OK;
        }
        return E_FAIL;
    }

    STDMETHOD(Close)(LPCVOID pData) {
        char *buffer = (char*)(pData);
        delete [] buffer;
        return S_OK;
    }
};

static void logShaderError(ID3DXBuffer* errors) {
    if (errors) {
        LOG::write("!! Shader compile errors:\n");
        LOG::write(reinterpret_cast<const char*>(errors->GetBufferPointer()));
        LOG::write("\n");
        errors->Release();
    }
    LOG::flush();
}

static bool createCoreEffectWithMods(const char *name, IDirect3DDevice9* device, vector<D3DXMACRO>& features, ID3DXEffectPool *effectPool, ID3DXEffect **pEffect, bool reportMods) {
    string path = pathCoreShaders + name;
    ID3DXBuffer* errors;
    CoreModInclude includer;
    HRESULT hr;

    // Attempt to compile with core mods first
    hr = D3DXCreateEffectFromFile(device, path.c_str(), &*features.begin(), &includer, D3DXSHADER_OPTIMIZATION_LEVEL3|D3DXFX_LARGEADDRESSAWARE, effectPool, pEffect, &errors);
    if (hr == D3D_OK) {
        if (reportMods) {
            for (auto& m : includer.modsFound) {
                LOG::logline("-- Using core mod %s", m.c_str());
            }
        }
        return true;
    } else {
        LOG::logline("!! Core shader %s failed to compile with core-mods. All core-mods are disabled. Checking for errors...", name);
        StatusOverlay::setStatus("Shader core mod error. Core mods are disabled for this session. Check mgeXE.log for error details.", StatusOverlay::PriorityError);
        if (errors) {
            errors->Release();
        }
    }

    // Individually test each core mod for errors
    auto modsFound = includer.modsFound;
    for(const auto& mod : modsFound) {
        ID3DXEffect *testEffect;
        includer.testSingleMod = mod;

        hr = D3DXCreateEffectFromFile(device, path.c_str(), &*features.begin(), &includer, D3DXSHADER_OPTIMIZATION_LEVEL0|D3DXFX_LARGEADDRESSAWARE, effectPool, &testEffect, &errors);
        if (hr == D3D_OK) {
            testEffect->Release();
        }
        else {
            LOG::logline("!! Shader core mod %s%s failed to compile. Disable or remove it until it is fixed.", pathCoreMods.c_str(), mod.c_str());
            logShaderError(errors);
        }
    }

    // Fallback to compiling without core mods
    hr = D3DXCreateEffectFromFile(device, path.c_str(), &*features.begin(), 0, D3DXSHADER_OPTIMIZATION_LEVEL3|D3DXFX_LARGEADDRESSAWARE, effectPool, pEffect, &errors);
    if (hr == D3D_OK) {
        return true;
    } else {
        LOG::logline("!! Core shader %s failed to compile. Do not replace core shaders. Reinstall MGE XE.", name);
        logShaderError(errors);
    }
    return false;
}

static const D3DXMACRO macroExpFog = { "USE_EXPFOG", "" };
static const D3DXMACRO macroScattering = { "USE_SCATTERING", "" };
static const D3DXMACRO macroFilterReflection = { "FILTER_WATER_REFLECTION", "" };
static const D3DXMACRO macroDynamicRipples = { "DYNAMIC_RIPPLES", "" };
static const D3DXMACRO macroTerminator = { 0, 0 };

bool DistantLand::initShader() {
    vector<D3DXMACRO> features;
    HRESULT hr;

    // Disable exponential fog if distant land is initially off
    if (~Configuration.MGEFlags & USE_DISTANT_LAND) {
        Configuration.MGEFlags &= ~(EXP_FOG | USE_ATM_SCATTER);
    }

    // Set shader defines corresponding to required features
    if (Configuration.MGEFlags & EXP_FOG) {
        features.push_back(macroExpFog);

        // Requires exp. fog
        if (Configuration.MGEFlags & USE_ATM_SCATTER) {
            features.push_back(macroScattering);
        }
    }
    if (Configuration.MGEFlags & BLUR_REFLECTIONS) {
        features.push_back(macroFilterReflection);
    }
    if (Configuration.MGEFlags & DYNAMIC_RIPPLES) {
        features.push_back(macroDynamicRipples);
    }
    features.push_back(macroTerminator);

    if (!effectPool) {
        hr = D3DXCreateEffectPool(&effectPool);
        if (hr != D3D_OK) {
            LOG::logline("!! Effect pool creation failure");
            return false;
        }
    }

    if (!createCoreEffectWithMods("XE Main.fx", device, features, effectPool, &effect, true)) {
        return false;
    }

    ehRcpRes = effect->GetParameterByName(0, "rcpRes");
    ehShadowRcpRes = effect->GetParameterByName(0, "shadowRcpRes");
    ehWorld = effect->GetParameterByName(0, "world");
    ehView = effect->GetParameterByName(0, "view");
    ehProj = effect->GetParameterByName(0, "proj");
    ehShadowViewproj = effect->GetParameterByName(0, "shadowViewProj");
    ehVertexBlendState = effect->GetParameterByName(0, "vertexBlendState");
    ehVertexBlendPalette = effect->GetParameterByName(0, "vertexBlendPalette");
    ehAlphaRef = effect->GetParameterByName(0, "alphaRef");
    ehMaterialAlpha = effect->GetParameterByName(0, "materialAlpha");
    ehHasAlpha = effect->GetParameterByName(0, "hasAlpha");
    ehHasBones = effect->GetParameterByName(0, "hasBones");
    ehHasVCol = effect->GetParameterByName(0, "hasVCol");
    ehTex0 = effect->GetParameterByName(0, "tex0");
    ehTex1 = effect->GetParameterByName(0, "tex1");
    ehTex2 = effect->GetParameterByName(0, "tex2");
    ehTex3 = effect->GetParameterByName(0, "tex3");
    ehEyePos = effect->GetParameterByName(0, "eyePos");
    ehFootPos = effect->GetParameterByName(0, "footPos");
    ehSunCol = effect->GetParameterByName(0, "sunCol");
    ehSunAmb = effect->GetParameterByName(0, "sunAmb");
    ehSunVec = effect->GetParameterByName(0, "sunVec");
    ehSunVecView = effect->GetParameterByName(0, "sunVecView");
    ehSunPos = effect->GetParameterByName(0, "sunPos");
    ehSunVis = effect->GetParameterByName(0, "sunVis");
    ehSkyCol = effect->GetParameterByName(0, "skyCol");
    ehFogColNear = effect->GetParameterByName(0, "fogColNear");
    ehFogColFar = effect->GetParameterByName(0, "fogColFar");
    ehFogStart = effect->GetParameterByName(0, "fogStart");
    ehFogRange = effect->GetParameterByName(0, "fogRange");
    ehFogNearStart = effect->GetParameterByName(0, "nearFogStart");
    ehFogNearRange = effect->GetParameterByName(0, "nearFogRange");
    ehNearViewRange = effect->GetParameterByName(0, "nearViewRange");
    ehWindVec = effect->GetParameterByName(0, "windVec");
    ehNiceWeather = effect->GetParameterByName(0, "niceWeather");
    ehTime = effect->GetParameterByName(0, "time");

    D3DVIEWPORT9 vp;
    device->GetViewport(&vp);
    float rcpres[2] = { 1.0f / vp.Width, 1.0f / vp.Height };
    effect->SetFloatArray(ehRcpRes, rcpres, 2);
    effect->SetFloat(ehShadowRcpRes, 1.0f / Configuration.DL.ShadowResolution);

    if (!createCoreEffectWithMods("XE Shadowmap.fx", device, features, effectPool, &effectShadow, false)) {
        return false;
    }
    if (!createCoreEffectWithMods("XE Depth.fx", device, features, effectPool, &effectDepth, false)) {
        return false;
    }

    // Atmosphere scattering specific parameters
    if (Configuration.MGEFlags & USE_ATM_SCATTER) {

        ehOutscatter = effect->GetParameterByName(0, "outscatter");
        ehInscatter = effect->GetParameterByName(0, "inscatter");
        ehSkyScatterFar = effect->GetParameterByName(0, "skyScatterColFar");

        // Mark moon geometry for detection
        MWBridge::get()->markMoonNodes(kMoonTag);
    }
    else {
        ehOutscatter = 0;
        ehInscatter = 0;
        ehSkyScatterFar = 0;
    }

    // Dynamic ripples specific parameters
    if (Configuration.MGEFlags & DYNAMIC_RIPPLES) {
        ehTex4 = effect->GetParameterByName(0, "tex4");
        ehTex5 = effect->GetParameterByName(0, "tex5");
        ehRippleOrigin = effect->GetParameterByName(0, "rippleOrigin");
        ehWaveHeight = effect->GetParameterByName(0, "waveHeight");
    }

    return true;
}

bool DistantLand::initDepth() {
    HRESULT hr;
    D3DVIEWPORT9 vp;

    // Set up depth frame texture, requires its own z-buffer (my card fails to support INTZ/DF24)
    device->GetViewport(&vp);

    hr = device->CreateTexture(vp.Width, vp.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &texDepthFrame, NULL);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create depth frame render target");
        return false;
    }

    hr = device->CreateDepthStencilSurface(vp.Width, vp.Height, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, FALSE, &surfDepthDepth, NULL);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create depth target z-buffer");
        return false;
    }

    return true;
}

bool DistantLand::initWater() {
    HRESULT hr;
    const UINT reflRes = 1024;

    // Reflection render target
    hr = device->CreateTexture(reflRes, reflRes, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texReflection, NULL);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create reflection render target");
        return false;
    }

    // Reflection Z-buffer
    hr = device->CreateDepthStencilSurface(reflRes, reflRes, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &surfReflectionZ, NULL);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create reflection Z buffer");
        return false;
    }

    // Water normals and geometry
    const int resS = (Configuration.MGEFlags & DYNAMIC_RIPPLES) ? 150 : 16;
    const int resT = (Configuration.MGEFlags & DYNAMIC_RIPPLES) ? 120 : 15;
    numWaterVerts = resS * resT + 1;
    numWaterTris = 2 * resS * resT - resS;

    hr = D3DXCreateVolumeTextureFromFile(device, "Data Files\\textures\\MGE\\water_NRM.dds", &texWater);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to load water texture");
        return false;
    }
    hr = device->CreateVertexDeclaration(WaterElem, &WaterDecl);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create water decl");
        return false;
    }
    hr = device->CreateVertexBuffer(numWaterVerts * 12, 0, 0, D3DPOOL_MANAGED, &vbWater, 0);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create water verts");
        return false;
    }
    hr = device->CreateIndexBuffer(numWaterTris * 6, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ibWater, 0);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create water indices");
        return false;
    }

    // Build radial water mesh
    D3DXVECTOR3* v;
    vbWater->Lock(0, 0, (void**)&v, 0);

    // Water plane lies at water level - 1.0 (not -4.0, which is the fog transition)
    const float dS = float(6.28318530717958647692 / resS);
    int s, t;
    float r, w = -1.0f;

    *v++ = D3DXVECTOR3(0, 0, w);
    for (t = 0; t < resT; ++t) {
        if (Configuration.MGEFlags & DYNAMIC_RIPPLES) {
            // Higher mesh density near player
            // The mesh requires density past 8192 units to cover the z discontinuity at distant land
            r = float(t) / float(resT);
            r = 9600.0f * (0.9f * powf(r, 3) + 0.1f * r);
            // Extend last ring past horizon
            if ((t+1) == resT) {
                r = 500000.0f;
            }
        } else {
            r = 4096.0f * (1.0f + t * t);
        }

        for (s = 0; s < resS; ++s) {
            *v++ = D3DXVECTOR3(r * cos(dS * s), r * sin(dS * s), w);
        }
    }

    vbWater->Unlock();

    USHORT* i;
    ibWater->Lock(0, 0, (void**)&i, 0);

    // Centre triangles
    for (s = 0; s < resS; ++s) {
        *i++ = 0;
        *i++ = 1 + s;
        *i++ = 1 + (s+1) % resS;
    }
    // Rings
    for (t = 1; t < resT; ++t) {
        for (s = 0; s < resS; ++s) {
            USHORT tbase = 1 + resS*(t-1), s2 = (s+1) % resS;
            *i++ = tbase + s;
            *i++ = resS + tbase + s;
            *i++ = tbase + s2;
            *i++ = resS + tbase+ s;
            *i++ = resS + tbase + s2;
            *i++ = tbase + s2;
        }
    }

    ibWater->Unlock();

#ifdef MGE_RTX
    // Build a parallel UV-bearing VB for the FFP distant-water pass.
    // Same vertex positions as vbWater (so we can re-use ibWater) but with
    // an additional float2 UV0 derived from object-space XY at 1/256 scale.
    // E-man's translucent water material relies on UV gradients to drive
    // its time-animated flipbook; without per-vertex UVs the entire surface
    // would sample one texel and look frozen.
    struct WaterFFPElem { float x, y, z, u, v; };
    static const D3DVERTEXELEMENT9 waterFFPDeclElems[] = {
        { 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    hr = device->CreateVertexDeclaration(waterFFPDeclElems, &waterFFPDecl);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create FFP water decl");
        return false;
    }
    hr = device->CreateVertexBuffer(numWaterVerts * sizeof(WaterFFPElem), 0, 0, D3DPOOL_MANAGED, &vbWaterFFP, 0);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create FFP water verts");
        return false;
    }
    {
        D3DXVECTOR3* srcVerts;
        WaterFFPElem* dstVerts;
        vbWater->Lock(0, 0, (void**)&srcVerts, D3DLOCK_READONLY);
        vbWaterFFP->Lock(0, 0, (void**)&dstVerts, 0);
        const float kUvScale = 1.0f / 256.0f;
        for (int i = 0; i < numWaterVerts; ++i) {
            dstVerts[i].x = srcVerts[i].x;
            dstVerts[i].y = srcVerts[i].y;
            dstVerts[i].z = srcVerts[i].z;
            dstVerts[i].u = srcVerts[i].x * kUvScale;
            dstVerts[i].v = srcVerts[i].y * kUvScale;
        }
        vbWaterFFP->Unlock();
        vbWater->Unlock();
    }
#endif

    if (Configuration.MGEFlags & DYNAMIC_RIPPLES) {
        // Setup water simulation
        if (!initDynamicWaves()) {
            return false;
        }

        // Disable Morrowind generated ripples
        MWBridge::get()->toggleRipples(false);
    }

    return true;
}

bool DistantLand::initDynamicWaves() {
    HRESULT hr;

    hr = device->CreateTexture(waveTexResolution, waveTexResolution, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &texRain, NULL);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create rain simulation texture");
        return false;
    }
    texRain->GetSurfaceLevel(0, &surfRain);
    device->ColorFill(surfRain, 0, 0);

    hr = device->CreateTexture(waveTexResolution, waveTexResolution, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &texRipples, NULL);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create ripple simulation texture");
        return false;
    }
    texRipples->GetSurfaceLevel(0, &surfRipples);
    device->ColorFill(surfRipples, 0, 0);

    hr = device->CreateTexture(waveTexResolution, waveTexResolution, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &texRippleBuffer, NULL);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create ripple simulation texture");
        return false;
    }
    texRippleBuffer->GetSurfaceLevel(0, &surfRippleBuffer);
    device->ColorFill(surfRippleBuffer, 0, 0);

    // Vertex buffer for wave texture
    static float waveVertices[] = {
        /*     -0.5f,                    -0.5f,                                               0,1,   0,0,0,0,
                -0.5f,                    waveTexResolution-0.5f,                 0,1,   0,1,0,1,
                waveTexResolution-0.5f,    -0.5f,                                  0,1,   1,0,1,0,
                waveTexResolution-0.5f,    waveTexResolution-0.5f,    0,1,   1,1,1,1 */

        // Use only one tri over the whole texture to prevent simulation seams at tri edges
        // Rendering to a surface that is bound as a source texture updates the texture after
        // each primitive, causing artifacts to appear at primitive boundaries
        -waveTexResolution/2  -0.5f,    waveTexResolution/2  -0.5f,  0,  1,     -0.5, 0.5,     0,0,
        waveTexResolution        -0.5f,    2*waveTexResolution  -0.5f,  0,  1,      1.0, 2.0,      0,1,
        waveTexResolution        -0.5f,    -waveTexResolution    -0.5f,  0,  1,     1.0, -1.0,     1,1
    };

    void* vp;
    hr = device->CreateVertexBuffer(3 * 32, D3DUSAGE_WRITEONLY, fvfWave, D3DPOOL_DEFAULT, &vbWaveSim, 0);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create wave simulation vb");
        return false;
    }
    if (vbWaveSim->Lock(0, 0, (void**)&vp, 0) != D3D_OK) {
        LOG::logline("!! Failed to lock wave simulation vb");
        return false;
    }
    memcpy(vp, waveVertices, sizeof(waveVertices));
    vbWaveSim->Unlock();

    return true;
}

bool DistantLand::initShadow() {
    const D3DFORMAT shadowFormat = D3DFMT_R16F, shadowZFormat = D3DFMT_D24S8;
    const UINT shadowSize = Configuration.DL.ShadowResolution, cascades = 2;
    HRESULT hr;

    // The shadow texture holds a horizontal-packed shadow atlas
    hr = device->CreateTexture(cascades * shadowSize, shadowSize, 1, D3DUSAGE_RENDERTARGET, shadowFormat, D3DPOOL_DEFAULT, &texShadow, NULL);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create shadow render target");
        return false;
    }
    hr = device->CreateTexture(cascades * shadowSize, shadowSize, 1, D3DUSAGE_RENDERTARGET, shadowFormat, D3DPOOL_DEFAULT, &texSoftShadow, NULL);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create shadow render target");
        return false;
    }
    hr = device->CreateDepthStencilSurface(cascades * shadowSize, shadowSize, shadowZFormat, D3DMULTISAMPLE_NONE, 0, TRUE, &surfShadowZ, NULL);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create shadow Z buffer");
        return false;
    }
    hr = device->CreateVertexBuffer(4 * 12, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vbFullFrame, 0);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create shadow processing verts");
        return false;
    }
    hr = device->CreateVertexBuffer(14 * 12, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vbClipCube, 0);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create shadow processing verts");
        return false;
    }

    // Used to cover an entire render target of any dimension
    D3DXVECTOR3* v;
    vbFullFrame->Lock(0, 0, (void**)&v, 0);
    v[0] = D3DXVECTOR3( -1.0f, 1.0f,  1.0f);
    v[1] = D3DXVECTOR3(-1.0f, -1.0f,  1.0f);
    v[2] = D3DXVECTOR3( 1.0f,  1.0f,  1.0f);
    v[3] = D3DXVECTOR3( 1.0f, -1.0f,  1.0f);
    vbFullFrame->Unlock();

    // Used to project the view frustum in world space
    // Slightly expanded from the canonical cube to allow for rasterization and filtering
    const float u = 1.01f;
    vbClipCube->Lock(0, 0, (void**)&v, 0);
    v[0] = D3DXVECTOR3(-u,  u, 0.0f);
    v[1] = D3DXVECTOR3(-u, -u, 0.0f);
    v[2] = D3DXVECTOR3( u,  u, 0.0f);
    v[3] = D3DXVECTOR3( u, -u, 0.0f);
    v[4] = D3DXVECTOR3( u, -u, 1.0f);
    v[5] = D3DXVECTOR3(-u, -u, 0.0f);
    v[6] = D3DXVECTOR3(-u, -u, 1.0f);
    v[7] = D3DXVECTOR3(-u,  u, 0.0f);
    v[8] = D3DXVECTOR3(-u,  u, 1.0f);
    v[9] = D3DXVECTOR3( u,  u, 0.0f);
    v[10] = D3DXVECTOR3( u,  u, 1.0f);
    v[11] = D3DXVECTOR3( u, -u, 1.0f);
    v[12] = D3DXVECTOR3(-u,  u, 1.0f);
    v[13] = D3DXVECTOR3(-u, -u, 1.0f);
    vbClipCube->Unlock();

    return true;
}

bool DistantLand::initDistantStaticsClient() {
    if (FAILED(device->CreateVertexDeclaration(StaticElem, &StaticDecl))) {
        LOG::logline("!! Failed to to create static vertex declaration");
        return false;
    }

    if (GetFileAttributes("Data Files\\distantland\\statics") == INVALID_FILE_ATTRIBUTES) {
        LOG::logline("!! Distant statics have not been generated");
        LOG::flush();
        return !(Configuration.MGEFlags & USE_DISTANT_LAND);
    }

    if (Configuration.UseSharedMemory) {
        auto staticsId = IPC::InvalidVector;
        auto subsetsId = IPC::InvalidVector;
        {
            auto maybeStatics = ipcClient.allocVecBlocking<DistantStatic>(1, 500000, 1);
            if (!maybeStatics.has_value()) {
                return false;
            }

            auto maybeSubsets = ipcClient.allocVecBlocking<DistantSubset>(1, 500000, 1);
            if (!maybeSubsets.has_value()) {
                return false;
            }

            auto& statics = maybeStatics.value();
            auto& subsets = maybeSubsets.value();
            if (!loadDistantStaticsClient(statics, subsets)) {
                return false;
            }

            staticsId = statics.id();
            subsetsId = subsets.id();
            if (!ipcClient.initDistantStatics(staticsId, subsetsId)) {
                return false;
            }

            // our views are destroyed
        }

        // free on server
        ipcClient.freeVecBlocking(staticsId);
        ipcClient.freeVec(subsetsId);
    } else {
        vector<DistantStatic> distantStatics;
        vector<DistantSubset> distantSubsets;
        if (!loadDistantStaticsClient(distantStatics, distantSubsets)) {
            return false;
        }
    }

    DistantLandShare::currentWorldSpace = nullptr;
    DistantLandShare::hasCurrentWorldSpace = false;
    isDistantLandLoaded = true;
    return true;
}

template<class T, class U>
bool DistantLand::loadStaticMeshes(HANDLE h, T& distantStatics, U& distantSubsets) {
    DWORD unused;

    size_t DistantStaticCount;
    ReadFile(h, &DistantStaticCount, 4, &unused, 0);
    distantStatics.reserve(DistantStaticCount);

    // we don't actually know yet how many subsets there will be, but it'll probably be at least this many
    distantSubsets.reserve(DistantStaticCount);
    
    HANDLE h2 = CreateFile("Data Files\\distantland\\statics\\static_meshes", GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    if (h2 == INVALID_HANDLE_VALUE) {
        LOG::logline("!! Required distant statics files are missing, regeneration required - distantland/statics/static_meshes");
        LOG::flush();
        return false;
    }

    // Bright yellow error texture
    IDirect3DTexture9* errorTexture;
    device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &errorTexture, NULL);

    D3DLOCKED_RECT yellow;
    errorTexture->LockRect(0, &yellow, NULL, 0);
    *(DWORD*)yellow.pBits = 0xffffff00;
    errorTexture->UnlockRect(0);

    // Read entire file into one big memory buffer
    DWORD file_size = GetFileSize(h2, NULL);
    auto file_buffer = std::make_unique<char[]>(file_size);
    ReadFile(h2, file_buffer.get(), file_size, &unused, NULL);
    membuf_reader reader(file_buffer.get());
    CloseHandle(h2);

    for (DWORD distantStaticIndex = 0; distantStaticIndex < DistantStaticCount; distantStaticIndex++) {
        DistantStatic i = {};
        reader.read(&i.numSubsets, 4);
        reader.read(&i.sphere.radius, 4);
        reader.read(&i.sphere.center, 12);
        reader.read(&i.type, 1);

        i.aabbMin = D3DXVECTOR3(FLT_MAX, FLT_MAX, FLT_MAX);
        i.aabbMax = D3DXVECTOR3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

        i.firstSubsetIndex = distantSubsets.size();
        for (size_t subsetIndex = 0; subsetIndex < i.numSubsets; subsetIndex++) {
            DistantSubset subset = {};

            // Get bounding sphere
            reader.read(&subset.sphere.radius, 4);
            reader.read(&subset.sphere.center, 12);

            // Get AABB min and max
            reader.read(&subset.aabbMin, 12);
            reader.read(&subset.aabbMax, 12);

            // Get vertex and face count
            reader.read(&subset.verts, 4);
            reader.read(&subset.faces, 4);

            // Update parent AABB
            i.aabbMin.x = std::min(i.aabbMin.x, subset.aabbMin.x);
            i.aabbMin.y = std::min(i.aabbMin.y, subset.aabbMin.y);
            i.aabbMin.z = std::min(i.aabbMin.z, subset.aabbMin.z);
            i.aabbMax.x = std::max(i.aabbMax.x, subset.aabbMax.x);
            i.aabbMax.y = std::max(i.aabbMax.y, subset.aabbMax.y);
            i.aabbMax.z = std::max(i.aabbMax.z, subset.aabbMax.z);

            // Load mesh data
            IDirect3DVertexBuffer9* vb;
            IDirect3DIndexBuffer9* ib;
            void* lockdata;

            device->CreateVertexBuffer(subset.verts * SIZEOFSTATICVERT, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, 0);
            vb->Lock(0, 0, &lockdata, 0);
            reader.read(lockdata, subset.verts * SIZEOFSTATICVERT);
            vb->Unlock();

            device->CreateIndexBuffer(subset.faces * 6, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, 0);
            ib->Lock(0, 0, &lockdata, 0);
            reader.read(lockdata, subset.faces * 6); // Morrowind nifs don't support 32 bit indices?
            ib->Unlock();

            subset.vbuffer = vb;
            subset.ibuffer = ib;

            // Texturing flags
            bool texturingFlags[2];
            reader.read(&texturingFlags, 2);
            subset.hasAlpha = texturingFlags[0];
            subset.hasUVController = texturingFlags[1];

            // Load referenced texture
            unsigned short pathsize;
            reader.read(&pathsize, 2);
            const char* texname = reader.get();
            reader.advance(pathsize);

            IDirect3DTexture9* tex = BSA::loadTexture(device, texname);
            if (!tex) {
                LOG::logline("Cannot load texture %s", texname);
                errorTexture->AddRef();
                tex = errorTexture;
            }
            subset.tex = tex;

#ifdef MGE_RTX
            // Record this distant static's source texture path so the per-texture
            // suppress list (MGE Distant Cull MCM) can skip its distant copies at draw
            // time. Skip the shared error texture (its name would be meaningless/last-wins).
            if (tex != errorTexture) {
                dlRegisterDistantTexture(tex, texname);
            }
#endif

            // Keep resource pointers for deallocation
            meshCollectionStatics.push_back(MeshResources(vb, ib, tex));

            distantSubsets.push_back(subset);
        }

        distantStatics.push_back(i);
    }
    file_buffer.reset();
    errorTexture->Release();


    // Texture memory reporting
    int texturesLoaded, texMemUsage;
    BSA::cacheStats(&texturesLoaded, &texMemUsage);

    LOG::logline("-- Distant static geometry memory use: %d MB", file_size / (1 << 20));
    LOG::logline("-- Distant textures loaded, %d textures", texturesLoaded);
    LOG::logline("-- Distant texture memory use: %d MB", texMemUsage);
    LOG::flush();

    return true;
}

void DistantLand::loadVisGroupsClient(HANDLE h) {
    DWORD unused;

    // Load dynamic vis groups
    size_t dynamicVisGroupCount;
    ReadFile(h, &dynamicVisGroupCount, 4, &unused, 0);
    dynamicVisGroups.clear();

    if (dynamicVisGroupCount > 0) {
        const size_t visGroupRecordSize = 130;
        size_t visDataSize = visGroupRecordSize * dynamicVisGroupCount;
        auto visData = std::make_unique<char[]>(visDataSize);
        ReadFile(h, visData.get(), visDataSize, &unused, 0);
        membuf_reader visReader(visData.get());

        // VisGroup indexes use a 1-based index, group 0 is reserved for testing
        dynamicVisGroups.resize(dynamicVisGroupCount + 1);

        for (size_t nVisGroup = 1; nVisGroup <= dynamicVisGroupCount; ++nVisGroup) {
            DynamicVisGroup& dvg = dynamicVisGroups[nVisGroup];
            visReader.read(&dvg.source, 1);
            dvg.enabled = true;
            dvg.gameObject = nullptr;

            char id[64];
            visReader.read(&id, sizeof(id));
            dvg.id = id;

            uint8_t rangeCount;
            visReader.read(&rangeCount, sizeof(rangeCount));

            DynamicVisGroup::Range ranges[8];
            visReader.read(&ranges, sizeof(ranges));
            dvg.ranges.assign(ranges, ranges + rangeCount);
        }

        visData.reset();
    }
}

template<class T, class U>
bool DistantLand::loadDistantStaticsClient(T& distantStatics, U& distantSubsets) {
    auto h = DistantLandShare::beginReadStatics();
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    if (!loadStaticMeshes(h, distantStatics, distantSubsets)) {
        CloseHandle(h);
        return false;
    }
    loadVisGroupsClient(h);
    
    if (Configuration.UseSharedMemory) {
        CloseHandle(h);
        return true; // server will handle the rest of the logic
    }

    DistantLandShare::readDistantStatics(h, distantStatics, distantSubsets, dynamicVisGroups);
    CloseHandle(h);
    return true;
}

bool DistantLand::initLandscapeClient() {
    HANDLE file = CreateFile("Data Files\\distantland\\world", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD file_size = GetFileSize(file, NULL);
    DWORD mesh_count, unused;
    ReadFile(file, &mesh_count, 4, &unused, 0);
    if (mesh_count == 0) {
        CloseHandle(file);
        return true;
    }

    auto id = IPC::InvalidVector;
    {
        auto& maybeBuffers = ipcClient.allocVecBlocking<IPC::LandscapeBuffers>(1, 200000, mesh_count);
        if (!maybeBuffers.has_value()) {
            return false;
        }

        auto& buffers = maybeBuffers.value();
        id = buffers.id();

        // the server will read data as we populate it
        if (!ipcClient.initLandscape(id)) {
            ipcClient.freeVecBlocking(id);
            return false;
        }

        buffers.start_write();
        for (DWORD i = 0; i < mesh_count; i++) {
            // skip info that will be handled by the server
            SetFilePointer(file, 40, NULL, FILE_CURRENT);

            DWORD verts = 0, faces = 0;
            IDirect3DVertexBuffer9* vb;
            IDirect3DIndexBuffer9* ib;
            void* lockdata;

            ReadFile(file, &verts, 4, &unused, 0);
            ReadFile(file, &faces, 4, &unused, 0);
            bool large = (verts > 0xFFFF || faces > 0xFFFF);

            device->CreateVertexBuffer(verts * SIZEOFLANDVERT, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, 0);
            vb->Lock(0, 0, &lockdata, 0);
            ReadFile(file, lockdata, verts * SIZEOFLANDVERT, &unused, 0);
            vb->Unlock();

            device->CreateIndexBuffer(faces * (large ? 12 : 6), D3DUSAGE_WRITEONLY, large ? D3DFMT_INDEX32 : D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, 0);
            ib->Lock(0, 0, &lockdata, 0);
            ReadFile(file, lockdata, faces * (large ? 12 : 6), &unused, 0);
            ib->Unlock();

            buffers.push_back({ vb, ib });

            meshCollectionLand.push_back(MeshResources(vb, ib, 0));
        }
        buffers.end_write();

        // Architecture B (task 8.5): the server processed the landscape data in parallel and,
        // as part of the same InitLandscape RPC, ran its Format_Loader and reported whether a
        // New_Format composite pool was loaded. Read that verdict back NOW, before freeVec
        // reuses the shared parameter union. hasCompositeSet=false (Old_Format / absent /
        // malformed / inert) leaves every New_Format-only path off and the renderer stays on
        // the bit-identical Single_Atlas_Path (Req 4.6, 6.2).
        std::uint32_t cellCount = 0, defaultEdge = 0;
        bool hasComposites = false;
        if (ipcClient.awaitInitLandscape(hasComposites, cellCount, defaultEdge)) {
            initCompositeStreaming(hasComposites, cellCount, defaultEdge);
        }

        // our views must be destroyed before we can free the vec
    }

    ipcClient.freeVec(id);
    CloseHandle(file);
    return true;
}

// Architecture B (task 8.5): activate IPC-client composite streaming once the server's
// Format_Loader verdict is known. Guarded entirely behind New_Format so an Old_Format /
// absent / malformed / inert load leaves the cache empty, allocates no channels, and keeps
// the distant land bit-identical to stock (Req 4.6, 6.2).
void DistantLand::initCompositeStreaming(bool hasComposites, std::uint32_t cellCount, std::uint32_t defaultEdgeTexels) {
    ++s_compositeChannelGeneration;
    if (s_compositeChannelGeneration == 0) {
        ++s_compositeChannelGeneration;
    }

    compositeDeltaView = IPC::VecView<CompositeCellId>();
    compositeHeadersView = IPC::VecView<CompositeChunkMsg>();
    compositeBytesView = IPC::VecView<std::uint8_t>();
    compositeDeltaSharedId = IPC::InvalidVector;
    compositeHeadersSharedId = IPC::InvalidVector;
    compositeBytesSharedId = IPC::InvalidVector;
    hasCompositeSet = false;
    compositeDefaultEdge = 0;

    if (!hasComposites || cellCount == 0) {
        // Old_Format / inert: nothing to stream. The empty cache makes every lookup() miss so
        // the Texture_Binder binds the atlas only (Req 4.6).
        return;
    }

    const std::uint32_t defaultEdge = defaultEdgeTexels ? defaultEdgeTexels : 1024;
    const std::uint32_t cellBytesEstimate =
        ResidentCompositeCache::compositeBytes(defaultEdge);
    if (defaultEdge > kCompositeMaxEdgeTexels || cellBytesEstimate == 0 ||
        cellBytesEstimate > kCompositeMaxPayloadBytes) {
        LOG::logline(
            "!! Composite streaming rejected invalid default shape: edge=%u bytes=%u",
            defaultEdge, cellBytesEstimate);
        return;
    }

    // New_Format: bind the resident cache to the same D3D9 device that feeds Remix and set the
    // default Texture_Memory_Budget (Req 7.2; 64 MB caps the Draw_Distance=2, 1024^2 worst case
    // at or below 64 MB, Req 7.4).
    compositeCache.init(device);
    compositeCache.setBudgetMB(kCompositeBudgetMB);
    compositeDefaultEdge = defaultEdge;

    // Allocate the three StreamVisibleComposites shared channels, mirroring the visible-mesh
    // vectors allocated in initIpc(). The delta channel carries the per-frame newVisible \
    // resident cell list; the header channel one CompositeChunkMsg per streamed cell; the byte
    // channel the concatenated DXT1 + mip blobs. Sizes are bounded by the visible ring at the
    // configured draw distance plus headroom, but reserved (not committed) up front so the
    // shared memory only grows as cells actually stream.
    auto maybeDelta = ipcClient.allocVecBlocking<CompositeCellId>(256, 65536, 1);
    auto maybeHeaders = ipcClient.allocVecBlocking<CompositeChunkMsg>(256, 65536, 1);
    // The server enforces a 3 MiB normal batch and permits one supported oversized first
    // payload. Reserving the protocol maximum therefore covers every valid response without
    // estimate multiplication or 32-bit overflow.
    auto maybeBytes = ipcClient.allocVecBlocking<std::uint8_t>(
        cellBytesEstimate, kCompositeMaxPayloadBytes, cellBytesEstimate);

    if (!maybeDelta.has_value() || !maybeHeaders.has_value() || !maybeBytes.has_value()) {
        LOG::logline("!! Failed to allocate composite streaming channels; using single atlas");
        auto freeAllocatedView = [](auto& view) {
            if (!view.has_value()) {
                return;
            }
            const auto id = view->id();
            view.reset();
            if (!ipcClient.freeVecBlocking(id)) {
                LOG::logline("!! Failed to free partially allocated composite vector %u", id);
            }
        };
        freeAllocatedView(maybeBytes);
        freeAllocatedView(maybeHeaders);
        freeAllocatedView(maybeDelta);
        compositeCache.releaseAll();
        compositeDefaultEdge = 0;
        return;
    }

    compositeDeltaView = std::move(maybeDelta.value());
    compositeHeadersView = std::move(maybeHeaders.value());
    compositeBytesView = std::move(maybeBytes.value());
    compositeDeltaSharedId = compositeDeltaView.id();
    compositeHeadersSharedId = compositeHeadersView.id();
    compositeBytesSharedId = compositeBytesView.id();

    hasCompositeSet = true;
    LOG::logline("-- Composite streaming active: %u cells, %u px default, %u MB budget",
                 cellCount, compositeDefaultEdge, kCompositeBudgetMB);
}

bool DistantLand::pollCompositeStreamBatch() {
    s_compositePreflightSample = CompositeTelemetry::FrameSample{};
    s_preparedCompositeFrameReady = false;
    s_preparedCompositeCells.clear();
    s_refreshDistantVisibility = true;

    const std::uint64_t sessionGeneration = ipcClient.sessionGeneration();
    const std::uint64_t cacheGeneration = compositeCache.generation();
    const bool transportEpochChanged =
        s_observedCompositeSessionGeneration != sessionGeneration ||
        s_observedCompositeChannelGeneration != s_compositeChannelGeneration;
    if (transportEpochChanged) {
        if (s_compositeInFlight.active) {
            LOG::logline(
                "-- Retiring composite batch from replaced IPC epoch: session=%llu channel=%llu",
                static_cast<unsigned long long>(s_compositeInFlight.sessionGeneration),
                static_cast<unsigned long long>(s_compositeInFlight.channelGeneration));
        }
        clearCompositeInFlight();
        resetCompositeControllerState();
        s_compositeTransportFailed = false;
        s_observedCompositeSessionGeneration = sessionGeneration;
        s_observedCompositeChannelGeneration = s_compositeChannelGeneration;
        s_observedCompositeCacheGeneration = cacheGeneration;
    }

    if (s_compositeTransportFailed) {
        s_refreshDistantVisibility = false;
        return false;
    }
    if (!s_compositeInFlight.active) {
        s_distantVisibilityStaleFrames = 0;
        return true;
    }

    LARGE_INTEGER pollStart = {};
    QueryPerformanceCounter(&pollStart);
    const IPC::WakeReason result = ipcClient.pollForCompletion();
    s_compositePreflightSample.rpcWaitUs += compositeElapsedMicros(pollStart);

    switch (result) {
    case IPC::WakeReason::Complete:
        // Completion state and output vectors remain retained until reconcile consumes them.
        s_distantVisibilityStaleFrames = 0;
        return true;
    case IPC::WakeReason::ServerLost:
        ++s_compositePreflightSample.rpcFailures;
        ++s_compositePreflightSample.batchFailures;
        for (const CellId& cell : s_compositeInFlight.cells) {
            s_compositeRequestController.markRetry(cell);
        }
        clearCompositeInFlight();
        s_compositeTransportFailed = true;
        LOG::logline(
            "!! Composite IPC host lost; suppressing distant visibility until a new IPC session starts");
        break;
    case IPC::WakeReason::Error:
        ++s_compositePreflightSample.rpcFailures;
        ++s_compositePreflightSample.batchFailures;
        for (const CellId& cell : s_compositeInFlight.cells) {
            s_compositeRequestController.markRetry(cell);
        }
        clearCompositeInFlight();
        s_compositeTransportFailed = true;
        LOG::logline(
            "!! Composite IPC wait failed; channel quarantined until IPC session replacement");
        break;
    case IPC::WakeReason::Timeout:
    case IPC::WakeReason::Update:
    default:
        break;
    }

    s_refreshDistantVisibility = false;
    // Visibility staleness spans sequential composite batches; per-batch age can be cleared
    // by the reconcile poll before this frame reaches the draw gate.
    if (!s_compositeTransportFailed && s_compositeInFlight.active &&
        s_distantVisibilityStaleFrames < std::numeric_limits<std::uint32_t>::max()) {
        ++s_distantVisibilityStaleFrames;
    }
    return false;
}

bool DistantLand::canRefreshDistantVisibility() {
    return s_refreshDistantVisibility;
}

bool DistantLand::canDrawDistantVisibility() {
    if (!hasCompositeSet || !Configuration.UseSharedMemory ||
        s_refreshDistantVisibility) {
        return true;
    }
    if (s_compositeTransportFailed || !s_distantVisibilityCellValid ||
        s_distantVisibilityCell != MWBridge::get()->getPlayerCell()) {
        return false;
    }
    return s_distantVisibilityStaleFrames <= kMaxCompositeStaleVisibilityFrames;
}

// Architecture B (task 8.5): per-frame IPC-client composite residency reconcile. Mirrors the
// authoritative model tests/composite_stream_model.py (reconcile + budget admission +
// telemetry tally): derive the Visible_Cell_Set, stream newVisible \ resident, admit the
// streamed blobs under the budget, evict cells that left the set (resident subset of visible),
// and record the atlas-served count. A no-op for Old_Format / inert / non-IPC loads.
void DistantLand::streamAndReconcileComposites() {
    if (!hasCompositeSet || !Configuration.UseSharedMemory) {
        return;
    }

    static constexpr std::uint32_t kMaxRequestCellsPerFrame = 4;
    static constexpr std::uint32_t kInFlightWarningFrames = 60;

    auto& requestController = s_compositeRequestController;
    auto& previousVisible = s_previousCompositeVisible;
    auto& previousVisibleValid = s_previousCompositeVisibleValid;
    auto& observedCacheGeneration = s_observedCompositeCacheGeneration;
    auto& inFlight = s_compositeInFlight;

    const std::uint64_t cacheGeneration = compositeCache.generation();
    if (observedCacheGeneration != cacheGeneration) {
        requestController.reset();
        s_compositeTelemetryReporter.reset();
        previousVisible.clear();
        previousVisibleValid = false;
        observedCacheGeneration = cacheGeneration;
    }

    static const LARGE_INTEGER qpcFrequency = []() {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        return frequency;
    }();
    const auto elapsedMicros = [&](const LARGE_INTEGER& start) -> std::uint64_t {
        LARGE_INTEGER end = {};
        QueryPerformanceCounter(&end);
        if (qpcFrequency.QuadPart <= 0 || end.QuadPart <= start.QuadPart) {
            return 0;
        }
        return static_cast<std::uint64_t>(
            ((end.QuadPart - start.QuadPart) * 1000000ll) / qpcFrequency.QuadPart);
    };

    LARGE_INTEGER reconcileStart = {};
    QueryPerformanceCounter(&reconcileStart);

    const float drawDistCells = std::max(1.0f, Configuration.DL.DrawDist);
    const int ring = static_cast<int>(std::ceil(drawDistCells));
    const int eyeCellX = static_cast<int>(std::floor(eyePos.x / kCellSize));
    const int eyeCellY = static_cast<int>(std::floor(eyePos.y / kCellSize));
    const bool exterior = MWBridge::get()->IsExterior();

    VisibleCellSet visible;
    if (exterior) {
        visible.reserve(static_cast<std::size_t>((2 * ring + 1) * (2 * ring + 1)));
        for (int dy = -ring; dy <= ring; ++dy) {
            for (int dx = -ring; dx <= ring; ++dx) {
                visible.insert(CellId{ eyeCellX + dx, eyeCellY + dy });
            }
        }
        compositeCache.includeTransitionTargets(visible);
    } else {
        compositeCache.setTransitionTargets({});
    }
    const std::uint32_t visibleCount = static_cast<std::uint32_t>(visible.size());

    const bool visibleSetChanged = !previousVisibleValid || visible != previousVisible;
    if (visibleSetChanged) {
        previousVisible = visible;
        previousVisibleValid = true;
    }

    compositeCache.evictNotVisible(visible);
    requestController.beginFrame(
        visible, visibleSetChanged, compositeCache.residentBytes(),
        compositeCache.budgetBytes());

    CompositeTelemetry::FrameSample frameSample = s_compositePreflightSample;
    frameSample.visibleCount = visibleCount;

    const auto clearInFlight = [&]() {
        clearCompositeInFlight();
    };

    const auto retryCells = [&](const std::vector<CellId>& cells) {
        for (const CellId& cell : cells) {
            if (visible.find(cell) != visible.end()) {
                requestController.markRetry(cell);
            }
        }
    };

    const auto completeInFlight = [&]() {
        CompositeBatchStatus batchStatus = CompositeBatchStatus::Pending;
        const bool hasBatchResult = ipcClient.takeCompositeStreamResult(batchStatus);
        if (inFlight.sessionGeneration != ipcClient.sessionGeneration() ||
            inFlight.channelGeneration != s_compositeChannelGeneration ||
            inFlight.cacheGeneration != compositeCache.generation()) {
            clearInFlight();
            return;
        }

        const std::uint32_t headerCount = compositeHeadersView.size();
        const std::uint32_t totalBytes = compositeBytesView.size();
        frameSample.responseBytes += totalBytes;

        struct ParsedOutcome {
            CompositeChunkMsg message;
            std::uint32_t byteOffset;
        };

        const std::unordered_set<CellId, CellIdHash> requestedSet(
            inFlight.cells.begin(), inFlight.cells.end());
        std::unordered_set<CellId, CellIdHash> returnedCells;
        std::vector<ParsedOutcome> outcomes;
        std::uint64_t byteCursor = 0;
        std::uint32_t largestPayload = 0;
        bool responseWellFormed = hasBatchResult &&
            headerCount <= inFlight.cells.size() &&
            totalBytes <= kCompositeMaxPayloadBytes;
        bool processOutcomes = false;
        bool batchFailed = false;

        if (responseWellFormed) {
            switch (batchStatus) {
            case CompositeBatchStatus::Complete:
                processOutcomes = true;
                break;
            case CompositeBatchStatus::OutputExhausted:
                processOutcomes = true;
                batchFailed = true;
                break;
            case CompositeBatchStatus::Inactive:
            case CompositeBatchStatus::InvalidVectors:
                batchFailed = true;
                responseWellFormed = headerCount == 0 && totalBytes == 0;
                break;
            case CompositeBatchStatus::Pending:
            default:
                batchFailed = true;
                responseWellFormed = false;
                break;
            }
        } else {
            batchFailed = true;
        }

        if (responseWellFormed && processOutcomes) {
            returnedCells.reserve(headerCount);
            outcomes.reserve(headerCount);
            for (std::uint32_t h = 0; h < headerCount; ++h) {
                const CompositeChunkMsg msg = compositeHeadersView[h];
                const CellId cell{ msg.cellX, msg.cellY };
                if (requestedSet.find(cell) == requestedSet.end() ||
                    !returnedCells.insert(cell).second) {
                    responseWellFormed = false;
                    break;
                }

                switch (msg.status) {
                case CompositeCellStatus::Found:
                    if (!ResidentCompositeCache::validatePayload(msg) ||
                        byteCursor + msg.byteLength > totalBytes) {
                        responseWellFormed = false;
                    } else {
                        outcomes.push_back(ParsedOutcome{
                            msg, static_cast<std::uint32_t>(byteCursor) });
                        byteCursor += msg.byteLength;
                        largestPayload = std::max(largestPayload, msg.byteLength);
                    }
                    break;
                case CompositeCellStatus::NotFound:
                case CompositeCellStatus::Deferred:
                case CompositeCellStatus::Error:
                    if (msg.byteLength != 0) {
                        responseWellFormed = false;
                    } else {
                        outcomes.push_back(ParsedOutcome{
                            msg, static_cast<std::uint32_t>(byteCursor) });
                    }
                    break;
                default:
                    responseWellFormed = false;
                    break;
                }
                if (!responseWellFormed) {
                    break;
                }
            }

            if (byteCursor != totalBytes ||
                (batchStatus == CompositeBatchStatus::Complete &&
                 returnedCells.size() != requestedSet.size())) {
                responseWellFormed = false;
            }
        }

        if (!responseWellFormed) {
            batchFailed = true;
            ++frameSample.malformedResponses;
            LOG::logline(
                "!! Composite stream response malformed: requested=%u headers=%u bytes=%u consumed=%llu status=%u",
                static_cast<unsigned>(inFlight.cells.size()), headerCount, totalBytes,
                static_cast<unsigned long long>(byteCursor),
                static_cast<unsigned>(batchStatus));
        }
        if (batchFailed) {
            ++frameSample.batchFailures;
        }

        if (!responseWellFormed || !processOutcomes) {
            retryCells(inFlight.cells);
            clearInFlight();
            return;
        }

        std::vector<std::uint8_t> blob;
        try {
            blob.resize(largestPayload);
        } catch (const std::bad_alloc&) {
            ++frameSample.uploadFailed;
            if (!batchFailed) {
                ++frameSample.batchFailures;
            }
            LOG::logline(
                "!! Composite stream response allocation failed: bytes=%u",
                largestPayload);
            retryCells(inFlight.cells);
            clearInFlight();
            return;
        }

        frameSample.responseCells += static_cast<std::uint32_t>(outcomes.size());
        for (const ParsedOutcome& outcome : outcomes) {
            const CompositeChunkMsg& msg = outcome.message;
            const CellId cell{ msg.cellX, msg.cellY };
            const bool stillVisible = visible.find(cell) != visible.end();

            switch (msg.status) {
            case CompositeCellStatus::Found: {
                if (!stillVisible) {
                    break;
                }
                for (std::uint32_t b = 0; b < msg.byteLength; ++b) {
                    blob[b] = compositeBytesView[outcome.byteOffset + b];
                }

                const CompositeAdmissionResult result =
                    compositeCache.admitWithResult(msg, blob.data());
                switch (result) {
                case CompositeAdmissionResult::AlreadyResident:
                    requestController.markResident(cell);
                    break;
                case CompositeAdmissionResult::Admitted:
                    requestController.markResident(cell);
                    ++frameSample.admittedCells;
                    frameSample.admittedBytes += msg.byteLength;
                    break;
                case CompositeAdmissionResult::BudgetRejected:
                    requestController.markBudgetBlocked(
                        cell, compositeCache.isTransitionTarget(cell));
                    ++frameSample.budgetRejected;
                    break;
                case CompositeAdmissionResult::UploadFailed:
                    requestController.markRetry(cell);
                    ++frameSample.uploadFailed;
                    break;
                }
                break;
            }
            case CompositeCellStatus::NotFound:
                ++frameSample.notFoundResponses;
                if (stillVisible) {
                    requestController.markMissing(cell);
                }
                break;
            case CompositeCellStatus::Deferred:
                ++frameSample.deferredResponses;
                if (stillVisible) {
                    requestController.markDeferred(cell);
                }
                break;
            case CompositeCellStatus::Error:
                ++frameSample.serverErrors;
                if (stillVisible) {
                    requestController.markRetry(cell);
                }
                break;
            }
        }

        // OutputExhausted may omit cells for which no explicit outcome fit. Omission is never
        // authoritative absence; only explicit NotFound enters the negative cache.
        for (const CellId& cell : inFlight.cells) {
            if (visible.find(cell) != visible.end() && requestController.isPending(cell)) {
                requestController.markRetry(cell);
            }
        }
        clearInFlight();
    };

    const auto pollInFlight = [&](bool advanceAge) {
        if (!inFlight.active) {
            return;
        }
        if (advanceAge && inFlight.ageFrames < std::numeric_limits<std::uint32_t>::max()) {
            ++inFlight.ageFrames;
        }

        LARGE_INTEGER pollStart = {};
        QueryPerformanceCounter(&pollStart);
        const IPC::WakeReason result = ipcClient.pollForCompletion();
        frameSample.rpcWaitUs += elapsedMicros(pollStart);

        switch (result) {
        case IPC::WakeReason::Complete:
            completeInFlight();
            break;
        case IPC::WakeReason::ServerLost:
            ++frameSample.rpcFailures;
            ++frameSample.batchFailures;
            retryCells(inFlight.cells);
            clearInFlight();
            s_compositeTransportFailed = true;
            s_refreshDistantVisibility = false;
            break;
        case IPC::WakeReason::Timeout:
            if (inFlight.ageFrames >= kInFlightWarningFrames &&
                !inFlight.timeoutReported) {
                inFlight.timeoutReported = true;
                ++frameSample.rpcTimeouts;
            }
            break;
        case IPC::WakeReason::Error:
            if (!inFlight.waitErrorReported) {
                inFlight.waitErrorReported = true;
                ++frameSample.rpcFailures;
                ++frameSample.batchFailures;
            }
            retryCells(inFlight.cells);
            clearInFlight();
            s_compositeTransportFailed = true;
            s_refreshDistantVisibility = false;
            break;
        case IPC::WakeReason::Update:
        default:
            break;
        }
    };

    // Poll before touching any shared request or response vector. Timeout/Error preserves
    // ownership; only completion or proven host loss releases the vectors for reuse.
    pollInFlight(true);

    struct Candidate {
        CellId cell;
        bool transitionTarget;
        std::int64_t distanceSq;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(visible.size());
    for (const CellId& cell : visible) {
        if (compositeCache.lookup(cell) != nullptr) {
            requestController.markResident(cell);
            continue;
        }

        const bool transitionTarget = compositeCache.isTransitionTarget(cell);
        if (!requestController.shouldRequest(cell, transitionTarget)) {
            continue;
        }

        const std::int64_t dx = static_cast<std::int64_t>(cell.x) - eyeCellX;
        const std::int64_t dy = static_cast<std::int64_t>(cell.y) - eyeCellY;
        candidates.push_back(Candidate{ cell, transitionTarget, dx * dx + dy * dy });
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            if (a.transitionTarget != b.transitionTarget) {
                return a.transitionTarget > b.transitionTarget;
            }
            if (a.distanceSq != b.distanceSq) {
                return a.distanceSq < b.distanceSq;
            }
            if (a.cell.y != b.cell.y) {
                return a.cell.y < b.cell.y;
            }
            return a.cell.x < b.cell.x;
        });

    std::vector<CellId> requestedCells;
    requestedCells.reserve(kMaxRequestCellsPerFrame);
    if (!inFlight.active && s_refreshDistantVisibility &&
        !s_compositeTransportFailed) {
        const std::uint64_t estimatedCellBytes = std::max<std::uint32_t>(
            1u, ResidentCompositeCache::compositeBytes(compositeDefaultEdge));
        std::uint64_t selectedBytes = 0;

        for (const Candidate& candidate : candidates) {
            if (requestedCells.size() >= kMaxRequestCellsPerFrame) {
                break;
            }
            if (!requestedCells.empty() &&
                selectedBytes + estimatedCellBytes > kCompositeStreamBatchBytes) {
                break;
            }

            requestedCells.push_back(candidate.cell);
            selectedBytes += estimatedCellBytes;
        }
    }

    frameSample.queuedCells = static_cast<std::uint32_t>(
        candidates.size() > requestedCells.size()
            ? candidates.size() - requestedCells.size()
            : 0);

    s_preparedCompositeCells.swap(requestedCells);

    const std::uint32_t visibleResident =
        compositeCache.visibleResidentCount(visible);
    compositeCache.setAtlasServedThisFrame(
        visibleCount > visibleResident ? visibleCount - visibleResident : 0);

    frameSample.reconcileUs = elapsedMicros(reconcileStart);
    s_preparedCompositeSample = frameSample;
    s_preparedCompositeVisibleSetChanged = visibleSetChanged;
    s_preparedCompositeFrameReady = true;
}

void DistantLand::issueCompositeStreamBatch() {
    if (!s_preparedCompositeFrameReady) {
        return;
    }

    if (s_refreshDistantVisibility) {
        s_distantVisibilityCell = MWBridge::get()->getPlayerCell();
        s_distantVisibilityCellValid = true;
    }

    CompositeTelemetry::FrameSample& frameSample = s_preparedCompositeSample;
    bool issued = false;
    if (!s_preparedCompositeCells.empty()) {
        frameSample.attemptedBatches = 1;
        frameSample.attemptedCells =
            static_cast<std::uint32_t>(s_preparedCompositeCells.size());

        const bool epochMatches =
            s_observedCompositeSessionGeneration == ipcClient.sessionGeneration() &&
            s_observedCompositeChannelGeneration == s_compositeChannelGeneration;
        if (hasCompositeSet && epochMatches && !s_compositeInFlight.active &&
            !s_compositeTransportFailed && canDrawDistantVisibility()) {
            LARGE_INTEGER lanePollStart = {};
            QueryPerformanceCounter(&lanePollStart);
            const IPC::WakeReason laneState = ipcClient.pollForCompletion();
            frameSample.rpcWaitUs += compositeElapsedMicros(lanePollStart);

            if (laneState == IPC::WakeReason::Complete) {
                compositeDeltaView.clear();
                bool requestWritten = true;
                for (const CellId& cell : s_preparedCompositeCells) {
                    CompositeCellId wire = {};
                    wire.cellX = cell.x;
                    wire.cellY = cell.y;
                    if (!compositeDeltaView.push_back(wire)) {
                        requestWritten = false;
                        break;
                    }
                }

                if (requestWritten) {
                    compositeHeadersView.clear();
                    compositeBytesView.clear();

                    LARGE_INTEGER rpcStart = {};
                    QueryPerformanceCounter(&rpcStart);
                    const bool rpcStarted = ipcClient.streamVisibleComposites(
                        compositeDeltaSharedId,
                        compositeHeadersSharedId,
                        compositeBytesSharedId);
                    frameSample.rpcWaitUs += compositeElapsedMicros(rpcStart);

                    if (rpcStarted) {
                        for (const CellId& cell : s_preparedCompositeCells) {
                            s_compositeRequestController.markPending(
                                cell, compositeCache.isTransitionTarget(cell));
                        }
                        frameSample.issuedBatches = 1;
                        frameSample.issuedCells = static_cast<std::uint32_t>(
                            s_preparedCompositeCells.size());
                        s_compositeInFlight.active = true;
                        s_compositeInFlight.timeoutReported = false;
                        s_compositeInFlight.waitErrorReported = false;
                        s_compositeInFlight.ageFrames = 0;
                        s_compositeInFlight.sessionGeneration =
                            ipcClient.sessionGeneration();
                        s_compositeInFlight.channelGeneration =
                            s_compositeChannelGeneration;
                        s_compositeInFlight.cacheGeneration =
                            compositeCache.generation();
                        s_compositeInFlight.cells = s_preparedCompositeCells;
                        issued = true;
                    } else {
                        ++frameSample.rpcFailures;
                        ++frameSample.batchFailures;
                        s_compositeTransportFailed = true;
                    }
                } else {
                    compositeDeltaView.clear();
                    ++frameSample.batchFailures;
                }
            } else if (laneState == IPC::WakeReason::ServerLost ||
                       laneState == IPC::WakeReason::Error) {
                ++frameSample.rpcFailures;
                ++frameSample.batchFailures;
                s_compositeTransportFailed = true;
            }
        }

        if (!issued) {
            frameSample.queuedCells += static_cast<std::uint32_t>(
                s_preparedCompositeCells.size());
        }
    }

    const CompositeRequestCounts requestCounts =
        s_compositeRequestController.counts();
    frameSample.deferredBudget = requestCounts.budgetBlocked;
    frameSample.deferredRetry = requestCounts.retryDeferred;
    frameSample.missingCached = requestCounts.missing;
    frameSample.pending = requestCounts.pending;

    s_compositeTelemetryReporter.recordFrame(
        compositeCache, frameSample, s_preparedCompositeVisibleSetChanged);
    s_preparedCompositeCells.clear();
    s_preparedCompositeFrameReady = false;
}

bool DistantLand::initLandscape() {
    HRESULT hr;

    hr = device->CreateVertexDeclaration(LandElem, &LandDecl);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to to create world vertex declaration");
        return false;
    }

    if (GetFileAttributes("Data Files\\distantland\\world") == INVALID_FILE_ATTRIBUTES) {
        LOG::logline("!! Distant land files have not been generated");
        LOG::flush();
        return !(Configuration.MGEFlags & USE_DISTANT_LAND);
    }

    hr = D3DXCreateTextureFromFileEx(device, "Data Files\\distantland\\world.dds", 0, 0, 0, 0, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, 0, 0, &texWorldColour);
    if (hr != D3D_OK) {
        LOG::logline("!! Could not load world texture for distant land - distantland/world.dds");
        LOG::flush();
        return false;
    }

    hr = D3DXCreateTextureFromFileEx(device, "Data Files\\distantland\\world_n.dds", 0, 0, 0, 0, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, 0, 0, &texWorldNormals);
    if (hr != D3D_OK) {
        LOG::logline("!! Could not load world normal map texture for distant land - distantland/world_n.dds");
        LOG::flush();
        return false;
    }

    hr = D3DXCreateTextureFromFileEx(device, "Data Files\\textures\\MGE\\world_detail.dds", 0, 0, 0, 0, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, 0, 0, &texWorldDetail);
    if (hr != D3D_OK) {
        LOG::logline("!! Could not load world detail texture for distant land - textures/MGE/world_detail.dds");
        LOG::flush();
        return false;
    }

    LOG::logline("-- Landscape textures loaded");

    if (Configuration.UseSharedMemory) {
        return initLandscapeClient();
    }

    HANDLE file = CreateFile("Data Files\\distantland\\world", GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD file_size = GetFileSize(file, NULL);
    DWORD mesh_count, unused;
    ReadFile(file, &mesh_count, 4, &unused, 0);

    vector<LandMesh> meshesLand;
    meshesLand.resize(mesh_count);

    if (!meshesLand.empty()) {
        D3DXVECTOR2 qtmin(FLT_MAX, FLT_MAX), qtmax(-FLT_MAX, -FLT_MAX);
        D3DXMATRIX world;
        D3DXMatrixIdentity(&world);

        // Load meshes and calculate max size of quadtree
        for (auto& i : meshesLand) {
            ReadFile(file, &i.sphere.radius, 4, &unused,0);
            ReadFile(file, &i.sphere.center, 12, &unused,0);

            D3DXVECTOR3 boxMin, boxMax;
            ReadFile(file, &boxMin, 12, &unused, 0);
            ReadFile(file, &boxMax, 12, &unused, 0);
            i.box.Set(boxMin, boxMax);

            ReadFile(file, &i.verts, 4, &unused, 0);
            ReadFile(file, &i.faces, 4, &unused, 0);

            bool large = (i.verts > 0xFFFF || i.faces > 0xFFFF);
            IDirect3DVertexBuffer9* vb;
            IDirect3DIndexBuffer9* ib;
            void* lockdata;

            device->CreateVertexBuffer(i.verts * SIZEOFLANDVERT, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, 0);
            vb->Lock(0, 0, &lockdata, 0);
            ReadFile(file, lockdata, i.verts * SIZEOFLANDVERT, &unused, 0);
            vb->Unlock();

            device->CreateIndexBuffer(i.faces * (large ? 12 : 6), D3DUSAGE_WRITEONLY, large ? D3DFMT_INDEX32 : D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, 0);
            ib->Lock(0, 0, &lockdata, 0);
            ReadFile(file, lockdata, i.faces * (large ? 12 : 6), &unused, 0);
            ib->Unlock();

            i.vbuffer = vb;
            i.ibuffer = ib;

            qtmin.x = std::min(qtmin.x, i.sphere.center.x - i.sphere.radius);
            qtmin.y = std::min(qtmin.y, i.sphere.center.y - i.sphere.radius);
            qtmax.x = std::max(qtmax.x, i.sphere.center.x + i.sphere.radius);
            qtmax.y = std::max(qtmax.y, i.sphere.center.y + i.sphere.radius);
        }

        DistantLandShare::LandQuadTree.SetBox(std::max(qtmax.x - qtmin.x, qtmax.y - qtmin.y), 0.5 * (qtmax + qtmin));

        // Add meshes to the quadtree
        for (auto& i : meshesLand) {
            meshCollectionLand.push_back(MeshResources(i.vbuffer, i.ibuffer, 0));
            DistantLandShare::LandQuadTree.AddMesh(i.sphere, i.box, world, false, false, texWorldColour, i.verts, i.vbuffer, i.faces, i.ibuffer);
        }
    }

    CloseHandle(file);
    DistantLandShare::LandQuadTree.CalcVolume();

    // Log approximate memory use
    LOG::logline("-- Distant landscape memory use: %d MB", file_size / (1 << 20));

    return true;
}

bool DistantLand::initGrass() {
    HRESULT hr;

    hr = device->CreateVertexDeclaration(GrassElem, &GrassDecl);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create grass decl");
        return false;
    }

    hr = device->CreateVertexBuffer(MaxGrassElements * GrassInstStride, D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vbGrassInstances, NULL);
    if (hr != D3D_OK) {
        LOG::logline("!! Failed to create grass instance buffer");
        return false;
    }

    return true;
}

void DistantLand::release() {
#ifdef MGE_RTX
    if (!RetainedWorld::shutdown()) {
        LOG::logline("RetainedWorld: renderer release deferred until retained teardown succeeds");
        return;
    }
#endif
    if (!ready) {
        return;
    }

    LOG::logline("-- Renderer unloading");

    recordMW.clear();
    recordSky.clear();

    PostShaders::release();
    FixedFunctionShader::release();

    DistantLandShare::mapWorldSpaces.clear();

    for (auto& iM : meshCollectionStatics) {
        iM.vb->Release();
        iM.ib->Release();
        iM.tex->Release();
    }
    meshCollectionStatics.clear();

    DistantLandShare::LandQuadTree.Clear();
    for (auto& iM : meshCollectionLand) {
        iM.vb->Release();
        iM.ib->Release();
        // A shared texture is used for land, and is released below
    }
    meshCollectionLand.clear();

    // Architecture B (task 8.5): release every resident composite's D3D9 default-pool texture
    // on renderer teardown (they must be freed before the device is, exactly as texWorldColour
    // below is). A no-op when composite streaming never activated. The StreamVisibleComposites
    // shared-vector channels are intentionally NOT freed here: they mirror the persistent
    // visible-mesh vectors (visLandShared etc.), which likewise survive release() and are
    // re-bound on the next init(). hasCompositeSet is cleared so a subsequent Old_Format
    // re-init can't run the streamer against stale channels; the cache stays empty until the
    // next initCompositeStreaming re-activates it.
    compositeCache.releaseAll();
    compositeCache.init(nullptr);
    hasCompositeSet = false;
    compositeDefaultEdge = 0;

    if (texWorldColour) {
        texWorldColour->Release();
        texWorldColour = nullptr;
        texWorldNormals->Release();
        texWorldNormals = nullptr;
        texWorldDetail->Release();
        texWorldDetail = nullptr;
    }

    BSA::clearTextureCache();

    if (Configuration.MGEFlags & DYNAMIC_RIPPLES) {
        surfRain->Release();
        surfRain = nullptr;
        texRain->Release();
        texRain = nullptr;
        surfRipples->Release();
        surfRipples = nullptr;
        texRipples->Release();
        texRipples = nullptr;
        surfRippleBuffer->Release();
        surfRippleBuffer = nullptr;
        texRippleBuffer->Release();
        texRippleBuffer = nullptr;
        vbWaveSim->Release();
        vbWaveSim = nullptr;
    }

    LandDecl->Release();
    LandDecl = nullptr;
    StaticDecl->Release();
    StaticDecl = nullptr;
    WaterDecl->Release();
    WaterDecl = nullptr;
    GrassDecl->Release();
    GrassDecl = nullptr;

    texShadow->Release();
    texShadow = nullptr;
    texSoftShadow->Release();
    texSoftShadow = nullptr;
    surfShadowZ->Release();
    surfShadowZ = nullptr;

    texWater->Release();
    texWater = nullptr;
    texReflection->Release();
    texReflection = nullptr;
    surfReflectionZ->Release();
    surfReflectionZ = nullptr;
    vbWater->Release();
    vbWater = nullptr;
    ibWater->Release();
    ibWater = nullptr;
    vbGrassInstances->Release();
    vbGrassInstances = nullptr;

#ifdef MGE_RTX
    if (vbWaterFFP) {
        vbWaterFFP->Release();
        vbWaterFFP = nullptr;
    }
    if (waterFFPDecl) {
        waterFFPDecl->Release();
        waterFFPDecl = nullptr;
    }
    if (cachedWaterTex) {
        cachedWaterTex->Release();
        cachedWaterTex = nullptr;
    }
#endif

    vbFullFrame->Release();
    vbFullFrame = nullptr;
    vbClipCube->Release();
    vbClipCube = nullptr;

    texDepthFrame->Release();
    texDepthFrame = nullptr;
    surfDepthDepth->Release();
    surfDepthDepth = nullptr;

    effectPool->Release();
    effectPool = nullptr;
    effectShadow->Release();
    effectShadow = nullptr;
    effectDepth->Release();
    effectDepth = nullptr;
    effect->Release();
    effect = nullptr;

    LOG::logline("-- Renderer unloaded");
    LOG::flush();

    fogNearEnd = 0;
    device = nullptr;
    ready = false;
}
