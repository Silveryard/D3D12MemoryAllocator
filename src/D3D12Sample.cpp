//
// Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "D3D12MemAlloc.h"
#include "Common.h"
#include "Tests.h"
#include <atomic>

namespace VS
{
    #include "Shaders\VS_Compiled.h"
}
namespace PS
{
    #include "Shaders\PS_Compiled.h"
}

static const wchar_t * const CLASS_NAME = L"D3D12MemAllocSample";
static const wchar_t * const WINDOW_TITLE = L"D3D12 Memory Allocator Sample";
static const int SIZE_X = 1024;
static const int SIZE_Y = 576; 
static const bool FULLSCREEN = false;
static const UINT PRESENT_SYNC_INTERVAL = 1;
static const DXGI_FORMAT RENDER_TARGET_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
static const DXGI_FORMAT DEPTH_STENCIL_FORMAT = DXGI_FORMAT_D32_FLOAT;
static const size_t FRAME_BUFFER_COUNT = 3; // number of buffers we want, 2 for double buffering, 3 for tripple buffering
static const D3D_FEATURE_LEVEL MY_D3D_FEATURE_LEVEL = D3D_FEATURE_LEVEL_12_0;

static const bool ENABLE_DEBUG_LAYER = true;
static const bool ENABLE_CPU_ALLOCATION_CALLBACKS = true;
static const bool ENABLE_CPU_ALLOCATION_CALLBACKS_PRINT = false;

static HINSTANCE g_Instance;
static HWND g_Wnd;

static UINT64 g_TimeOffset; // In ms.
static UINT64 g_TimeValue; // Time since g_TimeOffset, in ms.
static float g_Time; // g_TimeValue converted to float, in seconds.
static float g_TimeDelta;

static CComPtr<ID3D12Device> g_Device;
static D3D12MA::Allocator* g_Allocator;

static CComPtr<IDXGISwapChain3> g_SwapChain; // swapchain used to switch between render targets
static CComPtr<ID3D12CommandQueue> g_CommandQueue; // container for command lists
static CComPtr<ID3D12DescriptorHeap> g_RtvDescriptorHeap; // a descriptor heap to hold resources like the render targets
static CComPtr<ID3D12Resource> g_RenderTargets[FRAME_BUFFER_COUNT]; // number of render targets equal to buffer count
static CComPtr<ID3D12CommandAllocator> g_CommandAllocators[FRAME_BUFFER_COUNT]; // we want enough allocators for each buffer * number of threads (we only have one thread)
static CComPtr<ID3D12GraphicsCommandList> g_CommandList; // a command list we can record commands into, then execute them to render the frame
static CComPtr<ID3D12Fence> g_Fences[FRAME_BUFFER_COUNT];    // an object that is locked while our command list is being executed by the gpu. We need as many 
                                                      //as we have allocators (more if we want to know when the gpu is finished with an asset)
static HANDLE g_FenceEvent; // a handle to an event when our g_Fences is unlocked by the gpu
static UINT64 g_FenceValues[FRAME_BUFFER_COUNT]; // this value is incremented each frame. each g_Fences will have its own value
static UINT g_FrameIndex; // current rtv we are on
static UINT g_RtvDescriptorSize; // size of the rtv descriptor on the g_Device (all front and back buffers will be the same size)

static CComPtr<ID3D12PipelineState> g_PipelineStateObject;
static CComPtr<ID3D12RootSignature> g_RootSignature;
static CComPtr<ID3D12Resource> g_VertexBuffer;
static D3D12MA::Allocation* g_VertexBufferAllocation;
static CComPtr<ID3D12Resource> g_IndexBuffer;
static D3D12MA::Allocation* g_IndexBufferAllocation;
static D3D12_VERTEX_BUFFER_VIEW g_VertexBufferView;
static D3D12_INDEX_BUFFER_VIEW g_IndexBufferView;
static CComPtr<ID3D12Resource> g_DepthStencilBuffer;
static D3D12MA::Allocation* g_DepthStencilAllocation;
static CComPtr<ID3D12DescriptorHeap> g_DepthStencilDescriptorHeap;

struct Vertex {
    vec3 pos;
    vec2 texCoord;

    Vertex() { }
    Vertex(float x, float y, float z, float tx, float ty) :
        pos(x, y, z),
        texCoord(tx, ty)
    {
    }
};

struct ConstantBuffer0_PS
{
    vec4 Color;
};
struct ConstantBuffer1_VS
{
    mat4 WorldViewProj;
};

static const size_t ConstantBufferPerObjectAlignedSize = AlignUp<size_t>(sizeof(ConstantBuffer1_VS), 256);
static D3D12MA::Allocation* g_CbPerObjectUploadHeapAllocations[FRAME_BUFFER_COUNT];
static CComPtr<ID3D12Resource> g_CbPerObjectUploadHeaps[FRAME_BUFFER_COUNT];
static void* g_CbPerObjectAddress[FRAME_BUFFER_COUNT];
static uint32_t g_CubeIndexCount;

static CComPtr<ID3D12DescriptorHeap> g_MainDescriptorHeap[FRAME_BUFFER_COUNT];
static CComPtr<ID3D12Resource> g_ConstantBufferUploadHeap[FRAME_BUFFER_COUNT];
static D3D12MA::Allocation* g_ConstantBufferUploadAllocation[FRAME_BUFFER_COUNT];
static void* g_ConstantBufferAddress[FRAME_BUFFER_COUNT];

static CComPtr<ID3D12Resource> g_Texture;
static D3D12MA::Allocation* g_TextureAllocation;

static void* const CUSTOM_ALLOCATION_USER_DATA = (void*)(uintptr_t)0xDEADC0DE;

static std::atomic<size_t> g_CpuAllocationCount{0};

static void* CustomAllocate(size_t Size, size_t Alignment, void* pUserData)
{
    assert(pUserData == CUSTOM_ALLOCATION_USER_DATA);
    void* memory = _aligned_malloc(Size, Alignment);
    if(ENABLE_CPU_ALLOCATION_CALLBACKS_PRINT)
    {
        wprintf(L"Allocate Size=%llu Alignment=%llu -> %p\n", Size, Alignment, memory);
    }
    ++g_CpuAllocationCount;
    return memory;
}

static void CustomFree(void* pMemory, void* pUserData)
{
    assert(pUserData == CUSTOM_ALLOCATION_USER_DATA);
    if(pMemory)
    {
        --g_CpuAllocationCount;
        if(ENABLE_CPU_ALLOCATION_CALLBACKS_PRINT)
        {
            wprintf(L"Free %p\n", pMemory);
        }
        _aligned_free(pMemory);
    }
}

static void SetDefaultRasterizerDesc(D3D12_RASTERIZER_DESC& outDesc)
{
    outDesc.FillMode = D3D12_FILL_MODE_SOLID;
    outDesc.CullMode = D3D12_CULL_MODE_BACK;
    outDesc.FrontCounterClockwise = FALSE;
    outDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    outDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    outDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    outDesc.DepthClipEnable = TRUE;
    outDesc.MultisampleEnable = FALSE;
    outDesc.AntialiasedLineEnable = FALSE;
    outDesc.ForcedSampleCount = 0;
    outDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
}

