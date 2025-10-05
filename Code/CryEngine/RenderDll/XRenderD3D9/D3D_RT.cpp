#include "StdAfx.h"

#include "D3D_RT.h"
#include "DriverD3D.h"
#include "D3D_Shader.h"
#include <utility>
#include <string>
#include <d3dcompiler.h>
#include <CryGame/IGameFramework.h>
#include "Common/Textures/Texture.h"
#include "Common/RendererResources.h"


#ifndef ALIGN
#define ALIGN(value, alignment) (((value) + ((alignment) - 1)) & ~((alignment) - 1))
#endif

#ifndef D3D12_RESOURCE_SUBRESOURCE_ALL
#define D3D12_RESOURCE_SUBRESOURCE_ALL D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
#endif


// --- Composition state (custom HLSL fullscreen pass) ---
static ID3D12RootSignature* g_pRT_ComposeRS = nullptr;
static ID3D12PipelineState* g_pRT_ComposePSO = nullptr;
static ID3D12DescriptorHeap* g_pRT_ComposeSrvHeap = nullptr; // 2 SRVs: GI, Reflection
static ID3D12DescriptorHeap* g_pRT_ComposeRtvHeap = nullptr; // 1 RTV: HDR
static DXGI_FORMAT            g_RT_ComposeRTVFormat = DXGI_FORMAT_UNKNOWN;

// globals (near other static DXR helpers)
static ID3D12RootSignature* g_pRT_IrrRS = nullptr;
static ID3D12PipelineState* g_pRT_IrrPSO = nullptr;
static ID3D12Resource* g_pRT_IrradianceCube = nullptr; // holds generated diffuse irradiance


// Add CPU-only UAV heap globals for safe ClearUnorderedAccessView* calls
static ID3D12DescriptorHeap* g_pRT_CpuUavHeap = nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE g_RT_StatsCpuHandle = { 0 };

// Small dummy 1x1 texture used to populate SRV table (t1..t11)
static ID3D12Resource* g_pRT_NullSrvTex2D = nullptr;

static bool RT_IsLevelStreamingBusy();

// Mirror heap for ClearUAV CPU handles (slots align with [1..4] u0..u3 in shader-visible heap)
static ID3D12DescriptorHeap* g_pRT_ClearCpuHeap = nullptr;
static UINT g_RT_ClearCpuInc = 0;


// Put near the "Dummy 1x1 SRV texture" section
static bool g_RT_NullSrvInitialized = false;

static HRESULT RT_Init1x1RGBA8White(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12Resource** ppTex)
{
    if (!dev || !q || !ppTex) return E_INVALIDARG;
    if (!*ppTex) return E_POINTER;
    if (g_RT_NullSrvInitialized) return S_OK;

    // Create a small upload buffer holding 1 pixel RGBA8 = (255,255,255,255)
    const UINT64 rowPitch = 256; // D3D12 requires row pitch alignment for textures
    const UINT64 totalSize = rowPitch * 1;
    D3D12_HEAP_PROPERTIES hup{}; hup.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upr{}; upr.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; upr.Width = totalSize; upr.Height = 1; upr.DepthOrArraySize = 1; upr.MipLevels = 1; upr.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* upload = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hup, D3D12_HEAP_FLAG_NONE, &upr, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(hr) || !upload) return hr ? hr : E_FAIL;

    // Fill upload with 1 white texel at start of row
    {
        uint8_t* p = nullptr; D3D12_RANGE rr{ 0,0 };
        if (SUCCEEDED(upload->Map(0, &rr, reinterpret_cast<void**>(&p))) && p)
        {
            p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;
            D3D12_RANGE wr{ 0, 4 }; upload->Unmap(0, &wr);
        }
    }

    // Build copy CL
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl))))
    {
        SAFE_RELEASE(upload);
        SAFE_RELEASE(alloc);
        SAFE_RELEASE(cl);
        return E_FAIL;
    }

    // Describe source and destination copy
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = 1;
    src.PlacedFootprint.Footprint.Height = 1;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = (UINT)rowPitch;

    dst.pResource = *ppTex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    // Transition dest COMMON -> COPY_DEST (best effort; assume COMMON)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = *ppTex;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Transition dest COPY_DEST -> SRV readable
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = *ppTex;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    cl->Close();
    ID3D12CommandList* lists[] = { cl };
    q->ExecuteCommandLists(1, lists);

    // Wait
    ID3D12Fence* fence = nullptr;
    if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence)
    {
        const UINT64 fv = 1;
        if (SUCCEEDED(q->Signal(fence, fv)))
        {
            HANDLE e = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (e) { fence->SetEventOnCompletion(fv, e); WaitForSingleObject(e, 5000); CloseHandle(e); }
        }
        fence->Release();
    }

    SAFE_RELEASE(cl);
    SAFE_RELEASE(alloc);
    SAFE_RELEASE(upload);

#if defined(_DEBUG) || defined(PROFILE)
    (*ppTex)->SetName(L"DXR_NullSrvTex2D_1x1_White");
#endif

    g_RT_NullSrvInitialized = true;
    return S_OK;
}

// AO output texture (R32_FLOAT) for u3
static ID3D12Resource* g_pRT_AOOutput = nullptr;

static bool g_RT_AOOutputOwned = false;

static bool g_dxrComposeRegistered = false;
static CD3D_RT* g_pDXRForPostCompose = nullptr;

// Add near other temp collections (next to g_RT_TempHeaps)
static std::vector<ID3D12DescriptorHeap*> g_RT_TempHeaps;

// Add retired heap tracking (GPU-lifetime safe release)
struct RT_RetiredHeap { ID3D12DescriptorHeap* heap; UINT64 fence; };
static std::vector<RT_RetiredHeap> g_RT_RetiredHeaps;

// Forward declaration to fix C3861 (used in RT_ComposeDXRToHDR)
static ID3D12Resource* RT_GetNativeFromCTexture(CTexture* pTex);

// Forward declaration to fix C3861 (used in CreateLevelGeometryBLASAndTLAS)
static void RT_AppendTerrainPatch(std::vector<CD3D_RT::SafeGeometryData>& outGeometry);


static ID3D12DescriptorHeap* g_pRT_CpuUavHeapRefl = nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE g_RT_ReflCpuHandle = { 0 };

static std::vector<ID3D12Resource*> g_RT_TempUploads;


// Forward declaration to fix C3861 (used in RT_ComposeDXRToHDR)
static ID3D12Resource* RT_GetNativeFromCTexture(CTexture* pTex);

// Forward declaration to fix C3861 (used in CreateLevelGeometryBLASAndTLAS)
static void RT_AppendTerrainPatch(std::vector<CD3D_RT::SafeGeometryData>& outGeometry);


// Add these near other composition globals (top of file)
static bool g_RT_ComposeDebugMacro = false;          // reflects r_DXR_ComposeDebug
static bool g_RT_ComposeOverwriteNoBlend = false;    // reflects r_DXR_ComposeOverwrite

// Forward declaration to fix C3861 (used in CreateLevelGeometryBLASAndTLAS)
static void RT_AppendTerrainPatch(std::vector<CD3D_RT::SafeGeometryData>& outGeometry);


CD3D_RT::CD3D_RT(CGraphicsPipeline& pGraphicsPipeline)
    : CGraphicsPipelineStage(pGraphicsPipeline)
    , m_pDevice(nullptr)
    , m_pCommandQueue(nullptr)
    , m_pTopLevelAS(nullptr)
    , m_pRaytracingPSO(nullptr)
    , m_pGlobalRootSignature(nullptr)
    , m_pStateObjectProperties(nullptr)
    , m_pRayGenShaderTable(nullptr)
    , m_pMissShaderTable(nullptr)
    , m_pHitGroupShaderTable(nullptr)
    , m_pRaytracingOutput(nullptr)
    , m_pDescriptorHeap(nullptr)
    , m_pRayGenShaderID(nullptr)
    , m_pMissShaderID(nullptr)
    , m_pClosestHitShaderID(nullptr)
    , m_shadersCompiled(false)
    , m_pRayStatsBuffer(nullptr)           // NEW
    , m_pRayStatsReadbackBuffer(nullptr)   // NEW
    , m_pReflectionOutput(nullptr)
{
    // DIAGNOSTIC: Log construction
    CryLogAlways("[D3D_RT] Constructor: Creating ray tracing pipeline stage at %p", this);

    m_descriptorSize = 0;
    m_outputWidth = m_outputHeight = 0;
    m_pConstantsBuffer = nullptr;
    m_frameFence = nullptr;
    m_fenceEvent = nullptr;
    m_lastSignaledFence = 0;
    m_frameIndex = 0;
    m_statsFirstUse = true;

    CryLogAlways("[D3D_RT] Constructor: Ray tracing pipeline stage created successfully");
}

CD3D_RT::~CD3D_RT()
{
    Shutdown();
}

// add near other globals (top of file, after other statics)
static D3D12_RESOURCE_STATES g_RT_GIState = D3D12_RESOURCE_STATE_COMMON;
static D3D12_RESOURCE_STATES g_RT_ReflState = D3D12_RESOURCE_STATE_COMMON;
static D3D12_RESOURCE_STATES g_RT_AOState = D3D12_RESOURCE_STATE_COMMON;


// Add next to RTV/UAV helpers
static DXGI_FORMAT RT_TypelessToTypedSRV(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:       return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:       return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:   return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:    return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:            return DXGI_FORMAT_R32_FLOAT;
    default:                                   return f; // already typed (e.g., R11G11B10_FLOAT)
    }
}

// small helper to transition only when needed and update tracked state
static void RT_TransitionTracked(
    ID3D12GraphicsCommandList4* cl,
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES& trackedState,
    D3D12_RESOURCE_STATES       newState)
{
    if (!cl || !res) return;
    if (trackedState == newState) return;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = trackedState;
    b.Transition.StateAfter = newState;
    cl->ResourceBarrier(1, &b);
    trackedState = newState;
}

static HRESULT RT_CompileHlslFromPak(
    const char* relPath,
    const char* entryPoint,
    const char* target,
    const D3D_SHADER_MACRO* macros,
    UINT flags,
    ID3DBlob** ppBlobOut,
    std::string* outUsedPath,
    ID3DBlob** ppErrOut);




static HRESULT RT_EnsureIrradiancePipeline(ID3D12Device* dev)
{
    if (g_pRT_IrrRS && g_pRT_IrrPSO) return S_OK;
    if (!dev) return E_INVALIDARG;

    // Root: [0] SRV table t0, [1] UAV table u0, [2] CBV b0
    D3D12_DESCRIPTOR_RANGE1 sRange{}; sRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; sRange.NumDescriptors = 1; sRange.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE1 uRange{}; uRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uRange.NumDescriptors = 1; uRange.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER1 params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &sRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &uRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 0;
    params[2].Descriptor.RegisterSpace = 0;
    params[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = _countof(params);
    rs.Desc_1_1.pParameters = params;
    rs.Desc_1_1.NumStaticSamplers = 1;
    rs.Desc_1_1.pStaticSamplers = &samp;
    rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rs, &blob, &err);
    if (FAILED(hr))
    {
        if (err) { CryLogAlways("[DXR][Irr] RootSig serialize: %s", (const char*)err->GetBufferPointer()); err->Release(); }
        return hr;
    }
    hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_pRT_IrrRS));
    blob->Release();
    if (FAILED(hr)) return hr;

    // Compile compute shader (use BOM/UTF16-safe loader)
    ID3DBlob* cs = nullptr; err = nullptr;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    std::string usedPath;
    hr = RT_CompileHlslFromPak(
        "Engine/Shaders/HWScripts/CryFX/DXRIrradianceCS.hlsl",
        "main", "cs_5_0",
        nullptr, flags, &cs, &usedPath, &err);

    if (FAILED(hr))
    {
        CryLogAlways("[DXR][Irr] Compile failed 0x%08x %s", hr, err ? (const char*)err->GetBufferPointer() : "");
        SAFE_RELEASE(err);
        SAFE_RELEASE(g_pRT_IrrRS);
        return hr;
    }
    SAFE_RELEASE(err);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_pRT_IrrRS;
    pso.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    hr = dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pRT_IrrPSO));
    SAFE_RELEASE(cs);
    if (FAILED(hr)) { SAFE_RELEASE(g_pRT_IrrRS); return hr; }

    return S_OK;
}


// Add near other globals (e.g. after RT_EnsureDummyUAVs)
static ID3D12Resource* g_pRT_ZeroUpload = nullptr;
static UINT64          g_RT_ZeroUploadBytes = 0;


struct IrradianceCB
{
    UINT Face, OutDim, SampleCount, _pad;
};

// Add near g_RT_ComposeRTVFormat
static UINT g_RT_ComposeSampleCount = 1;

static ID3D12Resource* RT_BuildIrradianceCube(ID3D12Device* dev, ID3D12CommandQueue* queue, ID3D12Resource* srcCube, UINT outDim = 64, UINT sampleCount = 64)
{
    if (!dev || !queue || !srcCube) return nullptr;
    if (FAILED(RT_EnsureIrradiancePipeline(dev))) return nullptr;

    // Create destination cube (RGBA16F, UAV|SRV)
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = outDim;
    rd.Height = outDim;
    rd.DepthOrArraySize = 6;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* dst = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dst))))
        return nullptr;

    // Shader-visible heap: t0 (src cube SRV) + u0 (dst UAV array)
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.NumDescriptors = 2;
    ID3D12DescriptorHeap* heap = nullptr;
    if (FAILED(dev->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap))))
    {
        dst->Release();
        return nullptr;
    }
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();

    // t0 SRV (cube)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = 1;
        srv.Format = srcCube->GetDesc().Format;
        if (srv.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS) srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dev->CreateShaderResourceView(srcCube, &srv, cpu);
    }

    // u0 UAV (2D array of 6 slices)
    cpu.ptr += inc;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Format = rd.Format;
        uav.Texture2DArray.ArraySize = 6;
        uav.Texture2DArray.FirstArraySlice = 0;
        uav.Texture2DArray.MipSlice = 0;
        dev->CreateUnorderedAccessView(dst, nullptr, &uav, cpu);
    }

    // Command allocator + list
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl))))
    {
        SAFE_RELEASE(heap); dst->Release(); SAFE_RELEASE(alloc); SAFE_RELEASE(cl);
        return nullptr;
    }

    // Root + heap
    ID3D12DescriptorHeap* heaps[] = { heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_pRT_IrrRS);
    cl->SetPipelineState(g_pRT_IrrPSO);

    // Root tables
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
    cl->SetComputeRootDescriptorTable(0, gpu);             // t0
    gpu.ptr += inc;
    cl->SetComputeRootDescriptorTable(1, gpu);             // u0

    // Small upload CB for per-face parameters
    ID3D12Resource* cb = nullptr;
    {
        D3D12_HEAP_PROPERTIES hup{}; hup.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbd{}; cbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbd.Width = 256; cbd.Height = 1; cbd.DepthOrArraySize = 1; cbd.MipLevels = 1;
        cbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hup, D3D12_HEAP_FLAG_NONE, &cbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cb))))
        {
            cl->Close(); SAFE_RELEASE(cl); SAFE_RELEASE(alloc); SAFE_RELEASE(heap); dst->Release();
            return nullptr;
        }
    }

    const UINT groups = (outDim + 7u) / 8u;
    for (UINT face = 0; face < 6; ++face)
    {
        IrradianceCB data{ face, outDim, sampleCount, 0 };
        void* p = nullptr; D3D12_RANGE rr{ 0,0 };
        if (SUCCEEDED(cb->Map(0, &rr, &p)) && p)
        {
            memcpy(p, &data, sizeof(data));
            D3D12_RANGE wr{ 0, sizeof(data) }; cb->Unmap(0, &wr);
        }
        cl->SetComputeRootConstantBufferView(2, cb->GetGPUVirtualAddress());
        cl->Dispatch(groups, groups, 1);
    }

    // Transition result UAV -> SRV for sampling by DXR
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dst;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    cl->Close();

    // Execute and wait (one-shot)
    ID3D12CommandList* lists[] = { cl };
    queue->ExecuteCommandLists(1, lists);

    ID3D12Fence* fence = nullptr;
    if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence)
    {
        const UINT64 fv = 1;
        if (SUCCEEDED(queue->Signal(fence, fv)))
        {
            HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (evt) { fence->SetEventOnCompletion(fv, evt); WaitForSingleObject(evt, 30000); CloseHandle(evt); }
        }
        fence->Release();
    }

    SAFE_RELEASE(cb);
    SAFE_RELEASE(cl);
    SAFE_RELEASE(alloc);
    SAFE_RELEASE(heap);

    return dst;
}




static DXGI_FORMAT RT_TypelessToTypedRTV(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:   return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:       return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:       return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:    return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:            return DXGI_FORMAT_R32_FLOAT;
    default:                                   return f; // already typed (e.g., R11G11B10_FLOAT)
    }
}

static HRESULT RT_EnsureComposeRootSig(ID3D12Device* dev)
{
    if (g_pRT_ComposeRS) return S_OK;
    if (!dev) return E_INVALIDARG;

    // [0] CBV b0 (params), [1] SRV table t0..t3 (GI, Reflection, AO, Shadow), static s0
    D3D12_DESCRIPTOR_RANGE1 srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 4;
    srvRange.BaseShaderRegister = 0; // t0

    D3D12_ROOT_PARAMETER1 params[2]{};

    // b0
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // t0..t3
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderRegister = 0; // s0
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = _countof(params);
    rs.Desc_1_1.pParameters = params;
    rs.Desc_1_1.NumStaticSamplers = 1;
    rs.Desc_1_1.pStaticSamplers = &samp;

    // Remove HEAP_DIRECTLY_INDEXED flags for this simple table-based binding
    rs.Desc_1_1.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rs, &blob, &err);
    if (FAILED(hr))
    {
        if (err) { CryLogAlways("[Compose] RootSig serialize: %s", (const char*)err->GetBufferPointer()); err->Release(); }
        return hr;
    }
    hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_pRT_ComposeRS));
    blob->Release();
    return hr;
}


static inline Matrix44A RT_Transpose(const Matrix44A& m)
{
    // Matrix44A already has GetTransposed(); keep in a helper for clarity
    return m.GetTransposed();
}

static HRESULT RT_EnsureComposeRtvHeap(ID3D12Device* dev)
{
    if (g_pRT_ComposeRtvHeap) return S_OK;
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    d.NumDescriptors = 1;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // RTV heaps are always CPU-only
    return dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_pRT_ComposeRtvHeap));
}


static HRESULT RT_EnsureComposePSO(ID3D12Device* dev, DXGI_FORMAT rtvFmt, UINT sampleCount)
{
    if (!dev || !g_pRT_ComposeRS)
        return E_INVALIDARG;

    // Always rebuild to pick up CVar and shader changes
    SAFE_RELEASE(g_pRT_ComposePSO);
    g_RT_ComposeRTVFormat = rtvFmt;
    g_RT_ComposeSampleCount = sampleCount ? sampleCount : 1;

    // Hook up debug/overwrite from globals (set via CVars elsewhere)
    const bool debugEnabled = g_RT_ComposeDebugMacro;
    const bool overwrite = g_RT_ComposeOverwriteNoBlend;

    // Shader defines
    D3D_SHADER_MACRO macros[3];
    UINT mc = 0;
    if (debugEnabled)
        macros[mc++] = { "DXR_COMPOSE_DEBUG", "1" };
    static int s_rev = 0;
    char revBuf[16];
    _snprintf_s(revBuf, _TRUNCATE, "%d", ++s_rev);
    macros[mc++] = { "DXR_COMPOSE_REV", revBuf };
    macros[mc] = { nullptr, nullptr };

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* err = nullptr;

    // Fullscreen triangle VS with no vertex buffer
    static const char* kFullscreenVS = R"(
struct VSOut { float4 pos: SV_Position; float2 uv: TEXCOORD0; };
VSOut VS_Fullscreen(uint vid : SV_VertexID)
{
    float2 pos = float2( (vid == 2) ? -1.0 : 3.0, (vid == 1) ? -1.0 : 3.0 );
    VSOut o;
    o.pos = float4(pos, 0.0, 1.0);
    o.uv  = float2(0.5f * pos.x + 0.5f, 1.0f - (0.5f * pos.y + 0.5f));
    return o;
}
)";

    // Compile VS (inline source)
    HRESULT hr = D3DCompile(kFullscreenVS, strlen(kFullscreenVS), "DXRComposeVS", nullptr, nullptr,
        "VS_Fullscreen", "vs_5_0", flags, 0, &vs, &err);
    if (FAILED(hr))
    {
        if (err) { CryLogAlways("[Compose] VS compile failed: %s", (const char*)err->GetBufferPointer()); err->Release(); }
        return hr;
    }
    SAFE_RELEASE(err);

    // Compile PS from pak or fallback locations
    std::string usedPS;
    const char* rel = "Engine/Shaders/HWScripts/CryFX/DXRCompose.hlsl";
    hr = RT_CompileHlslFromPak(rel, "PSMain", "ps_5_0", macros, flags, &ps, &usedPS, &err);
    if (FAILED(hr))
    {
        SAFE_RELEASE(vs);
        if (err) { CryLogAlways("[Compose] PS compile failed: %s", (const char*)err->GetBufferPointer()); err->Release(); }
        return hr;
    }
    SAFE_RELEASE(err);

    // Blend: overwrite (no blend) or additive (ONE, ONE)
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = overwrite ? FALSE : TRUE;
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = overwrite ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = overwrite ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC rast = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rast.CullMode = D3D12_CULL_MODE_NONE;

    D3D12_DEPTH_STENCIL_DESC ds = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    ds.DepthEnable = FALSE;
    ds.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_pRT_ComposeRS;
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState = rast;
    pso.DepthStencilState = ds;
    pso.InputLayout = { nullptr, 0 }; // fullscreen triangle, no vertex input
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtvFmt;
    pso.SampleDesc.Count = g_RT_ComposeSampleCount;

    hr = dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pRT_ComposePSO));
    SAFE_RELEASE(vs);
    SAFE_RELEASE(ps);
    return hr;
}


static HRESULT RT_EnsureComposeSrvHeap(ID3D12Device* dev)
{
    if (g_pRT_ComposeSrvHeap) return S_OK;
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.NumDescriptors = 4;   // CHANGED: was 2 (GI, Reflection, AO, Shadow)
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    return dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_pRT_ComposeSrvHeap));
}



static bool RT_IsShaderVisibleHeap(ID3D12DescriptorHeap* heap)
{
    if (!heap) return false;
    D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
    return (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
}

bool CD3D_RT::TryBeginCompose(ID3D12Resource* pTarget)
{
    const uint64 frameId = gcpRendD3D ? (uint64)gcpRendD3D->GetRenderFrameID() : 0;
    static uint64 s_composeStamp = ~0ull;
    if (frameId == s_composeStamp)
        return false;
    if (!m_pRaytracingOutput)
        return false;

    s_composeStamp = frameId;
    m_lastComposeFrameId = frameId;
    m_lastComposeTarget = pTarget;
    return true;
}


// Helper: wrap 24.0h to [0,24)
static float RT_WrapHour24(float hour)
{
    if (!isfinite(hour)) return 0.0f;
    hour = fmodf(hour, 24.0f);
    if (hour < 0.0f) hour += 24.0f;
    // Treat exactly 24.0 as 0.0 to avoid edge artifacts
    if (fabsf(hour - 24.0f) <= 1e-6f) hour = 0.0f;
    return hour;
}


static bool ValidateMeshData(const CD3D_RT::SafeGeometryData& m, std::string& why)
{
    const size_t v = m.vertices.size();
    const size_t i = m.indices.size();

    if (v < 3) { why = "less than 3 vertices"; return false; }
    if (i < 3 || (i % 3) != 0) { why = "index count not multiple of 3"; return false; }
    if (i > 30'000'000) { why = "too many indices"; return false; }

    uint32_t maxIdx = 0;
    for (uint32_t idx : m.indices) maxIdx = (idx > maxIdx ? idx : maxIdx);
    if (maxIdx >= v) {
        char buf[128];
        sprintf_s(buf, "index %u out of range (v=%zu)", maxIdx, v);
        why = buf;
        return false;
    }

    for (const auto& p : m.vertices) {
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) {
            why = "NaN/Inf in vertices"; return false;
        }
    }

    for (size_t t = 0; t + 2 < i && t < 300; t += 3) {
        const Vec3& a = m.vertices[m.indices[t + 0]];
        const Vec3& b = m.vertices[m.indices[t + 1]];
        const Vec3& c = m.vertices[m.indices[t + 2]];
        Vec3 ab = b - a, ac = c - a;
        Vec3 cr = ab.Cross(ac);
        if ((cr.x * cr.x + cr.y * cr.y + cr.z * cr.z) < 1e-20f) {
            why = "degenerate triangles";
            return false;
        }
    }

    return true;
}


// ===== add this small accessor somewhere near the top (after ctor/dtor is fine) =====
CD3D_RT* CD3D_RT::GetForPostCompose()
{
    return g_pDXRForPostCompose;
}

CTexture* CD3D_RT::GetDXRGITexture() const
{
    return m_pTexDXR_GI;
}

CTexture* CD3D_RT::GetDXRReflectionTexture() const
{
    return m_pTexDXR_Refl;
}


static DXGI_FORMAT RT_TypelessToTypedUAV(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:           return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:  return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:                                 return f;
    }
}



static void RT_InsertCameraDebugQuad(std::vector<CD3D_RT::SafeGeometryData>& geo, const CCamera& cam)
{
    CD3D_RT::SafeGeometryData q;
    q.debugName = "DebugQuadNearCam";

    const Vec3 camPos = cam.GetPosition();
    const Vec3 fwd = cam.GetViewdir().GetNormalizedSafe(Vec3(0, 1, 0));
    const Vec3 right = fwd.Cross(Vec3(0, 0, 1)).GetNormalizedSafe(Vec3(1, 0, 0));
    const Vec3 up = right.Cross(fwd).GetNormalizedSafe(Vec3(0, 0, 1));

    const float half = 25.0f;          // 50m wide quad
    const float dist = 5.0f;           // 5m in front of camera
    const Vec3 center = camPos + fwd * dist;

    q.vertices = {
        center + (-right * half - up * half),
        center + (right * half - up * half),
        center + (right * half + up * half),
        center + (-right * half + up * half)
    };
    q.indices = { 0,1,2, 0,2,3 };
    q.worldTransform = Matrix34::CreateIdentity();

    // Lightweight validation
    std::string why;
    if (q.vertices.size() >= 3 && q.indices.size() == 6)
        geo.push_back(q);
}

static void RT_SortKeepNearest(std::vector<CD3D_RT::SafeGeometryData>& geo, const Vec3& camPos, size_t maxKeep)
{
    std::stable_sort(geo.begin(), geo.end(),
        [&camPos](const CD3D_RT::SafeGeometryData& a, const CD3D_RT::SafeGeometryData& b)
        {
            // Use first vertex as approximate center (fast)
            const float da = (a.vertices.empty() ? 1e30f : (a.vertices[0] - camPos).GetLengthSquared());
            const float db = (b.vertices.empty() ? 1e30f : (b.vertices[0] - camPos).GetLengthSquared());
            return da < db;
        });
    if (geo.size() > maxKeep)
        geo.resize(maxKeep);
}

// Helper: get native ID3D12Resource from CTexture
static ID3D12Resource* RT_GetNativeFromCTexture(CTexture* pTex)
{
    if (!pTex) return nullptr;
    auto* pDevTex = pTex->GetDevTexture();     if (!pDevTex)  return nullptr;
    auto* pBaseTex = pDevTex->GetBaseTexture(); if (!pBaseTex) return nullptr;
    ICryDX12Resource* pDXR = DX12_EXTRACT_ICRYDX12RESOURCE(pBaseTex);
    return pDXR ? pDXR->GetD3D12Resource() : nullptr;
}