static void SetDefaultBlendDesc(D3D12_BLEND_DESC& outDesc)
{
    outDesc.AlphaToCoverageEnable = FALSE;
    outDesc.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = {
        FALSE,FALSE,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        outDesc.RenderTarget[i] = defaultRenderTargetBlendDesc;
}

static void SetDefaultDepthStencilDesc(D3D12_DEPTH_STENCIL_DESC& outDesc)
{
    outDesc.DepthEnable = TRUE;
    outDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    outDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    outDesc.StencilEnable = FALSE;
    outDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    outDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    const D3D12_DEPTH_STENCILOP_DESC defaultStencilOp = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
    outDesc.FrontFace = defaultStencilOp;
    outDesc.BackFace = defaultStencilOp;
}

void WaitForFrame(size_t frameIndex) // wait until gpu is finished with command list
{
    // if the current g_Fences value is still less than "g_FenceValues", then we know the GPU has not finished executing
    // the command queue since it has not reached the "g_CommandQueue->Signal(g_Fences, g_FenceValues)" command
    if (g_Fences[frameIndex]->GetCompletedValue() < g_FenceValues[frameIndex])
    {
        // we have the g_Fences create an event which is signaled once the g_Fences's current value is "g_FenceValues"
        CHECK_HR( g_Fences[frameIndex]->SetEventOnCompletion(g_FenceValues[frameIndex], g_FenceEvent) );

        // We will wait until the g_Fences has triggered the event that it's current value has reached "g_FenceValues". once it's value
        // has reached "g_FenceValues", we know the command queue has finished executing
        WaitForSingleObject(g_FenceEvent, INFINITE);
    }
}

void WaitGPUIdle(size_t frameIndex)
{
    g_FenceValues[frameIndex]++;
    CHECK_HR( g_CommandQueue->Signal(g_Fences[frameIndex], g_FenceValues[frameIndex]) );
    WaitForFrame(frameIndex);
}

//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
// Row-by-row memcpy
inline void MemcpySubresource(
    _In_ const D3D12_MEMCPY_DEST* pDest,
    _In_ const D3D12_SUBRESOURCE_DATA* pSrc,
    SIZE_T RowSizeInBytes,
    UINT NumRows,
    UINT NumSlices)
{
    for (UINT z = 0; z < NumSlices; ++z)
    {
        BYTE* pDestSlice = reinterpret_cast<BYTE*>(pDest->pData) + pDest->SlicePitch * z;
        const BYTE* pSrcSlice = reinterpret_cast<const BYTE*>(pSrc->pData) + pSrc->SlicePitch * z;
        for (UINT y = 0; y < NumRows; ++y)
        {
            memcpy(pDestSlice + pDest->RowPitch * y,
                pSrcSlice + pSrc->RowPitch * y,
                RowSizeInBytes);
        }
    }
}

//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
inline UINT64 UpdateSubresources(
    _In_ ID3D12GraphicsCommandList* pCmdList,
    _In_ ID3D12Resource* pDestinationResource,
    _In_ ID3D12Resource* pIntermediate,
    _In_range_(0,D3D12_REQ_SUBRESOURCES) UINT FirstSubresource,
    _In_range_(0,D3D12_REQ_SUBRESOURCES-FirstSubresource) UINT NumSubresources,
    UINT64 RequiredSize,
    _In_reads_(NumSubresources) const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts,
    _In_reads_(NumSubresources) const UINT* pNumRows,
    _In_reads_(NumSubresources) const UINT64* pRowSizesInBytes,
    _In_reads_(NumSubresources) const D3D12_SUBRESOURCE_DATA* pSrcData)
{
    // Minor validation
    D3D12_RESOURCE_DESC IntermediateDesc = pIntermediate->GetDesc();
    D3D12_RESOURCE_DESC DestinationDesc = pDestinationResource->GetDesc();
    if (IntermediateDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || 
        IntermediateDesc.Width < RequiredSize + pLayouts[0].Offset || 
        RequiredSize > (SIZE_T)-1 || 
        (DestinationDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && 
        (FirstSubresource != 0 || NumSubresources != 1)))
    {
        return 0;
    }

    BYTE* pData;
    HRESULT hr = pIntermediate->Map(0, NULL, reinterpret_cast<void**>(&pData));
    if (FAILED(hr))
    {
        return 0;
    }

    for (UINT i = 0; i < NumSubresources; ++i)
    {
        if (pRowSizesInBytes[i] > (SIZE_T)-1) return 0;
        D3D12_MEMCPY_DEST DestData = { pData + pLayouts[i].Offset, pLayouts[i].Footprint.RowPitch, pLayouts[i].Footprint.RowPitch * pNumRows[i] };
        MemcpySubresource(&DestData, &pSrcData[i], (SIZE_T)pRowSizesInBytes[i], pNumRows[i], pLayouts[i].Footprint.Depth);
    }
    pIntermediate->Unmap(0, NULL);

    if (DestinationDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        D3D12_BOX SrcBox = {
            UINT( pLayouts[0].Offset ), 0, 0,
            UINT( pLayouts[0].Offset + pLayouts[0].Footprint.Width ), 0, 0 };
        pCmdList->CopyBufferRegion(
            pDestinationResource, 0, pIntermediate, pLayouts[0].Offset, pLayouts[0].Footprint.Width);
    }
    else
    {
        for (UINT i = 0; i < NumSubresources; ++i)
        {
            D3D12_TEXTURE_COPY_LOCATION Dst = {};
            Dst.pResource = pDestinationResource;
            Dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            Dst.SubresourceIndex = i + FirstSubresource;
            D3D12_TEXTURE_COPY_LOCATION Src = {};
            Src.pResource = pIntermediate;
            Src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            Src.PlacedFootprint = pLayouts[i];
            pCmdList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        }
    }
    return RequiredSize;
}

//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
inline UINT64 UpdateSubresources(
    _In_ ID3D12GraphicsCommandList* pCmdList,
    _In_ ID3D12Resource* pDestinationResource,
    _In_ ID3D12Resource* pIntermediate,
    UINT64 IntermediateOffset,
    _In_range_(0,D3D12_REQ_SUBRESOURCES) UINT FirstSubresource,
    _In_range_(0,D3D12_REQ_SUBRESOURCES-FirstSubresource) UINT NumSubresources,
    _In_reads_(NumSubresources) D3D12_SUBRESOURCE_DATA* pSrcData)
{
    UINT64 RequiredSize = 0;
    UINT64 MemToAlloc = static_cast<UINT64>(sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) + sizeof(UINT) + sizeof(UINT64)) * NumSubresources;
    if (MemToAlloc > SIZE_MAX)
    {
        return 0;
    }
    void* pMem = HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(MemToAlloc));
    if (pMem == NULL)
    {
        return 0;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts = reinterpret_cast<D3D12_PLACED_SUBRESOURCE_FOOTPRINT*>(pMem);
    UINT64* pRowSizesInBytes = reinterpret_cast<UINT64*>(pLayouts + NumSubresources);
    UINT* pNumRows = reinterpret_cast<UINT*>(pRowSizesInBytes + NumSubresources);

    D3D12_RESOURCE_DESC Desc = pDestinationResource->GetDesc();
    ID3D12Device* pDevice;
    pDestinationResource->GetDevice(__uuidof(*pDevice), reinterpret_cast<void**>(&pDevice));
    pDevice->GetCopyableFootprints(&Desc, FirstSubresource, NumSubresources, IntermediateOffset, pLayouts, pNumRows, pRowSizesInBytes, &RequiredSize);
    pDevice->Release();

    UINT64 Result = UpdateSubresources(pCmdList, pDestinationResource, pIntermediate, FirstSubresource, NumSubresources, RequiredSize, pLayouts, pNumRows, pRowSizesInBytes, pSrcData);
    HeapFree(GetProcessHeap(), 0, pMem);
    return Result;
}