// Add near other helpers
static HRESULT RT_CompileHlslFromPak(
    const char* relPath,
    const char* entryPoint,
    const char* target,
    const D3D_SHADER_MACRO* macros,
    UINT flags,
    ID3DBlob** ppBlobOut,
    std::string* outUsedPath,
    ID3DBlob** ppErrOut)
{
    if (!gEnv || !gEnv->pCryPak || !ppBlobOut) return E_FAIL;
    *ppBlobOut = nullptr;

    const char* candidates[] = {
        "Engine/Shaders/HWScripts/CryFX/DXRCompose.hlsl",
        relPath,
        "Engine/Shaders/DXR/DXRCompose.hlsl",
        "Shaders/HWScripts/CryFX/DXRCompose.hlsl"
    };

    for (const char* cand : candidates)
    {
        if (!cand || !gEnv->pCryPak->IsFileExist(cand))
            continue;

        FILE* f = gEnv->pCryPak->FOpen(cand, "rb");
        if (!f) continue;

        gEnv->pCryPak->FSeek(f, 0, SEEK_END);
        const long size = gEnv->pCryPak->FTell(f);
        gEnv->pCryPak->FSeek(f, 0, SEEK_SET);

        std::vector<uint8_t> data;
        data.resize((size_t)std::max<long>(size, 0L));
        if (size > 0)
            gEnv->pCryPak->FReadRaw(data.data(), 1, (size_t)size, f);
        gEnv->pCryPak->FClose(f);

        // Normalize encoding: strip BOM and convert UTF-16 to UTF-8 if needed.
        const auto is_utf8_bom = [&](const std::vector<uint8_t>& d) {
            return d.size() >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF;
            };
        const auto is_utf16_le_bom = [&](const std::vector<uint8_t>& d) {
            return d.size() >= 2 && d[0] == 0xFF && d[1] == 0xFE;
            };
        const auto is_utf16_be_bom = [&](const std::vector<uint8_t>& d) {
            return d.size() >= 2 && d[0] == 0xFE && d[1] == 0xFF;
            };

        std::vector<uint8_t> utf8;
        const uint8_t* src = data.data();
        size_t srcSize = data.size();

        if (is_utf8_bom(data))
        {
            // Strip UTF-8 BOM
            src += 3;
            srcSize -= 3;
            utf8.assign(src, src + srcSize);
        }
        else if (is_utf16_le_bom(data) || is_utf16_be_bom(data))
        {
            // Convert UTF-16 (LE/BE) to UTF-8
            // Build a wchar_t buffer from the UTF-16 bytes
            std::wstring w;
            w.reserve((srcSize - 2) / 2);

            if (is_utf16_le_bom(data))
            {
                for (size_t i = 2; i + 1 < srcSize; i += 2)
                {
                    wchar_t ch = wchar_t(src[i] | (uint16_t(src[i + 1]) << 8));
                    w.push_back(ch);
                }
            }
            else
            {
                for (size_t i = 2; i + 1 < srcSize; i += 2)
                {
                    wchar_t ch = wchar_t((uint16_t(src[i]) << 8) | src[i + 1]);
                    w.push_back(ch);
                }
            }

            if (!w.empty() && w.back() == L'\0')
                w.pop_back();

            // Convert wide -> UTF-8 bytes
            int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            if (needed <= 0) continue;
            utf8.resize(needed);
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), reinterpret_cast<char*>(utf8.data()), needed, nullptr, nullptr);
        }
        else
        {
            // Assume ASCII/UTF-8 already
            utf8.assign(src, src + srcSize);
        }

        ID3DBlob* shader = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT hr = D3DCompile(
            utf8.data(), utf8.size(),
            cand, macros, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint, target, flags, 0,
            &shader, &err);

        if (SUCCEEDED(hr))
        {
            if (ppErrOut) { SAFE_RELEASE(*ppErrOut); *ppErrOut = nullptr; }
            *ppBlobOut = shader;
            if (outUsedPath) *outUsedPath = cand;
            SAFE_RELEASE(err);
            return S_OK;
        }
        else
        {
            if (ppErrOut)
            {
                SAFE_RELEASE(*ppErrOut);
                *ppErrOut = err;
            }
            else
            {
                SAFE_RELEASE(err);
            }
            SAFE_RELEASE(shader);
            // try next candidate
        }
    }

    return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

// --- Denoiser globals (near other static helpers) ---
static ID3D12RootSignature* g_pRT_DenoiseRS = nullptr;
static ID3D12PipelineState* g_pRT_DenoisePSO = nullptr;

// History + caches
static ID3D12Resource* g_pRT_GIHistory[2] = { nullptr, nullptr };
static ID3D12Resource* g_pRT_ReflHistory[2] = { nullptr, nullptr };
static uint32_t g_RT_HistoryParity = 0;     // 0/1 -> write index; read = write^1
static bool     g_RT_HistoryValid = false; // becomes true after first denoise dispatch
static ID3D12Resource* g_pRT_GICache = nullptr; // per-frame copy of noisy GI
static ID3D12Resource* g_pRT_ReflCache = nullptr; // per-frame copy of noisy Refl


void CD3D_RT::ClearUAVSafely(ID3D12GraphicsCommandList* pCL,
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
    ID3D12Resource* pResource,
    const UINT clearValues[4])
{
    // Requirements:
    // - gpuHandle must come from our shader-visible heap (m_pDescriptorHeap)
    // - that heap must be currently bound on the command list
    // - cpuHandle should be a matching descriptor in a CPU heap (mirror)
    if (!pCL || !m_pDevice || !m_pDescriptorHeap || !pResource)
        return;
    if (gpuHandle.ptr == 0 || cpuHandle.ptr == 0)
        return;

    // Always (re-)bind our shader-visible heap to satisfy ClearUnorderedAccessView* requirement
    ID3D12DescriptorHeap* heaps[] = { m_pDescriptorHeap };
    pCL->SetDescriptorHeaps(1, heaps);

    // Clear UAV (Uint variant by default)
    const UINT zero[4] = { 0,0,0,0 };
    const UINT* vals = clearValues ? clearValues : zero;
    pCL->ClearUnorderedAccessViewUint(gpuHandle, cpuHandle, pResource, vals, 0, nullptr);
}


void CD3D_RT::DebugLogAllocatorState(const char* when)
{
    if (!m_frameFence)
    {
        //CryLogAlways("[D3D_RT][AllocDiag][%s] (no frame fence yet)", when ? when : "?");
        return;
    }

    const UINT64 completed = m_frameFence->GetCompletedValue();
    // Use fully qualified nested type & constant
    /*
    CryLogAlways(
        "[D3D_RT][AllocDiag][%s] frameIdx=%llu alloc=%p fenced=%llu usedOnce=%d completed=%llu lastSignaled=%llu lastDispatch=%llu lastASBuild=%llu retired=%zu",
        when ? when : "?",
        (unsigned long long)m_frameIndex,
        ctx.pAllocator,
        (unsigned long long)ctx.fenceValue,
        (int)ctx.usedOnce,
        (unsigned long long)completed,
        (unsigned long long)m_lastSignaledFence,
        (unsigned long long)m_lastDispatchFence,
        (unsigned long long)m_lastASBuildFence,
        m_retiredAllocators.size());*/
}


// Strengthen RT_SafeRelease
void CD3D_RT::RT_SafeRelease(ID3D12Resource*& r, const char* tag)
{
    if (!r) return;

    // Heuristic: if an AS build happened more recently than the frame fence completion, wait before deciding.
    if (m_frameFence && m_lastASBuildFence)
    {
        const UINT64 completedNow = m_frameFence->GetCompletedValue();
        if (completedNow < m_lastASBuildFence)
        {
            if (!m_fenceEvent)
                m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (m_fenceEvent)
            {
                m_frameFence->SetEventOnCompletion(m_lastASBuildFence, m_fenceEvent);
                WaitForSingleObject(m_fenceEvent, 10000);
            }
        }
    }

    UINT64 protectFence = std::max(std::max(m_lastDispatchFence, m_lastSignaledFence), m_lastASBuildFence);

    if (!m_frameFence)  // No fencing yet -> block once and release
    {
        RT_WaitForGpuIdle("SafeRelease_NoFrameFence");
        protectFence = 0;
    }

    bool canImmediate = false;
    if (m_frameFence && protectFence)
    {
        const UINT64 completed = m_frameFence->GetCompletedValue();
        canImmediate = (completed >= protectFence);
    }
    else if (!protectFence)
    {
        RT_WaitForGpuIdle("SafeRelease_ProtectFence0");
        canImmediate = true;
    }

    if (canImmediate)
    {
        CryLogAlways("[D3D_RT] RT_SafeRelease(%s) immediate %p (protectFence=%llu)",
            tag ? tag : "?", r, (unsigned long long)protectFence);
        r->Release();
    }
    else
    {
        const UINT64 deferFence = protectFence + 1;
        RT_DeferredRelease entry;
        entry.fenceValue = deferFence;
        entry.resources.push_back(r);
        m_deferred.push_back(entry);
        CryLogAlways("[D3D_RT] RT_SafeRelease(%s) deferred %p until fence >= %llu (currentCompleted=%llu)",
            tag ? tag : "?", r,
            (unsigned long long)deferFence,
            (unsigned long long)m_frameFence->GetCompletedValue());
    }
    r = nullptr;
}

void CD3D_RT::RT_ReclaimRetiredAllocators()
{
    // Reclaim command allocators when the fence has passed
    if (m_frameFence && !m_retiredAllocators.empty())
    {
        const UINT64 completed = m_frameFence->GetCompletedValue();
        for (size_t i = 0; i < m_retiredAllocators.size(); )
        {
            auto& r = m_retiredAllocators[i];
            if (!r.pAlloc)
            {
                m_retiredAllocators.erase(m_retiredAllocators.begin() + i);
                continue;
            }
            if (r.fenceValue && completed >= r.fenceValue)
            {
                r.pAlloc->Release();
                m_retiredAllocators.erase(m_retiredAllocators.begin() + i);
            }
            else
            {
                ++i;
            }
        }
    }

    // Reclaim transient descriptor heaps after the frame fence completes
    if (m_frameFence && !g_RT_RetiredHeaps.empty())
    {
        const UINT64 done = m_frameFence->GetCompletedValue();
        for (size_t i = 0; i < g_RT_RetiredHeaps.size(); )
        {
            auto& h = g_RT_RetiredHeaps[i];
            if (done >= h.fence)
            {
                if (h.heap) h.heap->Release();
                g_RT_RetiredHeaps.erase(g_RT_RetiredHeaps.begin() + i);
            }
            else
            {
                ++i;
            }
        }
    }
}

void CD3D_RT::RT_ReleaseASBuffers(AccelerationStructureBuffers& b, const char* tag)
{
    if (b.pScratch)      RT_SafeRelease(b.pScratch, (std::string(tag) + "_Scratch").c_str());
    if (b.pResult)       RT_SafeRelease(b.pResult, (std::string(tag) + "_Result").c_str());
    if (b.pInstanceDesc) RT_SafeRelease(b.pInstanceDesc, (std::string(tag) + "_InstDesc").c_str());
    b.resultVA = 0;
}


void CD3D_RT::Init()
{
    // CRITICAL FIX: Comprehensive device stability measures
    static std::atomic<bool> s_initializationInProgress(false);
    static std::atomic<bool> s_initializationCompleted(false);
    static std::atomic<bool> s_deviceStabilityFailure(false);
    static std::atomic<uint32_t> s_initCallCount(0);

    uint32_t currentCallCount = s_initCallCount.fetch_add(1);

    // IMMEDIATE EXIT conditions
    if (s_initializationCompleted.load() || s_deviceStabilityFailure.load())
        return;

    if (s_initializationInProgress.load())
    {
        if (currentCallCount <= 5)
            CryLogAlways("[D3D_RT] Init: Initialization already in progress, call #%u", currentCallCount);
        return;
    }

    if (currentCallCount > 50)
    {
        if (currentCallCount == 51)
        {
            CryLogAlways("[D3D_RT] Init: EXCESSIVE INIT CALLS (%u) - marking as device stability failure", currentCallCount);
            s_deviceStabilityFailure = true;
        }
        return;
    }

    bool expected = false;
    if (!s_initializationInProgress.compare_exchange_strong(expected, true))
        return;

    CryLogAlways("[D3D_RT] Init: ===== DEVICE-SAFE RAY TRACING INITIALIZATION ===== (Call #%u)", currentCallCount);

    // CRITICAL FIX: Shadow map and resource limit enforcement BEFORE any device access
    if (gEnv && gEnv->pConsole)
    {
        ICVar* pShadowCache = gEnv->pConsole->GetCVar("r_ShadowsCache");
        if (pShadowCache && pShadowCache->GetIVal() > 4096)
        {
            CryLogAlways("[D3D_RT] Init: CRITICAL DEVICE PROTECTION - Shadow cache %d > 4096, forcing to 4096",
                pShadowCache->GetIVal());
            pShadowCache->Set(4096);
        }
        ICVar* pTexMemBudget = gEnv->pConsole->GetCVar("sys_budget_videomem");
        if (pTexMemBudget && pTexMemBudget->GetIVal() > 4096)
        {
            CryLogAlways("[D3D_RT] Init: DEVICE PROTECTION - Clamping texture memory budget to 4096MB");
            pTexMemBudget->Set(4096);
        }
        ICVar* pRenderWidth = gEnv->pConsole->GetCVar("r_Width");
        ICVar* pRenderHeight = gEnv->pConsole->GetCVar("r_Height");
        if (pRenderWidth && pRenderHeight)
        {
            int width = pRenderWidth->GetIVal();
            int height = pRenderHeight->GetIVal();
            if (width > 4096 || height > 4096)
            {
                CryLogAlways("[D3D_RT] Init: DEVICE PROTECTION - Clamping render resolution from %dx%d to max 4096x4096", width, height);
                if (width > 4096) pRenderWidth->Set(4096);
                if (height > 4096) pRenderHeight->Set(4096);
            }
        }
    }

    if (!gcpRendD3D)
    {
        CryLogAlways("[D3D_RT] Init: gcpRendD3D is null - renderer not ready");
        s_initializationInProgress = false;
        return;
    }

    auto pDevice = gcpRendD3D->GetDevice();
    if (!pDevice)
    {
        CryLogAlways("[D3D_RT] Init: Device not ready, deferring initialization");
        s_initializationInProgress = false;
        return;
    }

    CCryDX12Device* pDX12Device = reinterpret_cast<CCryDX12Device*>(pDevice);
    if (!pDX12Device)
    {
        CryLogAlways("[D3D_RT] Init failed: Could not get D3D12 device wrapper");
        s_deviceStabilityFailure = true;
        s_initializationInProgress = false;
        return;
    }

    ID3D12Device* pNativeDevice = nullptr;
    HRESULT deviceHr = S_OK;
    __try
    {
        pNativeDevice = pDX12Device->GetD3D12Device();
        if (!pNativeDevice)
        {
            CryLogAlways("[D3D_RT] Init failed: Native D3D12 device is null");
            s_deviceStabilityFailure = true;
            s_initializationInProgress = false;
            return;
        }
        deviceHr = pNativeDevice->GetDeviceRemovedReason();
        if (FAILED(deviceHr))
        {
            CryLogAlways("[D3D_RT] Init failed: Device already removed/reset (hr=0x%08x)", deviceHr);
            s_deviceStabilityFailure = true;
            s_initializationInProgress = false;
            return;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        HRESULT hr = pNativeDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
        if (FAILED(hr))
        {
            CryLogAlways("[D3D_RT] Init failed: Device feature support check failed (hr=0x%08x)", hr);
            s_deviceStabilityFailure = true;
            s_initializationInProgress = false;
            return;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] Init failed: Exception during device validation (code: 0x%08x)", GetExceptionCode());
        s_deviceStabilityFailure = true;
        s_initializationInProgress = false;
        return;
    }

    if (m_pDevice || m_pRaytracingPSO || m_shadersCompiled)
    {
        CryLogAlways("[D3D_RT] Init: Cleaning up previous initialization");
        Shutdown();
    }

    ID3D12Device5* pDevice5 = nullptr;
    HRESULT hr = pNativeDevice->QueryInterface(IID_PPV_ARGS(&pDevice5));
    if (FAILED(hr))
    {
        CryLogAlways("[D3D_RT] Init: Device does not support D3D12Device5 interface (hr=0x%08x) - ray tracing not supported", hr);
        s_initializationInProgress = false;
        s_initializationCompleted = true;
        return;
    }

    NCryDX12::CDevice* pDX12NativeDevice = pDX12Device->GetDX12Device();
    if (!pDX12NativeDevice)
    {
        CryLogAlways("[D3D_RT] Init failed: Could not get DX12 native device");
        pDevice5->Release();
        s_deviceStabilityFailure = true;
        s_initializationInProgress = false;
        return;
    }

    ID3D12CommandQueue* pQueue = pDX12NativeDevice->GetScheduler().GetCommandListPool(CMDQUEUE_GRAPHICS).GetD3D12CommandQueue();
    if (!pQueue)
    {
        CryLogAlways("[D3D_RT] Init failed: Could not get command queue");
        pDevice5->Release();
        s_deviceStabilityFailure = true;
        s_initializationInProgress = false;
        return;
    }

    hr = Initialize(pDevice5, pQueue);
    if (FAILED(hr))
    {
        CryLogAlways("[D3D_RT] Init failed: Ray tracing initialization failed (hr=0x%08x)", hr);
        pDevice5->Release();
        s_deviceStabilityFailure = true;
        s_initializationInProgress = false;
        return;
    }

    hr = CompileAndLoadShaders();
    if (FAILED(hr))
    {
        CryLogAlways("[D3D_RT] Init failed: Shader compilation failed (hr=0x%08x)", hr);
        pDevice5->Release();
        s_deviceStabilityFailure = true;
        s_initializationInProgress = false;
        return;
    }

    hr = CreateRayTracingPipeline();
    if (FAILED(hr))
    {
        CryLogAlways("[D3D_RT] Init failed: Pipeline creation failed (hr=0x%08x)", hr);
        pDevice5->Release();
        s_deviceStabilityFailure = true;
        s_initializationInProgress = false;
        return;
    }

    hr = CreateShaderTables();
    if (FAILED(hr))
    {
        CryLogAlways("[D3D_RT] Init failed: Shader table creation failed (hr=0x%08x)", hr);
        pDevice5->Release();
        s_deviceStabilityFailure = true;
        s_initializationInProgress = false;
        return;
    }

    pDevice5->Release();
    pDevice5 = nullptr;

    s_initializationInProgress = false;
    s_initializationCompleted = true;

    CryLogAlways("[D3D_RT] Init: Skipping level geometry BLAS/TLAS build during initialization");
    CryLogAlways("[D3D_RT] Init: Geometry will be gathered and built later in Execute() when the device/queue are fully stable");
    CryLogAlways("[D3D_RT] Init: ===== DEVICE-SAFE RAY TRACING INITIALIZATION COMPLETE =====");
}



void CD3D_RT::RT_InitFrameContexts()
{
    if (m_frameFence) return;
    m_frameIndex = 0;

    HRESULT hr = m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_frameFence));
    if (FAILED(hr))
    {
        CryLogAlways("[D3D_RT] FrameFence creation failed (hr=0x%08x)", hr);
        return;
    }

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    for (auto& fc : m_frameCtx)
    {
        ID3D12CommandAllocator* tmp = nullptr;
        hr = m_pDevice->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&tmp));
        if (FAILED(hr))
        {
            CryLogAlways("[D3D_RT] CreateCommandAllocator failed (hr=0x%08x)", hr);
            fc.pAllocator = nullptr;
        }
        else
        {
            fc.pAllocator = tmp;
        }
    }
    CryLogAlways("[D3D_RT] Frame contexts initialized");
}

void CD3D_RT::RT_ShutdownFrameContexts()
{
    if (m_frameFence)
    {
        m_pCommandQueue->Signal(m_frameFence, ++m_lastSignaledFence);
        if (m_frameFence->GetCompletedValue() < m_lastSignaledFence)
        {
            m_frameFence->SetEventOnCompletion(m_lastSignaledFence, m_fenceEvent);
            DWORD wr = WaitForSingleObject(m_fenceEvent, 10000);
            if (wr != WAIT_OBJECT_0)
            {
                CryLogAlways("[D3D_RT] RT_ShutdownFrameContexts: timeout waiting on frame fence, forcing RT_WaitForGpuIdle");
                RT_WaitForGpuIdle("FrameCtxShutdown_Fallback");
            }
        }
    }
    for (auto& fc : m_frameCtx)
        SAFE_RELEASE(fc.pAllocator);
    SAFE_RELEASE(m_frameFence);
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
}

// Strengthen RT_BeginFrameAllocator
void CD3D_RT::RT_BeginFrameAllocator(ID3D12CommandAllocator** ppAlloc)
{
    *ppAlloc = nullptr;
    if (!m_pDevice || !m_pCommandQueue) return;
    if (!m_frameFence) RT_InitFrameContexts();

    RT_FrameContext& ctx = m_frameCtx[m_frameIndex % kRT_FrameContextCount];

    // Reuse existing allocator if it was NOT retired and we intend to record work
    if (!ctx.pAllocator)
    {
        HRESULT hr = m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx.pAllocator));
        if (FAILED(hr) || !ctx.pAllocator)
        {
            //CryLogAlways("[D3D_RT][Allocator] CreateCommandAllocator failed hr=0x%08x", hr);
            return;
        }
#if defined(_DEBUG) || defined(PROFILE)
        {
            wchar_t wname[64];
            swprintf_s(wname, L"DXR_FrameAlloc_%llu", (unsigned long long)m_frameIndex);
            ctx.pAllocator->SetName(wname);
        }
#endif
        //ctx.fenceValue = 0;
        //ctx.usedOnce = false;
        //CryLogAlways("[D3D_RT][Allocator] New allocator %p for frameIdx=%llu", ctx.pAllocator, (unsigned long long)m_frameIndex);
    }
    else
    {
        // If allocator had prior GPU usage, ensure fence passed before Reset (canonical path)
        if (ctx.fenceValue)
        {
            const UINT64 completed = m_frameFence ? m_frameFence->GetCompletedValue() : ctx.fenceValue;
            if (m_frameFence && completed < ctx.fenceValue)
            {
                // Wait minimally
                if (!m_fenceEvent) m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (m_fenceEvent)
                {
                    m_frameFence->SetEventOnCompletion(ctx.fenceValue, m_fenceEvent);
                    WaitForSingleObject(m_fenceEvent, 10000);
                }
            }
        }
        // Safe to Reset now (allocator definitively unused)
        HRESULT rhr = ctx.pAllocator->Reset();
        if (FAILED(rhr))
            //CryLogAlways("[D3D_RT][Allocator] Reset failed %p hr=0x%08x", ctx.pAllocator, rhr);
        ctx.fenceValue = 0;
        ctx.usedOnce = false;
        //CryLogAlways("[D3D_RT][Allocator] Reusing allocator %p for frameIdx=%llu", ctx.pAllocator, (unsigned long long)m_frameIndex);
    }

    *ppAlloc = ctx.pAllocator;
}

void CD3D_RT::RT_EndFrameAndSignal()
{
    if (!m_frameFence)
    {
        ++m_frameIndex;
        m_frameRecordedWork = false;
        return;
    }

    // Only signal and retire if actual GPU work was recorded
    if (m_frameRecordedWork)
    {
        UINT64 signalValue = ++m_lastSignaledFence;
        HRESULT hr = m_pCommandQueue->Signal(m_frameFence, signalValue);
        if (FAILED(hr))
            CryLogAlways("[D3D_RT] RT_EndFrameAndSignal: Signal failed hr=0x%08x", hr);

        RT_FrameContext& ctx = m_frameCtx[m_frameIndex % kRT_FrameContextCount];
        if (ctx.pAllocator)
        {
            ctx.fenceValue = signalValue;
            ctx.usedOnce = true;
            //CryLogAlways("[D3D_RT][Allocator] Mark allocator %p in-flight fence=%llu", ctx.pAllocator, (unsigned long long)signalValue);
        }
        m_lastDispatchFence = signalValue;
    }
    else
    {
        // No work: do NOT retire / signal / create dummy lists; allocator stays reusable
        //CryLogAlways("[D3D_RT][Allocator] No work for frameIdx=%llu - allocator reused (no signal)", (unsigned long long)m_frameIndex);
    }

    // Reclaim only those definitely finished
    RT_ReclaimRetiredAllocators();

    m_frameRecordedWork = false;
    ++m_frameIndex;
}


// NEW: Extended GPU idle wait using the persistent frame fence
void CD3D_RT::RT_WaitForGpuIdleEx(const char* reason)
{
    if (!m_pCommandQueue)
        return;

    // If we have a frame fence, prefer waiting on the latest signaled value.
    if (m_frameFence && m_lastSignaledFence)
    {
        const UINT64 target = m_lastSignaledFence;
        const UINT64 completed = m_frameFence->GetCompletedValue();
        if (completed < target)
        {
            if (m_fenceEvent == nullptr)
                m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            m_frameFence->SetEventOnCompletion(target, m_fenceEvent);
            DWORD wr = WaitForSingleObject(m_fenceEvent, 30000);
            if (wr != WAIT_OBJECT_0)
                CryLogAlways("[D3D_RT] RT_WaitForGpuIdleEx(%s): timeout/fail wr=%u (completed=%llu target=%llu)",
                    reason ? reason : "", wr, (unsigned long long)completed, (unsigned long long)target);
        }
        return;
    }

    // Fallback: original temp-fence path
    RT_WaitForGpuIdle(reason);
}

void CD3D_RT::RT_WaitForGpuIdle(const char* reason)
{
    if (!m_pDevice || !m_pCommandQueue)
        return;

    ID3D12Fence* pFence = nullptr;
    HRESULT hr = m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFence));
    if (FAILED(hr) || !pFence)
    {
        CryLogAlways("[D3D_RT] RT_WaitForGpuIdle(%s): Failed to create fence (hr=0x%08x)", reason ? reason : "", hr);
        return;
    }

    const UINT64 fenceValue = 1ull;
    hr = m_pCommandQueue->Signal(pFence, fenceValue);
    if (SUCCEEDED(hr))
    {
        if (pFence->GetCompletedValue() < fenceValue)
        {
            HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (evt)
            {
                hr = pFence->SetEventOnCompletion(fenceValue, evt);
                if (SUCCEEDED(hr))
                {
                    // Generous timeout (30s) to avoid premature releases on heavy builds
                    DWORD wr = WaitForSingleObject(evt, 30000);
                    if (wr != WAIT_OBJECT_0)
                    {
                        CryLogAlways("[D3D_RT] RT_WaitForGpuIdle(%s): TIMEOUT/FAIL wr=%u (continuing cautiously)", reason ? reason : "", wr);
                    }
                }
                CloseHandle(evt);
            }
        }
    }
    else
    {
        CryLogAlways("[D3D_RT] RT_WaitForGpuIdle(%s): Signal failed (hr=0x%08x)", reason ? reason : "", hr);
    }

    pFence->Release();
}

void CD3D_RT::RT_DeferRelease(ID3D12Resource* r)
{
    if (!r) return;
    // Attach to latest signaled (or soon-to-be) fence
    UINT64 fenceForRelease = m_lastSignaledFence ? m_lastSignaledFence : 1;

    if (m_deferred.empty() || m_deferred.back().fenceValue != fenceForRelease)
    {
        RT_DeferredRelease entry;
        entry.fenceValue = fenceForRelease;
        m_deferred.push_back(entry);
    }

    m_deferred.back().resources.push_back(r);
}

HRESULT CD3D_RT::Initialize(ID3D12Device5* pDevice, ID3D12CommandQueue* pCommandQueue)
{
    if (!pDevice || !pCommandQueue)
    {
        CryLogAlways("[D3D_RT] Initialize failed: Invalid device or command queue parameters");
        return E_INVALIDARG;
    }

    // CRITICAL FIX: Properly manage device reference counting
    if (m_pDevice)
    {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }

    // Store the device and command queue references with proper reference counting
    m_pDevice = pDevice;
    m_pDevice->AddRef(); // Add reference count
    m_pCommandQueue = pCommandQueue;

    // Check for ray tracing support
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    HRESULT hr = m_pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    if (FAILED(hr) || options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
    {
        CryLogAlways("[D3D_RT] Initialize failed: Ray tracing is not supported on this device (Tier: %d)",
            SUCCEEDED(hr) ? options5.RaytracingTier : -1);
        return E_FAIL;
    }

    // NEW: Check device resource limits to prevent conflicts with shadow mapping
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    hr = m_pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    if (SUCCEEDED(hr))
    {

        // Check texture size limits
        D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT vaSupport = {};
        hr = m_pDevice->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &vaSupport, sizeof(vaSupport));
        if (SUCCEEDED(hr))
        {
            CryLogAlways("[D3D_RT] GPU VA support: %u bits", vaSupport.MaxGPUVirtualAddressBitsPerResource);
        }
    }

    CryLogAlways("[D3D_RT] Initialize successful: Ray tracing tier %d supported", options5.RaytracingTier);
    return S_OK;
}