void InitD3D() // initializes direct3d 12
{
    IDXGIFactory4* dxgiFactory;
    CHECK_HR( CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)) );

    IDXGIAdapter1* adapter = nullptr; // adapters are the graphics card (this includes the embedded graphics on the motherboard)

    int adapterIndex = 0; // we'll start looking for directx 12  compatible graphics devices starting at index 0

    bool adapterFound = false; // set this to true when a good one was found

                               // find first hardware gpu that supports d3d 12
    while (dxgiFactory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
        {
            HRESULT hr = D3D12CreateDevice(adapter, MY_D3D_FEATURE_LEVEL, _uuidof(ID3D12Device), nullptr);
            if (SUCCEEDED(hr))
            {
                adapterFound = true;
                break;
            }
        }
        adapter->Release();
        adapterIndex++;
    }
    assert(adapterFound);

    // Must be done before D3D12 device is created.
    if(ENABLE_DEBUG_LAYER)
    {
        CComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
    }

    // Create the g_Device
    ID3D12Device* device = nullptr;
    CHECK_HR( D3D12CreateDevice(
        adapter,
        MY_D3D_FEATURE_LEVEL,
        IID_PPV_ARGS(&device)) );
    g_Device.Attach(device);

    // Create allocator

    {
        D3D12MA::ALLOCATOR_DESC desc = {};
        desc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
        desc.pDevice = device;

        D3D12MA::ALLOCATION_CALLBACKS allocationCallbacks = {};
        if(ENABLE_CPU_ALLOCATION_CALLBACKS)
        {
            allocationCallbacks.pAllocate = &CustomAllocate;
            allocationCallbacks.pFree = &CustomFree;
            allocationCallbacks.pUserData = CUSTOM_ALLOCATION_USER_DATA;
            desc.pAllocationCallbacks = &allocationCallbacks;
        }

        CHECK_HR( D3D12MA::CreateAllocator(&desc, &g_Allocator) );

        switch(g_Allocator->GetD3D12Options().ResourceHeapTier)
        {
        case D3D12_RESOURCE_HEAP_TIER_1:
            wprintf(L"ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_1\n");
            break;
        case D3D12_RESOURCE_HEAP_TIER_2:
            wprintf(L"ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2\n");
            break;
        default:
            assert(0);
        }
    }

    // -- Create the Command Queue -- //

    D3D12_COMMAND_QUEUE_DESC cqDesc = {}; // we will be using all the default values

    ID3D12CommandQueue* commandQueue = nullptr;
    CHECK_HR( g_Device->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&commandQueue)) ); // create the command queue
    g_CommandQueue.Attach(commandQueue);

    // -- Create the Swap Chain (double/tripple buffering) -- //

    DXGI_MODE_DESC backBufferDesc = {}; // this is to describe our display mode
    backBufferDesc.Width = SIZE_X; // buffer width
    backBufferDesc.Height = SIZE_Y; // buffer height
    backBufferDesc.Format = RENDER_TARGET_FORMAT; // format of the buffer (rgba 32 bits, 8 bits for each chanel)

                                                  // describe our multi-sampling. We are not multi-sampling, so we set the count to 1 (we need at least one sample of course)
    DXGI_SAMPLE_DESC sampleDesc = {};
    sampleDesc.Count = 1; // multisample count (no multisampling, so we just put 1, since we still need 1 sample)

                          // Describe and create the swap chain.
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = FRAME_BUFFER_COUNT; // number of buffers we have
    swapChainDesc.BufferDesc = backBufferDesc; // our back buffer description
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; // this says the pipeline will render to this swap chain
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // dxgi will discard the buffer (data) after we call present
    swapChainDesc.OutputWindow = g_Wnd; // handle to our window
    swapChainDesc.SampleDesc = sampleDesc; // our multi-sampling description
    swapChainDesc.Windowed = !FULLSCREEN; // set to true, then if in fullscreen must call SetFullScreenState with true for full screen to get uncapped fps

    IDXGISwapChain* tempSwapChain;

    CHECK_HR( dxgiFactory->CreateSwapChain(
        g_CommandQueue, // the queue will be flushed once the swap chain is created
        &swapChainDesc, // give it the swap chain description we created above
        &tempSwapChain // store the created swap chain in a temp IDXGISwapChain interface
    ) );

    g_SwapChain.Attach(static_cast<IDXGISwapChain3*>(tempSwapChain));

    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex(); 

    // -- Create the Back Buffers (render target views) Descriptor Heap -- //

    // describe an rtv descriptor heap and create
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAME_BUFFER_COUNT; // number of descriptors for this heap.
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; // this heap is a render target view heap

                                                       // This heap will not be directly referenced by the shaders (not shader visible), as this will store the output from the pipeline
                                                       // otherwise we would set the heap's flag to D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ID3D12DescriptorHeap* rtvDescriptorHeap = nullptr;
    CHECK_HR( g_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvDescriptorHeap)) );
    g_RtvDescriptorHeap.Attach(rtvDescriptorHeap);

    // get the size of a descriptor in this heap (this is a rtv heap, so only rtv descriptors should be stored in it.
    // descriptor sizes may vary from g_Device to g_Device, which is why there is no set size and we must ask the 
    // g_Device to give us the size. we will use this size to increment a descriptor handle offset
    g_RtvDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // get a handle to the first descriptor in the descriptor heap. a handle is basically a pointer,
    // but we cannot literally use it like a c++ pointer.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle { g_RtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart() };

    // Create a RTV for each buffer (double buffering is two buffers, tripple buffering is 3).
    for (int i = 0; i < FRAME_BUFFER_COUNT; i++)
    {
        // first we get the n'th buffer in the swap chain and store it in the n'th
        // position of our ID3D12Resource array
        ID3D12Resource* res = nullptr;
        CHECK_HR( g_SwapChain->GetBuffer(i, IID_PPV_ARGS(&res)) );
        g_RenderTargets[i].Attach(res);

        // the we "create" a render target view which binds the swap chain buffer (ID3D12Resource[n]) to the rtv handle
        g_Device->CreateRenderTargetView(g_RenderTargets[i], nullptr, rtvHandle);

        // we increment the rtv handle by the rtv descriptor size we got above
        rtvHandle.ptr += g_RtvDescriptorSize;
    }

    // -- Create the Command Allocators -- //

    for (int i = 0; i < FRAME_BUFFER_COUNT; i++)
    {
        ID3D12CommandAllocator* commandAllocator = nullptr;
        CHECK_HR( g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)) );
        g_CommandAllocators[i].Attach(commandAllocator);
    }

    // create the command list with the first allocator
    CHECK_HR( g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[0], NULL, IID_PPV_ARGS(&g_CommandList)) );

    // command lists are created in the recording state. our main loop will set it up for recording again so close it now
    g_CommandList->Close();

    // create a depth stencil descriptor heap so we can get a pointer to the depth stencil buffer
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK_HR( g_Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&g_DepthStencilDescriptorHeap)) );

    D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
    depthOptimizedClearValue.Format = DEPTH_STENCIL_FORMAT;
    depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
    depthOptimizedClearValue.DepthStencil.Stencil = 0;

    D3D12MA::ALLOCATION_DESC depthStencilAllocDesc = {};
    depthStencilAllocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC depthStencilResourceDesc = {};
    depthStencilResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthStencilResourceDesc.Alignment = 0;
    depthStencilResourceDesc.Width = SIZE_X;
    depthStencilResourceDesc.Height = SIZE_Y;
    depthStencilResourceDesc.DepthOrArraySize = 1;
    depthStencilResourceDesc.MipLevels = 1;
    depthStencilResourceDesc.Format = DEPTH_STENCIL_FORMAT;
    depthStencilResourceDesc.SampleDesc.Count = 1;
    depthStencilResourceDesc.SampleDesc.Quality = 0;
    depthStencilResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthStencilResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    CHECK_HR( g_Allocator->CreateResource(
        &depthStencilAllocDesc,
        &depthStencilResourceDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthOptimizedClearValue,
        &g_DepthStencilAllocation,
        IID_PPV_ARGS(&g_DepthStencilBuffer)
    ) );
    CHECK_HR( g_DepthStencilBuffer->SetName(L"Depth/Stencil Resource Heap") );

    D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = {};
    depthStencilDesc.Format = DEPTH_STENCIL_FORMAT;
    depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;
    g_Device->CreateDepthStencilView(g_DepthStencilBuffer, &depthStencilDesc, g_DepthStencilDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    // -- Create a Fence & Fence Event -- //

    // create the fences
    for (int i = 0; i < FRAME_BUFFER_COUNT; i++)
    {
        ID3D12Fence* fence = nullptr;
        CHECK_HR( g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)) );
        g_Fences[i].Attach(fence);
        g_FenceValues[i] = 0; // set the initial g_Fences value to 0
    }

    // create a handle to a g_Fences event
    g_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    assert(g_FenceEvent);

    D3D12_DESCRIPTOR_RANGE cbDescriptorRange;
    cbDescriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cbDescriptorRange.NumDescriptors = 1;
    cbDescriptorRange.BaseShaderRegister = 0;
    cbDescriptorRange.RegisterSpace = 0;
    cbDescriptorRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE textureDescRange;
    textureDescRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureDescRange.NumDescriptors = 1;
    textureDescRange.BaseShaderRegister = 0;
    textureDescRange.RegisterSpace = 0;
    textureDescRange.OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER  rootParameters[3];

    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable = {1, &cbDescriptorRange};
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[1].Descriptor = {1, 0};
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].DescriptorTable = {1, &textureDescRange};
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // create root signature

    // create a static sampler
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 0;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &sampler;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    CComPtr<ID3DBlob> signatureBlob;
    ID3DBlob* signatureBlobPtr;
    CHECK_HR( D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlobPtr, nullptr) );
    signatureBlob.Attach(signatureBlobPtr);

    ID3D12RootSignature* rootSignature = nullptr;
    CHECK_HR( device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)) );
    g_RootSignature.Attach(rootSignature);

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 2;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        CHECK_HR( g_Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_MainDescriptorHeap[i])) );
    }

    // # CONSTANT BUFFER

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i)
    {
        D3D12MA::ALLOCATION_DESC constantBufferUploadAllocDesc = {};
        constantBufferUploadAllocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC constantBufferResourceDesc = {};
        constantBufferResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        constantBufferResourceDesc.Alignment = 0;
        constantBufferResourceDesc.Width = 1024 * 64;
        constantBufferResourceDesc.Height = 1;
        constantBufferResourceDesc.DepthOrArraySize = 1;
        constantBufferResourceDesc.MipLevels = 1;
        constantBufferResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        constantBufferResourceDesc.SampleDesc.Count = 1;
        constantBufferResourceDesc.SampleDesc.Quality = 0;
        constantBufferResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        constantBufferResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        CHECK_HR( g_Allocator->CreateResource(
            &constantBufferUploadAllocDesc,
            &constantBufferResourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            &g_ConstantBufferUploadAllocation[i],
            IID_PPV_ARGS(&g_ConstantBufferUploadHeap[i])) );
        g_ConstantBufferUploadHeap[i]->SetName(L"Constant Buffer Upload Resource Heap");

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = g_ConstantBufferUploadHeap[i]->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = AlignUp<UINT>(sizeof(ConstantBuffer0_PS), 256);
        g_Device->CreateConstantBufferView(&cbvDesc, g_MainDescriptorHeap[i]->GetCPUDescriptorHandleForHeapStart());

        D3D12_RANGE readRange{0, 0};
        CHECK_HR( g_ConstantBufferUploadHeap[i]->Map(0, &readRange, &g_ConstantBufferAddress[i]) );
    }

    // create input layout

    // The input layout is used by the Input Assembler so that it knows
    // how to read the vertex data bound to it.

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // create a pipeline state object (PSO)

    // In a real application, you will have many pso's. for each different shader
    // or different combinations of shaders, different blend states or different rasterizer states,
    // different topology types (point, line, triangle, patch), or a different number
    // of render targets you will need a pso

    // VS is the only required shader for a pso. You might be wondering when a case would be where
    // you only set the VS. It's possible that you have a pso that only outputs data with the stream
    // output, and not on a render target, which means you would not need anything after the stream
    // output.

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {}; // a structure to define a pso
    psoDesc.InputLayout.NumElements = _countof(inputLayout);
    psoDesc.InputLayout.pInputElementDescs = inputLayout;
    psoDesc.pRootSignature = g_RootSignature; // the root signature that describes the input data this pso needs
    psoDesc.VS.BytecodeLength = sizeof(VS::g_main);
    psoDesc.VS.pShaderBytecode = VS::g_main;
    psoDesc.PS.BytecodeLength = sizeof(PS::g_main);
    psoDesc.PS.pShaderBytecode = PS::g_main;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; // type of topology we are drawing
    psoDesc.RTVFormats[0] = RENDER_TARGET_FORMAT; // format of the render target
    psoDesc.DSVFormat = DEPTH_STENCIL_FORMAT;
    psoDesc.SampleDesc = sampleDesc; // must be the same sample description as the swapchain and depth/stencil buffer
    psoDesc.SampleMask = 0xffffffff; // sample mask has to do with multi-sampling. 0xffffffff means point sampling is done
    SetDefaultRasterizerDesc(psoDesc.RasterizerState);
    SetDefaultBlendDesc(psoDesc.BlendState);
    psoDesc.NumRenderTargets = 1; // we are only binding one render target
    SetDefaultDepthStencilDesc(psoDesc.DepthStencilState);

    // create the pso
    ID3D12PipelineState* pipelineStateObject;
    CHECK_HR( device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineStateObject)) );
    g_PipelineStateObject.Attach(pipelineStateObject);

    // Create vertex buffer

    // a triangle
    Vertex vList[] = {
        // front face
        { -0.5f,  0.5f, -0.5f, 0.f, 0.f },
        {  0.5f, -0.5f, -0.5f, 1.f, 1.f },
        { -0.5f, -0.5f, -0.5f, 0.f, 1.f },
        {  0.5f,  0.5f, -0.5f, 1.f, 0.f },

        // right side face
        {  0.5f, -0.5f, -0.5f, 0.f, 1.f },
        {  0.5f,  0.5f,  0.5f, 1.f, 0.f },
        {  0.5f, -0.5f,  0.5f, 1.f, 1.f },
        {  0.5f,  0.5f, -0.5f, 0.f, 0.f },

        // left side face
        { -0.5f,  0.5f,  0.5f, 0.f, 0.f },
        { -0.5f, -0.5f, -0.5f, 1.f, 1.f },
        { -0.5f, -0.5f,  0.5f, 0.f, 1.f },
        { -0.5f,  0.5f, -0.5f, 1.f, 0.f },

        // back face
        {  0.5f,  0.5f,  0.5f, 0.f, 0.f },
        { -0.5f, -0.5f,  0.5f, 1.f, 1.f },
        {  0.5f, -0.5f,  0.5f, 0.f, 1.f },
        { -0.5f,  0.5f,  0.5f, 1.f, 0.f },

        // top face
        { -0.5f,  0.5f, -0.5f, 0.f, 0.f },
        {  0.5f,  0.5f,  0.5f, 1.f, 1.f },
        {  0.5f,  0.5f, -0.5f, 0.f, 1.f },
        { -0.5f,  0.5f,  0.5f, 1.f, 0.f },

        // bottom face
        {  0.5f, -0.5f,  0.5f, 0.f, 0.f },
        { -0.5f, -0.5f, -0.5f, 1.f, 1.f },
        {  0.5f, -0.5f, -0.5f, 0.f, 1.f },
        { -0.5f, -0.5f,  0.5f, 1.f, 0.f },
    };
    const uint32_t vBufferSize = sizeof(vList);

    // create default heap
    // default heap is memory on the GPU. Only the GPU has access to this memory
    // To get data into this heap, we will have to upload the data using
    // an upload heap
    D3D12MA::ALLOCATION_DESC vertexBufferAllocDesc = {};
    vertexBufferAllocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC vertexBufferResourceDesc = {};
    vertexBufferResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vertexBufferResourceDesc.Alignment = 0;
    vertexBufferResourceDesc.Width = vBufferSize;
    vertexBufferResourceDesc.Height = 1;
    vertexBufferResourceDesc.DepthOrArraySize = 1;
    vertexBufferResourceDesc.MipLevels = 1;
    vertexBufferResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    vertexBufferResourceDesc.SampleDesc.Count = 1;
    vertexBufferResourceDesc.SampleDesc.Quality = 0;
    vertexBufferResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    vertexBufferResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* vertexBufferPtr;
    CHECK_HR( g_Allocator->CreateResource(
        &vertexBufferAllocDesc,
        &vertexBufferResourceDesc, // resource description for a buffer
        D3D12_RESOURCE_STATE_COPY_DEST, // we will start this heap in the copy destination state since we will copy data
                                        // from the upload heap to this heap
        nullptr, // optimized clear value must be null for this type of resource. used for render targets and depth/stencil buffers
        &g_VertexBufferAllocation,
        IID_PPV_ARGS(&vertexBufferPtr)) );
    g_VertexBuffer.Attach(vertexBufferPtr);

    // we can give resource heaps a name so when we debug with the graphics debugger we know what resource we are looking at
    g_VertexBuffer->SetName(L"Vertex Buffer Resource Heap");

    // create upload heap
    // upload heaps are used to upload data to the GPU. CPU can write to it, GPU can read from it
    // We will upload the vertex buffer using this heap to the default heap
    D3D12MA::ALLOCATION_DESC vBufferUploadAllocDesc = {};
    vBufferUploadAllocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC vertexBufferUploadResourceDesc = {};
    vertexBufferUploadResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vertexBufferUploadResourceDesc.Alignment = 0;
    vertexBufferUploadResourceDesc.Width = vBufferSize;
    vertexBufferUploadResourceDesc.Height = 1;
    vertexBufferUploadResourceDesc.DepthOrArraySize = 1;
    vertexBufferUploadResourceDesc.MipLevels = 1;
    vertexBufferUploadResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    vertexBufferUploadResourceDesc.SampleDesc.Count = 1;
    vertexBufferUploadResourceDesc.SampleDesc.Quality = 0;
    vertexBufferUploadResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    vertexBufferUploadResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    CComPtr<ID3D12Resource> vBufferUploadHeap;
    D3D12MA::Allocation* vBufferUploadHeapAllocation = nullptr;
    CHECK_HR( g_Allocator->CreateResource(
        &vBufferUploadAllocDesc,
        &vertexBufferUploadResourceDesc, // resource description for a buffer
        D3D12_RESOURCE_STATE_GENERIC_READ, // GPU will read from this buffer and copy its contents to the default heap
        nullptr,
        &vBufferUploadHeapAllocation,
        IID_PPV_ARGS(&vBufferUploadHeap)) );
    vBufferUploadHeap->SetName(L"Vertex Buffer Upload Resource Heap");

    // store vertex buffer in upload heap
    D3D12_SUBRESOURCE_DATA vertexData = {};
    vertexData.pData = reinterpret_cast<BYTE*>(vList); // pointer to our vertex array
    vertexData.RowPitch = vBufferSize; // size of all our triangle vertex data
    vertexData.SlicePitch = vBufferSize; // also the size of our triangle vertex data

    CHECK_HR( g_CommandList->Reset(g_CommandAllocators[g_FrameIndex], NULL) );

    // we are now creating a command with the command list to copy the data from
    // the upload heap to the default heap
    UINT64 r = UpdateSubresources(g_CommandList, g_VertexBuffer, vBufferUploadHeap, 0, 0, 1, &vertexData);
    assert(r);

    // transition the vertex buffer data from copy destination state to vertex buffer state
    D3D12_RESOURCE_BARRIER vbBarrier = {};
    vbBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    vbBarrier.Transition.pResource = g_VertexBuffer;
    vbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    vbBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    vbBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_CommandList->ResourceBarrier(1, &vbBarrier);

    // Create index buffer

    // a quad (2 triangles)
    uint16_t iList[] = {
        // ffront face
        0, 1, 2, // first triangle
        0, 3, 1, // second triangle

        // left face
        4, 5, 6, // first triangle
        4, 7, 5, // second triangle

        // right face
        8, 9, 10, // first triangle
        8, 11, 9, // second triangle

        // back face
        12, 13, 14, // first triangle
        12, 15, 13, // second triangle

        // top face
        16, 17, 18, // first triangle
        16, 19, 17, // second triangle

        // bottom face
        20, 21, 22, // first triangle
        20, 23, 21, // second triangle
    };

    g_CubeIndexCount = (uint32_t)_countof(iList);

    size_t iBufferSize = sizeof(iList);

    // create default heap to hold index buffer
    D3D12MA::ALLOCATION_DESC indexBufferAllocDesc = {};
    indexBufferAllocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC indexBufferResourceDesc = {};
    indexBufferResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    indexBufferResourceDesc.Alignment = 0;
    indexBufferResourceDesc.Width = iBufferSize;
    indexBufferResourceDesc.Height = 1;
    indexBufferResourceDesc.DepthOrArraySize = 1;
    indexBufferResourceDesc.MipLevels = 1;
    indexBufferResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    indexBufferResourceDesc.SampleDesc.Count = 1;
    indexBufferResourceDesc.SampleDesc.Quality = 0;
    indexBufferResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    indexBufferResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    CHECK_HR( g_Allocator->CreateResource(
        &indexBufferAllocDesc,
        &indexBufferResourceDesc, // resource description for a buffer
        D3D12_RESOURCE_STATE_COPY_DEST, // start in the copy destination state
        nullptr, // optimized clear value must be null for this type of resource
        &g_IndexBufferAllocation,
        IID_PPV_ARGS(&g_IndexBuffer)) );

    // we can give resource heaps a name so when we debug with the graphics debugger we know what resource we are looking at
    g_IndexBuffer->SetName(L"Index Buffer Resource Heap");

    // create upload heap to upload index buffer
    D3D12MA::ALLOCATION_DESC iBufferUploadAllocDesc = {};
    iBufferUploadAllocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC indexBufferUploadResourceDesc = {};
    indexBufferUploadResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    indexBufferUploadResourceDesc.Alignment = 0;
    indexBufferUploadResourceDesc.Width = iBufferSize;
    indexBufferUploadResourceDesc.Height = 1;
    indexBufferUploadResourceDesc.DepthOrArraySize = 1;
    indexBufferUploadResourceDesc.MipLevels = 1;
    indexBufferUploadResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    indexBufferUploadResourceDesc.SampleDesc.Count = 1;
    indexBufferUploadResourceDesc.SampleDesc.Quality = 0;
    indexBufferUploadResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    indexBufferUploadResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    CComPtr<ID3D12Resource> iBufferUploadHeap;
    D3D12MA::Allocation* iBufferUploadHeapAllocation = nullptr;
    CHECK_HR( g_Allocator->CreateResource(
        &iBufferUploadAllocDesc,
        &indexBufferUploadResourceDesc, // resource description for a buffer
        D3D12_RESOURCE_STATE_GENERIC_READ, // GPU will read from this buffer and copy its contents to the default heap
        nullptr,
        &iBufferUploadHeapAllocation,
        IID_PPV_ARGS(&iBufferUploadHeap)) );
    CHECK_HR( iBufferUploadHeap->SetName(L"Index Buffer Upload Resource Heap") );

    // store vertex buffer in upload heap
    D3D12_SUBRESOURCE_DATA indexData = {};
    indexData.pData = iList; // pointer to our index array
    indexData.RowPitch = iBufferSize; // size of all our index buffer
    indexData.SlicePitch = iBufferSize; // also the size of our index buffer

                                        // we are now creating a command with the command list to copy the data from
                                        // the upload heap to the default heap
    r = UpdateSubresources(g_CommandList, g_IndexBuffer, iBufferUploadHeap, 0, 0, 1, &indexData);
    assert(r);

    // transition the index buffer data from copy destination state to vertex buffer state
    D3D12_RESOURCE_BARRIER ibBarrier = {};
    ibBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ibBarrier.Transition.pResource = g_IndexBuffer;
    ibBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    ibBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    ibBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_CommandList->ResourceBarrier(1, &ibBarrier);

    // create a vertex buffer view for the triangle. We get the GPU memory address to the vertex pointer using the GetGPUVirtualAddress() method
    g_VertexBufferView.BufferLocation = g_VertexBuffer->GetGPUVirtualAddress();
    g_VertexBufferView.StrideInBytes = sizeof(Vertex);
    g_VertexBufferView.SizeInBytes = vBufferSize;

    // create a index buffer view for the triangle. We get the GPU memory address to the vertex pointer using the GetGPUVirtualAddress() method
    g_IndexBufferView.BufferLocation = g_IndexBuffer->GetGPUVirtualAddress();
    g_IndexBufferView.Format = DXGI_FORMAT_R16_UINT;
    g_IndexBufferView.SizeInBytes = (UINT)iBufferSize;

    D3D12MA::ALLOCATION_DESC cbPerObjectUploadAllocDesc = {};
    cbPerObjectUploadAllocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC cbPerObjectUploadResourceDesc = {};
    cbPerObjectUploadResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbPerObjectUploadResourceDesc.Alignment = 0;
    cbPerObjectUploadResourceDesc.Width = 1024 * 64;
    cbPerObjectUploadResourceDesc.Height = 1;
    cbPerObjectUploadResourceDesc.DepthOrArraySize = 1;
    cbPerObjectUploadResourceDesc.MipLevels = 1;
    cbPerObjectUploadResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbPerObjectUploadResourceDesc.SampleDesc.Count = 1;
    cbPerObjectUploadResourceDesc.SampleDesc.Quality = 0;
    cbPerObjectUploadResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbPerObjectUploadResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
    {
        // create resource for cube 1
        CHECK_HR( g_Allocator->CreateResource(
            &cbPerObjectUploadAllocDesc,
            &cbPerObjectUploadResourceDesc, // size of the resource heap. Must be a multiple of 64KB for single-textures and constant buffers
            D3D12_RESOURCE_STATE_GENERIC_READ, // will be data that is read from so we keep it in the generic read state
            nullptr, // we do not have use an optimized clear value for constant buffers
            &g_CbPerObjectUploadHeapAllocations[i],
            IID_PPV_ARGS(&g_CbPerObjectUploadHeaps[i])) );
        g_CbPerObjectUploadHeaps[i]->SetName(L"Constant Buffer Upload Resource Heap");

        D3D12_RANGE readRange{0, 0};    // We do not intend to read from this resource on the CPU. (so end is less than or equal to begin)
                                          // map the resource heap to get a gpu virtual address to the beginning of the heap
        CHECK_HR( g_CbPerObjectUploadHeaps[i]->Map(0, &readRange, &g_CbPerObjectAddress[i]) );
    }

    // # TEXTURE

    D3D12_RESOURCE_DESC textureDesc;
    size_t imageBytesPerRow;
    size_t imageSize = SIZE_MAX;
    std::vector<char> imageData;
    {
        const UINT sizeX = 256;
        const UINT sizeY = 256;
        const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        const UINT bytesPerPixel = 4;

        imageBytesPerRow = sizeX * bytesPerPixel;
        imageSize = sizeY * imageBytesPerRow;

        imageData.resize(imageSize);
        char* rowPtr = (char*)imageData.data();
        for(UINT y = 0; y < sizeY; ++y)
        {
            char* pixelPtr = rowPtr;
            for(UINT x = 0; x < sizeX; ++x)
            {
                *(UINT8*)(pixelPtr    ) = (UINT8)x; // R
                *(UINT8*)(pixelPtr + 1) = (UINT8)y; // G
                *(UINT8*)(pixelPtr + 2) = 0x00; // B
                *(UINT8*)(pixelPtr + 3) = 0xFF; // A

                *(UINT8*)(pixelPtr    ) = x > 128 ? 0xFF : 00;
                *(UINT8*)(pixelPtr + 1) = y > 128 ? 0xFF : 00;
                pixelPtr += bytesPerPixel;
            }
            rowPtr += imageBytesPerRow;
        }

        textureDesc = {};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Alignment = 0;
        textureDesc.Width = sizeX;
        textureDesc.Height = sizeY;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = format;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    }

    D3D12MA::ALLOCATION_DESC textureAllocDesc = {};
    textureAllocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    CHECK_HR( g_Allocator->CreateResource(
        &textureAllocDesc,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, // pOptimizedClearValue
        &g_TextureAllocation,
        IID_PPV_ARGS(&g_Texture)) );
    g_Texture->SetName(L"g_Texture");

    UINT64 textureUploadBufferSize;
    device->GetCopyableFootprints(
        &textureDesc,
        0, // FirstSubresource
        1, // NumSubresources
        0, // BaseOffset
        nullptr, // pLayouts
        nullptr, // pNumRows
        nullptr, // pRowSizeInBytes
        &textureUploadBufferSize); // pTotalBytes

    D3D12MA::ALLOCATION_DESC textureUploadAllocDesc = {};
    textureUploadAllocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC textureUploadResourceDesc = {};
    textureUploadResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    textureUploadResourceDesc.Alignment = 0;
    textureUploadResourceDesc.Width = textureUploadBufferSize;
    textureUploadResourceDesc.Height = 1;
    textureUploadResourceDesc.DepthOrArraySize = 1;
    textureUploadResourceDesc.MipLevels = 1;
    textureUploadResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    textureUploadResourceDesc.SampleDesc.Count = 1;
    textureUploadResourceDesc.SampleDesc.Quality = 0;
    textureUploadResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    textureUploadResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    CComPtr<ID3D12Resource> textureUpload;
    D3D12MA::Allocation* textureUploadAllocation;
    CHECK_HR( g_Allocator->CreateResource(
        &textureUploadAllocDesc,
        &textureUploadResourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, // pOptimizedClearValue
        &textureUploadAllocation,
        IID_PPV_ARGS(&textureUpload)) );
    textureUpload->SetName(L"textureUpload");

    D3D12_SUBRESOURCE_DATA textureSubresourceData = {};
    textureSubresourceData.pData = imageData.data();
    textureSubresourceData.RowPitch = imageBytesPerRow;
    textureSubresourceData.SlicePitch = imageBytesPerRow * textureDesc.Height;

    UpdateSubresources(g_CommandList, g_Texture, textureUpload, 0, 0, 1, &textureSubresourceData);

    D3D12_RESOURCE_BARRIER textureBarrier = {};
    textureBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    textureBarrier.Transition.pResource = g_Texture;
    textureBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    textureBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    textureBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_CommandList->ResourceBarrier(1, &textureBarrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE descHandle = {
            g_MainDescriptorHeap[i]->GetCPUDescriptorHandleForHeapStart().ptr +
            g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
        g_Device->CreateShaderResourceView(g_Texture, &srvDesc, descHandle);
    }

    // # END OF INITIAL COMMAND LIST

    // Now we execute the command list to upload the initial assets (triangle data)
    g_CommandList->Close();
    ID3D12CommandList* ppCommandLists[] = { g_CommandList };
    commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // increment the fence value now, otherwise the buffer might not be uploaded by the time we start drawing
    WaitGPUIdle(g_FrameIndex);

    textureUploadAllocation->Release();
    iBufferUploadHeapAllocation->Release();
    vBufferUploadHeapAllocation->Release();
}