HRESULT CD3D_RT::CompileAndLoadShaders()
{
    // GUARANTEED: This method is called ONLY ONCE during Init(), never during Execute() loop
    CryLogAlways("[D3D_RT] ONCE-ONLY COMPILATION: Compiling ray tracing shaders from HLSL source files...");

    // SAFETY CHECK: If already compiled, never recompile
    if (m_shadersCompiled)
    {
        CryLogAlways("[D3D_RT] CompileAndLoadShaders: Shaders already compiled, skipping to prevent duplicate work");
        return S_OK;
    }

    // Use the shader compiler to compile all ray tracing shaders
    bool success = m_shaderCompiler.CompileRayTracingShaders();
    if (!success)
    {
        CryLogAlways("[D3D_RT] CompileAndLoadShaders CRITICAL ERROR: Shader compilation failed completely");
        CryLogAlways("[D3D_RT] This means HLSL files are missing or DXC compilation failed");
        CryLogAlways("[D3D_RT] Ray tracing pipeline CANNOT be created without valid shaders");
        return E_FAIL;
    }

    // Validate that we have all required shaders with REAL bytecode
    if (!ValidateCompiledShaders())
    {
        CryLogAlways("[D3D_RT] CompileAndLoadShaders CRITICAL ERROR: Shader validation failed");
        CryLogAlways("[D3D_RT] Shaders were 'compiled' but contain invalid or placeholder bytecode");
        return E_FAIL;
    }

    // MARK AS COMPILED - This prevents any future recompilation attempts
    m_shadersCompiled = true;
    CryLogAlways("[D3D_RT] CompileAndLoadShaders COMPLETE: All shaders compiled ONCE and validated with REAL DXIL bytecode");
    return S_OK;
}


bool CD3D_RT::ValidateCompiledShaders()
{
    CryLogAlways("[D3D_RT] ENHANCED VALIDATION: Validating compiled shaders for ray tracing compatibility...");

    const auto& rayGenBytecode = m_shaderCompiler.GetRayGenShaderBytecode();
    const auto& missBytecode = m_shaderCompiler.GetMissShaderBytecode();
    const auto& closestHitBytecode = m_shaderCompiler.GetClosestHitShaderBytecode();

    auto validateDXILContainer = [](const std::vector<BYTE>& bc, const char* name, bool warnOnTiny) -> bool
        {
            if (bc.size() < 32)
            {
                CryLogAlways("[D3D_RT] CRITICAL: %s shader bytecode too small (%zu bytes) - not a valid DXIL container", name, bc.size());
                return false;
            }

            const DWORD* header = reinterpret_cast<const DWORD*>(bc.data());
            const DWORD signature = header[0];                   // 'DXBC'
            if (signature != 0x43425844)
            {
                CryLogAlways("[D3D_RT] CRITICAL: %s shader missing DXBC signature (0x%08X)", name, signature);
                return false;
            }

            const DWORD containerSize = header[6];
            const DWORD partCount = header[7];

            if (containerSize != bc.size())
            {
                CryLogAlways("[D3D_RT] CRITICAL: %s shader container size mismatch: header=%u, buf=%zu", name, containerSize, bc.size());
                return false;
            }
            if (partCount == 0)
            {
                CryLogAlways("[D3D_RT] CRITICAL: %s shader has 0 parts - invalid container", name);
                return false;
            }

            if (warnOnTiny && bc.size() < 512)
            {
                CryLogAlways("[D3D_RT] WARNING: %s shader DXIL is unusually small (%zu bytes, %u parts) but structurally valid. Continuing.", name, bc.size(), partCount);
            }
            else
            {
                CryLogAlways("[D3D_RT] %s shader VALIDATED: %zu bytes, %u parts - DXIL OK", name, bc.size(), partCount);
            }
            return true;
        };

    // Required presence
    if (rayGenBytecode.empty()) { CryLogAlways("[D3D_RT] CRITICAL: RayGen shader bytecode is empty"); return false; }
    if (missBytecode.empty()) { CryLogAlways("[D3D_RT] CRITICAL: Miss shader bytecode is empty"); return false; }
    if (closestHitBytecode.empty()) { CryLogAlways("[D3D_RT] CRITICAL: ClosestHit shader bytecode is empty"); return false; }

    bool ok = true;
    ok &= validateDXILContainer(rayGenBytecode, "RayGen",     /*warnOnTiny*/ true);
    ok &= validateDXILContainer(missBytecode, "Miss",       /*warnOnTiny*/ false);
    ok &= validateDXILContainer(closestHitBytecode, "ClosestHit", /*warnOnTiny*/ false);

    if (!ok)
    {
        CryLogAlways("[D3D_RT] VALIDATION FAILED: One or more DXIL containers are invalid");
        return false;
    }

    CryLogAlways("[D3D_RT] VALIDATION SUCCESS: All DXIL containers are structurally valid");
    CryLogAlways("[D3D_RT]   - RayGen: %zu bytes", rayGenBytecode.size());
    CryLogAlways("[D3D_RT]   - Miss: %zu bytes", missBytecode.size());
    CryLogAlways("[D3D_RT]   - ClosestHit: %zu bytes", closestHitBytecode.size());
    return true;
}

HRESULT CD3D_RT::CreateRayTracingPipeline()
{
    // Already created?
    if (m_pRaytracingPSO && m_pGlobalRootSignature)
    {
        CryLogAlways("[D3D_RT] CreateRayTracingPipeline: Pipeline already created, skipping");
        return S_OK;
    }

    CryLogAlways("[D3D_RT] Creating ray tracing pipeline with VALIDATED compiled shaders...");

    // Root signature
    HRESULT hr = CreateGlobalRootSignature();
    if (FAILED(hr))
    {
        CryLogAlways("[D3D_RT] CreateRayTracingPipeline failed: Could not create root signature (hr=0x%08x)", hr);
        return hr;
    }

    // State object (PSO)
    hr = CreateRayTracingPSO();
    if (FAILED(hr))
    {
        CryLogAlways("[D3D_RT] CreateRayTracingPipeline failed: Could not create PSO (hr=0x%08x)", hr);
        return hr;
    }

    CryLogAlways("[D3D_RT] CreateRayTracingPipeline successful");
    return S_OK;
}

HRESULT CD3D_RT::CreateGlobalRootSignature()
{
    if (m_pGlobalRootSignature)
    {
        CryLogAlways("[D3D_RT] CreateGlobalRootSignature: Root signature already created, skipping");
        return S_OK;
    }

    CryLogAlways("[D3D_RT] CreateGlobalRootSignature: Building GLOBAL root signature (DXR pipeline)");

    std::vector<D3D12_ROOT_PARAMETER1> rootParameters;
    rootParameters.reserve(4);

    // [0] CBV b0
    {
        D3D12_ROOT_PARAMETER1 p = {};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = 0;
        p.Descriptor.RegisterSpace = 0;
        p.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters.push_back(p);
    }

    // [1] SRV table: TLAS (t0)
    static D3D12_DESCRIPTOR_RANGE1 tlasRange = {};
    tlasRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tlasRange.NumDescriptors = 1;
    tlasRange.BaseShaderRegister = 0;
    tlasRange.RegisterSpace = 0;
    tlasRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    tlasRange.OffsetInDescriptorsFromTableStart = 0;
    {
        D3D12_ROOT_PARAMETER1 p = {};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = 1;
        p.DescriptorTable.pDescriptorRanges = &tlasRange;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters.push_back(p);
    }

    // [2] UAV table: u0-u3 (GI, Refl, Stats, AO)
    static D3D12_DESCRIPTOR_RANGE1 uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 4;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    uavRange.OffsetInDescriptorsFromTableStart = 0;
    {
        D3D12_ROOT_PARAMETER1 p = {};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = 1;
        p.DescriptorTable.pDescriptorRanges = &uavRange;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters.push_back(p);
    }

    // [3] SRV table: t1‑t11 (GBuffer + environment)
    static D3D12_DESCRIPTOR_RANGE1 srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 12;      // t1..t11
    srvRange.BaseShaderRegister = 1;   // starts at t1
    srvRange.RegisterSpace = 0;
    srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    srvRange.OffsetInDescriptorsFromTableStart = 0;
    {
        D3D12_ROOT_PARAMETER1 p = {};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = 1;
        p.DescriptorTable.pDescriptorRanges = &srvRange;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters.push_back(p);
    }

    // Static sampler s0
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Updated flags (direct indexing + IA layout for future hybrid use)
    const D3D12_ROOT_SIGNATURE_FLAGS rootFlags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;


    D3D12_VERSIONED_ROOT_SIGNATURE_DESC verDesc = {};
    verDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    verDesc.Desc_1_1.NumParameters = static_cast<UINT>(rootParameters.size());
    verDesc.Desc_1_1.pParameters = rootParameters.data();
    verDesc.Desc_1_1.NumStaticSamplers = 1;
    verDesc.Desc_1_1.pStaticSamplers = &sampler;
    verDesc.Desc_1_1.Flags = rootFlags;

    ID3DBlob* pBlob = nullptr;
    ID3DBlob* pErr = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&verDesc, &pBlob, &pErr);
    if (FAILED(hr))
    {
        if (pErr)
        {
            CryLogAlways("[D3D_RT] CreateGlobalRootSignature: Serialize failed: %s",
                (const char*)pErr->GetBufferPointer());
            pErr->Release();
        }
        CryLogAlways("[D3D_RT] CreateGlobalRootSignature: D3D12SerializeVersionedRootSignature hr=0x%08x", hr);
        return hr;
    }

    hr = m_pDevice->CreateRootSignature(
        0,
        pBlob->GetBufferPointer(),
        pBlob->GetBufferSize(),
        IID_PPV_ARGS(&m_pGlobalRootSignature));

    pBlob->Release();

    if (FAILED(hr) || !m_pGlobalRootSignature)
    {
        CryLogAlways("[D3D_RT] CreateGlobalRootSignature: CreateRootSignature failed hr=0x%08x", hr);
        return hr ? hr : E_FAIL;
    }

    // Estimate DWORD cost (simple heuristic)
    {
        UINT dwordCost = 0;
        for (const auto& rp : rootParameters)
        {
            switch (rp.ParameterType)
            {
            case D3D12_ROOT_PARAMETER_TYPE_CBV:
            case D3D12_ROOT_PARAMETER_TYPE_SRV:
            case D3D12_ROOT_PARAMETER_TYPE_UAV: dwordCost += 2; break;
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: dwordCost += 1; break;
            case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: dwordCost += rp.Constants.Num32BitValues; break;
            default: break;
            }
        }
        CryLogAlways("[D3D_RT] CreateGlobalRootSignature: Estimated root DWORD cost=%u", dwordCost);
        if (dwordCost > 48)
            CryLogAlways("[D3D_RT] WARNING: High root signature cost (%u DWORDs) - consider consolidation", dwordCost);
    }

    CryLogAlways("[D3D_RT] CreateGlobalRootSignature: SUCCESS");
    CryLogAlways("[D3D_RT]   Layout:");
    CryLogAlways("[D3D_RT]     [0] CBV  b0");
    CryLogAlways("[D3D_RT]     [1] SRV  t0          (TLAS)");
    CryLogAlways("[D3D_RT]     [2] UAV  u0-u3       (GI, Refl, Stats, AO)");
    CryLogAlways("[D3D_RT]     [3] SRV  t1-t11      (GBuffer+Env)");
    CryLogAlways("[D3D_RT]     Static sampler s0");
    CryLogAlways("[D3D_RT]     Flags=0x%08x", (unsigned)rootFlags);

    return S_OK;
}


HRESULT CD3D_RT::CreateRayTracingPSO()
{
    CryLogAlways("[D3D_RT] CreateRayTracingPSO: Building DXR state object...");

    if (!m_shadersCompiled || !m_pGlobalRootSignature || !m_pDevice)
    {
        CryLogAlways("[D3D_RT] CreateRayTracingPSO: prerequisites missing (compiled=%d rootSig=%p device=%p)",
            (int)m_shadersCompiled, m_pGlobalRootSignature, m_pDevice);
        return E_FAIL;
    }

    const auto& rg = m_shaderCompiler.GetRayGenShaderBytecode();
    const auto& ms = m_shaderCompiler.GetMissShaderBytecode();
    const auto& ch = m_shaderCompiler.GetClosestHitShaderBytecode();
    if (rg.empty() || ms.empty() || ch.empty())
    {
        CryLogAlways("[D3D_RT] CreateRayTracingPSO: Missing compiled shader bytecode");
        return E_FAIL;
    }

    static const wchar_t* kRayGen = L"RayGenMain";
    static const wchar_t* kMiss = L"MissMain";
    static const wchar_t* kClosest = L"ClosestHitMain";
    static const wchar_t* kHitGroup = L"HitGroup";

    static D3D12_EXPORT_DESC rgExports[] = { { kRayGen,   nullptr, D3D12_EXPORT_FLAG_NONE } };
    static D3D12_EXPORT_DESC msExports[] = { { kMiss,     nullptr, D3D12_EXPORT_FLAG_NONE } };
    static D3D12_EXPORT_DESC chExports[] = { { kClosest,  nullptr, D3D12_EXPORT_FLAG_NONE } };

    const UINT PAYLOAD_SIZE = 80; // >= actual HLSL payload
    const UINT ATTR_SIZE = 8;

    D3D12_DXIL_LIBRARY_DESC rgLib{}; rgLib.DXILLibrary = { rg.data(), rg.size() }; rgLib.NumExports = _countof(rgExports); rgLib.pExports = rgExports;
    D3D12_DXIL_LIBRARY_DESC msLib{}; msLib.DXILLibrary = { ms.data(), ms.size() }; msLib.NumExports = _countof(msExports); msLib.pExports = msExports;
    D3D12_DXIL_LIBRARY_DESC chLib{}; chLib.DXILLibrary = { ch.data(), ch.size() }; chLib.NumExports = _countof(chExports); chLib.pExports = chExports;

    D3D12_HIT_GROUP_DESC hg{};
    hg.HitGroupExport = kHitGroup;
    hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hg.ClosestHitShaderImport = kClosest;

    D3D12_RAYTRACING_SHADER_CONFIG shaderCfg{};
    shaderCfg.MaxPayloadSizeInBytes = PAYLOAD_SIZE;
    shaderCfg.MaxAttributeSizeInBytes = ATTR_SIZE;

    // Keep recursion small to prevent runaway work if g_GIBounces is mis-set
    static const UINT kMaxRecDepth = 3; // RayGen -> CHS/Miss (no deep recursion)
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineCfg{};
    pipelineCfg.MaxTraceRecursionDepth = kMaxRecDepth;

    const wchar_t* assocExports[] = { kRayGen, kMiss, kHitGroup };
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc{};
    assoc.NumExports = _countof(assocExports);
    assoc.pExports = assocExports;

    D3D12_STATE_OBJECT_CONFIG soCfg{};
    soCfg.Flags = D3D12_STATE_OBJECT_FLAG_NONE;

    std::array<D3D12_STATE_SUBOBJECT, 9> subs{};
    UINT i = 0;

    ID3D12RootSignature* pGlobalRS = m_pGlobalRootSignature;
    subs[i].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subs[i++].pDesc = &pGlobalRS;

    subs[i].Type = D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG;
    subs[i++].pDesc = &soCfg;

    subs[i].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subs[i++].pDesc = &rgLib;
    subs[i].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subs[i++].pDesc = &msLib;
    subs[i].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subs[i++].pDesc = &chLib;

    subs[i].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subs[i++].pDesc = &hg;

    const UINT shaderCfgIdx = i;
    subs[i].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subs[i++].pDesc = &shaderCfg;

    assoc.pSubobjectToAssociate = &subs[shaderCfgIdx];
    subs[i].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    subs[i++].pDesc = &assoc;

    subs[i].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subs[i++].pDesc = &pipelineCfg;

    const UINT subobjectCount = i;

    for (UINT k = 0; k < subobjectCount; ++k)
    {
        if (!subs[k].pDesc || subs[k].pDesc == reinterpret_cast<void*>(uintptr_t(-1)))
        {
            CryLogAlways("[D3D_RT] CreateRayTracingPSO: INVALID subobject %u type=%u pDesc=%p",
                k, subs[k].Type, subs[k].pDesc);
            return E_INVALIDARG;
        }
    }
    if (subs[0].Type != D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE ||
        *reinterpret_cast<ID3D12RootSignature* const*>(subs[0].pDesc) != m_pGlobalRootSignature)
    {
        CryLogAlways("[D3D_RT] CreateRayTracingPSO: Root signature ptr mismatch");
        return E_INVALIDARG;
    }

    D3D12_STATE_OBJECT_DESC desc{};
    desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    desc.NumSubobjects = subobjectCount;
    desc.pSubobjects = subs.data();

    HRESULT hr = m_pDevice->CreateStateObject(&desc, IID_PPV_ARGS(&m_pRaytracingPSO));
    CryLogAlways("[D3D_RT] CreateRayTracingPSO: CreateStateObject hr=0x%08x (MaxRecDepth=%u)", hr, kMaxRecDepth);
    if (FAILED(hr) || !m_pRaytracingPSO)
    {
        SAFE_RELEASE(m_pRaytracingPSO);
        return hr ? hr : E_FAIL;
    }

    hr = m_pRaytracingPSO->QueryInterface(IID_PPV_ARGS(&m_pStateObjectProperties));
    if (FAILED(hr) || !m_pStateObjectProperties)
    {
        CryLogAlways("[D3D_RT] CreateRayTracingPSO: QueryInterface(StateObjectProperties) failed (hr=0x%08x)", hr);
        SAFE_RELEASE(m_pRaytracingPSO);
        return hr ? hr : E_FAIL;
    }

    m_pRayGenShaderID = m_pStateObjectProperties->GetShaderIdentifier(kRayGen);
    m_pMissShaderID = m_pStateObjectProperties->GetShaderIdentifier(kMiss);
    m_pClosestHitShaderID = m_pStateObjectProperties->GetShaderIdentifier(kHitGroup);

    if (!m_pRayGenShaderID || !m_pMissShaderID || !m_pClosestHitShaderID)
    {
        CryLogAlways("[D3D_RT] CreateRayTracingPSO: Shader identifier lookup failed");
        SAFE_RELEASE(m_pStateObjectProperties);
        SAFE_RELEASE(m_pRaytracingPSO);
        return E_INVALIDARG;
    }

    CryLogAlways("[D3D_RT] CreateRayTracingPSO: SUCCESS");
    return S_OK;
}

CTexture* CD3D_RT::GetDXRAOTexture() const
{
    return m_pTexDXR_AO;
}

// Helper: create/resize DXR outputs with custom format
static CTexture* RT_CreateOrResizeDXROutFmt(CTexture* pTex, const char* name, int w, int h, ETEX_Format fmt)
{
    const uint32 flags = FT_NOMIPS | FT_USAGE_UNORDERED_ACCESS | FT_DONT_STREAM;
    if (pTex && pTex->GetWidth() == w && pTex->GetHeight() == h && pTex->GetDstFormat() == fmt)
        return pTex;
    if (pTex) pTex->Release();

    _smart_ptr<CTexture> pObj = CTexture::GetOrCreateTextureObject(name, w, h, 1, eTT_2D, flags, fmt);
    if (!pObj) return nullptr;
    pObj->Create2DTexture(w, h, 1, flags, nullptr, fmt);
    pObj->AddRef();
    return pObj.get();
}


HRESULT CD3D_RT::CreateShaderTables()
{
    if (m_pRayGenShaderTable && m_pMissShaderTable && m_pHitGroupShaderTable)
    {
        CryLogAlways("[D3D_RT] CreateShaderTables: Shader tables already created, skipping");
        return S_OK;
    }

    CryLogAlways("[D3D_RT] Creating shader tables...");

    if (!m_pStateObjectProperties)
    {
        CryLogAlways("[D3D_RT] CreateShaderTables failed: State object properties not available");
        return E_FAIL;
    }

    const UINT shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
    const UINT tableAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT; // 64
    const UINT recordAlignment = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT; // 32

    auto Align = [](UINT v, UINT a) { return (v + (a - 1)) & ~(a - 1); };

    // Create heap properties for upload buffer
    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    // Common buffer desc builder
    auto MakeBufferDesc = [](UINT size) {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        return d;
        };

    // Ray Generation Shader Table (single record)
    {
        const UINT rayGenRecordSize = Align(shaderIdentifierSize, recordAlignment);   // 32
        const UINT rayGenTableSize = Align(rayGenRecordSize, tableAlignment);        // 64

        D3D12_RESOURCE_DESC bufferDesc = MakeBufferDesc(rayGenTableSize);
        HRESULT hr = m_pDevice->CreateCommittedResource(
            &uploadHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_pRayGenShaderTable)
        );
        if (FAILED(hr))
        {
            CryLogAlways("[D3D_RT] CreateShaderTables failed: Ray Gen shader table creation failed");
            return hr;
        }

        BYTE* pData = nullptr;
        hr = m_pRayGenShaderTable->Map(0, nullptr, reinterpret_cast<void**>(&pData));
        if (SUCCEEDED(hr))
        {
            memcpy(pData, m_pRayGenShaderID, shaderIdentifierSize);
            // zero the padding to be nice
            memset(pData + shaderIdentifierSize, 0, rayGenTableSize - shaderIdentifierSize);
            m_pRayGenShaderTable->Unmap(0, nullptr);
        }
    }

    // Miss Shader Table (single record, but stride must be >= 32 and we align to 64)
    {
        const UINT missRecordSize = Align(shaderIdentifierSize, recordAlignment); // 32
        const UINT missStride = Align(missRecordSize, tableAlignment);        // 64
        const UINT missTableSize = missStride;                                   // 1 record

        D3D12_RESOURCE_DESC bufferDesc = MakeBufferDesc(missTableSize);
        HRESULT hr = m_pDevice->CreateCommittedResource(
            &uploadHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_pMissShaderTable)
        );
        if (FAILED(hr))
        {
            CryLogAlways("[D3D_RT] CreateShaderTables failed: Miss shader table creation failed");
            return hr;
        }

        BYTE* pData = nullptr;
        hr = m_pMissShaderTable->Map(0, nullptr, reinterpret_cast<void**>(&pData));
        if (SUCCEEDED(hr))
        {
            memcpy(pData, m_pMissShaderID, shaderIdentifierSize);
            memset(pData + shaderIdentifierSize, 0, missTableSize - shaderIdentifierSize);
            m_pMissShaderTable->Unmap(0, nullptr);
        }
    }

    // Hit Group Shader Table (single record, stride align to 64)
    {
        const UINT hitRecordSize = Align(shaderIdentifierSize, recordAlignment); // 32
        const UINT hitStride = Align(hitRecordSize, tableAlignment);         // 64
        const UINT hitTableSize = hitStride;

        D3D12_RESOURCE_DESC bufferDesc = MakeBufferDesc(hitTableSize);
        HRESULT hr = m_pDevice->CreateCommittedResource(
            &uploadHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_pHitGroupShaderTable)
        );
        if (FAILED(hr))
        {
            CryLogAlways("[D3D_RT] CreateShaderTables failed: Hit group shader table creation failed");
            return hr;
        }

        BYTE* pData = nullptr;
        hr = m_pHitGroupShaderTable->Map(0, nullptr, reinterpret_cast<void**>(&pData));
        if (SUCCEEDED(hr))
        {
            memcpy(pData, m_pClosestHitShaderID, shaderIdentifierSize);
            memset(pData + shaderIdentifierSize, 0, hitTableSize - shaderIdentifierSize);
            m_pHitGroupShaderTable->Unmap(0, nullptr);
        }
    }

    CryLogAlways("[D3D_RT] CreateShaderTables successful: All shader tables created (64B aligned)");
    return S_OK;
}



ID3D12Resource* CD3D_RT::GetD3D12ResourceFromHandle(buffer_handle_t handle)
{
    // Get the D3D12 resource from CryEngine's buffer manager
    // This is a CryEngine-specific implementation that depends on the internal buffer management

    if (handle == 0 || handle == ~0u)
    {
        return nullptr;
    }

    // Access the D3D12 device buffer manager to get the actual D3D12 resource
    CDeviceBufferManager& bufferManager = gcpRendD3D->m_DevBufMan;

    // Get the D3D buffer from the handle using CryEngine's buffer management system
    buffer_size_t offset;
    D3DBuffer* pD3DBuffer = bufferManager.GetD3D(handle, &offset);

    if (!pD3DBuffer)
    {
        return nullptr;
    }

    // Extract the D3D12 resource from the CryEngine buffer wrapper
    // In D3D12 implementation, D3DBuffer is actually a CCryDX12Buffer
    auto pDX12Buffer = reinterpret_cast<CCryDX12Buffer*>(pD3DBuffer);
    if (!pDX12Buffer)
    {
        return nullptr;
    }

    // Get the actual D3D12 resource from the DX12 buffer wrapper
    ICryDX12Resource* pDX12Resource = DX12_EXTRACT_ICRYDX12RESOURCE(pDX12Buffer);
    if (!pDX12Resource)
    {
        return nullptr;
    }

    // Return the native D3D12 resource
    return pDX12Resource->GetD3D12Resource();
}


// Adjust UpdateAccelerationStructures(): build new first, then defer old
void CD3D_RT::UpdateAccelerationStructures()
{
    static std::mutex s_updateMutex;
    std::lock_guard<std::mutex> lock(s_updateMutex);
    static uint32_t frameCounter = 0;
    frameCounter++;

    const uint32_t rebuildInterval = 3600;
    if ((frameCounter % rebuildInterval) != 0)
        return;
    if (RT_IsLevelStreamingBusy())
        return;

    // Ensure previous ray dispatch using current TLAS/BLAS has fully completed
    RT_WaitForLastDispatch("UpdateAccelerationStructures_BeforeRelease");

    // Keep old pointers for deferred lifetime
    AccelerationStructureBuffers oldTLAS = m_tlasBuffers;
    AccelerationStructureBuffers oldLegacyBLAS = m_blasBuffers;
    std::vector<AccelerationStructureBuffers> oldSceneBLAS;
    oldSceneBLAS.swap(m_sceneBLAS);             // move out
    std::vector<ID3D12Resource*> oldSceneResults;
    oldSceneResults.swap(m_sceneBLASResults);
    ClearKeepAliveUploads();
    m_pTopLevelAS = nullptr;
    m_tlasBuffers = {};
    m_blasBuffers = {};

    HRESULT hr = CreateSceneBLASAndTLASFromView();
    if (FAILED(hr) || !m_pTopLevelAS)
    {
        hr = CreateTestSceneWithEnhancedSafety();
    }

    if (m_pTopLevelAS && m_pDescriptorHeap)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_pDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrv{};
        tlasSrv.Format = DXGI_FORMAT_UNKNOWN;
        tlasSrv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        tlasSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tlasSrv.RaytracingAccelerationStructure.Location = m_pTopLevelAS->GetGPUVirtualAddress();
        m_pDevice->CreateShaderResourceView(nullptr, &tlasSrv, cpu);
    }

    // Defer release of old AS resources (safer than releasing before rebuild)
    auto DeferASBuffers = [this](AccelerationStructureBuffers& b, const char* tag)
        {
            if (b.pScratch)      RT_SafeRelease(b.pScratch, (std::string(tag) + "_Scratch").c_str());
            if (b.pResult)       RT_SafeRelease(b.pResult, (std::string(tag) + "_Result").c_str());
            if (b.pInstanceDesc) RT_SafeRelease(b.pInstanceDesc, (std::string(tag) + "_Inst").c_str());
            b.resultVA = 0;
        };
    DeferASBuffers(oldTLAS, "OldTLAS");
    DeferASBuffers(oldLegacyBLAS, "OldLegacyBLAS");
    for (auto& b : oldSceneBLAS) DeferASBuffers(b, "OldSceneBLAS");
    for (auto* r : oldSceneResults) RT_SafeRelease(r, "OldSceneBLASResult");
}