void Update()
{
    {
        const float f = sin(g_Time * (PI * 2.f)) * 0.5f + 0.5f;
        ConstantBuffer0_PS cb;
        cb.Color = vec4(f, f, f, 1.f);
        memcpy(g_ConstantBufferAddress[g_FrameIndex], &cb, sizeof(cb));
    }

    {
        const mat4 projection = mat4::Perspective(
            45.f * (PI / 180.f), // fovY
            (float)SIZE_X / (float)SIZE_Y, // aspectRatio
            0.1f, // zNear
            1000.0f); // zFar
        const mat4 view = mat4::LookAt(
            vec3(0.f, 0.f, 0.f), // at
            vec3(-.4f, 1.7f, -3.5f), // eye
            vec3(0.f, 1.f, 0.f)); // up
        const mat4 viewProjection = view * projection;

        mat4 cube1World = mat4::RotationZ(g_Time);

        ConstantBuffer1_VS cb;
        mat4 worldViewProjection = cube1World * viewProjection;
        cb.WorldViewProj = worldViewProjection.Transposed();
        memcpy(g_CbPerObjectAddress[g_FrameIndex], &cb, sizeof(cb));

        mat4 cube2World = mat4::Scaling(0.5f) *
            mat4::RotationX(g_Time * 2.0f) *
            mat4::Translation(vec3(-1.2f, 0.f, 0.f)) *
            cube1World;

        worldViewProjection = cube2World * viewProjection;
        cb.WorldViewProj = worldViewProjection.Transposed();
        memcpy((char*)g_CbPerObjectAddress[g_FrameIndex] + ConstantBufferPerObjectAlignedSize, &cb, sizeof(cb));
    }
}