// Add near other wait helpers (after RT_WaitForGpuIdle / RT_WaitForGpuIdleEx)
void CD3D_RT::RT_WaitForLastDispatch(const char* reason)
{
    if (!m_frameFence || !m_lastDispatchFence)
        return;
    const UINT64 needed = m_lastDispatchFence;
    const UINT64 done = m_frameFence->GetCompletedValue();
    if (done >= needed)
        return;

    if (!m_fenceEvent)
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    if (m_fenceEvent)
    {
        m_frameFence->SetEventOnCompletion(needed, m_fenceEvent);
        DWORD wr = WaitForSingleObject(m_fenceEvent, 30000);
        if (wr != WAIT_OBJECT_0)
        {
            CryLogAlways("[D3D_RT] RT_WaitForLastDispatch(%s): timeout waiting (needed=%llu done=%llu)",
                reason ? reason : "", (unsigned long long)needed, (unsigned long long)done);
        }
    }
}

void CD3D_RT::Execute()
{
    // CRITICAL FIX: Level loading detection and device stability protection
    static std::mutex s_execMutex;
    std::lock_guard<std::mutex> lock(s_execMutex);

    static std::atomic<bool> s_deviceStabilityFailure(false);
    static std::atomic<uint32_t> s_executeCallCount(0);

    uint32_t currentCallCount = s_executeCallCount.fetch_add(1);

    if (s_deviceStabilityFailure.load())
    {
        if (currentCallCount % 1000 == 0)
            CryLogAlways("[D3D_RT] Execute: Device stability failure detected, skipping RT execution (call #%u)", currentCallCount);
        RT_EndFrameAndSignal();
        return;
    }

    bool isLevelLoading = false;
    static bool s_wasLevelLoading = false;

    if (gEnv && gEnv->pSystem)
    {
        ESystemGlobalState globalState = gEnv->pSystem->GetSystemGlobalState();
        if (globalState >= ESYSTEM_GLOBAL_STATE_LEVEL_LOAD_START_PREPARE &&
            globalState <= ESYSTEM_GLOBAL_STATE_LEVEL_LOAD_COMPLETE)
        {
            isLevelLoading = true;
        }
        if (globalState == ESYSTEM_GLOBAL_STATE_LEVEL_LOAD_START_TEXTURES ||
            globalState == ESYSTEM_GLOBAL_STATE_LEVEL_LOAD_START_PRECACHE ||
            globalState == ESYSTEM_GLOBAL_STATE_LEVEL_LOAD_ENDING)
        {
            isLevelLoading = true;
        }
    }
    if (gcpRendD3D)
    {
        if (gcpRendD3D->m_bInShutdown || gcpRendD3D->m_bDeviceSupportsInstancing == false)
            isLevelLoading = true;
    }
    if (gEnv && gEnv->p3DEngine)
    {
        if (!gEnv->p3DEngine->GetITerrain())
            isLevelLoading = true;
        __try
        {
            if (gEnv->p3DEngine->IsTerrainTextureStreamingInProgress())
                isLevelLoading = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            isLevelLoading = true;
        }
    }

    if (isLevelLoading)
    {
        if (!s_wasLevelLoading)
        {
            CryLogAlways("[D3D_RT] Execute: Level loading detected - DISABLING ray tracing until complete");
            s_wasLevelLoading = true;
        }
        m_frameRecordedWork = false;
        RT_EndFrameAndSignal();
        return;
    }
    else if (s_wasLevelLoading)
    {
        CryLogAlways("[D3D_RT] Execute: Level loading complete - re-enabling ray tracing");
        s_wasLevelLoading = false;
    }

    if (m_pDevice)
    {
        HRESULT deviceHr = m_pDevice->GetDeviceRemovedReason();
        if (FAILED(deviceHr))
        {
            CryLogAlways("[D3D_RT] Execute: Device removed detected (hr=0x%08x) - marking stability failure", deviceHr);
            s_deviceStabilityFailure = true;
            RT_EndFrameAndSignal();
            return;
        }
    }

    m_frameRecordedWork = false;
    DebugLogAllocatorState("BeginExecute");

    if (gEnv && gEnv->pConsole)
    {
        if (ICVar* pShadowCache = gEnv->pConsole->GetCVar("r_ShadowsCache"))
        {
            int v = pShadowCache->GetIVal();
            if (v > 4096)
            {
                pShadowCache->Set(4096);
                static bool sLoggedClamp = false;
                if (!sLoggedClamp)
                {
                    CryLogAlways("[D3D_RT] Execute: CRITICAL DEVICE PROTECTION - Shadow cache %d -> 4096", v);
                    sLoggedClamp = true;
                }
            }
        }
        if (ICVar* pTexMemBudget = gEnv->pConsole->GetCVar("sys_budget_videomem"))
        {
            if (pTexMemBudget->GetIVal() > 4096)
            {
                pTexMemBudget->Set(4096);
                static bool sLoggedTexClamp = false;
                if (!sLoggedTexClamp)
                {
                    CryLogAlways("[D3D_RT] Execute: DEVICE PROTECTION - Texture memory budget clamped to 4096MB");
                    sLoggedTexClamp = true;
                }
            }
        }
    }

    if (!m_pDevice || !m_pCommandQueue || !m_pRaytracingPSO || !m_shadersCompiled)
    {
        if (currentCallCount % 100 == 0)
        {
            CryLogAlways("[D3D_RT] Execute: Ray tracing not ready (Device=%p, Queue=%p, PSO=%p, Shaders=%d) - call #%u",
                m_pDevice, m_pCommandQueue, m_pRaytracingPSO, m_shadersCompiled ? 1 : 0, currentCallCount);
        }
        RT_EndFrameAndSignal();
        return;
    }

    // Ensure DXR outputs/heaps exist (lazy-create on demand)
    if (!m_pRaytracingOutput || !m_pDescriptorHeap)
    {
        const HRESULT cr = CreateRayTracingResources();
        if (FAILED(cr) || !m_pRaytracingOutput || !m_pDescriptorHeap)
        {
            CryLogAlways("[D3D_RT] Execute: DXR outputs missing after resize check - deferring execution");
            RT_EndFrameAndSignal();
            return;
        }
    }

    static std::atomic<bool> s_geometryBuilt(false);
    if (!s_geometryBuilt.load())
    {
        CryLogAlways("[D3D_RT] Execute: Building level geometry BLAS/TLAS (deferred from Init)...");
        HRESULT hr = CreateLevelGeometryBLASAndTLAS();
        if (SUCCEEDED(hr))
        {
            s_geometryBuilt = true;
            CryLogAlways("[D3D_RT] Execute: Level geometry BLAS/TLAS built successfully");
        }
        else
        {
            CryLogAlways("[D3D_RT] Execute: Level geometry build failed (hr=0x%08x) - will retry next frame", hr);
            RT_EndFrameAndSignal();
            return;
        }
    }

    if (!m_pTopLevelAS)
    {
        CryLogAlways("[D3D_RT] Execute: Top Level AS missing - skipping ray tracing");
        RT_EndFrameAndSignal();
        return;
    }

    ID3D12CommandAllocator* pAllocator = nullptr;
    RT_BeginFrameAllocator(&pAllocator);
    if (!pAllocator)
    {
        CryLogAlways("[D3D_RT] Execute: Failed to get command allocator");
        RT_EndFrameAndSignal();
        return;
    }

    ID3D12GraphicsCommandList4* pCommandList = nullptr;
    HRESULT hr = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pAllocator, nullptr, IID_PPV_ARGS(&pCommandList));
    if (FAILED(hr) || !pCommandList)
    {
        CryLogAlways("[D3D_RT] Execute: CreateCommandList failed (hr=0x%08x)", hr);
        RT_EndFrameAndSignal();
        return;
    }

#if defined(_DEBUG) || defined(PROFILE)
    {
        wchar_t wname[64];
        swprintf_s(wname, L"DXR_Execute_%llu", (unsigned long long)m_frameIndex);
        pCommandList->SetName(wname);
    }
#endif

    ID3D12GraphicsCommandList4* pDXRCommandList = pCommandList;
    if (!pDXRCommandList)
    {
        CryLogAlways("[D3D_RT] Execute: Command list does not support DXR interface");
        pCommandList->Release();
        RT_EndFrameAndSignal();
        return;
    }

    ExecuteRayTracingWithDebug(pDXRCommandList, m_outputWidth, m_outputHeight);
    m_frameRecordedWork = true;
    m_lastDispatchFence = m_lastSignaledFence + 1;

    hr = pCommandList->Close();
    if (FAILED(hr))
    {
        CryLogAlways("[D3D_RT] Execute: CommandList Close failed (hr=0x%08x)", hr);
        pCommandList->Release();
        s_deviceStabilityFailure = true;
        RT_EndFrameAndSignal();
        return;
    }

    ID3D12CommandList* ppCommandLists[] = { pCommandList };
    m_pCommandQueue->ExecuteCommandLists(1, ppCommandLists);

    pCommandList->Release();

    // Signal the frame fence and advance frame index
    RT_EndFrameAndSignal();

    // Compose the DXR outputs onto HDR after the fence is signaled.
    // This blocks on m_lastDispatchFence internally and draws a fullscreen pass.
    {
        const HRESULT chr = ComposeToHDROneShot();
        if (FAILED(chr))
        {
            // S_FALSE is acceptable (no HDR/Scene target), only log hard failures
            if (chr != S_FALSE)
                CryLogAlways("[D3D_RT] Execute: ComposeToHDROneShot failed hr=0x%08x", chr);
        }
    }

    if (currentCallCount % 1000 == 0)
        CryLogAlways("[D3D_RT] Execute: Successfully completed 1000 ray tracing frames (total: %u)", currentCallCount);

    DebugLogAllocatorState("EndExecute");
}


HRESULT CD3D_RT::CreateRayTracingResources()
{
    CryLogAlways("[D3D_RT] CreateRayTracingResources: Engine-managed DXR outputs (CTexture SRV|UAV)");

    if (!m_pDevice)
        return E_FAIL;

    {
        HRESULT hr = m_pDevice->GetDeviceRemovedReason();
        if (FAILED(hr))
        {
            CryLogAlways("[D3D_RT] CreateRayTracingResources: device removed (hr=0x%08x)", hr);
            return hr;
        }
    }

    // Prefer Scene target for size; HDR is engine-owned later in the frame
    CTexture* pTexSceneTarget = m_graphicsPipelineResources.m_pTexSceneTarget;
    CTexture* pTexHDRTarget = m_graphicsPipelineResources.m_pTexHDRTarget;

    ID3D12Resource* pScene = RT_GetNativeFromCTexture(pTexSceneTarget);
    ID3D12Resource* pHDR = RT_GetNativeFromCTexture(pTexHDRTarget);
    ID3D12Resource* pCompose = pScene ? pScene : pHDR;

    if (!pCompose)
    {
        CryLogAlways("[D3D_RT] CreateRayTracingResources: No valid compose target");
        return E_FAIL;
    }

    const D3D12_RESOURCE_DESC composeDesc = pCompose->GetDesc();
    UINT outW = (UINT)composeDesc.Width;
    UINT outH = composeDesc.Height;
    const UINT kMaxDim = 4096;
    outW = min(outW, kMaxDim);
    outH = min(outH, kMaxDim);

    // Descriptor heap (shader-visible)
    if (!m_pDescriptorHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = 32;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hrHeap = m_pDevice->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_pDescriptorHeap));
        if (FAILED(hrHeap) || !m_pDescriptorHeap)
        {
            CryLogAlways("[D3D_RT] CreateRayTracingResources: Descriptor heap creation failed (hr=0x%08x)", hrHeap);
            return hrHeap ? hrHeap : E_FAIL;
        }
        if ((m_pDescriptorHeap->GetDesc().Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0)
        {
            CryLogAlways("[D3D_RT] CreateRayTracingResources: CRITICAL heap not shader-visible");
            SAFE_RELEASE(m_pDescriptorHeap);
            return E_FAIL;
        }
    }
    if (m_descriptorSize == 0)
        m_descriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // CPU-only mirror heap for ClearUnorderedAccessView* CPU handles
    if (!g_pRT_ClearCpuHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC ch{};
        ch.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        ch.NumDescriptors = 32;
        ch.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        const HRESULT hrCH = m_pDevice->CreateDescriptorHeap(&ch, IID_PPV_ARGS(&g_pRT_ClearCpuHeap));
        if (FAILED(hrCH))
            CryLogAlways("[D3D_RT] CreateRayTracingResources: CPU UAV mirror heap creation failed (hr=0x%08x)", hrCH);
        else
            g_RT_ClearCpuInc = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // Dummy 1x1 SRV texture
    if (!g_pRT_NullSrvTex2D)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 1; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (SUCCEEDED(m_pDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_pRT_NullSrvTex2D))))
        {
            // Initialize the dummy to white so missing albedo doesn't zero out GI
            RT_Init1x1RGBA8White(m_pDevice, m_pCommandQueue, &g_pRT_NullSrvTex2D);
        }
    }

    // Outputs
    m_pTexDXR_AO = RT_CreateOrResizeDXROutFmt(m_pTexDXR_AO, "$DXR_AO", (int)outW, (int)outH, eTF_R32F);
    if (m_pTexDXR_AO)
    {
        g_pRT_AOOutput = RT_GetNativeFromCTexture(m_pTexDXR_AO);
        g_RT_AOOutputOwned = false;
    }
    else
        CryLogAlways("[D3D_RT] CreateRayTracingResources: WARNING AO texture create failed");

    // CRITICAL: Use 32-bit float RGBA for GI/Reflections to match RWTexture2D<float4> exactly
    m_pTexDXR_GI = RT_CreateOrResizeDXROutFmt(m_pTexDXR_GI, "$DXR_GI", (int)outW, (int)outH, eTF_R16G16B16A16F);
    if (!m_pTexDXR_GI)
    {
        CryLogAlways("[D3D_RT] CreateRayTracingResources: ERROR GI texture create failed");
        return E_FAIL;
    }
    m_pTexDXR_Refl = RT_CreateOrResizeDXROutFmt(m_pTexDXR_Refl, "$DXR_Refl", (int)outW, (int)outH, eTF_R16G16B16A16F);

    m_pRaytracingOutput = RT_GetNativeFromCTexture(m_pTexDXR_GI);
    m_pReflectionOutput = m_pTexDXR_Refl ? RT_GetNativeFromCTexture(m_pTexDXR_Refl) : m_pRaytracingOutput;
    if (!m_pRaytracingOutput)
        return E_FAIL;

    // Constants buffer (size must cover RayTracingConstantsGPU, 256B-aligned)
    const UINT cbRequired = ALIGN((UINT)sizeof(RayTracingConstantsGPU), 256u);
    if (!m_pConstantsBuffer || m_pConstantsBuffer->GetDesc().Width < cbRequired)
    {
        SAFE_RELEASE(m_pConstantsBuffer);
        D3D12_HEAP_PROPERTIES hup{}; hup.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbd{};
        cbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbd.Width = cbRequired; // FIX: previously hardcoded to 256 (overflowed)
        cbd.Height = 1;
        cbd.DepthOrArraySize = 1;
        cbd.MipLevels = 1;
        cbd.Format = DXGI_FORMAT_UNKNOWN;
        cbd.SampleDesc.Count = 1;
        cbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hrCB = m_pDevice->CreateCommittedResource(&hup, D3D12_HEAP_FLAG_NONE, &cbd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pConstantsBuffer));
        if (FAILED(hrCB))
        {
            CryLogAlways("[D3D_RT] CreateRayTracingResources: constants buffer create failed (size=%u, hr=0x%08x)", cbRequired, hrCB);
        }
#if defined(_DEBUG) || defined(PROFILE)
        else { m_pConstantsBuffer->SetName(L"DXR_RayTracingConstants"); }
#endif
    }

    m_outputWidth = outW;
    m_outputHeight = outH;

    g_RT_GIState = D3D12_RESOURCE_STATE_COMMON;
    g_RT_ReflState = D3D12_RESOURCE_STATE_COMMON;
    g_RT_AOState = D3D12_RESOURCE_STATE_COMMON;
    if (m_pReflectionOutput == m_pRaytracingOutput)
        g_RT_ReflState = g_RT_GIState;


    // Standalone AO fallback if engine texture failed
    if (!g_pRT_AOOutput)
    {
        D3D12_HEAP_PROPERTIES heapProps{}; heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = outW; desc.Height = outH;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (SUCCEEDED(m_pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_pRT_AOOutput))))
        {
            g_RT_AOOutputOwned = true;
            g_pRT_AOOutput->SetName(L"DXR_AO_Output_u3");
        }
        else
            CryLogAlways("[D3D_RT] CreateRayTracingResources: AO standalone allocation failed");
    }

    // Stats buffer (needed before descriptor writes)
    CreateRayStatsBuffer();

    auto HasInvalidMipChain = [&](ID3D12Resource* r)->bool
        {
            if (!r) return false;
            D3D12_RESOURCE_DESC d = r->GetDesc();
            if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) return false;
            if (d.MipLevels == 0) return false;
            UINT maxDim = (UINT)std::max<UINT64>(d.Width, d.Height);
            UINT allowed = 1; while (maxDim > 1) { maxDim >>= 1; ++allowed; }
            return d.MipLevels > allowed;
        };

    // Descriptor writes
    {
        auto incVis = [&](D3D12_CPU_DESCRIPTOR_HANDLE& h) { h.ptr += m_descriptorSize; };

        D3D12_CPU_DESCRIPTOR_HANDLE cpuVis = m_pDescriptorHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_CPU_DESCRIPTOR_HANDLE cpuMirror{};
        if (g_pRT_ClearCpuHeap)
            cpuMirror = g_pRT_ClearCpuHeap->GetCPUDescriptorHandleForHeapStart();
        const UINT mirrorInc = g_RT_ClearCpuInc;

        auto incBoth = [&](D3D12_CPU_DESCRIPTOR_HANDLE& a, D3D12_CPU_DESCRIPTOR_HANDLE& b)
            {
                a.ptr += m_descriptorSize;
                if (b.ptr) b.ptr += mirrorInc;
            };

        // t0 TLAS
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrv{};
            tlasSrv.Format = DXGI_FORMAT_UNKNOWN;
            tlasSrv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            tlasSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            tlasSrv.RaytracingAccelerationStructure.Location =
                m_pTopLevelAS ? m_pTopLevelAS->GetGPUVirtualAddress() : 0;
            m_pDevice->CreateShaderResourceView(nullptr, &tlasSrv, cpuVis);
        }

        // u0 GI
        incBoth(cpuVis, cpuMirror);
        {
            ID3D12Resource* r = m_pRaytracingOutput;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Format = RT_TypelessToTypedUAV(r->GetDesc().Format);
            if (uav.Format == DXGI_FORMAT_UNKNOWN) uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            m_pDevice->CreateUnorderedAccessView(r, nullptr, &uav, cpuVis);
            if (cpuMirror.ptr) m_pDevice->CreateUnorderedAccessView(r, nullptr, &uav, cpuMirror);
        }
        // u1 Reflection
        incBoth(cpuVis, cpuMirror);
        {
            ID3D12Resource* r = m_pReflectionOutput ? m_pReflectionOutput : m_pRaytracingOutput;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Format = RT_TypelessToTypedUAV(r->GetDesc().Format);
            if (uav.Format == DXGI_FORMAT_UNKNOWN) uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            m_pDevice->CreateUnorderedAccessView(r, nullptr, &uav, cpuVis);
            if (cpuMirror.ptr) m_pDevice->CreateUnorderedAccessView(r, nullptr, &uav, cpuMirror);
        }
        // u2 Stats
        incBoth(cpuVis, cpuMirror);
        {
            ID3D12Resource* r = m_pRayStatsBuffer;
            D3D12_RESOURCE_DESC rd = r->GetDesc();
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Format = DXGI_FORMAT_R32_UINT;
            uav.Buffer.FirstElement = 0;
            UINT elemCount = (UINT)(rd.Width / sizeof(UINT));
            if (elemCount == 0) elemCount = 1;
            uav.Buffer.NumElements = elemCount;
            m_pDevice->CreateUnorderedAccessView(r, nullptr, &uav, cpuVis);
            if (cpuMirror.ptr) m_pDevice->CreateUnorderedAccessView(r, nullptr, &uav, cpuMirror);
        }
        // u3 AO
        incBoth(cpuVis, cpuMirror);
        {
            ID3D12Resource* r = g_pRT_AOOutput;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Format = DXGI_FORMAT_R32_FLOAT;
            m_pDevice->CreateUnorderedAccessView(r, nullptr, &uav, cpuVis);
            if (cpuMirror.ptr) m_pDevice->CreateUnorderedAccessView(r, nullptr, &uav, cpuMirror);
        }

        // Helpers
        auto CreateSrv2D = [&](ID3D12Resource* r, D3D12_CPU_DESCRIPTOR_HANDLE h)
            {
                if (!r || HasInvalidMipChain(r)) r = g_pRT_NullSrvTex2D;
                D3D12_RESOURCE_DESC d = r->GetDesc();
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                // Mips
                {
                    UINT maxDim = (UINT)std::max<UINT64>(d.Width, d.Height);
                    UINT allowed = 1;
                    while (maxDim > 1) { maxDim >>= 1; ++allowed; }
                    UINT requested = d.MipLevels ? d.MipLevels : 1;
                    srv.Texture2D.MipLevels = std::min(requested, allowed);
                }
                srv.Format = d.Format;
                switch (srv.Format)
                {
                case DXGI_FORMAT_R8G8B8A8_TYPELESS: srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
                case DXGI_FORMAT_R16G16B16A16_TYPELESS: srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
                case DXGI_FORMAT_R32_TYPELESS: srv.Format = DXGI_FORMAT_R32_FLOAT; break;
                default: break;
                }
                if (r == g_pRT_NullSrvTex2D)
                    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                m_pDevice->CreateShaderResourceView(r, &srv, h);
            };
        auto CreateSrvCube = [&](ID3D12Resource* r, D3D12_CPU_DESCRIPTOR_HANDLE h)
            {
                if (!r || HasInvalidMipChain(r))
                {
                    CreateSrv2D(nullptr, h);
                    return;
                }
                D3D12_RESOURCE_DESC d = r->GetDesc();
                if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (d.DepthOrArraySize % 6) == 0 && d.MipLevels >= 1)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                    {
                        UINT maxDim = (UINT)std::max<UINT64>(d.Width, d.Height);
                        UINT allowed = 1;
                        while (maxDim > 1) { maxDim >>= 1; ++allowed; }
                        UINT requested = d.MipLevels ? d.MipLevels : 1;
                        srv.TextureCube.MipLevels = std::min(requested, allowed);
                    }
                    srv.TextureCube.MostDetailedMip = 0;
                    srv.TextureCube.ResourceMinLODClamp = 0.0f;
                    srv.Format = d.Format;
                    switch (srv.Format)
                    {
                    case DXGI_FORMAT_R8G8B8A8_TYPELESS: srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
                    case DXGI_FORMAT_R16G16B16A16_TYPELESS: srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
                    default: break;
                    }
                    m_pDevice->CreateShaderResourceView(r, &srv, h);
                }
                else
                {
                    CreateSrv2D(r, h);
                }
            };
        auto GetSrvResourceOrNull = [&](CTexture* pTex)->ID3D12Resource*
            {
                return RT_GetNativeFromCTexture(pTex);
            };

        // GBuffer: t1..t4
        CTexture* pTexGBufferDiffuse = m_graphicsPipelineResources.m_pTexSceneDiffuse;
        CTexture* pTexGBufferNormals = m_graphicsPipelineResources.m_pTexSceneNormalsMap;
        CTexture* pTexLinearDepth = m_graphicsPipelineResources.m_pTexLinearDepth;
        CTexture* pTexGBufferSpecular = m_graphicsPipelineResources.m_pTexSceneSpecular;

        incVis(cpuVis); CreateSrv2D(GetSrvResourceOrNull(pTexGBufferDiffuse), cpuVis); // t1
        incVis(cpuVis); CreateSrv2D(GetSrvResourceOrNull(pTexGBufferNormals), cpuVis); // t2
        incVis(cpuVis); CreateSrv2D(GetSrvResourceOrNull(pTexLinearDepth), cpuVis); // t3
        incVis(cpuVis); CreateSrv2D(GetSrvResourceOrNull(pTexGBufferSpecular), cpuVis); // t4

        // Luminance (t5)
        CTexture* pLumTex = nullptr;
#if defined(MAX_GPU_NUM)
        pLumTex = m_graphicsPipelineResources.m_pTexHDRMeasuredLuminance[0];
#endif
        if (!pLumTex)
            pLumTex = CRendererResources::s_ptexHDRMeasuredLuminanceDummy;
        ID3D12Resource* pLumNative = GetSrvResourceOrNull(pLumTex);
        incVis(cpuVis); CreateSrv2D(pLumNative, cpuVis); // t5

        // Environment overlay 2D (t6)
        CTexture* pSkyOverlay = nullptr;
        if (gcpRendD3D)
        {
            const int tid = gcpRendD3D->GetRenderThreadID();
            const auto& skyInfo = gcpRendD3D->m_p3DEngineCommon[tid].m_SkyInfo;
            if (skyInfo.m_bApplySkyBox && skyInfo.m_pSkyBoxTexture)
                pSkyOverlay = skyInfo.m_pSkyBoxTexture.get();
        }
        incVis(cpuVis); CreateSrv2D(GetSrvResourceOrNull(pSkyOverlay), cpuVis); // t6

        CTexture* pSkyCubeTex = nullptr;

        // Prefer an active/global env probe first (pseudo-logic; replace with your actual source)
        if (gcpRendD3D)
        {
            // Example: try the current global environment cubemap if your project exposes one.
            //pSkyCubeTex = gcpRendD3D->GetGlobalEnvProbeCube(); // <- use your real accessor if available
        }

        // Fallbacks
        if (!pSkyCubeTex)
            pSkyCubeTex = CRendererResources::s_ptexDefaultProbeCM;

        ID3D12Resource* pSkyCubeNative = GetSrvResourceOrNull(pSkyCubeTex);
        incVis(cpuVis); CreateSrvCube(pSkyCubeNative, cpuVis); // t7


        // Irradiance cube (t8)
        if (!g_pRT_IrradianceCube && pSkyCubeNative)
            g_pRT_IrradianceCube = RT_BuildIrradianceCube(m_pDevice, m_pCommandQueue, pSkyCubeNative, 64, 64);
        ID3D12Resource* pIrr = g_pRT_IrradianceCube ? g_pRT_IrradianceCube : pSkyCubeNative;
        incVis(cpuVis); CreateSrvCube(pIrr, cpuVis); // t8

        // Remaining SRVs (t9..t12) -> dummies (future use)
        for (int i = 0; i < 4; ++i)
        {
            incVis(cpuVis); CreateSrv2D(nullptr, cpuVis);
        }

        CryLogAlways("[DXR][SRVMap] t1 Diff=%p t2 Norm=%p t3 Depth=%p t4 Spec=%p t5 Lum=%p t6 Env2D=%p t7 EnvCube=%p t8 Irr=%p",
            RT_GetNativeFromCTexture(pTexGBufferDiffuse),
            RT_GetNativeFromCTexture(pTexGBufferNormals),
            RT_GetNativeFromCTexture(pTexLinearDepth),
            RT_GetNativeFromCTexture(pTexGBufferSpecular),
            pLumNative,
            GetSrvResourceOrNull(pSkyOverlay),
            pSkyCubeNative,
            pIrr);
    }

    // Register for late composition
    if (!g_dxrComposeRegistered)
    {
        g_pDXRForPostCompose = this;
        g_dxrComposeRegistered = true;
        CryLogAlways("[D3D_RT] CreateRayTracingResources: Registered for late composition");
    }

    CryLogAlways("[D3D_RT] CreateRayTracingResources: SUCCESS (GI=%ux%u)", outW, outH);
    return S_OK;
}


void CD3D_RT::ExecuteRayTracingWithDebug(ID3D12GraphicsCommandList4* pCommandList, UINT width, UINT height)
{
    // Hard validation
    if (!pCommandList || !m_pDevice || !m_pDescriptorHeap || !m_pRaytracingOutput) { CryLogAlways("[DXR][DBG] Missing core resources"); return; }
    if (!m_pRaytracingPSO) { CryLogAlways("[DXR][DBG] Missing PSO"); return; }
    if (!m_pTopLevelAS) { CryLogAlways("[DXR][DBG] Missing TLAS"); return; }

    // Derive dispatch dimensions from the actual GI UAV (fixes partial writes)
    D3D12_RESOURCE_DESC giDesc = m_pRaytracingOutput->GetDesc();
    const UINT dispatchW = (UINT)std::min<UINT64>(giDesc.Width, 0xFFFFFFFFull);
    const UINT dispatchH = giDesc.Height ? giDesc.Height : 1U;
    if (dispatchW == 0 || dispatchH == 0) { CryLogAlways("[DXR][DBG] Zero GI dimensions"); return; }

    if (width != dispatchW || height != dispatchH)
        CryLogAlways("[DXR][DBG] Mismatch: requested %ux%u but GI is %ux%u. Using GI size.", width, height, dispatchW, dispatchH);

    // 0) Refresh TLAS SRV (t0) right before dispatch (guard against stale descriptors)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS tlasVA = m_pTopLevelAS->GetGPUVirtualAddress();
        if (tlasVA == 0) { CryLogAlways("[DXR][DBG] TLAS VA is 0 - aborting"); return; }

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_pDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrv{};
        tlasSrv.Format = DXGI_FORMAT_UNKNOWN;
        tlasSrv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        tlasSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tlasSrv.RaytracingAccelerationStructure.Location = tlasVA;
        m_pDevice->CreateShaderResourceView(nullptr, &tlasSrv, cpu);
    }

    // 1) Transition outputs to UAV for writing
    RT_TransitionTracked(pCommandList, m_pRaytracingOutput, g_RT_GIState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (m_pReflectionOutput)
        RT_TransitionTracked(pCommandList, m_pReflectionOutput, g_RT_ReflState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (g_pRT_AOOutput)
        RT_TransitionTracked(pCommandList, g_pRT_AOOutput, g_RT_AOState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (m_pRayStatsBuffer)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_pRayStatsBuffer;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        pCommandList->ResourceBarrier(1, &b);
    }

    // 2) Pre-dispatch clear to guarantee full-frame visibility (r_DXR_UAVClear: 0/1)
    int preClear = 1; // default ON to diagnose "small patches"
    if (preClear && g_pRT_ClearCpuHeap)
    {
        const UINT gpuInc = m_descriptorSize;
        const UINT cpuInc = g_RT_ClearCpuInc;
        const D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = m_pDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = g_pRT_ClearCpuHeap->GetCPUDescriptorHandleForHeapStart();

        ID3D12DescriptorHeap* heaps[] = { m_pDescriptorHeap };
        pCommandList->SetDescriptorHeaps(1, heaps);

        // u0: GI
        {
            D3D12_GPU_DESCRIPTOR_HANDLE gpuUAV{ gpuStart.ptr + gpuInc * 1 };
            D3D12_CPU_DESCRIPTOR_HANDLE cpuUAV{ cpuStart.ptr + cpuInc * 1 };
            const float clr[4] = { 0.8f, 0.0f, 0.0f, 1.0f }; // red
            pCommandList->ClearUnorderedAccessViewFloat(gpuUAV, cpuUAV, m_pRaytracingOutput, clr, 0, nullptr);
        }
        // u1: Refl (if different)
        if (m_pReflectionOutput && m_pReflectionOutput != m_pRaytracingOutput)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE gpuUAV{ gpuStart.ptr + gpuInc * 2 };
            D3D12_CPU_DESCRIPTOR_HANDLE cpuUAV{ cpuStart.ptr + cpuInc * 2 };
            const float clr[4] = { 0.0f, 0.0f, 0.8f, 1.0f }; // blue
            pCommandList->ClearUnorderedAccessViewFloat(gpuUAV, cpuUAV, m_pReflectionOutput, clr, 0, nullptr);
        }
        // u3: AO
        if (g_pRT_AOOutput)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE gpuUAV{ gpuStart.ptr + gpuInc * 4 };
            D3D12_CPU_DESCRIPTOR_HANDLE cpuUAV{ cpuStart.ptr + cpuInc * 4 };
            const float clr[4] = { 0.0f, 0.8f, 0.0f, 1.0f }; // green
            pCommandList->ClearUnorderedAccessViewFloat(gpuUAV, cpuUAV, g_pRT_AOOutput, clr, 0, nullptr);
        }

        // Ensure clears are visible to subsequent UAV writes
        D3D12_RESOURCE_BARRIER uavs[3]{};
        int n = 0;
        uavs[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavs[n++].UAV.pResource = m_pRaytracingOutput;
        if (m_pReflectionOutput && m_pReflectionOutput != m_pRaytracingOutput)
        {
            uavs[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavs[n++].UAV.pResource = m_pReflectionOutput;
        }
        if (g_pRT_AOOutput)
        {
            uavs[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavs[n++].UAV.pResource = g_pRT_AOOutput;
        }
        if (n) pCommandList->ResourceBarrier(n, uavs);
    }

    // 3) Bind heap + global RS + DXR PSO
    {
        ID3D12DescriptorHeap* heaps[] = { m_pDescriptorHeap };
        pCommandList->SetDescriptorHeaps(1, heaps);
    }
    pCommandList->SetComputeRootSignature(m_pGlobalRootSignature);
    pCommandList->SetPipelineState1(m_pRaytracingPSO);

    // Root tables: [0]=b0, [1]=t0, [2]=u0..u3, [3]=t1..t12
    const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = m_pDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    if (m_pConstantsBuffer)
        pCommandList->SetComputeRootConstantBufferView(0, m_pConstantsBuffer->GetGPUVirtualAddress());
    pCommandList->SetComputeRootDescriptorTable(1, heapStart);                                   // t0
    pCommandList->SetComputeRootDescriptorTable(2, { heapStart.ptr + m_descriptorSize * 1 });    // u0..u3
    pCommandList->SetComputeRootDescriptorTable(3, { heapStart.ptr + m_descriptorSize * 5 });    // t1..t12

    // 4) Upload constants with the real GI size
    UpdateRayTracingConstants(dispatchW, dispatchH, 0);

    // 5) Dispatch rays (guarded by r_DXR_NoDispatch)
    int noDispatch = 0;
    if (gEnv && gEnv->pConsole) if (ICVar* cv = gEnv->pConsole->GetCVar("r_DXR_NoDispatch")) noDispatch = cv->GetIVal();

    if (!noDispatch)
    {
        D3D12_DISPATCH_RAYS_DESC dr{};
        dr.RayGenerationShaderRecord.StartAddress = m_pRayGenShaderTable->GetGPUVirtualAddress();
        dr.RayGenerationShaderRecord.SizeInBytes = 64;
        dr.MissShaderTable.StartAddress = m_pMissShaderTable->GetGPUVirtualAddress();
        dr.MissShaderTable.SizeInBytes = 64;
        dr.MissShaderTable.StrideInBytes = 64;
        dr.HitGroupTable.StartAddress = m_pHitGroupShaderTable->GetGPUVirtualAddress();
        dr.HitGroupTable.SizeInBytes = 64;
        dr.HitGroupTable.StrideInBytes = 64;
        dr.Width = dispatchW;
        dr.Height = dispatchH;
        dr.Depth = 1;
        pCommandList->DispatchRays(&dr);
    }
    else
    {
        CryLogAlways("[DXR][DBG] r_DXR_NoDispatch=1 -> skipping DispatchRays");
    }

    // 6) UAV barriers after writes
    {
        D3D12_RESOURCE_BARRIER uavs[4]{};
        int n = 0;
        uavs[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavs[n++].UAV.pResource = m_pRaytracingOutput;
        if (m_pReflectionOutput && m_pReflectionOutput != m_pRaytracingOutput)
        {
            uavs[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavs[n++].UAV.pResource = m_pReflectionOutput;
        }
        if (m_pRayStatsBuffer)
        {
            uavs[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavs[n++].UAV.pResource = m_pRayStatsBuffer;
        }
        if (g_pRT_AOOutput)
        {
            uavs[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavs[n++].UAV.pResource = g_pRT_AOOutput;
        }
        if (n) pCommandList->ResourceBarrier(n, uavs);
    }

    // 7) Stats readback/log
    {
        uint32 frame = gcpRendD3D ? (uint32)gcpRendD3D->GetRenderFrameID() : 0;
        uint32 curRays = dispatchW * dispatchH;
        static uint32 s_total = 0;
        s_total += curRays;
        ReadRayStats(pCommandList, frame, curRays, s_total);
    }

    // 8) Transition outputs to SRV for compose
    const D3D12_RESOURCE_STATES kSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    RT_TransitionTracked(pCommandList, m_pRaytracingOutput, g_RT_GIState, kSRV);
    if (m_pReflectionOutput)
        RT_TransitionTracked(pCommandList, m_pReflectionOutput, g_RT_ReflState, kSRV);
    if (g_pRT_AOOutput)
        RT_TransitionTracked(pCommandList, g_pRT_AOOutput, g_RT_AOState, kSRV);

    // 9) Diag
    static uint32 s_log = 0;
    if ((s_log++ & 0x7F) == 0)
        CryLogAlways("[DXR][DBG] DispatchRays %ux%u (States GI=%u Refl=%u AO=%u, preClear=%d)", dispatchW, dispatchH, (uint32)g_RT_GIState, (uint32)g_RT_ReflState, (uint32)g_RT_AOState, preClear);
}

// NEW: Create buffer for GPU to write hit/miss statistics
HRESULT CD3D_RT::CreateRayStatsBuffer()
{
    if (!m_pDevice) return E_FAIL;
    if (m_pRayStatsBuffer) return S_OK;

    const UINT64 bufferSize = 256;

    D3D12_HEAP_PROPERTIES hpDefault = {};
    hpDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bufferSize;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Create in COMMON (no warning). We'll transition before first UAV use.
    HRESULT hr = m_pDevice->CreateCommittedResource(
        &hpDefault, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pRayStatsBuffer));
    if (FAILED(hr)) return hr;

    hpDefault.Type = D3D12_HEAP_TYPE_READBACK;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = m_pDevice->CreateCommittedResource(
        &hpDefault, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_pRayStatsReadbackBuffer));

    return hr;
}

// NEW: Read back hit/miss statistics from GPU
void CD3D_RT::ReadRayStats(ID3D12GraphicsCommandList4* pCommandList, uint32_t frameNumber, uint32_t currentRayCount, uint32_t totalRaysDispatched)
{
    if (!m_pRayStatsBuffer || !m_pRayStatsReadbackBuffer || !pCommandList) return;

    // UAV -> COPY_SOURCE
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_pRayStatsBuffer;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pCommandList->ResourceBarrier(1, &b);
    }

    pCommandList->CopyResource(m_pRayStatsReadbackBuffer, m_pRayStatsBuffer);

    // COPY_SOURCE -> UAV
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_pRayStatsBuffer;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pCommandList->ResourceBarrier(1, &b);
    }

    // existing scheduling logic follows...
    static bool s_pendingReadback = false;
    static uint32_t s_lastReadFrame = 0;
    if (!s_pendingReadback) { s_pendingReadback = true; s_lastReadFrame = frameNumber; return; }
    if (frameNumber - s_lastReadFrame >= 300) { ProcessRayStatsReadback(frameNumber, currentRayCount, totalRaysDispatched); s_pendingReadback = false; }
}

// B) Add helper to create DEFAULT buffers for BLAS building
// Implement CreateDedicatedUploadBuffer for proper resource management
HRESULT CD3D_RT::CreateDedicatedUploadBuffer(const void* srcData, UINT64 byteSize, ID3D12Resource** ppBuffer)
{
    if (!m_pDevice || !ppBuffer || byteSize == 0)
    {
        return E_INVALIDARG;
    }

    *ppBuffer = nullptr;

    // CRITICAL: Device state validation
    HRESULT deviceRemovedHr = m_pDevice->GetDeviceRemovedReason();
    if (FAILED(deviceRemovedHr))
    {
        CryLogAlways("[D3D_RT] CreateDedicatedUploadBuffer: Device removed/reset (hr=0x%08x)", deviceRemovedHr);
        return deviceRemovedHr;
    }

    // CRITICAL: Size validation
    if (byteSize > (1ull << 32)) // Max 4GB
    {
        CryLogAlways("[D3D_RT] CreateDedicatedUploadBuffer: Size too large (%llu bytes)", byteSize);
        return E_INVALIDARG;
    }

    // Create UPLOAD heap properties
    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeapProps.CreationNodeMask = 1;
    uploadHeapProps.VisibleNodeMask = 1;

    // Create resource description
    D3D12_RESOURCE_DESC uploadBufferDesc = {};
    uploadBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadBufferDesc.Alignment = 0;
    uploadBufferDesc.Width = byteSize;
    uploadBufferDesc.Height = 1;
    uploadBufferDesc.DepthOrArraySize = 1;
    uploadBufferDesc.MipLevels = 1;
    uploadBufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadBufferDesc.SampleDesc.Count = 1;
    uploadBufferDesc.SampleDesc.Quality = 0;
    uploadBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadBufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    // Create upload buffer
    HRESULT hr = m_pDevice->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(ppBuffer));

    if (FAILED(hr))
    {
        CryLogAlways("[D3D_RT] CreateDedicatedUploadBuffer: Failed to create upload buffer (hr=0x%08x, size=%llu)", hr, byteSize);
        return hr;
    }

    // Map and copy data if provided
    if (srcData)
    {
        void* pMappedData = nullptr;
        D3D12_RANGE readRange = { 0, 0 }; // We're not reading

        hr = (*ppBuffer)->Map(0, &readRange, &pMappedData);
        if (SUCCEEDED(hr) && pMappedData)
        {
            __try
            {
                memcpy(pMappedData, srcData, static_cast<size_t>(byteSize));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                CryLogAlways("[D3D_RT] CreateDedicatedUploadBuffer: Exception during memcpy");
                (*ppBuffer)->Unmap(0, nullptr);
                (*ppBuffer)->Release();
                *ppBuffer = nullptr;
                return E_FAIL;
            }

            D3D12_RANGE writtenRange = { 0, byteSize };
            (*ppBuffer)->Unmap(0, &writtenRange);
        }
        else
        {
            CryLogAlways("[D3D_RT] CreateDedicatedUploadBuffer: Failed to map upload buffer (hr=0x%08x)", hr);
            (*ppBuffer)->Release();
            *ppBuffer = nullptr;
            return hr;
        }
    }

    return S_OK;
}




HRESULT CD3D_RT::CreateDefaultBufferFromData(
    const void* srcData,
    UINT64 byteSize,
    ID3D12Resource** ppDefaultBuffer,
    ID3D12Resource** ppUploadBuffer,
    ID3D12GraphicsCommandList* pCmdList,
    const char* debugName)
{
    if (!m_pDevice || !srcData || byteSize == 0 || !ppDefaultBuffer || !ppUploadBuffer || !pCmdList)
        return E_INVALIDARG;

    *ppDefaultBuffer = nullptr;
    *ppUploadBuffer = nullptr;

    // 1) Create the DEFAULT heap buffer
    CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC   defaultDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

    HRESULT hr = m_pDevice->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &defaultDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(ppDefaultBuffer));
    if (FAILED(hr)) return hr;

    if (debugName && *debugName)
    {
        wchar_t wname[256] = {};
        MultiByteToWideChar(CP_UTF8, 0, debugName, -1, wname, (int)(sizeof(wname) / sizeof(wname[0])));
        (*ppDefaultBuffer)->SetName(wname);
    }

    // 2) Create the UPLOAD heap buffer and copy CPU data into it
    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC   uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

    hr = m_pDevice->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(ppUploadBuffer));
    if (FAILED(hr))
    {
        SAFE_RELEASE(*ppDefaultBuffer);
        return hr;
    }

    if (debugName && *debugName)
    {
        std::string upStr(debugName);
        upStr += " [UPLOAD]";
        wchar_t wup[256] = {};
        MultiByteToWideChar(CP_UTF8, 0, upStr.c_str(), -1, wup, (int)(sizeof(wup) / sizeof(wup[0])));
        (*ppUploadBuffer)->SetName(wup);
    }

    // Map + memcpy
    void* pData = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    hr = (*ppUploadBuffer)->Map(0, &readRange, &pData);
    if (FAILED(hr))
    {
        SAFE_RELEASE(*ppUploadBuffer);
        SAFE_RELEASE(*ppDefaultBuffer);
        return hr;
    }
    memcpy(pData, srcData, static_cast<size_t>(byteSize));
    (*ppUploadBuffer)->Unmap(0, nullptr);

    // 3) Transition DEFAULT buffer COMMON -> COPY_DEST
    {
        const auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
            *ppDefaultBuffer,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST);
        pCmdList->ResourceBarrier(1, &toCopyDest);
    }

    // 4) Record copy into DEFAULT
    pCmdList->CopyBufferRegion(*ppDefaultBuffer, 0, *ppUploadBuffer, 0, byteSize);

    // 5) Transition DEFAULT buffer COPY_DEST -> GENERIC_READ
    {
        const auto toGenericRead = CD3DX12_RESOURCE_BARRIER::Transition(
            *ppDefaultBuffer,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        pCmdList->ResourceBarrier(1, &toGenericRead);
    }

    return S_OK;
}




// 2) After GPU finishes building scene TLAS, set m_pTopLevelAS and refresh the SRV descriptor.
HRESULT CD3D_RT::CreateLevelGeometryBLASAndTLAS()
{
    CryLogAlways("[D3D_RT] CreateLevelGeometryBLASAndTLAS: Begin");

    if (!m_pDevice) return E_FAIL;
    if (RT_IsLevelStreamingBusy())
    {
        CryLogAlways("[D3D_RT] CreateLevelGeometryBLASAndTLAS: Streaming busy -> defer");
        return E_PENDING;
    }

    auto refreshQueue = [this]() -> bool
        {
            if (!gcpRendD3D) return false;
            void* pDevWrap = gcpRendD3D->GetDevice();
            if (!pDevWrap) return false;
            CCryDX12Device* pDX12 = reinterpret_cast<CCryDX12Device*>(pDevWrap);
            if (!pDX12) return false;
            NCryDX12::CDevice* native = pDX12->GetDX12Device();
            if (!native) return false;
            m_pCommandQueue = native->GetScheduler().GetCommandListPool(CMDQUEUE_GRAPHICS).GetD3D12CommandQueue();
            return m_pCommandQueue != nullptr;
        };
    if (!refreshQueue()) return E_FAIL;

    HRESULT hrDev = m_pDevice->GetDeviceRemovedReason();
    if (FAILED(hrDev)) return hrDev;

    std::vector<SafeGeometryData> levelGeometry;
    if (!ExtractLevelGeometry(levelGeometry))
        CryLogAlways("[D3D_RT] CreateLevelGeometryBLASAndTLAS: Extraction returned no real geometry (may be procedural only)");

    // Append terrain patch (near camera). Must happen BEFORE limiting count.
    RT_AppendTerrainPatch(levelGeometry);

    if (levelGeometry.empty())
    {
        CryLogAlways("[D3D_RT] CreateLevelGeometryBLASAndTLAS: No geometry after terrain patch");
        return E_FAIL;
    }

    // Guarantee at least one near‑camera surface survives:
    CCamera cam = gEnv && gEnv->pSystem ? gEnv->pSystem->GetViewCamera() : CCamera();
    const Vec3 camPos = cam.GetPosition();

    // If NOTHING is within 200m horizontally of camera, inject a big debug quad
    bool hasNear = false;
    for (auto& g : levelGeometry)
    {
        if (!g.vertices.empty())
        {
            if ((g.vertices[0] - camPos).GetLengthSquared() < (200.0f * 200.0f))
            {
                hasNear = true;
                break;
            }
        }
    }
    if (!hasNear)
    {
        CryLogAlways("[D3D_RT] CreateLevelGeometryBLASAndTLAS: Injecting debug quad (no near geometry)");
        RT_InsertCameraDebugQuad(levelGeometry, cam);
    }

    // Sort by distance & keep nearest 8
    RT_SortKeepNearest(levelGeometry, camPos, 8);

    CryLogAlways("[D3D_RT] CreateLevelGeometryBLASAndTLAS: Using %zu nearest objects after sort (cam=%.1f,%.1f,%.1f)",
        levelGeometry.size(), camPos.x, camPos.y, camPos.z);

    // Allocate build command objects
    ID3D12CommandAllocator* pAlloc = nullptr;
    if (FAILED(m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pAlloc))))
        return E_FAIL;
#if defined(_DEBUG) || defined(PROFILE)
    pAlloc->SetName(L"DXR_ASBuildAlloc_Level");
#endif

    ID3D12GraphicsCommandList4* pCL = nullptr;
    if (FAILED(m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pAlloc, nullptr, IID_PPV_ARGS(&pCL))))
    {
        pAlloc->Release();
        return E_FAIL;
    }

    // Clear previous
    for (auto& b : m_sceneBLAS) RT_ReleaseASBuffers(b, "SceneBLAS");
    m_sceneBLAS.clear();
    for (auto* r : m_sceneBLASResults) if (r) RT_SafeRelease(r, "SceneBLASResult");
    m_sceneBLASResults.clear();
    ClearKeepAliveUploads();
    RT_ReleaseASBuffers(m_tlasBuffers, "TLAS_PreBuild");
    m_pTopLevelAS = nullptr;

    // Build BLAS
    for (size_t i = 0; i < levelGeometry.size(); ++i)
    {
        const auto& geom = levelGeometry[i];
        CryLogAlways("[D3D_RT] BLAS %zu '%s' (%zuV / %zuI)", i, geom.debugName.c_str(), geom.vertices.size(), geom.indices.size());

        struct V { float p[3]; };
        std::vector<V> verts; verts.reserve(geom.vertices.size());
        for (auto& v : geom.vertices) verts.push_back({ v.x, v.y, v.z });

        ID3D12Resource* vbDef = nullptr; ID3D12Resource* vbUp = nullptr;
        ID3D12Resource* ibDef = nullptr; ID3D12Resource* ibUp = nullptr;

        if (FAILED(CreateDefaultBufferFromData(verts.data(), verts.size() * sizeof(V),
            &vbDef, &vbUp, pCL, (geom.debugName + "_VB").c_str())))
            continue;

        if (FAILED(CreateDefaultBufferFromData(geom.indices.data(), geom.indices.size() * sizeof(uint32_t),
            &ibDef, &ibUp, pCL, (geom.debugName + "_IB").c_str())))
        {
            vbDef->Release(); vbUp->Release();
            continue;
        }

        m_keepAliveUploads.emplace_back(vbDef, ibDef, vbUp, ibUp, geom.debugName);

        AccelerationStructureBuffers blas;
        if (SUCCEEDED(BuildBottomLevelAS(pCL,
            vbDef->GetGPUVirtualAddress(), (UINT)geom.vertices.size(), sizeof(V), DXGI_FORMAT_R32G32B32_FLOAT,
            ibDef->GetGPUVirtualAddress(), (UINT)geom.indices.size(), DXGI_FORMAT_R32_UINT,
            blas)))
        {
            if (blas.resultVA == 0 && blas.pResult)
            {
                __try { blas.resultVA = blas.pResult->GetGPUVirtualAddress(); }
                __except (EXCEPTION_EXECUTE_HANDLER) { blas.resultVA = 0; }
            }
            m_sceneBLAS.push_back(std::move(blas));
            if (m_sceneBLAS.back().pResult)
            {
                m_sceneBLAS.back().pResult->AddRef();
                m_sceneBLASResults.push_back(m_sceneBLAS.back().pResult);
            }
        }
    }

    if (m_sceneBLASResults.empty())
    {
        CryLogAlways("[D3D_RT] CreateLevelGeometryBLASAndTLAS: No BLAS built");
        pCL->Release();
        pAlloc->Release();
        return E_FAIL;
    }

    // Build TLAS
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> vaList;
    for (auto& b : m_sceneBLAS)
        if (b.resultVA) vaList.push_back(b.resultVA);

    if (vaList.empty())
    {
        CryLogAlways("[D3D_RT] CreateLevelGeometryBLASAndTLAS: No valid BLAS VA for TLAS");
        pCL->Release(); pAlloc->Release();
        return E_FAIL;
    }

    if (FAILED(BuildTopLevelAS(pCL, vaList, m_tlasBuffers)) || !m_tlasBuffers.pResult)
    {
        CryLogAlways("[D3D_RT] CreateLevelGeometryBLASAndTLAS: TLAS build failed");
        pCL->Release(); pAlloc->Release();
        return E_FAIL;
    }
    m_pTopLevelAS = m_tlasBuffers.pResult;

    if (FAILED(pCL->Close()))
    {
        pCL->Release(); pAlloc->Release();
        return E_FAIL;
    }

    // Submit & wait (build must finish before first DispatchRays)
    {
        ID3D12CommandList* lists[] = { pCL };
        m_pCommandQueue->ExecuteCommandLists(1, lists);

        ID3D12Fence* fence = nullptr;
        if (SUCCEEDED(m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        {
            const UINT64 fv = ++m_lastSignaledFence;
            if (SUCCEEDED(m_pCommandQueue->Signal(fence, fv)))
            {
                HANDLE e = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (e)
                {
                    fence->SetEventOnCompletion(fv, e);
                    WaitForSingleObject(e, 30000);
                    CloseHandle(e);
                }
                m_lastASBuildFence = fv;
            }
            fence->Release();
        }
    }

    // Refresh TLAS descriptor if heap ready
    if (m_pDescriptorHeap && m_pTopLevelAS)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_pDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC tlasDesc{};
        tlasDesc.Format = DXGI_FORMAT_UNKNOWN;
        tlasDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        tlasDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tlasDesc.RaytracingAccelerationStructure.Location = m_pTopLevelAS->GetGPUVirtualAddress();
        m_pDevice->CreateShaderResourceView(nullptr, &tlasDesc, cpu);
    }

    pCL->Release();
    pAlloc->Release();

    CryLogAlways("[D3D_RT] CreateLevelGeometryBLASAndTLAS: SUCCESS (buildFence=%llu)",
        (unsigned long long)m_lastASBuildFence);
    return S_OK;
}