void Render() // execute the command list
{
    // # Here was UpdatePipeline function.

    // swap the current rtv buffer index so we draw on the correct buffer
    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex();
    // We have to wait for the gpu to finish with the command allocator before we reset it
    WaitForFrame(g_FrameIndex);
    // increment g_FenceValues for next frame
    g_FenceValues[g_FrameIndex]++;

    // we can only reset an allocator once the gpu is done with it
    // resetting an allocator frees the memory that the command list was stored in
    CHECK_HR( g_CommandAllocators[g_FrameIndex]->Reset() );

    // reset the command list. by resetting the command list we are putting it into
    // a recording state so we can start recording commands into the command allocator.
    // the command allocator that we reference here may have multiple command lists
    // associated with it, but only one can be recording at any time. Make sure
    // that any other command lists associated to this command allocator are in
    // the closed state (not recording).
    // Here you will pass an initial pipeline state object as the second parameter,
    // but in this tutorial we are only clearing the rtv, and do not actually need
    // anything but an initial default pipeline, which is what we get by setting
    // the second parameter to NULL
    CHECK_HR( g_CommandList->Reset(g_CommandAllocators[g_FrameIndex], NULL) );

    // here we start recording commands into the g_CommandList (which all the commands will be stored in the g_CommandAllocators)

    // transition the "g_FrameIndex" render target from the present state to the render target state so the command list draws to it starting from here
    D3D12_RESOURCE_BARRIER presentToRenderTargetBarrier = {};
    presentToRenderTargetBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    presentToRenderTargetBarrier.Transition.pResource = g_RenderTargets[g_FrameIndex];
    presentToRenderTargetBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    presentToRenderTargetBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    presentToRenderTargetBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_CommandList->ResourceBarrier(1, &presentToRenderTargetBarrier);

    // here we again get the handle to our current render target view so we can set it as the render target in the output merger stage of the pipeline
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {
        g_RtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart().ptr + g_FrameIndex * g_RtvDescriptorSize};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
        g_DepthStencilDescriptorHeap->GetCPUDescriptorHandleForHeapStart();

    // set the render target for the output merger stage (the output of the pipeline)
    g_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    g_CommandList->ClearDepthStencilView(g_DepthStencilDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Clear the render target by using the ClearRenderTargetView command
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    g_CommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    g_CommandList->SetPipelineState(g_PipelineStateObject);

    g_CommandList->SetGraphicsRootSignature(g_RootSignature);

    ID3D12DescriptorHeap* descriptorHeaps[] = { g_MainDescriptorHeap[g_FrameIndex] };
    g_CommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    g_CommandList->SetGraphicsRootDescriptorTable(0, g_MainDescriptorHeap[g_FrameIndex]->GetGPUDescriptorHandleForHeapStart());
    g_CommandList->SetGraphicsRootDescriptorTable(2, g_MainDescriptorHeap[g_FrameIndex]->GetGPUDescriptorHandleForHeapStart());

    D3D12_VIEWPORT viewport{0.f, 0.f, (float)SIZE_X, (float)SIZE_Y, 0.f, 1.f};
    g_CommandList->RSSetViewports(1, &viewport); // set the viewports

    D3D12_RECT scissorRect{0, 0, SIZE_X, SIZE_Y};
    g_CommandList->RSSetScissorRects(1, &scissorRect); // set the scissor rects

    g_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); // set the primitive topology
    g_CommandList->IASetVertexBuffers(0, 1, &g_VertexBufferView); // set the vertex buffer (using the vertex buffer view)
    g_CommandList->IASetIndexBuffer(&g_IndexBufferView);

    g_CommandList->SetGraphicsRootConstantBufferView(1,
        g_CbPerObjectUploadHeaps[g_FrameIndex]->GetGPUVirtualAddress());
    g_CommandList->DrawIndexedInstanced(g_CubeIndexCount, 1, 0, 0, 0);

    g_CommandList->SetGraphicsRootConstantBufferView(1,
        g_CbPerObjectUploadHeaps[g_FrameIndex]->GetGPUVirtualAddress() + ConstantBufferPerObjectAlignedSize);
    g_CommandList->DrawIndexedInstanced(g_CubeIndexCount, 1, 0, 0, 0);

    // transition the "g_FrameIndex" render target from the render target state to the present state. If the debug layer is enabled, you will receive a
    // warning if present is called on the render target when it's not in the present state
    D3D12_RESOURCE_BARRIER renderTargetToPresentBarrier = {};
    renderTargetToPresentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    renderTargetToPresentBarrier.Transition.pResource = g_RenderTargets[g_FrameIndex];
    renderTargetToPresentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    renderTargetToPresentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    renderTargetToPresentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_CommandList->ResourceBarrier(1, &renderTargetToPresentBarrier);

    CHECK_HR( g_CommandList->Close() );

    // ================

    // create an array of command lists (only one command list here)
    ID3D12CommandList* ppCommandLists[] = { g_CommandList };

    // execute the array of command lists
    g_CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // this command goes in at the end of our command queue. we will know when our command queue 
    // has finished because the g_Fences value will be set to "g_FenceValues" from the GPU since the command
    // queue is being executed on the GPU
    CHECK_HR( g_CommandQueue->Signal(g_Fences[g_FrameIndex], g_FenceValues[g_FrameIndex]) );

    // present the current backbuffer
    CHECK_HR( g_SwapChain->Present(PRESENT_SYNC_INTERVAL, 0) );
}