// NEW: Process readback data and log hit/miss statistics
void CD3D_RT::ProcessRayStatsReadback(uint32_t frameNumber, uint32_t currentRayCount, uint32_t totalRaysDispatched)
{
    if (!m_pRayStatsReadbackBuffer)
    {
        return;
    }

    // Map readback buffer
    void* pData = nullptr;
    D3D12_RANGE readRange = { 0, 64 }; // Read first 64 bytes
    HRESULT hr = m_pRayStatsReadbackBuffer->Map(0, &readRange, &pData);

    if (SUCCEEDED(hr) && pData)
    {
        uint32_t* pStats = static_cast<uint32_t*>(pData);
        uint32_t hitCount = pStats[0];    // Hits written to offset 0 by shaders
        uint32_t missCount = pStats[1];   // Misses written to offset 4 by shaders
        uint32_t totalCounted = hitCount + missCount;

        // Calculate statistics
        float avgRaysPerFrame = static_cast<float>(totalRaysDispatched) / static_cast<float>(frameNumber);
        float hitRatio = totalCounted > 0 ? (static_cast<float>(hitCount) / static_cast<float>(totalCounted)) * 100.0f : 0.0f;
        float missRatio = totalCounted > 0 ? (static_cast<float>(missCount) / static_cast<float>(totalCounted)) * 100.0f : 0.0f;

        CryLogAlways("[D3D_RT] RAY STATS: Frame %u - %u rays dispatched (%ux%u)",
            frameNumber, currentRayCount, gcpRendD3D->GetWidth(), gcpRendD3D->GetHeight());
        CryLogAlways("[D3D_RT] RAY STATS: Total %u rays over %u frames (avg %.0f rays/frame)",
            totalRaysDispatched, frameNumber, avgRaysPerFrame);
        CryLogAlways("[D3D_RT] HIT/MISS STATS: %u hits (%.1f%%), %u misses (%.1f%%), %u total counted",
            hitCount, hitRatio, missCount, missRatio, totalCounted);

        // Expected statistics for test triangle
        if (frameNumber <= 1800) // First 30 seconds
        {
            CryLogAlways("[D3D_RT] EXPECTED: Test triangle covers small screen area - should see mostly misses (>95%%)");
            CryLogAlways("[D3D_RT] TRIANGLE INFO: Centered triangle with vertices at (0,0.5,0), (0.5,-0.5,0), (-0.5,-0.5,0)");

            if (missRatio > 95.0f)
            {
                CryLogAlways("[D3D_RT] ANALYSIS: ✓ High miss ratio confirms most rays miss the small triangle (expected)");
            }
            else if (hitRatio > 10.0f)
            {
                CryLogAlways("[D3D_RT] ANALYSIS: ⚠ Unexpectedly high hit ratio - triangle may be larger than expected");
            }
        }

        D3D12_RANGE writtenRange = { 0, 0 };
        m_pRayStatsReadbackBuffer->Unmap(0, &writtenRange);
    }
    else
    {
        CryLogAlways("[D3D_RT] ProcessRayStatsReadback: Failed to map readback buffer (hr=0x%08x)", hr);
    }
}


// COMPLETE CONSOLIDATED GEOMETRY EXTRACTION FUNCTION - ALL LOGIC IN ONE PLACE
bool CD3D_RT::ExtractLevelGeometry(std::vector<SafeGeometryData>& outGeometry)
{
    CryLogAlways("[D3D_RT] ExtractLevelGeometry: COMPREHENSIVE GEOMETRY EXTRACTION - All logic consolidated in one function...");

    // NEW: Do not touch meshes while the engine streams assets — prevents device stalls/removal
    if (RT_IsLevelStreamingBusy())
    {
        CryLogAlways("[D3D_RT] ExtractLevelGeometry: Streaming in progress - deferring geometry extraction");
        return false;
    }

    // CRITICAL: Get 3D Engine interface safely
    I3DEngine* p3DEngine = gEnv ? gEnv->p3DEngine : nullptr;
    if (!p3DEngine)
    {
        CryLogAlways("[D3D_RT] ExtractLevelGeometry: 3D Engine not available, creating procedural geometry...");
        goto CREATE_PROCEDURAL_GEOMETRY;
    }

    // CRITICAL: Get current camera/view information safely - moved outside try block
    {
        CCamera camera;
        Vec3 cameraPos;
        AABB queryBox;
        std::vector<IRenderNode*> renderNodes;

        __try
        {
            camera = gEnv->pSystem->GetViewCamera();
            cameraPos = camera.GetPosition();
            queryBox.min = cameraPos - Vec3(100.0f, 100.0f, 50.0f); // 200m x 200m x 100m around camera
            queryBox.max = cameraPos + Vec3(100.0f, 100.0f, 50.0f);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            CryLogAlways("[D3D_RT] ExtractLevelGeometry: Exception getting camera, creating procedural geometry...");
            goto CREATE_PROCEDURAL_GEOMETRY;
        }

        CryLogAlways("[D3D_RT] ExtractLevelGeometry: Querying geometry around camera position (%.1f, %.1f, %.1f)",
            cameraPos.x, cameraPos.y, cameraPos.z);

        // SAFE: Query render nodes in the area without accessing buffers
        __try
        {
            // FIXED: Actually call GetObjectsByType to populate the array
            PodArray<IRenderNode*> nodeArray;

            // First call to get count
            uint32_t nodeCount = p3DEngine->GetObjectsByType(eERType_Brush);
            CryLogAlways("[D3D_RT] ExtractLevelGeometry: Found %u total brush objects in level", nodeCount);

            if (nodeCount > 0)
            {
                // Allocate space for nodes
                std::vector<IRenderNode*> allNodes(nodeCount);

                // Second call to get actual nodes
                uint32_t actualCount = p3DEngine->GetObjectsByType(eERType_Brush, allNodes.data());
                CryLogAlways("[D3D_RT] ExtractLevelGeometry: Retrieved %u brush objects", actualCount);

                // Filter nodes within query box
                renderNodes.reserve(actualCount);
                for (uint32_t i = 0; i < actualCount; ++i)
                {
                    if (allNodes[i])
                    {
                        // FIX: use the returned bbox value (avoid uninitialized AABB)
                        const AABB nodeBBox = allNodes[i]->GetBBox();
                        if (Overlap::AABB_AABB(queryBox, nodeBBox))
                        {
                            renderNodes.push_back(allNodes[i]);
                        }
                    }
                }
            }

            CryLogAlways("[D3D_RT] ExtractLevelGeometry: Found %zu render nodes in query area", renderNodes.size());
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            CryLogAlways("[D3D_RT] ExtractLevelGeometry: Exception during render node query, creating procedural geometry...");
            goto CREATE_PROCEDURAL_GEOMETRY;
        }

        if (renderNodes.empty())
        {
            CryLogAlways("[D3D_RT] ExtractLevelGeometry: No render nodes found in query area, creating procedural geometry...");
            goto CREATE_PROCEDURAL_GEOMETRY;
        }

        // EXTRACT GEOMETRY FROM RENDER NODES SAFELY - moved variables outside scope
        {
            uint32_t successCount = 0;
            uint32_t skipCount = 0;
            uint32_t processedCount = 0;

            for (IRenderNode* pNode : renderNodes)
            {
                if (!pNode)
                {
                    skipCount++;
                    continue;
                }

                processedCount++;

                // INLINE GEOMETRY EXTRACTION FROM RENDER NODE - declare variables at function scope
                IStatObj* pStatObj = nullptr;
                Matrix34 worldMatrix;
                IRenderMesh* pRenderMesh = nullptr;
                SafeGeometryData nodeGeometry;

                __try
                {
                    // Try to get stat object safely
                    __try
                    {
                        pStatObj = pNode->GetEntityStatObj();
                        // FIX: capture bbox from return value (avoid uninitialized usage)
                        const AABB bbox = pNode->GetBBox();
                        worldMatrix.SetIdentity(); // Start with identity
                        worldMatrix.SetTranslation(bbox.GetCenter());
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        // If getting stat object fails, skip this node
                        skipCount++;
                        continue;
                    }

                    if (!pStatObj)
                    {
                        skipCount++;
                        continue; // No geometry
                    }

                    // Get render mesh safely
                    __try
                    {
                        pRenderMesh = pStatObj->GetRenderMesh();
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        skipCount++;
                        continue;
                    }

                    if (!pRenderMesh)
                    {
                        skipCount++;
                        continue; // No render mesh
                    }

                    // NEW: Guard mesh access with ThreadAccessLock to avoid races
                    IRenderMesh::ThreadAccessLock meshLock(pRenderMesh);

                    // INLINE MESH DATA EXTRACTION WITHOUT BUFFER MANAGER
                    __try
                    {
                        // CRITICAL: Use IRenderMesh's direct vertex access methods instead of buffer manager
                        int vertexCount = 0;
                        int indexCount = 0;

                        __try
                        {
                            vertexCount = pRenderMesh->GetVerticesCount();
                            indexCount = pRenderMesh->GetIndicesCount();
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER)
                        {
                            skipCount++;
                            continue;
                        }

                        if (vertexCount <= 0 || indexCount <= 0 || vertexCount > 100000 || indexCount > 300000)
                        {
                            skipCount++;
                            continue; // Invalid or too large
                        }

                        // FIXED: Use correct IRenderMesh APIs for vertex and index access
                        __try
                        {
                            // Get indices using correct API - FIXED parameter count
                            vtx_idx* pIndices = pRenderMesh->GetIndexPtr(FSL_READ);
                            if (!pIndices)
                            {
                                skipCount++;
                                continue;
                            }

                            // Get vertex positions using correct API - FIXED parameter count
                            int32_t vertexStride;
                            uint8_t* pVertexData = pRenderMesh->GetPosPtr(vertexStride, FSL_READ);
                            if (!pVertexData || vertexStride <= 0)
                            {
                                pRenderMesh->UnlockIndexStream();
                                skipCount++;
                                continue;
                            }

                            // Extract vertex positions
                            nodeGeometry.vertices.reserve(vertexCount);
                            for (int i = 0; i < vertexCount; ++i)
                            {
                                // Read position from vertex data
                                const Vec3* pPos = reinterpret_cast<const Vec3*>(pVertexData + i * vertexStride);

                                // Transform to world space
                                Vec3 worldPos = worldMatrix.TransformPoint(*pPos);
                                nodeGeometry.vertices.push_back(worldPos);
                            }

                            // Extract indices
                            nodeGeometry.indices.reserve(indexCount);
                            for (int i = 0; i < indexCount; ++i)
                            {
                                nodeGeometry.indices.push_back(static_cast<uint32_t>(pIndices[i]));
                            }

                            // Set debug name
                            const char* meshName = pRenderMesh->GetSourceName();
                            nodeGeometry.debugName = meshName ? meshName : "UnknownMesh";
                            nodeGeometry.worldTransform = worldMatrix;

                            // Unlock streams
                            pRenderMesh->UnlockStream(VSF_GENERAL);
                            pRenderMesh->UnlockIndexStream();

                            // INLINE VALIDATION
                            std::string validationError;
                            const size_t v = nodeGeometry.vertices.size();
                            const size_t i = nodeGeometry.indices.size();

                            if (v < 3) { validationError = "less than 3 vertices"; }
                            else if (i < 3 || (i % 3) != 0) { validationError = "index count not multiple of 3"; }
                            else if (i > 30'000'000) { validationError = "too many indices"; }
                            else {
                                uint32_t maxIdx = 0;
                                for (uint32_t idx : nodeGeometry.indices) maxIdx = (idx > maxIdx ? idx : maxIdx);
                                if (maxIdx >= v) {
                                    char buf[128];
                                    sprintf_s(buf, "index %u out of range (v=%zu)", maxIdx, v);
                                    validationError = buf;
                                }
                                else {
                                    // Check for NaN/Inf in vertices
                                    bool hasInvalidVertices = false;
                                    for (const auto& p : nodeGeometry.vertices) {
                                        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) {
                                            hasInvalidVertices = true;
                                            break;
                                        }
                                    }
                                    if (hasInvalidVertices) {
                                        validationError = "NaN/Inf in vertices";
                                    }
                                    else {
                                        // Check for degenerate triangles
                                        bool hasDegenerate = false;
                                        for (size_t t = 0; t + 2 < i && t < 300; t += 3) {
                                            const Vec3& a = nodeGeometry.vertices[nodeGeometry.indices[t + 0]];
                                            const Vec3& b = nodeGeometry.vertices[nodeGeometry.indices[t + 1]];
                                            const Vec3& c = nodeGeometry.vertices[nodeGeometry.indices[t + 2]];
                                            Vec3 ab = b - a, ac = c - a;
                                            Vec3 cr = ab.Cross(ac);
                                            if ((cr.x * cr.x + cr.y * cr.y + cr.z * cr.z) < 1e-20f) {
                                                hasDegenerate = true;
                                                break;
                                            }
                                        }
                                        if (hasDegenerate) {
                                            validationError = "degenerate triangles";
                                        }
                                    }
                                }
                            }

                            if (!validationError.empty())
                            {
                                CryLogAlways("[D3D_RT] ExtractLevelGeometry: Validation failed for mesh '%s': %s",
                                    nodeGeometry.debugName.c_str(), validationError.c_str());
                                skipCount++;
                                continue;
                            }

                            // SUCCESS - Add to output
                            outGeometry.push_back(nodeGeometry);
                            successCount++;

                            CryLogAlways("[D3D_RT] ExtractLevelGeometry: Successfully extracted geometry from render node %u: '%s' (%zu vertices, %zu indices)",
                                successCount, nodeGeometry.debugName.c_str(), nodeGeometry.vertices.size(), nodeGeometry.indices.size());
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER)
                        {
                            // Make sure to unlock on exception
                            __try
                            {
                                pRenderMesh->UnlockStream(VSF_GENERAL);
                                pRenderMesh->UnlockIndexStream();
                            }
                            __except (EXCEPTION_EXECUTE_HANDLER) {}
                            skipCount++;
                            continue;
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        CryLogAlways("[D3D_RT] ExtractLevelGeometry: Exception during mesh data extraction");
                        skipCount++;
                        continue;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    CryLogAlways("[D3D_RT] ExtractLevelGeometry: Exception processing render node");
                    skipCount++;
                    continue;
                }

                // Limit the number of objects to prevent excessive memory usage
                if (successCount >= 20) // Max 20 objects for ray tracing for better performance
                {
                    CryLogAlways("[D3D_RT] ExtractLevelGeometry: Reached maximum object limit (%u), stopping extraction", successCount);
                    break;
                }

                // Also limit total processed nodes to prevent long processing times
                if (processedCount >= 100)
                {
                    CryLogAlways("[D3D_RT] ExtractLevelGeometry: Reached maximum processing limit (%u nodes), stopping", processedCount);
                    break;
                }
            }

            CryLogAlways("[D3D_RT] ExtractLevelGeometry: Processing complete - extracted %u objects, skipped %u objects from %u processed nodes",
                successCount, skipCount, processedCount);

            // If we got some real geometry, enhance it with a few procedural objects for variety
            if (successCount > 0)
            {
                CryLogAlways("[D3D_RT] ExtractLevelGeometry: SUCCESS - Extracted real level geometry from %u render nodes!", successCount);

                // Add a few procedural objects to enhance the scene - INLINE PROCEDURAL CREATION
                CryLogAlways("[D3D_RT] ExtractLevelGeometry: Adding procedural enhancement objects...");

                // 1. Large ground plane
                {
                    SafeGeometryData groundPlane;
                    groundPlane.debugName = "Enhancement_GroundPlane";

                    const float size = 30.0f;
                    groundPlane.vertices = {
                        Vec3(-size, -size, 0.0f),  // Bottom-left
                        Vec3(size, -size, 0.0f),   // Bottom-right
                        Vec3(size,  size, 0.0f),   // Top-right
                        Vec3(-size,  size, 0.0f)   // Top-left
                    };

                    groundPlane.indices = {
                        0, 1, 2,  // First triangle
                        0, 2, 3   // Second triangle
                    };

                    groundPlane.worldTransform = Matrix34::CreateIdentity();
                    outGeometry.push_back(groundPlane);
                }

                // 2. A couple of enhancement boxes
                for (int i = 0; i < 2; ++i)
                {
                    SafeGeometryData enhancementBox;
                    enhancementBox.debugName = "Enhancement_Box" + std::to_string(i);

                    const float width = 2.0f + i;
                    const float height = 4.0f + i * 2.0f;
                    const float depth = 2.0f + i;

                    Vec3 offset = Vec3(i * 15.0f - 15.0f, 20.0f, height * 0.5f);

                    enhancementBox.vertices = {
                        // Bottom face
                        Vec3(-width, -depth, 0.0f) + offset,
                        Vec3(width, -depth, 0.0f) + offset,
                        Vec3(width,  depth, 0.0f) + offset,
                        Vec3(-width,  depth, 0.0f) + offset,
                        // Top face
                        Vec3(-width, -depth, height) + offset,
                        Vec3(width, -depth, height) + offset,
                        Vec3(width,  depth, height) + offset,
                        Vec3(-width,  depth, height) + offset
                    };

                    enhancementBox.indices = {
                        // Bottom face
                        0, 2, 1,  0, 3, 2,
                        // Top face
                        4, 5, 6,  4, 6, 7,
                        // Front face
                        0, 1, 5,  0, 5, 4,
                        // Back face
                        2, 7, 6,  2, 3, 7,
                        // Left face
                        0, 4, 7,  0, 7, 3,
                        // Right face
                        1, 6, 5,  1, 2, 6
                    };

                    enhancementBox.worldTransform = Matrix34::CreateIdentity();
                    outGeometry.push_back(enhancementBox);
                }

                CryLogAlways("[D3D_RT] ExtractLevelGeometry: Enhanced real geometry with 3 procedural objects. Total: %zu objects", outGeometry.size());
                return true;
            }
        }
    }

    // If no real geometry was extracted, fall through to full procedural generation
    CryLogAlways("[D3D_RT] ExtractLevelGeometry: No real geometry extracted, creating full procedural scene...");

CREATE_PROCEDURAL_GEOMETRY:
    // INLINE PROCEDURAL GEOMETRY CREATION
    CryLogAlways("[D3D_RT] ExtractLevelGeometry: Creating comprehensive procedural geometry scene...");

    // Create multiple test geometries to simulate level objects
    std::vector<SafeGeometryData> proceduralMeshes;

    // 1. Large ground plane
    {
        SafeGeometryData groundPlane;
        groundPlane.debugName = "ProceduralGroundPlane";

        // Create a large quad for the ground
        const float size = 50.0f;
        groundPlane.vertices = {
            Vec3(-size, -size, 0.0f),  // Bottom-left
            Vec3(size, -size, 0.0f),   // Bottom-right
            Vec3(size,  size, 0.0f),   // Top-right
            Vec3(-size,  size, 0.0f)   // Top-left
        };

        groundPlane.indices = {
            0, 1, 2,  // First triangle
            0, 2, 3   // Second triangle
        };

        groundPlane.worldTransform = Matrix34::CreateIdentity();
        proceduralMeshes.push_back(groundPlane);
    }

    // 2. Several building-like boxes at different positions
    for (int i = 0; i < 5; ++i)
    {
        SafeGeometryData building;
        building.debugName = "ProceduralBuilding" + std::to_string(i);

        // Create a box representing a building
        const float width = 3.0f + i * 1.5f;
        const float height = 6.0f + i * 2.0f;
        const float depth = 3.0f + i * 1.0f;

        // Position buildings at different locations
        Vec3 offset = Vec3(i * 12.0f - 24.0f, i * 8.0f - 16.0f, height * 0.5f);

        building.vertices = {
            // Bottom face
            Vec3(-width, -depth, 0.0f) + offset,
            Vec3(width, -depth, 0.0f) + offset,
            Vec3(width,  depth, 0.0f) + offset,
            Vec3(-width,  depth, 0.0f) + offset,
            // Top face
            Vec3(-width, -depth, height) + offset,
            Vec3(width, -depth, height) + offset,
            Vec3(width,  depth, height) + offset,
            Vec3(-width,  depth, height) + offset
        };

        building.indices = {
            // Bottom face
            0, 2, 1,  0, 3, 2,
            // Top face
            4, 5, 6,  4, 6, 7,
            // Front face
            0, 1, 5,  0, 5, 4,
            // Back face
            2, 7, 6,  2, 3, 7,
            // Left face
            0, 4, 7,  0, 7, 3,
            // Right face
            1, 6, 5,  1, 2, 6
        };

        building.worldTransform = Matrix34::CreateIdentity();
        proceduralMeshes.push_back(building);
    }

    // 3. Some tree-like structures
    for (int i = 0; i < 3; ++i)
    {
        SafeGeometryData tree;
        tree.debugName = "ProceduralTree" + std::to_string(i);

        // Create a simple tree (cylinder trunk + cone top)
        const float trunkRadius = 0.8f;
        const float trunkHeight = 8.0f;
        const float treeSpacing = 25.0f;

        Vec3 treePos = Vec3(i * treeSpacing - 25.0f, 30.0f, 0.0f);

        // Simple tree representation as a tall box
        tree.vertices = {
            Vec3(-trunkRadius, -trunkRadius, 0.0f) + treePos,
            Vec3(trunkRadius, -trunkRadius, 0.0f) + treePos,
            Vec3(trunkRadius,  trunkRadius, 0.0f) + treePos,
            Vec3(-trunkRadius,  trunkRadius, 0.0f) + treePos,
            Vec3(-trunkRadius, -trunkRadius, trunkHeight) + treePos,
            Vec3(trunkRadius, -trunkRadius, trunkHeight) + treePos,
            Vec3(trunkRadius,  trunkRadius, trunkHeight) + treePos,
            Vec3(-trunkRadius,  trunkRadius, trunkHeight) + treePos
        };

        tree.indices = {
            0, 2, 1,  0, 3, 2,  // Bottom
            4, 5, 6,  4, 6, 7,  // Top
            0, 1, 5,  0, 5, 4,  // Front
            2, 7, 6,  2, 3, 7,  // Back
            0, 4, 7,  0, 7, 3,  // Left
            1, 6, 5,  1, 2, 6   // Right
        };

        tree.worldTransform = Matrix34::CreateIdentity();
        proceduralMeshes.push_back(tree);
    }

    // Validate and add procedural meshes - INLINE VALIDATION
    for (const auto& mesh : proceduralMeshes)
    {
        std::string validationError;
        const size_t v = mesh.vertices.size();
        const size_t i = mesh.indices.size();

        if (v < 3) { validationError = "less than 3 vertices"; }
        else if (i < 3 || (i % 3) != 0) { validationError = "index count not multiple of 3"; }
        else if (i > 30'000'000) { validationError = "too many indices"; }
        else {
            uint32_t maxIdx = 0;
            for (uint32_t idx : mesh.indices) maxIdx = (idx > maxIdx ? idx : maxIdx);
            if (maxIdx >= v) {
                char buf[128];
                sprintf_s(buf, "index %u out of range (v=%zu)", maxIdx, v);
                validationError = buf;
            }
            else {
                // Check for NaN/Inf and degenerate triangles
                bool valid = true;
                for (const auto& p : mesh.vertices) {
                    if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) {
                        validationError = "NaN/Inf in vertices";
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    for (size_t t = 0; t + 2 < i && t < 300; t += 3) {
                        const Vec3& a = mesh.vertices[mesh.indices[t + 0]];
                        const Vec3& b = mesh.vertices[mesh.indices[t + 1]];
                        const Vec3& c = mesh.vertices[mesh.indices[t + 2]];
                        Vec3 ab = b - a, ac = c - a;
                        Vec3 cr = ab.Cross(ac);
                        if ((cr.x * cr.x + cr.y * cr.y + cr.z * cr.z) < 1e-20f) {
                            validationError = "degenerate triangles";
                            break;
                        }
                    }
                }
            }
        }

        if (validationError.empty())
        {
            outGeometry.push_back(mesh);
            CryLogAlways("[D3D_RT] ExtractLevelGeometry: Added procedural mesh '%s'", mesh.debugName.c_str());
        }
        else
        {
            CryLogAlways("[D3D_RT] ExtractLevelGeometry: Skipped invalid procedural mesh '%s': %s", mesh.debugName.c_str(), validationError.c_str());
        }
    }

    CryLogAlways("[D3D_RT] ExtractLevelGeometry: COMPLETE - Created %zu procedural geometry objects", outGeometry.size());
    return !outGeometry.empty();
}


// Helper: detect if the level/engine is streaming/loading content (safe, exception guarded)
static bool RT_IsLevelStreamingBusy()
{
    if (!gEnv) return false;

    bool streaming = false;

    __try
    {
        I3DEngine* p3DEngine = gEnv->p3DEngine;
        if (p3DEngine)
        {
            // Texture streaming still in progress?
            __try
            {
                streaming |= p3DEngine->IsTerrainTextureStreamingInProgress();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                streaming = true; // be conservative on exceptions
            }

            // Segmented world operation (load/save/move) in progress?
            __try
            {
                streaming |= p3DEngine->IsSegmentOperationInProgress();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                streaming = true; // be conservative on exceptions
            }

            // Optional: we could query object streaming stats, but avoid relying on struct fields to keep this header-agnostic.
            // I3DEngine::SObjectsStreamingStatus status = {};
            // p3DEngine->GetObjectsStreamingStatus(status);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        streaming = true; // be conservative on exceptions
    }

    return streaming;
}



// make composition wait for RT dispatch & skip on device removal
HRESULT CD3D_RT::ComposeToHDROneShot()
{
    auto fmtName = [](DXGI_FORMAT f) -> const char*
        {
            switch (f)
            {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:   return "R16G16B16A16_FLOAT(10)";
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:return "R16G16B16A16_TYPELESS(27)";
            case DXGI_FORMAT_R8G8B8A8_UNORM:       return "R8G8B8A8_UNORM(28)";
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return "R8G8B8A8_TYPELESS(27)";
            case DXGI_FORMAT_B8G8R8A8_UNORM:       return "B8G8R8A8_UNORM(87)";
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return "B8G8R8A8_TYPELESS(90)";
            case DXGI_FORMAT_R10G10B10A2_UNORM:    return "R10G10B10A2_UNORM(24)";
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS(25)";
            case DXGI_FORMAT_R11G11B10_FLOAT:      return "R11G11B10_FLOAT(26)";
            default: return "other";
            }
        };

    const UINT64 frameId = gcpRendD3D ? (UINT64)gcpRendD3D->GetRenderFrameID() : 0;
    CryLogAlways("[D3D_RT][ComposeFS] Begin frame=%llu Dev=%p Queue=%p GI=%p",
        (unsigned long long)frameId, m_pDevice, m_pCommandQueue, m_pRaytracingOutput);

    if (!m_pDevice || !m_pCommandQueue) return E_FAIL;
    if (!m_pRaytracingOutput) return S_FALSE;

    if (FAILED(m_pDevice->GetDeviceRemovedReason()))
        return E_FAIL;

    RT_WaitForLastDispatch("ComposeFS");

    // Destination RT: prefer HDR, fallback to Scene
    CTexture* pDstTex = m_graphicsPipelineResources.m_pTexHDRTarget;
    if (!pDstTex) pDstTex = m_graphicsPipelineResources.m_pTexSceneTarget;
    if (!pDstTex)
    {
        CryLogAlways("[D3D_RT][ComposeFS] Skip: No HDR/Scene destination");
        return S_FALSE;
    }

    const bool dstIsHDR = (pDstTex == m_graphicsPipelineResources.m_pTexHDRTarget);
    const char* dstName = dstIsHDR ? "HDRTarget" : "SceneTarget";

    ID3D12Resource* pDst = RT_GetNativeFromCTexture(pDstTex);
    ID3D12Resource* pGI = m_pRaytracingOutput;
    ID3D12Resource* pRefl = m_pReflectionOutput ? m_pReflectionOutput : m_pRaytracingOutput;
    ID3D12Resource* pAO = g_pRT_AOOutput;

    if (!pDst || !pGI)
    {
        CryLogAlways("[D3D_RT][ComposeFS] Abort: Null native resources dst=%p gi=%p", pDst, pGI);
        return E_FAIL;
    }

    const D3D12_RESOURCE_DESC dDesc = pDst->GetDesc();
    const D3D12_RESOURCE_DESC giDesc = pGI->GetDesc();

    CryLogAlways("[D3D_RT][ComposeFS] Dst=%s %ux%u fmt=%s(%u) samples=%u | Src(GI) %ux%u fmt=%s(%u) samples=%u",
        dstName, (unsigned)dDesc.Width, dDesc.Height, fmtName(dDesc.Format), (unsigned)dDesc.Format, dDesc.SampleDesc.Count,
        (unsigned)giDesc.Width, giDesc.Height, fmtName(giDesc.Format), (unsigned)giDesc.Format, giDesc.SampleDesc.Count);

    // Resize DXR outputs if needed
    if (!m_pTexDXR_GI ||
        m_pTexDXR_GI->GetWidth() != (int)dDesc.Width ||
        m_pTexDXR_GI->GetHeight() != (int)dDesc.Height)
    {
        CryLogAlways("[D3D_RT][ComposeFS] Resize DXR outputs: GI=%dx%d -> %ux%u (%s)",
            m_pTexDXR_GI ? m_pTexDXR_GI->GetWidth() : 0, m_pTexDXR_GI ? m_pTexDXR_GI->GetHeight() : 0,
            (unsigned)dDesc.Width, (unsigned)dDesc.Height, dstName);

        const HRESULT cr = CreateRayTracingResources();
        if (FAILED(cr))
        {
            CryLogAlways("[D3D_RT][ComposeFS] CreateRayTracingResources failed 0x%08x", cr);
            return S_FALSE;
        }
        pGI = m_pRaytracingOutput;
        pRefl = m_pReflectionOutput ? m_pReflectionOutput : m_pRaytracingOutput;
        pAO = g_pRT_AOOutput;
    }

    // Force overwrite blending and shader debug for visibility (temporary)
    g_RT_ComposeOverwriteNoBlend = true;    // overwrite, not additive, so we can see it for sure
    g_RT_ComposeDebugMacro = true;          // DXRCompose.hlsl will output GI/reflections directly

    // RootSig + PSO (DXRCompose.hlsl::PSMain)
    HRESULT hr = RT_EnsureComposeRootSig(m_pDevice);
    if (FAILED(hr)) { CryLogAlways("[D3D_RT][ComposeFS] RootSig failed 0x%08x", hr); return hr; }

    DXGI_FORMAT rtvFmt = RT_TypelessToTypedRTV(dDesc.Format);
    if (dDesc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS) rtvFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (rtvFmt == DXGI_FORMAT_UNKNOWN) { CryLogAlways("[D3D_RT][ComposeFS] ERROR: Unknown typed RTV format"); return E_INVALIDARG; }

    hr = RT_EnsureComposePSO(m_pDevice, rtvFmt, dDesc.SampleDesc.Count ? dDesc.SampleDesc.Count : 1);
    if (FAILED(hr)) { CryLogAlways("[D3D_RT][ComposeFS] PSO failed 0x%08x", hr); return hr; }

    if (FAILED(RT_EnsureComposeSrvHeap(m_pDevice))) return E_FAIL;
    if (FAILED(RT_EnsureComposeRtvHeap(m_pDevice))) return E_FAIL;

    // SRVs t0..t3
    const UINT srvInc = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSRV = g_pRT_ComposeSrvHeap->GetCPUDescriptorHandleForHeapStart();
    auto CreateSrv2D = [&](ID3D12Resource* r, DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE h)
        {
            if (!r) r = g_pRT_NullSrvTex2D;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Format = fmt;
            srv.Texture2D.MipLevels = 1;
            if (srv.Format == DXGI_FORMAT_UNKNOWN)
            {
                D3D12_RESOURCE_DESC d = r->GetDesc();
                srv.Format = RT_TypelessToTypedSRV(d.Format);
                if (srv.Format == DXGI_FORMAT_UNKNOWN) srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            m_pDevice->CreateShaderResourceView(r, &srv, h);
        };
    CreateSrv2D(pGI, DXGI_FORMAT_UNKNOWN, cpuSRV);                // t0 GI
    cpuSRV.ptr += srvInc; CreateSrv2D(pRefl, DXGI_FORMAT_UNKNOWN, cpuSRV); // t1 Refl
    cpuSRV.ptr += srvInc; CreateSrv2D(pAO, DXGI_FORMAT_R32_FLOAT, cpuSRV); // t2 AO
    cpuSRV.ptr += srvInc; CreateSrv2D(pAO ? pAO : pGI, pAO ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN, cpuSRV); // t3 Shadow/placeholder

    // RTV
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_pRT_ComposeRtvHeap->GetCPUDescriptorHandleForHeapStart();
    {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = rtvFmt;
        rtvDesc.ViewDimension = (dDesc.SampleDesc.Count > 1) ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        if (rtvDesc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D)
        {
            rtvDesc.Texture2D.MipSlice = 0;
            rtvDesc.Texture2D.PlaneSlice = 0;
        }
        m_pDevice->CreateRenderTargetView(pDst, &rtvDesc, rtv);
    }

    // Record CL
    ID3D12CommandAllocator* pAlloc = nullptr;
    hr = m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pAlloc));
    if (FAILED(hr)) return hr;

    ID3D12GraphicsCommandList* pCL = nullptr;
    hr = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pAlloc, nullptr, IID_PPV_ARGS(&pCL));
    if (FAILED(hr)) { SAFE_RELEASE(pAlloc); return hr; }
#if defined(_DEBUG) || defined(PROFILE)
    pCL->SetName(L"DXR_Compose_Fullscreen_HDR");
#endif

    // Ensure SRV states for sampling
    if (ID3D12GraphicsCommandList4* cl4; SUCCEEDED(pCL->QueryInterface(IID_PPV_ARGS(&cl4))) && cl4)
    {
        const D3D12_RESOURCE_STATES kSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        RT_TransitionTracked(cl4, pGI, g_RT_GIState, kSRV);
        if (pRefl) RT_TransitionTracked(cl4, pRefl, g_RT_ReflState, kSRV);
        if (pAO)   RT_TransitionTracked(cl4, pAO, g_RT_AOState, kSRV);
        cl4->Release();
    }

    // Bind SRV heap
    {
        ID3D12DescriptorHeap* heaps[] = { g_pRT_ComposeSrvHeap };
        pCL->SetDescriptorHeaps(1, heaps);
    }

    // Compose constants (with safe non-zero fallbacks)
    struct ComposeCB { float GIWeight, ReflWeight, AOWeight, _pad; float InvRT[2]; float _pad2[2]; };
    ID3D12Resource* pCB = nullptr;
    {
        D3D12_HEAP_PROPERTIES hup{}; hup.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbd{};
        cbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbd.Width = ALIGN(sizeof(ComposeCB), 256u);
        cbd.Height = 1; cbd.DepthOrArraySize = 1; cbd.MipLevels = 1;
        cbd.Format = DXGI_FORMAT_UNKNOWN;
        cbd.SampleDesc.Count = 1; cbd.SampleDesc.Quality = 0;
        cbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        cbd.Flags = D3D12_RESOURCE_FLAG_NONE;

        hr = m_pDevice->CreateCommittedResource(&hup, D3D12_HEAP_FLAG_NONE, &cbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&pCB));
        if (FAILED(hr)) { SAFE_RELEASE(pCL); SAFE_RELEASE(pAlloc); return hr; }

        float giW = CRenderer::CV_r_rayTracingGIIntensity;
        float reflW = CRenderer::CV_r_rayTracingReflectionIntensity;
        float aoW = CRenderer::CV_r_rayTracingAOIntensity;

        // Fallbacks for visibility
        if (giW == 0.0f) giW = 1.0f;
        if (reflW == 0.0f) reflW = 0.0f; // keep reflections off initially
        if (aoW == 0.0f) aoW = 0.0f; // AO modulates, leave off

        ComposeCB data{};
        data.GIWeight = giW;
        data.ReflWeight = reflW;
        data.AOWeight = aoW;
        data.InvRT[0] = dDesc.Width ? 1.0f / float(dDesc.Width) : 0.0f;
        data.InvRT[1] = dDesc.Height ? 1.0f / float(dDesc.Height) : 0.0f;

        void* ptr = nullptr;
        if (SUCCEEDED(pCB->Map(0, nullptr, &ptr)) && ptr)
        {
            memcpy(ptr, &data, sizeof(data));
            pCB->Unmap(0, nullptr);
        }
    }

    // VP/Scissor
    D3D12_VIEWPORT vp{ 0, 0, (float)dDesc.Width, (float)dDesc.Height, 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, (LONG)dDesc.Width, (LONG)dDesc.Height };
    pCL->RSSetViewports(1, &vp);
    pCL->RSSetScissorRects(1, &sc);

    // Optional: clear HDR target to a bright color to verify visibility
    {
        const float dbg[4] = { 0.0f, 0.0f, 0.0f, 0.f }; // change to {0,1,0,0} to see a green flash
        pCL->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        pCL->ClearRenderTargetView(rtv, dbg, 0, nullptr);
    }

    // Bind RS/PSO (DXRCompose) and draw fullscreen
    pCL->SetGraphicsRootSignature(g_pRT_ComposeRS);
    pCL->SetPipelineState(g_pRT_ComposePSO);
    pCL->SetGraphicsRootConstantBufferView(0, pCB->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE gpuSRV = g_pRT_ComposeSrvHeap->GetGPUDescriptorHandleForHeapStart();
    pCL->SetGraphicsRootDescriptorTable(1, gpuSRV);

    pCL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pCL->DrawInstanced(3, 1, 0, 0);

    hr = pCL->Close();
    if (FAILED(hr))
    {
        SAFE_RELEASE(pCL); SAFE_RELEASE(pAlloc); SAFE_RELEASE(pCB);
        return hr;
    }

    ID3D12CommandList* lists[] = { pCL };
    m_pCommandQueue->ExecuteCommandLists(1, lists);

    // Short debug wait
    if (ID3D12Fence* pFence; SUCCEEDED(m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFence))) && pFence)
    {
        const UINT64 fv = 1;
        m_pCommandQueue->Signal(pFence, fv);
        if (HANDLE e = CreateEvent(nullptr, FALSE, FALSE, nullptr))
        {
            pFence->SetEventOnCompletion(fv, e);
            WaitForSingleObject(e, 1000);
            CloseHandle(e);
        }
        pFence->Release();
    }

    SAFE_RELEASE(pCL);
    SAFE_RELEASE(pAlloc);
    SAFE_RELEASE(pCB);

    CryLogAlways("[D3D_RT][ComposeFS] Draw OK -> %s (%ux%u, dstFmt=%s)",
        dstName, (unsigned)dDesc.Width, (unsigned)dDesc.Height, fmtName(dDesc.Format));
    return S_OK;
}


void CD3D_RT::ClearKeepAliveUploads()
{
    // Defer release all geometry buffers (DEFAULT + UPLOAD) via RT_SafeRelease
    for (auto& k : m_keepAliveUploads)
    {
        if (k.pVertexBuffer)   RT_SafeRelease(k.pVertexBuffer, (k.debugName + "_VB").c_str());
        if (k.pIndexBuffer)    RT_SafeRelease(k.pIndexBuffer, (k.debugName + "_IB").c_str());
        if (k.pVertexUpload)   RT_SafeRelease(k.pVertexUpload, (k.debugName + "_VBUpload").c_str());
        if (k.pIndexUpload)    RT_SafeRelease(k.pIndexUpload, (k.debugName + "_IBUpload").c_str());
    }
    m_keepAliveUploads.clear();
}

// 3) Do the same for the test scene: set m_pTopLevelAS and refresh SRV only after the fence.
HRESULT CD3D_RT::CreateTestSceneWithEnhancedSafety()
{
    CryLogAlways("[D3D_RT] CreateTestSceneWithEnhancedSafety: Begin");

    if (!m_pDevice) return E_FAIL;
    HRESULT dr = m_pDevice->GetDeviceRemovedReason();
    if (FAILED(dr)) return dr;

    // Ensure queue
    if (!m_pCommandQueue && gcpRendD3D)
    {
        if (void* pDevWrap = gcpRendD3D->GetDevice())
        {
            CCryDX12Device* pDX12 = reinterpret_cast<CCryDX12Device*>(pDevWrap);
            if (pDX12)
            {
                NCryDX12::CDevice* native = pDX12->GetDX12Device();
                if (native)
                    m_pCommandQueue = native->GetScheduler().GetCommandListPool(CMDQUEUE_GRAPHICS).GetD3D12CommandQueue();
            }
        }
    }
    if (!m_pCommandQueue) return E_FAIL;

    ID3D12CommandAllocator* pAlloc = nullptr;
    if (FAILED(m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pAlloc))))
        return E_FAIL;
#if defined(_DEBUG) || defined(PROFILE)
    pAlloc->SetName(L"DXR_ASBuildAlloc_TestScene");
#endif

    ID3D12GraphicsCommandList4* pCL = nullptr;
    if (FAILED(m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pAlloc, nullptr, IID_PPV_ARGS(&pCL))))
    {
        pAlloc->Release();
        return E_FAIL;
    }

    // Geometry (simple triangle)
    struct V { float p[3]; };
    V verts[3] = { { {0.f, 0.5f, 0.f} }, { {0.5f, -0.5f, 0.f} }, { {-0.5f, -0.5f, 0.f} } };
    uint32_t idx[3] = { 0,1,2 };

    SafeGeometryData tri;
    tri.debugName = "TestTriangle";
    tri.vertices = { Vec3(0.f,0.5f,0.f), Vec3(0.5f,-0.5f,0.f), Vec3(-0.5f,-0.5f,0.f) };
    tri.indices = { 0,1,2 };

    std::string why;
    if (!ValidateMeshData(tri, why))
    {
        CryLogAlways("[D3D_RT] CreateTestSceneWithEnhancedSafety: Validation failed %s", why.c_str());
        pCL->Release(); pAlloc->Release();
        return E_FAIL;
    }

    ID3D12Resource* vbDef = nullptr; ID3D12Resource* vbUp = nullptr;
    if (FAILED(CreateDefaultBufferFromData(verts, sizeof(verts), &vbDef, &vbUp, pCL, "TestTriangle_VB")))
    {
        pCL->Release(); pAlloc->Release();
        return E_FAIL;
    }

    ID3D12Resource* ibDef = nullptr; ID3D12Resource* ibUp = nullptr;
    if (FAILED(CreateDefaultBufferFromData(idx, sizeof(idx), &ibDef, &ibUp, pCL, "TestTriangle_IB")))
    {
        vbDef->Release(); vbUp->Release();
        pCL->Release(); pAlloc->Release();
        return E_FAIL;
    }

    // Clear old AS
    RT_ReleaseASBuffers(m_tlasBuffers, "TLAS");
    RT_ReleaseASBuffers(m_blasBuffers, "LegacyBLAS");
    m_pTopLevelAS = nullptr;

    if (FAILED(BuildBottomLevelAS(pCL,
        vbDef->GetGPUVirtualAddress(), 3, sizeof(V), DXGI_FORMAT_R32G32B32_FLOAT,
        ibDef->GetGPUVirtualAddress(), 3, DXGI_FORMAT_R32_UINT,
        m_blasBuffers)))
    {
        vbDef->Release(); vbUp->Release();
        ibDef->Release(); ibUp->Release();
        pCL->Release(); pAlloc->Release();
        return E_FAIL;
    }
    if (m_blasBuffers.resultVA == 0 && m_blasBuffers.pResult)
    {
        __try { m_blasBuffers.resultVA = m_blasBuffers.pResult->GetGPUVirtualAddress(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { m_blasBuffers.resultVA = 0; }
    }
    if (!m_blasBuffers.resultVA)
    {
        RT_ReleaseASBuffers(m_blasBuffers, "TestScene_BLAS_InvalidVA");
        vbDef->Release(); vbUp->Release();
        ibDef->Release(); ibUp->Release();
        pCL->Release(); pAlloc->Release();
        return E_FAIL;
    }

    // TLAS build
    {
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> va{ m_blasBuffers.resultVA };
        if (FAILED(BuildTopLevelAS(pCL, va, m_tlasBuffers)) || !m_tlasBuffers.pResult)
        {
            vbDef->Release(); vbUp->Release();
            ibDef->Release(); ibUp->Release();
            pCL->Release(); pAlloc->Release();
            return E_FAIL;
        }
        m_pTopLevelAS = m_tlasBuffers.pResult;
    }

    if (FAILED(pCL->Close()))
    {
        vbDef->Release(); vbUp->Release();
        ibDef->Release(); ibUp->Release();
        pCL->Release(); pAlloc->Release();
        return E_FAIL;
    }

    // Execute AS build & wait
    {
        ID3D12CommandList* lists[] = { pCL };
        m_pCommandQueue->ExecuteCommandLists(1, lists);

        ID3D12Fence* fence = nullptr;
        if (SUCCEEDED(m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        {
            const UINT64 fv = ++m_lastSignaledFence;
            if (SUCCEEDED(m_pCommandQueue->Signal(fence, fv)))
            {
                HANDLE e = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (e)
                {
                    fence->SetEventOnCompletion(fv, e);
                    WaitForSingleObject(e, 30000);
                    CloseHandle(e);
                }
                m_lastASBuildFence = fv;
            }
            fence->Release();
        }
        else
        {
            // Fallback: block whole queue
            RT_WaitForGpuIdle("TestScene_ASBuild_NoFence");
            m_lastASBuildFence = ++m_lastSignaledFence;
        }
    }

    // Refresh TLAS SRV AFTER execution
    if (m_pDescriptorHeap && m_pTopLevelAS)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_pDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrv{};
        tlasSrv.Format = DXGI_FORMAT_UNKNOWN;
        tlasSrv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        tlasSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tlasSrv.RaytracingAccelerationStructure.Location = m_pTopLevelAS->GetGPUVirtualAddress();
        m_pDevice->CreateShaderResourceView(nullptr, &tlasSrv, cpu);
    }

    pCL->Release();
    pAlloc->Release();

    CryLogAlways("[D3D_RT] CreateTestSceneWithEnhancedSafety: SUCCESS (buildFence=%llu)",
        (unsigned long long)m_lastASBuildFence);
    return S_OK;
}

// Upload all constants used by HLSL RayTracingConstants
void CD3D_RT::UpdateRayTracingConstants(UINT width, UINT height, uint32 frameNumber)
{
    if (!m_pConstantsBuffer)
        return;

    auto getCVarI = [](const char* name, int def) -> int {
        if (!gEnv || !gEnv->pConsole) return def;
        if (ICVar* cv = gEnv->pConsole->GetCVar(name)) return cv->GetIVal();
        return def;
        };
    auto getCVarF = [](const char* name, float def) -> float {
        if (!gEnv || !gEnv->pConsole) return def;
        if (ICVar* cv = gEnv->pConsole->GetCVar(name)) return cv->GetFVal();
        return def;
        };

    // 1) Camera, matrices, time
    Matrix44A view(IDENTITY);
    Matrix44A proj(IDENTITY);
    Vec3      cameraPos(ZERO);
    float     timeSec = 0.0f;

    if (gEnv)
    {
        if (gEnv->pSystem)
        {
            const CCamera cam = gEnv->pSystem->GetViewCamera();
            cam.CalculateRenderMatrices(); // ensure internal cached matrices are current
            view = Matrix44A(cam.GetRenderViewMatrix());
            proj = Matrix44A(cam.GetRenderProjectionMatrix());
            cameraPos = cam.GetPosition();
        }
        if (gEnv->pTimer)
            timeSec = gEnv->pTimer->GetCurrTime();
    }

    Matrix44A invView = view.GetInverted();
    Matrix44A invProj = proj.GetInverted();
    Matrix44A viewProj = view * proj;
    Matrix44A invViewProj = viewProj.GetInverted();

    // Previous view-projection (for reprojection / motion)
    static Matrix44A s_prevViewProj = Matrix44A(IDENTITY);
    static bool s_hasPrev = false;
    Matrix44A prevViewProj = s_hasPrev ? s_prevViewProj : viewProj;
    s_prevViewProj = viewProj;
    s_hasPrev = true;

    // 2) Sun / environment
    Vec3  sunDir = Vec3(0.0f, 1.0f, 1.0f).GetNormalized();
    Vec3  sunColor(1.f, 1.f, 1.f);
    float sunIntensityLux = 120000.f;
    if (gEnv && gEnv->p3DEngine)
    {
        sunDir = gEnv->p3DEngine->GetSunDirNormalized();
        sunColor = gEnv->p3DEngine->GetSunColor();
        const float luma = sunColor.Dot(Vec3(0.2126f, 0.7152f, 0.0722f));
        sunIntensityLux = std::max(0.0f, luma * 100000.0f);
    }

    // Feature toggles (CVars assumed initialized elsewhere)
    const uint32 enableGI = CRenderer::CV_r_rayTracingGI;
    const uint32 enableReflections = CRenderer::CV_r_rayTracingReflections;
    const uint32 enableShadows = CRenderer::CV_r_rayTracingShadows;
    const uint32 enableAO = CRenderer::CV_r_rayTracingAO;

    const float giIntensity = CRenderer::CV_r_rayTracingGIIntensity;
    const float aoIntensity = CRenderer::CV_r_rayTracingAOIntensity;
    const float aoRadius = CRenderer::CV_r_rayTracingAORadius;
    const float reflIntensity = CRenderer::CV_r_rayTracingReflectionIntensity;
    const float reflRoughCut = CRenderer::CV_r_rayTracingReflectionRoughness;
    const float shadowIntensity = CRenderer::CV_r_rayTracingShadowIntensity;
    const float shadowDistance = CRenderer::CV_r_rayTracingShadowDistance;

    // Bounces / samples (already capped externally if needed)
    const uint32 giBounces = CRenderer::CV_r_rayTracingGIBounces;
    const uint32 giSamples = 5;
    const uint32 aoSamples = 5;
    const uint32 reflSamples = 5;
    const uint32 shadowSamples = 1;

    // Emissive / env
    const float envIntensity = 0.80f;
    const float emissiveNits = 0.0f;
    const Vec3  emissiveColor = Vec3(1.f, 1.f, 1.f);
    const uint32 useEmissive = emissiveNits > 0.0f ? 1u : 0u;

    const uint32 statsEnabled = CRenderer::CV_r_rayTracingDebug;



    // Screen dimensions
    const float invW = width ? 1.0f / float(width) : 0.0f;
    const float invH = height ? 1.0f / float(height) : 0.0f;

    // Frame number
    uint32 frame = frameNumber;
    if (frame == 0 && gcpRendD3D)
        frame = (uint32)gcpRendD3D->GetRenderFrameID();

    // --- NEW: Transpose matrices for HLSL column-major usage ---
    // CryEngine math stores row-major; HLSL (default) expects column-major.
    Matrix44A tInvViewProj = RT_Transpose(invViewProj);
    Matrix44A tView = RT_Transpose(view);
    Matrix44A tProj = RT_Transpose(proj);
    Matrix44A tInvView = RT_Transpose(invView);
    Matrix44A tInvProj = RT_Transpose(invProj);
    Matrix44A tPrevViewProj = RT_Transpose(prevViewProj);

    RayTracingConstantsGPU c{};
    c.InvViewProj = tInvViewProj;
    c.View = tView;
    c.Proj = tProj;
    c.InvView = tInvView;
    c.InvProj = tInvProj;
    c.PrevViewProj = tPrevViewProj;

    c.CameraPosition = cameraPos;
    c.Time = timeSec;
    c.SunDirection = sunDir;
    c.SunIntensity = sunIntensityLux;
    c.SunColor = sunColor;
    c.FrameNumber = frame;

    //c.GIIntensity = giIntensity;
    c.GIIntensity = (giIntensity > 0.0f) ? giIntensity : 1.0f; // fallback so hits are visible
    c.ReflectionIntensity = reflIntensity;
    c.ShadowIntensity = shadowIntensity;
    c.AOIntensity = aoIntensity;

    c.GIBounces = giBounces;
    c.GISamples = giSamples;
    c.ReflectionSamples = reflSamples;
    c.ShadowSamples = shadowSamples;
    c.AORadius = aoRadius;
    c.AOSamples = aoSamples;
    c.ReflectionRoughnessCutoff = reflRoughCut;
    c.ShadowDistance = shadowDistance;

    c.ScreenWidth = width;
    c.ScreenHeight = height;
    c.InvScreenWidth = invW;
    c.InvScreenHeight = invH;

    c.EnableGI = enableGI;
    c.EnableReflections = enableReflections;
    c.EnableShadows = enableShadows;
    c.EnableAO = enableAO;

    c.EmissiveColor = emissiveColor;
    c.EmissiveLuminanceNits = emissiveNits;
    c.EnvIntensity = envIntensity;
    c.UseEmissive = useEmissive;

    c.PadEmissiveEnv[0] = 0.0f;
    c.PadEmissiveEnv[1] = 0.0f;
    c.StatsEnabled = 1;

    // Initialize newly added fields (safe defaults)
    c.ResetAccumulation = 0;
    c._padAccum[0] = c._padAccum[1] = c._padAccum[2] = 0;
    c.MaxRayDistance = 10000.0f;   // matches r_FarZ or desired limit
    c.BootstrapGISpp = 1;
    c.BootstrapReflSpp = 1;
    c.ExpBlendEarly = 0.0f;
    c.ExpBlendFrames = 0.0f;
    c.RoughReflEnvCutoff = 0.8f;
    c.padTemporalExtra[0] = 0.0f;
    c.padTemporalExtra[1] = 0.0f;

    // Upload
    void* pDst = nullptr;
    const D3D12_RANGE noRead{ 0, 0 };
    if (SUCCEEDED(m_pConstantsBuffer->Map(0, &noRead, &pDst)) && pDst)
    {
        memcpy(pDst, &c, sizeof(c));
        m_pConstantsBuffer->Unmap(0, nullptr);
    }


    static uint32 s_debugPrint = 0;
    if ((s_debugPrint++ & 0xFF) == 0)
    {
        CryLogAlways("[DXR][Consts] Frame=%u Cam=(%.2f,%.2f,%.2f) SunDir=(%.2f,%.2f,%.2f) GI=%u Refl=%u AO=%u Sh=%u",
            frame, cameraPos.x, cameraPos.y, cameraPos.z,
            sunDir.x, sunDir.y, sunDir.z,
            enableGI, enableReflections, enableAO, enableShadows);
    }

}



HRESULT CD3D_RT::CreateUploadBuffer(const void* srcData, UINT64 byteSize, ID3D12Resource** ppBuffer)
{
    if (!m_pDevice || !ppBuffer) return E_INVALIDARG;

    // CRITICAL FIX: Add device state validation
    HRESULT deviceRemovedHr = m_pDevice->GetDeviceRemovedReason();
    if (FAILED(deviceRemovedHr))
    {
        CryLogAlways("[D3D_RT] CreateUploadBuffer: Device removed/reset (hr=0x%08x)", deviceRemovedHr);
        return deviceRemovedHr;
    }

    // CRITICAL FIX: Add size validation
    if (byteSize == 0 || byteSize > (1ull << 32)) // Max 4GB
    {
        CryLogAlways("[D3D_RT] CreateUploadBuffer: Invalid size (%llu bytes)", byteSize);
        return E_INVALIDARG;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Alignment = 0;
    resDesc.Width = byteSize;
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc.Count = 1;
    resDesc.SampleDesc.Quality = 0;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    *ppBuffer = nullptr;

    HRESULT hr = m_pDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(ppBuffer));

    if (FAILED(hr))
    {
        CryLogAlways("[D3D_RT] CreateUploadBuffer: Failed to create resource (hr=0x%08x, size=%llu)", hr, byteSize);
        return hr;
    }

    // CRITICAL FIX: Validate resource creation success
    if (!*ppBuffer)
    {
        CryLogAlways("[D3D_RT] CreateUploadBuffer: CreateCommittedResource succeeded but returned null resource");
        return E_FAIL;
    }

    if (srcData && byteSize)
    {
        void* pData = nullptr;
        D3D12_RANGE readRange = { 0, 0 }; // We're not reading from the buffer

        hr = (*ppBuffer)->Map(0, &readRange, &pData);
        if (SUCCEEDED(hr))
        {
            // CRITICAL FIX: Validate mapped pointer before use
            if (!pData)
            {
                CryLogAlways("[D3D_RT] CreateUploadBuffer: Map succeeded but returned null pointer");
                (*ppBuffer)->Release();
                *ppBuffer = nullptr;
                return E_FAIL;
            }

            // CRITICAL FIX: Use safer memory copy with bounds checking
            __try
            {
                // CRITICAL FIX: Validate source pointer before copy
                if (IsBadReadPtr(srcData, static_cast<UINT_PTR>(byteSize)))
                {
                    CryLogAlways("[D3D_RT] CreateUploadBuffer: Invalid source data pointer");
                    (*ppBuffer)->Unmap(0, nullptr);
                    (*ppBuffer)->Release();
                    *ppBuffer = nullptr;
                    return E_INVALIDARG;
                }

                memcpy(pData, srcData, static_cast<size_t>(byteSize));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                CryLogAlways("[D3D_RT] CreateUploadBuffer: Exception during memcpy (src=%p, dst=%p, size=%llu)",
                    srcData, pData, byteSize);
                (*ppBuffer)->Unmap(0, nullptr);
                (*ppBuffer)->Release();
                *ppBuffer = nullptr;
                return E_FAIL;
            }

            D3D12_RANGE writtenRange = { 0, byteSize };
            (*ppBuffer)->Unmap(0, &writtenRange);
        }
        else
        {
            CryLogAlways("[D3D_RT] CreateUploadBuffer: Failed to map resource (hr=0x%08x)", hr);
            (*ppBuffer)->Release();
            *ppBuffer = nullptr;
            return hr;
        }
    }

    return S_OK;
}


HRESULT CD3D_RT::BuildBottomLevelAS(
    ID3D12GraphicsCommandList4* pCmdList,
    D3D12_GPU_VIRTUAL_ADDRESS vertexBufferAddress,
    UINT vertexCount,
    UINT vertexStride,
    DXGI_FORMAT vertexFormat,
    D3D12_GPU_VIRTUAL_ADDRESS indexBufferAddress,
    UINT indexCount,
    DXGI_FORMAT indexFormat,
    AccelerationStructureBuffers& outBLAS)
{
    if (!m_pDevice || !pCmdList)
    {
        CryLogAlways("[D3D_RT] BuildBottomLevelAS: Invalid device or command list");
        return E_INVALIDARG;
    }

    if (vertexBufferAddress == 0 || vertexCount == 0 || vertexCount > 1000000)
        return E_INVALIDARG;
    if (indexBufferAddress == 0 || indexCount == 0 || indexCount > 3000000 || (indexCount % 3) != 0)
        return E_INVALIDARG;
    if (vertexFormat != DXGI_FORMAT_R32G32B32_FLOAT)
        return E_INVALIDARG;
    if (indexFormat != DXGI_FORMAT_R32_UINT && indexFormat != DXGI_FORMAT_R16_UINT)
        return E_INVALIDARG;

    HRESULT deviceRemovedHr = m_pDevice->GetDeviceRemovedReason();
    if (FAILED(deviceRemovedHr)) return deviceRemovedHr;

    RT_ReleaseASBuffers(outBLAS, "BuildBLAS_Previous");

    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {};
    geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometryDesc.Triangles.VertexBuffer.StartAddress = vertexBufferAddress;
    geometryDesc.Triangles.VertexBuffer.StrideInBytes = vertexStride;
    geometryDesc.Triangles.VertexCount = vertexCount;
    geometryDesc.Triangles.VertexFormat = vertexFormat;
    geometryDesc.Triangles.IndexBuffer = indexBufferAddress;
    geometryDesc.Triangles.IndexCount = indexCount;
    geometryDesc.Triangles.IndexFormat = indexFormat;
    geometryDesc.Triangles.Transform3x4 = 0;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottomLevelInputs = {};
    bottomLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottomLevelInputs.Flags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
    bottomLevelInputs.NumDescs = 1;
    bottomLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottomLevelInputs.pGeometryDescs = &geometryDesc;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    m_pDevice->GetRaytracingAccelerationStructurePrebuildInfo(&bottomLevelInputs, &prebuildInfo);
    if (prebuildInfo.ResultDataMaxSizeInBytes == 0 || prebuildInfo.ScratchDataSizeInBytes == 0)
        return E_FAIL;

    const UINT64 scratchSize = ALIGN(prebuildInfo.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    const UINT64 resultSize = ALIGN(prebuildInfo.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Alignment = 0;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Scratch in COMMON -> transition to UAV
    bufDesc.Width = scratchSize;
    HRESULT hr = m_pDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&outBLAS.pScratch));
    if (FAILED(hr)) return hr;

    // Result created directly in RAYTRACING_ACCELERATION_STRUCTURE (no barrier to RAS needed)
    bufDesc.Width = resultSize;
    hr = m_pDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&outBLAS.pResult));
    if (FAILED(hr)) { outBLAS.Release(); return hr; }

    // Only transition SCRATCH -> UAV
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = outBLAS.pScratch;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pCmdList->ResourceBarrier(1, &b);
    }

    // Build BLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = bottomLevelInputs;
    buildDesc.ScratchAccelerationStructureData = outBLAS.pScratch->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData = outBLAS.pResult->GetGPUVirtualAddress();

    pCmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // Ensure ordering
    {
        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = outBLAS.pResult;
        pCmdList->ResourceBarrier(1, &uavBarrier);
    }

    outBLAS.resultVA = outBLAS.pResult ? outBLAS.pResult->GetGPUVirtualAddress() : 0;


    CryLogAlways("[D3D_RT] BuildBottomLevelAS: Built BLAS (result=%llu bytes) with SCRATCH=UAV, RESULT initial=RAS",
        (unsigned long long)resultSize);
    return S_OK;
}