void Cleanup() // release com ojects and clean up memory
{
    // wait for the gpu to finish all frames
    for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
    {
        WaitForFrame(i);
        CHECK_HR( g_CommandQueue->Wait(g_Fences[i], g_FenceValues[i]) );
    }

    // get swapchain out of full screen before exiting
    BOOL fs = false;
    CHECK_HR( g_SwapChain->GetFullscreenState(&fs, NULL) );
    if (fs)
        g_SwapChain->SetFullscreenState(false, NULL);

    WaitGPUIdle(0);

    g_Texture.Release();
    g_TextureAllocation->Release(); g_TextureAllocation = nullptr;
    g_IndexBuffer.Release();
    g_IndexBufferAllocation->Release(); g_IndexBufferAllocation = nullptr;
    g_VertexBuffer.Release();
    g_VertexBufferAllocation->Release(); g_VertexBufferAllocation = nullptr;
    g_PipelineStateObject.Release();
    g_RootSignature.Release();

    CloseHandle(g_FenceEvent);
    g_CommandList.Release();
    g_CommandQueue.Release();

    for (size_t i = FRAME_BUFFER_COUNT; i--; )
    {
        g_CbPerObjectUploadHeaps[i].Release();
        g_CbPerObjectUploadHeapAllocations[i]->Release(); g_CbPerObjectUploadHeapAllocations[i] = nullptr;
        g_MainDescriptorHeap[i].Release();
        g_ConstantBufferUploadHeap[i].Release();
        g_ConstantBufferUploadAllocation[i]->Release(); g_ConstantBufferUploadAllocation[i] = nullptr;
    }

    g_DepthStencilDescriptorHeap.Release();
    g_DepthStencilBuffer.Release();
    g_DepthStencilAllocation->Release(); g_DepthStencilAllocation = nullptr;
    g_RtvDescriptorHeap.Release();
    for (size_t i = FRAME_BUFFER_COUNT; i--; )
    {
        g_RenderTargets[i].Release();
        g_CommandAllocators[i].Release();
        g_Fences[i].Release();
    }
    
    g_Allocator->Release(); g_Allocator = nullptr;
    if(ENABLE_CPU_ALLOCATION_CALLBACKS)
    {
        assert(g_CpuAllocationCount.load() == 0);
    }
    
    g_Device.Release();
    g_SwapChain.Release();
}