// Add this static helper above CreateLevelGeometryBLASAndTLAS (or near ExtractLevelGeometry)
static void RT_AppendTerrainPatch(std::vector<CD3D_RT::SafeGeometryData>& outGeometry)
{
    if (!gEnv || !gEnv->pSystem) return;

    // Grid settings (coarse for stability)
    const int  quads = 64;          // 64x64 quads
    const int  verts = quads + 1;
    const float step = 2.0f;        // meters per cell
    const float half = (quads * step) * 0.5f;

    CCamera cam = gEnv->pSystem->GetViewCamera();
    const Vec3 cpos = cam.GetPosition();

    CD3D_RT::SafeGeometryData mesh;
    mesh.debugName = "TerrainPatch";

    mesh.vertices.reserve(verts * verts);
    mesh.indices.reserve(quads * quads * 6);

    auto SampleHeight = [&](float wx, float wy) -> float
        {
            I3DEngine* p3D = gEnv ? gEnv->p3DEngine : nullptr;
            if (p3D)
            {
                // Interpolated height, valid for any x/y
                return p3D->GetTerrainElevation(wx, wy);
                // If you prefer clamped to world bounds, use:
                // return p3D->GetTerrainZ(wx, wy);
            }
            return 0.0f; // fallback flat
        };

    // Build vertex grid centered around camera
    for (int j = 0; j < verts; ++j)
    {
        for (int i = 0; i < verts; ++i)
        {
            const float x = cpos.x + (i * step - half);
            const float y = cpos.y + (j * step - half);
            const float z = SampleHeight(x, y);
            mesh.vertices.emplace_back(x, y, z);
        }
    }

    // Indices
    auto idx = [&](int i, int j) { return j * verts + i; };
    for (int j = 0; j < quads; ++j)
    {
        for (int i = 0; i < quads; ++i)
        {
            uint32_t v0 = idx(i, j);
            uint32_t v1 = idx(i + 1, j);
            uint32_t v2 = idx(i + 1, j + 1);
            uint32_t v3 = idx(i, j + 1);

            mesh.indices.push_back(v0); mesh.indices.push_back(v1); mesh.indices.push_back(v2);
            mesh.indices.push_back(v0); mesh.indices.push_back(v2); mesh.indices.push_back(v3);
        }
    }

    std::string why;
    if (!ValidateMeshData(mesh, why))
    {
        CryLogAlways("[D3D_RT] TerrainPatch: validation failed: %s", why.c_str());
        return;
    }

    outGeometry.push_back(std::move(mesh));
    CryLogAlways("[D3D_RT] TerrainPatch: appended %d x %d grid (step=%.2f)", quads, quads, step);
}


HRESULT CD3D_RT::BuildTopLevelAS(
    ID3D12GraphicsCommandList4* pCmdList,
    const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& blasGpuVAs,
    AccelerationStructureBuffers& outTLAS)
{
    if (!m_pDevice || !pCmdList) return E_INVALIDARG;
    if (blasGpuVAs.empty() || blasGpuVAs.size() > 100000) return E_INVALIDARG;
    for (auto va : blasGpuVAs) if (va == 0) return E_INVALIDARG;

    HRESULT hr = m_pDevice->GetDeviceRemovedReason();
    if (FAILED(hr)) return hr;

    RT_ReleaseASBuffers(outTLAS, "BuildTLAS_Previous");

    const UINT numInstances = static_cast<UINT>(blasGpuVAs.size());
    const UINT instanceBytes = numInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);

    // Upload instance descs
    ID3D12Resource* pInstUpload = nullptr;
    hr = CreateUploadBuffer(nullptr, instanceBytes, &pInstUpload);
    if (FAILED(hr) || !pInstUpload) return FAILED(hr) ? hr : E_FAIL;

    {
        D3D12_RAYTRACING_INSTANCE_DESC* pInst = nullptr;
        D3D12_RANGE rr{ 0, 0 };
        hr = pInstUpload->Map(0, &rr, reinterpret_cast<void**>(&pInst));
        if (FAILED(hr) || !pInst) { pInstUpload->Release(); return FAILED(hr) ? hr : E_FAIL; }

        static const float I[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
        for (UINT i = 0; i < numInstances; ++i)
        {
            auto& d = pInst[i];
            memcpy(d.Transform, I, sizeof(I));
            d.InstanceID = i;
            d.InstanceMask = 0xFF;
            d.InstanceContributionToHitGroupIndex = 0;
            d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            d.AccelerationStructure = blasGpuVAs[i];
        }
        D3D12_RANGE wr{ 0, instanceBytes };
        pInstUpload->Unmap(0, &wr);
    }

    // Default buffer for instance descs
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = instanceBytes;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;

        hr = m_pDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&outTLAS.pInstanceDesc));
        if (FAILED(hr)) { pInstUpload->Release(); return hr; }

        auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
            outTLAS.pInstanceDesc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        pCmdList->ResourceBarrier(1, &toCopy);

        pCmdList->CopyBufferRegion(outTLAS.pInstanceDesc, 0, pInstUpload, 0, instanceBytes);

        auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(
            outTLAS.pInstanceDesc, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
        pCmdList->ResourceBarrier(1, &toRead);
    }

    // Keep upload alive
    g_RT_TempUploads.push_back(pInstUpload);

    // Prebuild info
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = numInstances;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.InstanceDescs = outTLAS.pInstanceDesc->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};
    m_pDevice->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pre);
    if (pre.ResultDataMaxSizeInBytes == 0 || pre.ScratchDataSizeInBytes == 0) return E_FAIL;

    const UINT64 scratchSize = ALIGN(pre.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    const UINT64 resultSize = ALIGN(pre.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Scratch in COMMON -> transition to UAV
    rd.Width = scratchSize;
    hr = m_pDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&outTLAS.pScratch));
    if (FAILED(hr)) return hr;

    // Result created directly in RAYTRACING_ACCELERATION_STRUCTURE (no barrier to RAS needed)
    rd.Width = resultSize;
    hr = m_pDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&outTLAS.pResult));
    if (FAILED(hr)) return hr;

    // Only transition SCRATCH -> UAV
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = outTLAS.pScratch;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pCmdList->ResourceBarrier(1, &b);
    }

    // Build TLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.Inputs = in;
    desc.ScratchAccelerationStructureData = outTLAS.pScratch->GetGPUVirtualAddress();
    desc.DestAccelerationStructureData = outTLAS.pResult->GetGPUVirtualAddress();

    pCmdList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

    // Ensure ordering; RESULT stays in RAS
    {
        D3D12_RESOURCE_BARRIER uavBar{};
        uavBar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBar.UAV.pResource = outTLAS.pResult;
        pCmdList->ResourceBarrier(1, &uavBar);
    }

    outTLAS.resultVA = outTLAS.pResult ? outTLAS.pResult->GetGPUVirtualAddress() : 0;

    CryLogAlways("[D3D_RT] TLAS: Recorded build (%u instances, result=%llu bytes, scratch=%llu bytes) with SCRATCH=UAV, RESULT initial=RAS",
        numInstances, (unsigned long long)resultSize, (unsigned long long)scratchSize);

    return S_OK;
}



void CD3D_RT::Shutdown()
{
    CryLogAlways("[D3D_RT] Shutdown: BEGIN (flush + safe releases)");

    static std::atomic<bool> s_shutdownInProgress(false);
    static std::atomic<bool> s_shutdownCompleted(false);
    if (s_shutdownCompleted.load() || s_shutdownInProgress.exchange(true))
        return;

    // Flush entire engine scheduler (all queues) BEFORE signaling our idle fence
    if (gcpRendD3D)
    {
        if (void* dev = gcpRendD3D->GetDevice())
        {
            CCryDX12Device* cryDev = reinterpret_cast<CCryDX12Device*>(dev);
            if (cryDev)
            {
                CryLogAlways("[D3D_RT] Shutdown: Engine FlushAndWaitForGPU()");
                cryDev->FlushAndWaitForGPU();
            }
        }
    }

    // Final frame fence signal if frame contexts alive
    if (m_frameFence && m_pCommandQueue)
    {
        m_pCommandQueue->Signal(m_frameFence, ++m_lastSignaledFence);
    }

    // Queue idle (graphics queue fence + any residual)
    RT_WaitForGpuIdle("Shutdown_Begin");

    SAFE_RELEASE(g_pRT_ClearCpuHeap); // mirror heap for ClearUAV
    g_RT_ClearCpuInc = 0;

    // Drain deferred (now should be safe)
    if (m_frameFence)
    {
        UINT64 done = m_frameFence->GetCompletedValue();
        for (auto& d : m_deferred)
            if (done >= d.fenceValue)
                for (auto* r : d.resources) SAFE_RELEASE(r);
        m_deferred.clear();
    }

    __try
    {
        m_pTopLevelAS = nullptr;
        RT_ReleaseASBuffers(m_tlasBuffers, "Shutdown_TLAS");
        RT_ReleaseASBuffers(m_blasBuffers, "Shutdown_SingleBLAS");
        for (ID3D12Resource*& r : m_sceneBLASResults) RT_SafeRelease(r, "Shutdown_SceneBLASResult");
        for (auto& blas : m_sceneBLAS) RT_ReleaseASBuffers(blas, "Shutdown_SceneBLAS");
        m_sceneBLAS.clear();
        ClearKeepAliveUploads();

        SAFE_RELEASE(m_pStateObjectProperties);
        SAFE_RELEASE(m_pRaytracingPSO);
        SAFE_RELEASE(m_pGlobalRootSignature);
        SAFE_RELEASE(m_pRayGenShaderTable);
        SAFE_RELEASE(m_pMissShaderTable);
        SAFE_RELEASE(m_pHitGroupShaderTable);

        SAFE_RELEASE(g_pRT_ComposePSO);
        SAFE_RELEASE(g_pRT_ComposeRS);
        SAFE_RELEASE(g_pRT_ComposeSrvHeap);
        SAFE_RELEASE(g_pRT_ComposeRtvHeap);
        g_RT_ComposeRTVFormat = DXGI_FORMAT_UNKNOWN;

        // Output & misc
        m_pRaytracingOutput = nullptr;
        m_pReflectionOutput = nullptr;

        // AO: release depending on ownership
        if (g_RT_AOOutputOwned) { RT_SafeRelease(g_pRT_AOOutput, "AOOutput"); }
        g_pRT_AOOutput = nullptr;
        g_RT_AOOutputOwned = false;

        if (m_pTexDXR_GI) { m_pTexDXR_GI->Release();   m_pTexDXR_GI = nullptr; }
        if (m_pTexDXR_Refl) { m_pTexDXR_Refl->Release(); m_pTexDXR_Refl = nullptr; }
        if (m_pTexDXR_AO) { m_pTexDXR_AO->Release();   m_pTexDXR_AO = nullptr; }

        SAFE_RELEASE(m_pDescriptorHeap);
        SAFE_RELEASE(m_pConstantsBuffer);
        SAFE_RELEASE(m_pRayStatsBuffer);
        SAFE_RELEASE(m_pRayStatsReadbackBuffer);
        RT_SafeRelease(g_pRT_NullSrvTex2D, "NullSrvTex");

        SAFE_RELEASE(g_pRT_CpuUavHeap);
        SAFE_RELEASE(g_pRT_CpuUavHeapRefl);
        g_RT_StatsCpuHandle.ptr = 0;
        g_RT_ReflCpuHandle.ptr = 0;

        RT_SafeRelease(g_pRT_IrradianceCube, "IrradianceCube");
        SAFE_RELEASE(g_pRT_IrrPSO);
        SAFE_RELEASE(g_pRT_IrrRS);

        SAFE_RELEASE(g_pRT_DenoiseRS);

        m_outputWidth = m_outputHeight = 0;
        m_descriptorSize = 0;
        m_pRayGenShaderID = m_pMissShaderID = m_pClosestHitShaderID = nullptr;
        m_shadersCompiled = false;

        // Final engine flush for any lingering deferred releases we queued inside RT_SafeRelease
        if (gcpRendD3D)
        {
            if (void* dev = gcpRendD3D->GetDevice())
            {
                CCryDX12Device* cryDev = reinterpret_cast<CCryDX12Device*>(dev);
                if (cryDev)
                {
                    cryDev->FlushAndWaitForGPU();
                }
            }
        }

        if (m_pDevice)
        {
            ULONG rc = m_pDevice->Release();
            CryLogAlways("[D3D_RT] Shutdown: Device released (ref=%u)", rc);
            m_pDevice = nullptr;
        }
        m_pCommandQueue = nullptr;

        RT_ShutdownFrameContexts();

        s_shutdownCompleted = true;
        CryLogAlways("[D3D_RT] Shutdown: COMPLETE");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] Shutdown: EXCEPTION (code=0x%08x)", GetExceptionCode());
        s_shutdownCompleted = true;
    }

    s_shutdownInProgress = false;
}

// Helper: fetch GPU VA with sub-allocation offset - CRASH-SAFE VERSION
bool CD3D_RT::GetDeviceAddressFromBufferHandle(buffer_handle_t handle, DeviceAddressRange& outRange, UINT64 requiredSize, UINT64 requiredOffsetBytes)
{
    CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: SAFE implementation with proper validation...");

    // Clear output first
    outRange.pResource = nullptr;
    outRange.gpuVA = 0;
    outRange.sizeInBytes = 0;

    // CRITICAL: Validate handle first
    if (handle == 0 || handle == ~0u)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Invalid handle (0x%x)", handle);
        return false;
    }

    // CRITICAL: Device state validation before any buffer operations
    if (!m_pDevice)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Device not available");
        return false;
    }

    __try
    {
        HRESULT deviceRemovedHr = m_pDevice->GetDeviceRemovedReason();
        if (FAILED(deviceRemovedHr))
        {
            CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Device removed/reset (hr=0x%08x)", deviceRemovedHr);
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Exception checking device state");
        return false;
    }

    // SAFE: Access buffer manager with validation
    if (!gcpRendD3D)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Buffer manager not available");
        return false;
    }

    // CRITICAL FIX: Get the buffer with proper sub-allocation offset
    buffer_size_t subOffset = 0;
    D3DBuffer* pD3DBuffer = nullptr;

    __try
    {
        // SAFE: Access buffer manager with exception handling
        pD3DBuffer = gcpRendD3D->m_DevBufMan.GetD3D(handle, &subOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Exception accessing buffer manager");
        return false;
    }

    if (!pD3DBuffer)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Failed to get D3D buffer from handle 0x%x", handle);
        return false;
    }

    // CRITICAL FIX: Validate sub-allocation offset
    if (requiredOffsetBytes > 0 && subOffset != requiredOffsetBytes)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Sub-allocation offset mismatch (expected=%llu, actual=%llu)",
            requiredOffsetBytes, (UINT64)subOffset);
        // Don't fail here, but warn - subOffset might be correct
    }

    // CRITICAL FIX: Extract D3D12 resource with proper validation
    auto pDX12Buffer = reinterpret_cast<CCryDX12Buffer*>(pD3DBuffer);
    if (!pDX12Buffer)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Not a DX12 buffer");
        return false;
    }

    // CRITICAL FIX: Get the actual D3D12 resource with validation
    ICryDX12Resource* pDX12Resource = nullptr;
    __try
    {
        pDX12Resource = DX12_EXTRACT_ICRYDX12RESOURCE(pDX12Buffer);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Exception extracting DX12 resource");
        return false;
    }

    if (!pDX12Resource)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Failed to extract DX12 resource");
        return false;
    }

    // CRITICAL FIX: Get native D3D12 resource with validation
    ID3D12Resource* pNativeResource = nullptr;
    __try
    {
        pNativeResource = pDX12Resource->GetD3D12Resource();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Exception getting native resource");
        return false;
    }

    if (!pNativeResource)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Native D3D12 resource is null");
        return false;
    }

    // CRITICAL FIX: Validate resource description and heap type
    D3D12_RESOURCE_DESC resourceDesc;
    __try
    {
        resourceDesc = pNativeResource->GetDesc();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Exception getting resource description");
        return false;
    }

    // CRITICAL FIX: Validate this is a buffer resource
    if (resourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Resource is not a buffer (dimension=%d)", resourceDesc.Dimension);
        return false;
    }

    // CRITICAL FIX: Check heap properties for DXR compatibility
    D3D12_HEAP_PROPERTIES heapProps = {};
    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;

    __try
    {
        HRESULT hr = pNativeResource->GetHeapProperties(&heapProps, &heapFlags);
        if (FAILED(hr))
        {
            CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Failed to get heap properties (hr=0x%08x)", hr);
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Exception getting heap properties");
        return false;
    }

    // CRITICAL FIX: Validate heap type for DXR requirements
    if (heapProps.Type != D3D12_HEAP_TYPE_DEFAULT)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Buffer not in DEFAULT heap (type=%d) - unsuitable for DXR", heapProps.Type);
        // For DXR, we need DEFAULT heap buffers - fall back to copying CPU data
        return false;
    }

    // CRITICAL FIX: Get GPU virtual address with proper offset handling
    D3D12_GPU_VIRTUAL_ADDRESS baseGpuVA = 0;
    __try
    {
        baseGpuVA = pNativeResource->GetGPUVirtualAddress();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Exception getting GPU virtual address");
        return false;
    }

    if (baseGpuVA == 0)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Invalid GPU virtual address (0)");
        return false;
    }

    // CRITICAL FIX: Add sub-allocation offset to base address
    D3D12_GPU_VIRTUAL_ADDRESS finalGpuVA = baseGpuVA + (UINT64)subOffset;

    // CRITICAL FIX: Validate final address alignment for DXR
    if (finalGpuVA & 0xFF) // 256-byte alignment required for DXR
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: GPU address not 256-byte aligned (0x%llx)", finalGpuVA);
        return false;
    }

    // CRITICAL FIX: Validate size requirements
    UINT64 availableSize = resourceDesc.Width - (UINT64)subOffset;
    if (requiredSize > 0 && availableSize < requiredSize)
    {
        CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: Insufficient size (available=%llu, required=%llu)",
            availableSize, requiredSize);
        return false;
    }

    // SUCCESS: Fill output structure
    outRange.pResource = pNativeResource;
    outRange.gpuVA = finalGpuVA;
    outRange.sizeInBytes = availableSize;

    CryLogAlways("[D3D_RT] GetDeviceAddressFromBufferHandle: SUCCESS - handle=0x%x, gpuVA=0x%llx, size=%llu, subOffset=%llu",
        handle, finalGpuVA, availableSize, (UINT64)subOffset);

    return true;
}

bool CD3D_RT::GetMeshDeviceStreams(IRenderMesh* pRM,
    DeviceAddressRange& outVB, UINT& outVertexCount, UINT& outVertexStride, DXGI_FORMAT& outVertexFormat,
    DeviceAddressRange& outIB, UINT& outIndexCount, DXGI_FORMAT& outIndexFormat,
    UINT64& outIBOffsetBytes)
{
    CryLogAlways("[D3D_RT] GetMeshDeviceStreams: SAFE implementation with proper validation...");

    // Clear all outputs first
    outVB = {};
    outIB = {};
    outVertexCount = 0;
    outVertexStride = 0;
    outVertexFormat = DXGI_FORMAT_UNKNOWN;
    outIndexCount = 0;
    outIndexFormat = DXGI_FORMAT_UNKNOWN;
    outIBOffsetBytes = 0;

    // CRITICAL: Validate render mesh
    if (!pRM)
    {
        CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Render mesh is null");
        return false;
    }

    // CRITICAL: Device state validation
    if (!m_pDevice)
    {
        CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Device not available");
        return false;
    }

    __try
    {
        HRESULT deviceRemovedHr = m_pDevice->GetDeviceRemovedReason();
        if (FAILED(deviceRemovedHr))
        {
            CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Device removed/reset (hr=0x%08x)", deviceRemovedHr);
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Exception checking device state");
        return false;
    }

    // SAFE: Get mesh properties with validation
    int meshVertexCount = 0;
    int meshIndexCount = 0;

    __try
    {
        meshVertexCount = pRM->GetVerticesCount();
        meshIndexCount = pRM->GetIndicesCount();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Exception getting mesh counts");
        return false;
    }

    // CRITICAL: Validate mesh has reasonable vertex/index counts
    if (meshVertexCount <= 0 || meshVertexCount > 1000000)
    {
        CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Invalid vertex count (%d)", meshVertexCount);
        return false;
    }

    if (meshIndexCount <= 0 || meshIndexCount > 3000000 || (meshIndexCount % 3) != 0)
    {
        CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Invalid index count (%d)", meshIndexCount);
        return false;
    }

    // CRITICAL FIX: Get vertex stream with proper error handling
    buffer_handle_t vertexHandle = 0;
    UINT vertexOffset = 0;
    UINT vertexStride = 0;
    DXGI_FORMAT vertexFormat = DXGI_FORMAT_UNKNOWN;

    __try
    {
        // SAFE: Get vertex stream information
        int32_t vertexStride;
        uint8_t* pVertexData = pRM->GetPosPtr(vertexStride, FSL_READ);
        if (!pVertexData || vertexStride <= 0)
        {
            CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Failed to get vertex positions");
            return false;
        }

        vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT; // Assume position format for now
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Exception getting vertex stream");
        return false;
    }

    // CRITICAL FIX: Get index stream with proper error handling
    buffer_handle_t indexHandle = 0;
    UINT indexOffset = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;

    __try
    {
        // SAFE: Get index stream information
        vtx_idx* pIndices = pRM->GetIndexPtr(FSL_READ);
        if (!pIndices) {
            CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Failed to get index data");
            return false;
        }

        // Determine index format based on vtx_idx size
        if (sizeof(vtx_idx) == 2)
        {
            indexFormat = DXGI_FORMAT_R16_UINT;
        }
        else if (sizeof(vtx_idx) == 4)
        {
            indexFormat = DXGI_FORMAT_R32_UINT;
        }
        else
        {
            CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Unsupported index element size (%zu)", sizeof(vtx_idx));
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Exception getting index stream");
        return false;
    }

    // CRITICAL FIX: Validate handles are valid
    if (vertexHandle == 0 || vertexHandle == ~0u || indexHandle == 0 || indexHandle == ~0u)
    {
        CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Invalid buffer handles (VB=0x%x, IB=0x%x)", vertexHandle, indexHandle);
        return false;
    }

    // CRITICAL FIX: Get device addresses using our safe helper
    UINT64 requiredVertexSize = (UINT64)meshVertexCount * vertexStride;
    UINT64 requiredIndexSize = (UINT64)meshIndexCount * (indexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4);

    if (!GetDeviceAddressFromBufferHandle(vertexHandle, outVB, requiredVertexSize, vertexOffset))
    {
        CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Failed to get vertex buffer device address");
        return false;
    }

    if (!GetDeviceAddressFromBufferHandle(indexHandle, outIB, requiredIndexSize, indexOffset))
    {
        CryLogAlways("[D3D_RT] GetMeshDeviceStreams: Failed to get index buffer device address");
        // Clear vertex buffer output since we're failing
        outVB = {};
        return false;
    }

    // SUCCESS: Set output parameters
    outVertexCount = (UINT)meshVertexCount;
    outVertexStride = vertexStride;
    outVertexFormat = vertexFormat;
    outIndexCount = (UINT)meshIndexCount;
    outIndexFormat = indexFormat;
    outIBOffsetBytes = indexOffset;

    CryLogAlways("[D3D_RT] GetMeshDeviceStreams: SUCCESS - VB(0x%llx, %u verts, stride=%u), IB(0x%llx, %u indices, format=%d)",
        outVB.gpuVA, outVertexCount, outVertexStride, outIB.gpuVA, outIndexCount, outIndexFormat);

    return true;
}


// Build BLAS/TLAS from the current render view (visible level)
HRESULT CD3D_RT::CreateSceneBLASAndTLASFromView()
{
    // Now calls the new level geometry extraction
    return CreateLevelGeometryBLASAndTLAS();
}