static void ExecuteTests()
{
    try
    {
        TestContext ctx = {};
        ctx.device = g_Device;
        ctx.allocator = g_Allocator;
        Test(ctx);
    }
    catch(const std::exception& ex)
    {
        wprintf(L"ERROR: %hs\n", ex.what());
    }
}

static void OnKeyDown(WPARAM key)
{
    switch (key)
    {
    case 'T':
        ExecuteTests();
        break;

    case VK_ESCAPE:
        PostMessage(g_Wnd, WM_CLOSE, 0, 0);
        break;
    }
}

static LRESULT WINAPI WndProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg)
    {
    case WM_CREATE:
        g_Wnd = wnd;
        InitD3D();
        g_TimeOffset = GetTickCount64();
        return 0;

    case WM_DESTROY:
        Cleanup();
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;
    }

    return DefWindowProc(wnd, msg, wParam, lParam);
}

ID3D12GraphicsCommandList* BeginCommandList()
{
    CHECK_HR( g_CommandList->Reset(g_CommandAllocators[g_FrameIndex], NULL) );

    return g_CommandList;
}

void EndCommandList(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->Close();

    ID3D12CommandList* genericCmdList = cmdList;
    g_CommandQueue->ExecuteCommandLists(1, &genericCmdList);

    WaitGPUIdle(g_FrameIndex);
}

int main()
{
    g_Instance = (HINSTANCE)GetModuleHandle(NULL);

    CoInitialize(NULL);

    WNDCLASSEX wndClass;
    ZeroMemory(&wndClass, sizeof(wndClass));
    wndClass.cbSize = sizeof(wndClass);
    wndClass.style = CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS;
    wndClass.hbrBackground = NULL;
    wndClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndClass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wndClass.hInstance = g_Instance;
    wndClass.lpfnWndProc = &WndProc;
    wndClass.lpszClassName = CLASS_NAME;

    ATOM classR = RegisterClassEx(&wndClass);
    assert(classR);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    DWORD exStyle = 0;

    RECT rect = { 0, 0, SIZE_X, SIZE_Y };
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    g_Wnd = CreateWindowEx(
        exStyle,
        CLASS_NAME,
        WINDOW_TITLE,
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL,
        NULL,
        g_Instance,
        0);
    assert(g_Wnd);

    MSG msg;
    for (;;)
    {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            const UINT64 newTimeValue = GetTickCount64() - g_TimeOffset;
            g_TimeDelta = (float)(newTimeValue - g_TimeValue) * 0.001f;
            g_TimeValue = newTimeValue;
            g_Time = (float)newTimeValue * 0.001f;

            Update();
            Render();
        }
    }
    return (int)msg.wParam;
} 
