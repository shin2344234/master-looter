// Adapted for Master Looter from Trinity (https://github.com/XeTrinityz/Trinity),
// MIT License, Copyright (c) 2026 XeTrinityz. See THIRD_PARTY_NOTICES.md.
// Changes: Master Looter namespaces, logger, settings and menu hooks; icon atlas code removed.
#include "dx12_hook.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <atomic>
#include <vector>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include "input.h"
#include "xinput_hook.h"
#include "hdr_composite_shader.h"
#include "../core/log.h"
#include "../core/mod.h"
#include "../core/settings.h"
#include "../core/state.h"
#include "../gui/menu.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace ml::hooks
{
    // --- Original function pointers -----------------------------------------
    using Present_t         = HRESULT (WINAPI*)(IDXGISwapChain3*, UINT, UINT);
    using ResizeBuffers_t   = HRESULT (WINAPI*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using ExecuteCmdLists_t = void    (WINAPI*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    using SetColorSpace1_t  = HRESULT (WINAPI*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);

    static Present_t         oPresent             = nullptr;
    static ResizeBuffers_t   oResizeBuffers       = nullptr;
    static ExecuteCmdLists_t oExecuteCommandLists = nullptr;
    static SetColorSpace1_t  oSetColorSpace1      = nullptr;

    // --- Frame Generation (DLSS-G) support ----------------------------------
    // To draw over DLSS Frame Generation we must render on the swapchain the
    // GAME presents to - the writable, pre-interpolation one - NOT the native
    // swapchain Streamline pushes finished frames through (its buffers are
    // read-only to us => DXGI_ERROR_ACCESS_DENIED, device removed).
    //
    // The proven approach (OptiScaler / ReShade): WRAP, don't detour. We return a
    // WrappedIDXGISwapChain to the game that forwards every method to the real
    // (Streamline-proxy) swapchain but intercepts Present/Present1 to draw the
    // overlay first. The wrapper is handed out by a VirtualProtect vtable-slot
    // patch of the factory's CreateSwapChain*/CreateSwapChainForHwnd - NOT a
    // MinHook detour of the DXGI exports (that deadlocked startup and fought
    // Streamline's own interposer). See InstallSwapChainCreationPatch.
    using CSFH_t = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

    static CSFH_t oFactoryCreateSwapChainForHwnd = nullptr;

    // Set once a swapchain has been wrapped: the wrapper now does all overlay
    // drawing, so the native Present byte-hook must stop drawing (its buffers are
    // Streamline's read-only finished frames). Never cleared for the session.
    static bool g_wrapperActive = false;

    // Frames that reached this mod through any path: the wrapper's Present, the
    // stacked detours, either flavour. Zero when the game has been running for
    // a while means this mod has no way onto the screen, which OverlayBlindWatch
    // acts on rather than leaving the player with a menu key that does nothing.
    static std::atomic<unsigned> g_presentSeen{0};

    // Reentrancy guard for swapchain creation. When the game calls our patched
    // CreateSwapChainForHwnd, the real implementation (Streamline's interposer)
    // can itself create swapchains through the SAME class vtable slot we patched -
    // and it does exactly this every time Frame Generation is toggled, since SL is
    // spec-required to tear down and recreate the swapchain on an FG on/off. Only
    // the OUTERMOST call (the one the game made) may be wrapped; nested internal
    // ones are Streamline's own plumbing and must pass straight through, or we
    // recurse into the interposer mid-(re)creation and hang / remove the device.
    // This is the crash that appears "after a couple loads": the first swapchain
    // wraps cleanly, then an FG toggle re-enters us during SL's rebuild.
    static thread_local bool t_inSwapChainCreate = false;

    // Resets the guard however the call leaves, including through an exception
    // out of the interposer. A guard left set makes every later creation look
    // nested, so nothing is ever wrapped or re-pinned again for that thread.
    struct CreateGuard
    {
        bool wasNested;
        CreateGuard() : wasNested(t_inSwapChainCreate) { t_inSwapChainCreate = true; }
        ~CreateGuard() { if (!wasNested) t_inSwapChainCreate = false; }
    };

    // How many of our wrappers are alive.
    //
    // The reentrancy guard above only catches a creation nested inside our own
    // call, which is what happens while the game is making its first swapchain.
    // Toggling frame generation later is not nested: the game asks Streamline to
    // change mode, and Streamline rebuilds its swapchain from its own code, so
    // the creation arrives here looking exactly like a fresh game-issued one.
    // Wrapping it hands our object to code that allocated its own and expects to
    // get that back, and re-pins the present queue to whichever queue Streamline
    // passed rather than the game's.
    //
    // A live wrapper is what tells the two apart. The game cannot create a
    // replacement for its own window without releasing the old one first, so if
    // ours is still alive this creation belongs to somebody else. Leave it alone.
    static volatile LONG g_liveWrappers = 0;

    // The window our wrapped (main) swapchain belongs to. The engine and Streamline
    // can spin up auxiliary swapchains on other windows; we only ever want the one
    // the game renders the world into. Locked on the first successful wrap so later
    // auxiliary creations are ignored.
    static HWND g_wrappedHwnd = nullptr;

    // The window the game's first full-size swapchain was created for, set on
    // both paths. g_wrappedHwnd is only ever set by WrapSwapChain, so on the
    // stacked path it stays null and nothing knows which window is the game's.
    static HWND g_gameHwnd = nullptr;

    // --- Rendering resources ------------------------------------------------
    struct FrameContext
    {
        ID3D12CommandAllocator*     commandAllocator = nullptr;
        ID3D12Resource*             renderTarget     = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle        = {};
        UINT64                      fenceValue       = 0; // GPU signal for this frame's overlay work
    };

    static ID3D12Device*              g_device      = nullptr;
    static ID3D12DescriptorHeap*      g_rtvHeap     = nullptr;
    static ID3D12DescriptorHeap*      g_srvHeap     = nullptr;
    static ID3D12GraphicsCommandList* g_commandList = nullptr;
    // SRV heap capacity: slot 0 ImGui font, slots 1-3 the fixed UI atlases,
    // the rest a session budget for lazily-loaded per-item icons (~64KB of
    // GPU memory each; see icons.cpp).
    //
    // Raised from 512 for Inventory -> Add Item: that browses the game's WHOLE
    // item catalog rather than just what you carry, so the number of distinct
    // icons a session can touch went from "a few hundred at most" to thousands.
    // Icons are loaded on demand and never evicted, so the old budget ran out
    // partway through browsing and every icon after it - including the ones in
    // the Editor - silently drew blank for the rest of the session.
    //
    // The heap itself is cheap (a descriptor is 32 bytes; this is ~128KB). The
    // real cost is one texture per icon actually looked at, so a session that
    // never opens Add Item pays nothing extra. If this is ever exhausted again
    // the answer is LRU eviction, not another bump.
    static constexpr UINT             kSrvHeapSlots = 8;
    // The most-recent DIRECT queue confirmed to live on the swapchain's device
    // (i.e. the present queue). Published ONLY from hkExecuteCommandLists while
    // the queue argument is guaranteed alive, and AddRef'd - we never keep
    // un-refcounted queue pointers (the engine destroys temporary queues during
    // loading; a cached raw pointer is a use-after-free).
    static ID3D12CommandQueue*        g_presentQueue   = nullptr; // guarded by g_queueLock, AddRef'd
    static CRITICAL_SECTION           g_queueLock;
    static bool                       g_queueLockReady = false;
    // Set once we have the AUTHORITATIVE present queue - the one passed to
    // CreateSwapChainForHwnd, which for D3D12 IS the swapchain's present queue and
    // owns the back buffers. Once pinned, the ExecuteCommandLists heuristic must not
    // replace it: under Multi Frame Generation the busiest DIRECT queue there is
    // Streamline's present pacer, and submitting our overlay on the pacer queue is
    // rejected (ACCESS_DENIED -> device removed). This was the instant-crash cause.
    static bool                       g_presentQueuePinned = false;
    static ID3D12Fence*               g_fence          = nullptr;
    static HANDLE                     g_fenceEvent     = nullptr;
    static UINT64                     g_fenceValue     = 0;
    static std::vector<FrameContext>  g_frames;
    static UINT                       g_bufferCount    = 0;
    // The swapchain our render targets currently describe. Compared by identity
    // each Present so we can rebuild when Streamline (DLSS Frame Generation)
    // silently swaps the native swapchain out from under us - see
    // ReconcileSwapChain. Raw pointer: used ONLY for the identity compare,
    // never dereferenced when stale.
    static IDXGISwapChain3*           g_swapChain      = nullptr;
    static HWND                       g_hwnd           = nullptr;
    // Full back-buffer signature our render targets currently describe. A video
    // settings change (resolution, HDR) resizes the buffers IN PLACE - same
    // swapchain pointer, often the same count - so pointer+count alone can't tell
    // us the buffers moved. Track width/height/format too and rebuild on any
    // change, or our RTVs keep pointing at freed buffers => ACCESS_DENIED.
    static UINT                       g_scWidth        = 0;
    static UINT                       g_scHeight       = 0;
    static DXGI_FORMAT                g_scFormat       = DXGI_FORMAT_UNKNOWN;
    static bool                       g_imguiReady     = false;
    static bool                       g_initFailed     = false;
    static bool                       g_renderDisabled = false;

    // --- HDR-aware overlay compositing ---------------------------------------
    // ImGui always renders into an SDR (R8G8B8A8_UNORM, plain sRGB-gamma
    // numeric) offscreen target - decoupled from whatever pixel format/color
    // space the real back buffer is actually in. A small composite pass then
    // re-encodes that image into the back buffer's ACTUAL color space (scRGB
    // linear or HDR10 PQ) and alpha-blends it in. Without this an HDR back
    // buffer reads our plain 0-1 UI colors as if they were scene-referred /
    // PQ-encoded values directly, and the menu comes out blown-out and
    // oversaturated - the bug this whole section exists to fix.
    static ID3D12Resource*             g_offscreenTex     = nullptr;
    static ID3D12DescriptorHeap*       g_offscreenRtvHeap = nullptr;
    static D3D12_CPU_DESCRIPTOR_HANDLE g_offscreenRtv     = {};
    static UINT                        g_offscreenW       = 0;
    static UINT                        g_offscreenH       = 0;

    // Fixed SRV heap slot for the offscreen texture, reserved right after the
    // ImGui font (slot 0). icons.cpp's fixed atlases + item-icon budget are
    // pushed one slot later to make room (see the IconsInit call below).
    static constexpr UINT kOffscreenSrvSlot = 1;

    static ID3D12RootSignature* g_compositeRootSig = nullptr;
    static ID3D12PipelineState* g_compositePSO     = nullptr;
    static DXGI_FORMAT          g_compositeFormat  = DXGI_FORMAT_UNKNOWN;

    // SDR reference white targeted inside the HDR range (ITU-R BT.2408's
    // recommended "graphics white" for HDR overlays/subtitles). Not the peak
    // brightness - just how bright plain white UI text reads next to the
    // game's own HDR highlights.
    static constexpr float kPaperWhiteNits = 203.0f;

    // The color space the REAL swap chain is currently presenting in. DXGI has
    // no "get current color space" query - the only way to know it is to watch
    // every IDXGISwapChain3::SetColorSpace1 call the engine makes (native path
    // hkSetColorSpace1, wrapped path WrappedIDXGISwapChain::SetColorSpace1).
    // Defaults to plain SDR so nothing changes for players without HDR enabled,
    // or before the engine has told us otherwise.
    static DXGI_COLOR_SPACE_TYPE g_colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    // 0 = SDR passthrough (byte-identical to drawing straight on the back
    // buffer), 1 = scRGB linear, 2 = HDR10 / ST.2084 PQ. Any other color space
    // (e.g. wide-gamut SDR) falls back to passthrough rather than guessing.
    static UINT CompositeModeForColorSpace(DXGI_COLOR_SPACE_TYPE cs)
    {
        switch (cs)
        {
            case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:    return 1;
            case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return 2;
            default:                                         return 0;
        }
    }

    static void OnColorSpaceChanged(DXGI_COLOR_SPACE_TYPE cs)
    {
        if (cs == g_colorSpace) return;
        g_colorSpace = cs;
        const char* name = CompositeModeForColorSpace(cs) == 2 ? "HDR10 (PQ)"
                          : CompositeModeForColorSpace(cs) == 1 ? "scRGB (linear)"
                          : "SDR";
        LOG("Swapchain color space changed to %s (%d) - overlay compositing %s.",
            name, static_cast<int>(cs),
            CompositeModeForColorSpace(cs) != 0 ? "HDR-adjusted" : "unadjusted");
    }

    // ------------------------------------------------------------------------
    static void CleanupRenderTargets()
    {
        for (auto& f : g_frames)
        {
            if (f.renderTarget) { f.renderTarget->Release(); f.renderTarget = nullptr; }
        }
    }

    static bool CreateRenderTargets(IDXGISwapChain3* swapChain)
    {
        const UINT rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();

        for (UINT i = 0; i < g_bufferCount; ++i)
        {
            ID3D12Resource* buffer = nullptr;
            if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer))))
                return false;

            g_device->CreateRenderTargetView(buffer, nullptr, handle);
            g_frames[i].renderTarget = buffer;
            g_frames[i].rtvHandle    = handle;
            handle.ptr += rtvSize;
        }
        return true;
    }

    static void ReleaseOffscreenTarget()
    {
        if (g_offscreenTex)     { g_offscreenTex->Release();     g_offscreenTex     = nullptr; }
        if (g_offscreenRtvHeap) { g_offscreenRtvHeap->Release(); g_offscreenRtvHeap = nullptr; }
        g_offscreenW = 0;
        g_offscreenH = 0;
    }

    // (Re)creates the offscreen SDR target ImGui renders into, sized to match
    // the real back buffer. A no-op when the size hasn't changed. Requires
    // g_srvHeap to already exist (its SRV lands at the reserved fixed slot).
    static bool CreateOffscreenTarget(UINT width, UINT height)
    {
        if (g_offscreenTex && width == g_offscreenW && height == g_offscreenH)
            return true;

        ReleaseOffscreenTarget();
        if (width == 0 || height == 0)
            return false;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = width;
        desc.Height           = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = desc.Format; // Color left {0,0,0,0} - matches the per-frame clear.

        // Explicit initial state, not COMMON - this is a fresh resource we own
        // outright, and DrawOverlay's first barrier transitions FROM this state.
        if (FAILED(g_device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                IID_PPV_ARGS(&g_offscreenTex))))
            return false;
        g_offscreenTex->SetName(L"MasterLooterOverlayOffscreen");

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = 1;
        if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_offscreenRtvHeap))))
        {
            ReleaseOffscreenTarget();
            return false;
        }
        g_offscreenRtv = g_offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart();
        g_device->CreateRenderTargetView(g_offscreenTex, nullptr, g_offscreenRtv);

        const UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE srv = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        srv.ptr += static_cast<SIZE_T>(kOffscreenSrvSlot) * inc;
        g_device->CreateShaderResourceView(g_offscreenTex, nullptr, srv);

        g_offscreenW = width;
        g_offscreenH = height;
        return true;
    }

    // Builds the composite pass's PSO for the given back-buffer format (cheap:
    // no textures, no font atlas - just a tiny fullscreen-triangle pipeline) and
    // caches it. Rebuilds only when the format actually changes (HDR toggle,
    // which reallocates the swap chain with a different back-buffer format).
    // The root signature never depends on format, so it's created once and reused.
    static bool CreateCompositePipeline(DXGI_FORMAT rtvFormat)
    {
        if (!g_compositeRootSig)
        {
            D3D12_DESCRIPTOR_RANGE srvRange = {};
            srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRange.NumDescriptors                     = 1;
            srvRange.BaseShaderRegister                 = 0;
            srvRange.OffsetInDescriptorsFromTableStart  = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            D3D12_ROOT_PARAMETER params[2] = {};
            params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants.ShaderRegister = 0;
            params[0].Constants.Num32BitValues  = 4; // uint mode; float paperWhiteNits; float2 pad;
            params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

            params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable.NumDescriptorRanges  = 1;
            params[1].DescriptorTable.pDescriptorRanges    = &srvRange;
            params[1].ShaderVisibility                     = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_STATIC_SAMPLER_DESC sampler = {};
            sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
            rsDesc.NumParameters     = 2;
            rsDesc.pParameters       = params;
            rsDesc.NumStaticSamplers = 1;
            rsDesc.pStaticSamplers   = &sampler;

            ID3DBlob* sig = nullptr;
            ID3DBlob* err = nullptr;
            const HRESULT serialized = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
            if (err) err->Release();
            if (FAILED(serialized))
                return false;

            const HRESULT created = g_device->CreateRootSignature(
                0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_compositeRootSig));
            sig->Release();
            if (FAILED(created))
                return false;
        }

        if (g_compositePSO) { g_compositePSO->Release(); g_compositePSO = nullptr; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = g_compositeRootSig;
        pso.VS = { g_hdrCompositeVS, sizeof(g_hdrCompositeVS) };
        pso.PS = { g_hdrCompositePS, sizeof(g_hdrCompositePS) };
        // Same blend equation ImGui itself uses - straight (non-premultiplied)
        // alpha, so compositing the offscreen sprite reproduces exactly what
        // drawing ImGui directly onto the back buffer would have (mode 0).
        pso.BlendState.RenderTarget[0].BlendEnable    = TRUE;
        pso.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
        pso.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
        pso.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        pso.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask                      = 0xFFFFFFFFu;
        pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthEnable   = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets      = 1;
        pso.RTVFormats[0]         = rtvFormat;
        pso.SampleDesc.Count      = 1;

        return SUCCEEDED(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_compositePSO)));
    }

    static bool EnsureCompositePipeline(DXGI_FORMAT rtvFormat)
    {
        if (g_compositePSO && rtvFormat == g_compositeFormat)
            return true;
        if (!CreateCompositePipeline(rtvFormat))
            return false;
        g_compositeFormat = rtvFormat;
        return true;
    }

    // Block until our last overlay submit has retired so it is safe to release
    // the resources it referenced. Cheap when nothing is in flight.
    static void WaitForOverlayIdle()
    {
        if (g_fence && g_fenceValue && g_fenceEvent &&
            g_fence->GetCompletedValue() < g_fenceValue)
        {
            g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 1000);
        }
    }

    // Rebuild the RTV heap and per-frame command allocators for a new back-buffer
    // count. The caller must have flushed our overlay work first (WaitForOverlayIdle)
    // and must call CreateRenderTargets afterwards to repopulate the views.
    static bool ResizeFrameResources(UINT newCount)
    {
        CleanupRenderTargets();
        for (auto& f : g_frames)
            if (f.commandAllocator) { f.commandAllocator->Release(); f.commandAllocator = nullptr; }
        if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }

        g_bufferCount = newCount;
        g_frames.clear();
        g_frames.resize(newCount);

        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        d.NumDescriptors = newCount;
        d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_rtvHeap))))
        {
            LOG_ERR("ResizeFrameResources: RTV heap (%u) creation failed.", newCount);
            return false;
        }

        for (UINT i = 0; i < newCount; ++i)
        {
            if (FAILED(g_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&g_frames[i].commandAllocator))))
            {
                LOG_ERR("ResizeFrameResources: command allocator %u creation failed.", i);
                return false;
            }
        }
        return true;
    }

    // Keep our render targets pinned to whatever swapchain the game is actually
    // presenting. Streamline's DLSS Frame Generation destroys and recreates the
    // native swapchain when FG is toggled - a new pointer, and usually a larger
    // back-buffer count - WITHOUT routing through ResizeBuffers. Left unhandled,
    // our cached RTVs point at freed buffers and GetCurrentBackBufferIndex() can
    // run past g_frames => device removed. Detect a change by the full back-buffer
    // signature (identity, count, size, format) and rebuild in place. Returns false
    // (skip this frame) if the rebuild fails.
    static bool ReconcileSwapChain(IDXGISwapChain3* swapChain)
    {
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (FAILED(swapChain->GetDesc(&desc)))
            return false;

        const UINT        newW   = desc.BufferDesc.Width;
        const UINT        newH   = desc.BufferDesc.Height;
        const DXGI_FORMAT newFmt = desc.BufferDesc.Format;

        // Compare the FULL back-buffer signature, not just pointer + count. A video
        // settings change (resolution, HDR on/off) reallocates the buffers in place
        // with the same swapchain and often the same count - detecting only
        // pointer/count leaves us drawing into freed buffers (ACCESS_DENIED). This
        // is what crashed on entering / applying game settings.
        if (swapChain == g_swapChain && desc.BufferCount == g_bufferCount &&
            newW == g_scWidth && newH == g_scHeight && newFmt == g_scFormat)
            return true; // unchanged - the common path

        // A different D3D device would invalidate our heaps/fence entirely; that
        // never happens for a DLSS-G toggle (Streamline reuses the device), so
        // rather than attempt a full teardown here we just skip the frame.
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&dev))))
        {
            const bool sameDevice = (dev == g_device);
            dev->Release();
            if (!sameDevice)
            {
                static bool s_warned = false;
                if (!s_warned) { s_warned = true; LOG_ERR("Swapchain device changed - overlay paused this frame."); }
                return false;
            }
        }

        // Our last submit referenced the old buffers/allocators - retire it first.
        WaitForOverlayIdle();

        if (desc.BufferCount != g_bufferCount)
        {
            if (!ResizeFrameResources(desc.BufferCount))
                return false;
        }
        else
        {
            CleanupRenderTargets(); // same count, fresh buffers
        }

        g_hwnd      = desc.OutputWindow;
        g_swapChain = swapChain;
        g_scWidth   = newW;
        g_scHeight  = newH;
        g_scFormat  = newFmt;

        if (!CreateRenderTargets(swapChain))
        {
            LOG_ERR("ReconcileSwapChain: render target rebuild failed.");
            return false;
        }

        // ImGui's own DX12 backend is baked for a FIXED SDR format (see
        // InitImGui) and never needs rebuilding here - only the offscreen
        // target it draws into has to track the back buffer's size.
        if (!CreateOffscreenTarget(g_scWidth, g_scHeight))
        {
            LOG_ERR("ReconcileSwapChain: offscreen target rebuild failed.");
            return false;
        }

        LOG("Swapchain reconciled (%ux%u, %u buffers, fmt %d) - in-place reconfigure.",
            g_scWidth, g_scHeight, g_bufferCount, static_cast<int>(g_scFormat));
        return true;
    }

    static bool InitImGui(IDXGISwapChain3* swapChain)
    {
        // We only get here in the process that actually presents - open the
        // console now and flush the buffered startup logs into it, and claim
        // Trinity.ini so the launcher's copy of the ASI can never save over us.
        ml::Mod::OnRenderProcess();

        if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device))))
        {
            LOG_ERR("InitImGui: swapChain->GetDevice failed.");
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc = {};
        swapChain->GetDesc(&desc);
        g_hwnd        = desc.OutputWindow;
        g_bufferCount = desc.BufferCount;
        g_scWidth     = desc.BufferDesc.Width;
        g_scHeight    = desc.BufferDesc.Height;
        g_scFormat    = desc.BufferDesc.Format;
        g_frames.clear();
        g_frames.resize(g_bufferCount);

        // NOTE: no queue selection here. Queues seen before init may already be
        // destroyed (the engine creates temporary devices/queues during loading),
        // so touching them is a use-after-free. The present queue is captured
        // live in hkExecuteCommandLists once g_device is known; DrawOverlay
        // simply skips frames until that has happened (typically 1 frame).

        // Shader-visible SRV heap: slot 0 is the ImGui font atlas, slots 1+
        // our own icon-atlas textures, then the per-item icon budget.
        {
            D3D12_DESCRIPTOR_HEAP_DESC d = {};
            d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            d.NumDescriptors = kSrvHeapSlots;
            d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_srvHeap))))
            {
                LOG_ERR("InitImGui: SRV heap creation failed.");
                return false;
            }
        }

        // RTV heap, one descriptor per back buffer.
        {
            D3D12_DESCRIPTOR_HEAP_DESC d = {};
            d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            d.NumDescriptors = g_bufferCount;
            d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(g_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_rtvHeap))))
            {
                LOG_ERR("InitImGui: RTV heap creation failed.");
                return false;
            }
        }

        // One command allocator per frame + a single command list.
        for (UINT i = 0; i < g_bufferCount; ++i)
        {
            if (FAILED(g_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&g_frames[i].commandAllocator))))
            {
                LOG_ERR("InitImGui: command allocator %u creation failed.", i);
                return false;
            }
        }

        if (FAILED(g_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                g_frames[0].commandAllocator, nullptr,
                IID_PPV_ARGS(&g_commandList))))
        {
            LOG_ERR("InitImGui: command list creation failed.");
            return false;
        }
        g_commandList->Close();
        // Name it so DRED (DumpDred) can tell a fault in OUR overlay submit apart
        // from a Streamline / engine command list.
        g_commandList->SetName(L"MasterLooterOverlayCmdList");

        // Fence so we never reset an allocator the GPU is still using.
        if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
        {
            LOG_ERR("InitImGui: fence creation failed.");
            return false;
        }
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_fenceEvent)
        {
            LOG_ERR("InitImGui: fence event creation failed.");
            return false;
        }

        if (!CreateRenderTargets(swapChain))
        {
            LOG_ERR("InitImGui: render target creation failed.");
            return false;
        }

        if (!CreateOffscreenTarget(g_scWidth, g_scHeight))
        {
            LOG_ERR("InitImGui: offscreen target creation failed.");
            return false;
        }

        // ImGui.
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; // don't litter the game folder
        // ImGui's recoverable-error path draws a tooltip, and a tooltip is a
        // window, so an error raised outside a frame (a font file that is
        // not there, in InitStyle below) is a Begin() with no frame and a
        // null read. Log the error and nothing else.
        io.ConfigErrorRecoveryEnableTooltip = false;
        io.ConfigErrorRecoveryEnableAssert  = false;

        gui::InitStyle(static_cast<float>(desc.BufferDesc.Height) / 1080.0f);

        ImGui_ImplWin32_Init(g_hwnd);
        // Always a fixed SDR format, independent of the real back buffer's
        // format/color space - see the "HDR-aware overlay compositing" comment
        // above. The composite pass (EnsureCompositePipeline) is what actually
        // targets g_scFormat.
        ImGui_ImplDX12_Init(
            g_device, g_bufferCount, DXGI_FORMAT_R8G8B8A8_UNORM, g_srvHeap,
            g_srvHeap->GetCPUDescriptorHandleForHeapStart(),
            g_srvHeap->GetGPUDescriptorHandleForHeapStart());


        input::Init(g_hwnd);

        g_swapChain = swapChain; // baseline for ReconcileSwapChain

        LOG_OK("Overlay ready - %ux%u, %u back buffers.",
               desc.BufferDesc.Width, desc.BufferDesc.Height, g_bufferCount);
        return true;
    }

    // --- Hooks --------------------------------------------------------------
    // Adopt q as the queue we submit overlay work on. authoritative=true is for the
    // queue passed to CreateSwapChainForHwnd - for D3D12 that first argument IS the
    // swapchain's present queue, the one that owns the back buffers - which we pin so
    // the ExecuteCommandLists heuristic can never later swap in Streamline's MFG
    // present-pacer queue (submitting there is rejected -> device removed).
    static void PublishPresentQueue(ID3D12CommandQueue* q, bool authoritative)
    {
        if (!q || !g_queueLockReady) return;
        EnterCriticalSection(&g_queueLock);
        if (authoritative || !g_presentQueuePinned)
        {
            if (q != g_presentQueue)
            {
                q->AddRef();
                if (g_presentQueue) g_presentQueue->Release();
                g_presentQueue = q;
            }
            if (authoritative && !g_presentQueuePinned)
            {
                g_presentQueuePinned = true;
                LOG("Present queue pinned from swapchain creation - owns the back buffers.");
            }
        }
        LeaveCriticalSection(&g_queueLock);
    }

    static void WINAPI hkExecuteCommandLists(
        ID3D12CommandQueue* queue, UINT numLists, ID3D12CommandList* const* lists)
    {
        // Fallback queue discovery ONLY until the authoritative present queue is
        // pinned from swapchain creation. Once pinned, never touch it here - under
        // MFG the busiest DIRECT queue is Streamline's pacer, not ours.
        if (!g_presentQueuePinned && g_queueLockReady && g_device && queue &&
            queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            EnterCriticalSection(&g_queueLock);
            if (queue != g_presentQueue)
            {
                ID3D12Device* d = nullptr;
                if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&d))))
                {
                    if (d == g_device)
                    {
                        // New most-recent DIRECT queue on our device: take a ref,
                        // drop the ref on the previous one.
                        queue->AddRef();
                        if (g_presentQueue)
                            g_presentQueue->Release();
                        g_presentQueue = queue;

                        static bool s_logged = false;
                        if (!s_logged)
                        {
                            s_logged = true;
                            LOG("Present queue captured (heuristic - creation queue not yet seen).");
                        }
                    }
                    d->Release();
                }
            }
            LeaveCriticalSection(&g_queueLock);
        }

        oExecuteCommandLists(queue, numLists, lists);
    }

    // Records + submits the overlay. Kept as its own function so hkPresent can
    // wrap the call in __try/__except without object-unwinding conflicts.
    static void DrawOverlay(IDXGISwapChain3* swapChain)
    {
        // Pin our render targets to the live swapchain before touching them.
        // A DLSS Frame Generation toggle silently swaps it; if the rebuild
        // fails, sit this frame out rather than draw into freed buffers.
        if (!ReconcileSwapChain(swapChain))
            return;

        if (!EnsureCompositePipeline(g_scFormat))
            return;

        // Hold our own ref on the present queue for the duration of this frame
        // so a concurrent republish can't pull it out from under us. Not
        // captured yet? Skip - it arrives within a frame via
        // hkExecuteCommandLists.
        ID3D12CommandQueue* submitQueue = nullptr;
        EnterCriticalSection(&g_queueLock);
        if (g_presentQueue)
        {
            submitQueue = g_presentQueue;
            submitQueue->AddRef();
        }
        LeaveCriticalSection(&g_queueLock);
        if (!submitQueue)
            return;

        // A language change needs glyphs the atlas does not hold. Rebuild
        // between frames, and only once our last submit has retired: the
        // backend releases the font texture to recreate it and the GPU may
        // still be reading the old one. The NewFrame below builds both again.
        if (gui::FontsNeedRebuild())
        {
            WaitForOverlayIdle();
            ImGui_ImplDX12_InvalidateDeviceObjects();
            gui::RebuildFonts();
        }

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        gui::Render();

        ImGui::Render();

        const UINT idx = swapChain->GetCurrentBackBufferIndex();
        if (idx >= g_frames.size() || !g_frames[idx].renderTarget)
        {
            // Live index outran our (just-reconciled) frame array - never index
            // out of bounds. Should not happen after ReconcileSwapChain, but a
            // freed device is worse than a dropped overlay frame.
            submitQueue->Release();
            return;
        }
        FrameContext& frame = g_frames[idx];

        if (frame.fenceValue != 0 && g_fence->GetCompletedValue() < frame.fenceValue)
        {
            g_fence->SetEventOnCompletion(frame.fenceValue, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 1000);
        }

        frame.commandAllocator->Reset();
        g_commandList->Reset(frame.commandAllocator, nullptr);

        // --- Pass 1: ImGui draws into the offscreen SDR target -------------
        D3D12_RESOURCE_BARRIER offscreenBarrier = {};
        offscreenBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        offscreenBarrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        offscreenBarrier.Transition.pResource   = g_offscreenTex;
        offscreenBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        offscreenBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        offscreenBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_commandList->ResourceBarrier(1, &offscreenBarrier);

        const float transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_commandList->OMSetRenderTargets(1, &g_offscreenRtv, FALSE, nullptr);
        g_commandList->ClearRenderTargetView(g_offscreenRtv, transparent, 0, nullptr);
        g_commandList->SetDescriptorHeaps(1, &g_srvHeap);


        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList);

        offscreenBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        offscreenBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_commandList->ResourceBarrier(1, &offscreenBarrier);

        // --- Pass 2: composite the offscreen image onto the real back buffer,
        // re-encoding for its actual color space (see CreateCompositePipeline).
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource   = frame.renderTarget;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_commandList->ResourceBarrier(1, &barrier);
        g_commandList->OMSetRenderTargets(1, &frame.rtvHandle, FALSE, nullptr);

        const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(g_scWidth), static_cast<float>(g_scHeight), 0.0f, 1.0f };
        const D3D12_RECT     scissor  = { 0, 0, static_cast<LONG>(g_scWidth), static_cast<LONG>(g_scHeight) };
        g_commandList->RSSetViewports(1, &viewport);
        g_commandList->RSSetScissorRects(1, &scissor);

        g_commandList->SetGraphicsRootSignature(g_compositeRootSig);
        g_commandList->SetPipelineState(g_compositePSO);
        g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const struct { UINT mode; float paperWhiteNits; float pad[2]; } params =
            { CompositeModeForColorSpace(g_colorSpace), kPaperWhiteNits, { 0.0f, 0.0f } };
        g_commandList->SetGraphicsRoot32BitConstants(0, 4, &params, 0);

        D3D12_GPU_DESCRIPTOR_HANDLE offscreenSrvGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        offscreenSrvGpu.ptr += static_cast<UINT64>(kOffscreenSrvSlot) *
            g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_commandList->SetGraphicsRootDescriptorTable(1, offscreenSrvGpu);

        g_commandList->DrawInstanced(3, 1, 0, 0);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(1, &barrier);

        if (FAILED(g_commandList->Close()))
        {
            submitQueue->Release();
            return;
        }

        ID3D12CommandList* toExec[] = { g_commandList };
        submitQueue->ExecuteCommandLists(1, toExec);
        submitQueue->Signal(g_fence, ++g_fenceValue);
        frame.fenceValue = g_fenceValue;
        submitQueue->Release();
    }

    // Init-if-needed and draw the overlay into swapChain's current back buffer.
    // Returns whether a frame was actually submitted (for the post-present device
    // check). isGameFacing is false for native presents while a wrapper owns the
    // drawing - those are Streamline's read-only finished frames, left untouched.
    static bool RenderOverlay(IDXGISwapChain3* swapChain, bool isGameFacing)
    {
        // Counted before anything can refuse: every path that could draw comes
        // through here, so this is the one number that says whether this mod is
        // on the screen at all. OverlayBlindWatch reads it.
        g_presentSeen.fetch_add(1, std::memory_order_relaxed);

        if (!isGameFacing)
            return false;

#ifdef ML_NO_DRAW
        // Test build. The swapchain is wrapped exactly as normal and then left
        // completely alone: no ImGui init, no render targets, no descriptor
        // heaps, no offscreen target, nothing submitted. Not one reference is
        // taken on a back buffer.
        //
        // This is the only placement that answers the question. Wrapping does
        // two separable things: it puts our proxy where the game expects its
        // own swapchain, and it points our render target views at that chain's
        // back buffers. The first version of this test returned after InitImGui
        // had already run, so the render targets existed anyway and it would
        // have proved nothing.
        //
        // Survives a frame generation toggle: the resources are the problem,
        // and releasing them at the right moment keeps the overlay. Still dies:
        // the proxy itself is the problem and the wrapper cannot stay.
        {
            static bool s_said = false;
            if (!s_said)
            {
                s_said = true;
                LOG("*** TEST BUILD: swapchain wrapped, nothing drawn and no GPU resources taken. ***");
                LOG("    The menu will never appear. That is the point of the build.");
            }
        }
        return false;
#endif

        if (!g_imguiReady && !g_initFailed)
        {
            // Init needs only the swapchain (device comes from it). The present
            // queue is captured separately by hkExecuteCommandLists.
            // Said before the attempt, not after: a Proton log for issue #15
            // ended at the same second the first frame was due, with no fault
            // line, and nothing recorded whether this had started.
            LOG("[overlay] first frame through %s; initialising the menu renderer", g_wrapperActive ? "the wrapper" : "the stacked detour");
            if (InitImGui(swapChain))
                g_imguiReady = true;
            else
            {
                g_initFailed = true;
                LOG_ERR("Overlay init failed - Master Looter's menu is disabled for this session.");
            }
        }

        if (!g_imguiReady || g_renderDisabled)
            return false;

        // Hook the game's XInput module once it has loaded (no-op thereafter) so
        // controller input is blocked from the game while the menu is up.
        hooks::EnsureXInputHooks();

        gui::PollToggle();

        if (!gui::WantsDraw())
        {
            ImGui::GetIO().MouseDrawCursor = false;
            return false;
        }

        bool drew = false;
        __try
        {
            DrawOverlay(swapChain);
            drew = true;
            // Proof the render path is alive. State::Captures reads this so an
            // overlay that stops drawing releases the pad instead of holding it
            // for the rest of the session.
            State::Get().lastDrawAt = GetTickCount();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERR("Overlay crashed (exception 0x%08X) - overlay disabled, game continues.",
                    GetExceptionCode());
            g_renderDisabled = true;
        }
        return drew;
    }

    // On a device removal, dump DRED (Device Removed Extended Data): the GPU
    // breadcrumb trail (which queue/command list was mid-execution) and the page
    // fault (the faulting virtual address + the resource that owned it, if named).
    // This is what turns an opaque ACCESS_DENIED under DLSS-G Frame Generation into
    // "OUR command list faulted writing back buffer X" vs "a Streamline-owned
    // resource" - the difference decides the real fix. Requires DRED to have been
    // armed before device creation (EnableDredIfAvailable, from InstallDX12Hooks).
    static void DumpDred()
    {
        if (!g_device) return;

        ID3D12DeviceRemovedExtendedData* dred = nullptr;
        if (FAILED(g_device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred)
        {
            LOG_ERR("DRED: interface unavailable on this device - fault cause not captured.");
            return;
        }

        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc = {};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)) && bc.pHeadAutoBreadcrumbNode)
        {
            int n = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE* node = bc.pHeadAutoBreadcrumbNode;
                 node && n < 8; node = node->pNext, ++n)
            {
                const UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                LOG_ERR("DRED breadcrumb[%d]: queue='%s' list='%s' completed %u/%u ops",
                        n,
                        node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "?",
                        node->pCommandListDebugNameA  ? node->pCommandListDebugNameA  : "?",
                        last, node->BreadcrumbCount);
            }
        }
        else
        {
            // No GPU breadcrumbs => the removal was NOT a GPU-side command failure.
            // Points at a DXGI/driver-level rejection (e.g. writing a read-only
            // shared back buffer during an FG swapchain reconfigure), not our submit.
            LOG_ERR("DRED: no GPU breadcrumbs - removal was not a GPU command fault.");
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA)
        {
            LOG_ERR("DRED page fault VA=0x%llx", static_cast<unsigned long long>(pf.PageFaultVA));
            for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a; a = a->pNext)
                LOG_ERR("  live alloc at fault: '%s'", a->ObjectNameA ? a->ObjectNameA : "?");
            for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a; a = a->pNext)
                LOG_ERR("  recently-freed alloc: '%s'", a->ObjectNameA ? a->ObjectNameA : "?");
        }
        else
        {
            LOG_ERR("DRED: no page fault recorded - not a bad-address access.");
        }

        dred->Release();
    }

    // Fail safe: if our overlay work removed the device, stop rendering instead
    // of spamming a dead GPU.
    static void PostPresentDeviceCheck(bool drew)
    {
        if (drew && g_device)
        {
            const HRESULT removed = g_device->GetDeviceRemovedReason();
            if (removed != S_OK)
            {
                LOG_ERR("Device removed (0x%08X) after overlay submit - overlay disabled.", removed);
                DumpDred();
                g_renderDisabled = true;
            }
        }
    }

    // Shared ResizeBuffers handling: retire our work + drop views, let the real
    // resize happen (caller), then rebuild from the post-resize truth.
    static void PreResizeCleanup()
    {
        if (!g_imguiReady) return;
        WaitForOverlayIdle();
        CleanupRenderTargets();
    }
    static void PostResizeRebuild(IDXGISwapChain3* swapChain)
    {
        if (!g_imguiReady) return;
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(swapChain->GetDesc(&desc)))
        {
            if (desc.BufferCount != g_bufferCount)
                ResizeFrameResources(desc.BufferCount);
            g_hwnd      = desc.OutputWindow;
            g_swapChain = swapChain;
            g_scWidth   = desc.BufferDesc.Width;
            g_scHeight  = desc.BufferDesc.Height;
            g_scFormat  = desc.BufferDesc.Format;
        }
        CreateRenderTargets(swapChain);
        // Must follow the g_scWidth/g_scHeight update above (and precede the
        // next ReconcileSwapChain, which would otherwise see the new signature
        // as "unchanged" and never resize the offscreen target to match).
        CreateOffscreenTarget(g_scWidth, g_scHeight);
    }

    // Native-swapchain Present/ResizeBuffers byte-detours (from the dummy vtable).
    // These are the drawing path ONLY when no wrapper is active (wrapping failed,
    // or a legacy no-proxy path). Once a wrapper owns drawing they forward
    // untouched - under DLSS-G the native present is Streamline's read-only frame.
    static HRESULT WINAPI hkPresent(IDXGISwapChain3* swapChain, UINT syncInterval, UINT flags)
    {
        const bool drew = RenderOverlay(swapChain, !g_wrapperActive);
        const HRESULT hr = oPresent(swapChain, syncInterval, flags);
        PostPresentDeviceCheck(drew);
        return hr;
    }

    typedef HRESULT (WINAPI* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    static Present1_t oPresent1 = nullptr;
    static HRESULT WINAPI hkPresent1(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
    {
        // Only reached on the stacked path (see the factory hook), where the
        // chain is the game's own IDXGISwapChain4 and the cast holds.
        const bool drew = RenderOverlay(static_cast<IDXGISwapChain3*>(swapChain), !g_wrapperActive);
        const HRESULT hr = oPresent1(swapChain, syncInterval, flags, pp);
        PostPresentDeviceCheck(drew);
        return hr;
    }
    static HRESULT WINAPI hkResizeBuffers(
        IDXGISwapChain3* swapChain, UINT bufferCount,
        UINT width, UINT height, DXGI_FORMAT format, UINT flags)
    {
        if (!g_imguiReady || g_wrapperActive)
            return oResizeBuffers(swapChain, bufferCount, width, height, format, flags);
        PreResizeCleanup();
        const HRESULT hr = oResizeBuffers(swapChain, bufferCount, width, height, format, flags);
        PostResizeRebuild(swapChain);
        LOG("[hook] ResizeBuffers through the stacked detour: %ux%u, %u buffers, fmt %d, hr 0x%08X; views released before and rebuilt after",
            width, height, bufferCount, static_cast<int>(format), static_cast<unsigned>(hr));
        return hr;
    }

    // The wrapper overrides ResizeBuffers1 as well as ResizeBuffers, and a
    // D3D12 game that passes a queue per buffer reconfigures through this
    // one. The stacked path had only the plain detour, so a reconfigure that
    // kept the size and count left the views on freed buffers.
    typedef HRESULT (WINAPI* ResizeBuffers1_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
    static ResizeBuffers1_t oResizeBuffers1 = nullptr;
    static HRESULT WINAPI hkResizeBuffers1(
        IDXGISwapChain3* swapChain, UINT bufferCount,
        UINT width, UINT height, DXGI_FORMAT format, UINT flags,
        const UINT* nodeMask, IUnknown* const* queues)
    {
        if (!g_imguiReady || g_wrapperActive)
            return oResizeBuffers1(swapChain, bufferCount, width, height, format, flags, nodeMask, queues);
        PreResizeCleanup();
        const HRESULT hr = oResizeBuffers1(swapChain, bufferCount, width, height, format, flags, nodeMask, queues);
        PostResizeRebuild(swapChain);
        LOG("[hook] ResizeBuffers1 through the stacked detour: %ux%u, %u buffers, fmt %d, hr 0x%08X; views released before and rebuilt after",
            width, height, bufferCount, static_cast<int>(format), static_cast<unsigned>(hr));
        return hr;
    }

    // Only reachable when wrapping failed (see g_wrapperActive) - the wrapper's
    // own SetColorSpace1 override covers the normal case.
    static HRESULT WINAPI hkSetColorSpace1(IDXGISwapChain3* swapChain, DXGI_COLOR_SPACE_TYPE colorSpace)
    {
        const HRESULT hr = oSetColorSpace1(swapChain, colorSpace);
        if (SUCCEEDED(hr)) OnColorSpaceChanged(colorSpace);
        return hr;
    }

    // Log which module an address lives in - tells us from a live test whether the
    // factory we patched is the DLSS-G proxy (sl.*/nvngx*) or native (dxgi.dll).
    // Which DLL owns an address. On a clean install every one of these is a
    // system DLL; anything else means another mod is already on that code, and
    // that is the first thing to know when a launch crash is reported.
    static const char* OwningModule(void* addr, char* out, size_t n)
    {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &m);
        char path[MAX_PATH] = "?";
        if (m) GetModuleFileNameA(m, path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        strncpy(out, base ? base + 1 : path, n - 1);
        out[n - 1] = 0;
        return out;
    }

    // Forward declared: this wants to follow a detour, and the decoder is
    // defined below with the rest of the byte reading.
    static void DescribeDetour(const uint8_t* p, const void* at, char* out, size_t n);

    static void LogHookedModule(const char* what, void* addr)
    {
        char mod[64];
        OwningModule(addr, mod, sizeof mod);
        // The swapchain's own Present can be detoured too, and when it is, the
        // owning module is still the system DLL. Following the jump is the only
        // thing that names what is really in front of us.
        char into[96] = "";
        if (addr && !IsBadReadPtr(addr, 16))
        {
            uint8_t b[16];
            memcpy(b, addr, sizeof b);
            DescribeDetour(b, addr, into, sizeof into);
        }
        LOG("%s @ %p in %s%s", what, addr, mod, into);
    }

    // The DLLs these addresses belong to before anyone touches them. Anything
    // else and a second mod got there first, which is worth saying out loud.
    static bool IsSystemOwner(const char* mod)
    {
        return _stricmp(mod, "dxgi.dll") == 0 || _stricmp(mod, "d3d12.dll") == 0 ||
               _stricmp(mod, "d3d12core.dll") == 0 || _stricmp(mod, "dxgi.DLL") == 0;
    }

    // Whether the module that owns an address actually lives in the Windows
    // system directory, as against merely being called what a system DLL is
    // called.
    //
    // ReShade installs itself as dxgi.dll in the game folder, and so do other
    // overlays. Judged by name alone that proxy passes as the system library,
    // and this mod then writes an inline detour into another mod's code. That
    // was the whole of the hang reproduced for issue 34: with the swapchain
    // wrapper off the one hook still installed was ExecuteCommandLists, and it
    // had resolved to "dxgi.dll", which on that machine was ReShade's proxy in
    // bin64. On a clean machine the same function resolves to D3D12Core.dll.
    static bool ModuleInSystemDir(void* addr)
    {
        HMODULE m = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(addr), &m) || !m)
            return false;
        char path[MAX_PATH] = "";
        if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;
        char sys[MAX_PATH] = "";
        const UINT n = GetSystemDirectoryA(sys, MAX_PATH);
        if (!n || n >= MAX_PATH) return false;
        return _strnicmp(path, sys, n) == 0 && (path[n] == '\\' || path[n] == '/');
    }

    // The same question answered by where the module lives, which is the one
    // that cannot be fooled. ReShade installs itself as dxgi.dll beside the
    // game,

    // A function whose first bytes are already a jump has been detoured in
    // place by someone else. The owning module stays the system DLL in that
    // case, so the module name alone cannot see it; only the bytes can.
    // Where that jump actually lands, so the log can name the mod that got
    // there first instead of only saying somebody did.
    //
    // Three of the open reports are some form of "it will not load alongside
    // these other mods", and every one of them arrived as a log saying Present
    // was already detoured, with no way to tell whether that was RenoDX,
    // ReShade, OptiScaler or a transmog mod without asking the reporter to
    // list their load order by hand.
    //
    // push/ret is left alone deliberately: its immediate is 32 bits, which
    // cannot address a module on x64, so resolving it would print a number
    // that means nothing.
    static bool DetourTarget(const uint8_t* p, const void* at, void** out)
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(at);
        if (p[0] == 0xE9)
        {
            int32_t rel = 0; memcpy(&rel, p + 1, sizeof rel);
            *out = reinterpret_cast<void*>(a + 5 + static_cast<intptr_t>(rel));
            return true;
        }
        if (p[0] == 0xEB)
        {
            *out = reinterpret_cast<void*>(a + 2 + static_cast<intptr_t>(static_cast<int8_t>(p[1])));
            return true;
        }
        if (p[0] == 0xFF && p[1] == 0x25)
        {
            // The displacement names a slot holding the address, not the
            // address, so this one needs a read that may fail.
            int32_t disp = 0; memcpy(&disp, p + 2, sizeof disp);
            void* slot = reinterpret_cast<void*>(a + 6 + static_cast<intptr_t>(disp));
            if (IsBadReadPtr(slot, sizeof(void*))) return false;
            memcpy(out, slot, sizeof(void*));
            return true;
        }
        if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0)
        {
            memcpy(out, p + 2, sizeof(void*));
            return true;
        }
        return false;
    }

    // ", into RenoDX.asi at 00007FF..." or empty when it cannot be followed.
    //
    // Up to four hops, because some hook libraries land the first jump on a
    // stub in the system DLL's own padding and only then leave for the module
    // that owns the hook. Stopping at hop one would name dxgi.dll, which is
    // the answer we already had. Anything that lands outside a system DLL is
    // the answer, so the walk stops there.
    static void DescribeDetour(const uint8_t* p, const void* at, char* out, size_t n)
    {
        out[0] = 0;
        void* dest = nullptr;
        if (!DetourTarget(p, at, &dest) || !dest) return;

        char owner[64];
        OwningModule(dest, owner, sizeof owner);
        // Keep walking while the destination is still not an answer.
        //
        // Two kinds of non-answer. A system DLL means we landed on a stub in
        // dxgi's own padding. A "?" means no loaded module owns the address at
        // all, which is a relay page the other mod allocated for itself, and
        // that is the common case: the first build of this reported "into ? at
        // 00007FFB007B04CA" for all three hooked functions, which is the same
        // amount of information as before.
        //
        // A relay is usually one more jump, often the rip-relative indirect
        // form, and that one does land in the module that owns the hook. Six
        // hops because a chain of two or three is normal and the cost of one
        // more is a read.
        for (int hop = 0; hop < 6 && (IsSystemOwner(owner) || owner[0] == '?'); ++hop)
        {
            if (IsBadReadPtr(dest, 16)) break;
            uint8_t b[16];
            memcpy(b, dest, sizeof b);
            void* next = nullptr;
            if (!DetourTarget(b, dest, &next) || !next || next == dest) break;
            dest = next;
            OwningModule(dest, owner, sizeof owner);
        }
        // Ending on "?" is still worth saying, but say what it means. An
        // address in no module is code somebody allocated, not a mystery.
        if (owner[0] == '?')
            snprintf(out, n, ", into an allocated stub at %p", dest);
        else
            snprintf(out, n, ", into %s at %p", owner, dest);
    }

    static const char* ExistingDetour(const uint8_t* p)
    {
        if (p[0] == 0xE9) return "jmp rel32";
        if (p[0] == 0xFF && p[1] == 0x25) return "jmp [rip+disp]";
        if (p[0] == 0xEB) return "jmp short";
        if (p[0] == 0x68 && p[5] == 0xC3) return "push/ret";
        if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0) return "mov rax, imm64; jmp rax";
        return nullptr;
    }

    // Returns false when someone else has already detoured this function, in
    // which case stacking a second detour on top is what we must not do.
    // Which module's code an existing detour on fn lands in, following relay
    // stubs the way DescribeDetour does. False when fn is not detoured.
    static bool DetourOwner(void* fn, char* out, size_t n)
    {
        if (!fn || IsBadReadPtr(fn, 16)) return false;
        uint8_t b[16];
        memcpy(b, fn, sizeof b);
        if (!ExistingDetour(b)) return false;
        void* dest = nullptr;
        if (!DetourTarget(b, fn, &dest) || !dest) { strncpy(out, "?", n); out[n - 1] = 0; return true; }
        OwningModule(dest, out, n);
        for (int hop = 0; hop < 6 && (IsSystemOwner(out) || out[0] == '?'); ++hop)
        {
            if (IsBadReadPtr(dest, 16)) break;
            uint8_t c[16];
            memcpy(c, dest, sizeof c);
            void* next = nullptr;
            if (!DetourTarget(c, dest, &next) || !next) break;
            dest = next;
            OwningModule(dest, out, n);
        }
        return true;
    }

    // Detours on Present that the wrapper has always lived alongside. Steam's
    // overlay draws and keys nothing; the frame generation interposer is the
    // proxy the wrapper was built to sit under. Anything else that got to
    // Present first is assumed to key its own work on the swapchain object it
    // was handed, which Crimson Route does, and such a mod must not be handed
    // the wrapper.
    static bool BenignPresentOwner(const char* mod)
    {
        if (!mod || !mod[0] || mod[0] == '?') return false;
        if (IsSystemOwner(mod)) return true;
        if (_stricmp(mod, "gameoverlayrenderer64.dll") == 0) return true;
        if (_stricmp(mod, "MasterLooter.asi") == 0) return true;
        if (_strnicmp(mod, "sl.", 3) == 0 || _strnicmp(mod, "nvngx", 5) == 0) return true;
        return false;
    }

    // A detour's prologue names only the mod that detoured Present last. One
    // that keys the swapchain but hooked before Steam's overlay is invisible
    // that way, so the mods known to key it are also checked by name.
    static const char* KeyingOverlayLoaded()
    {
        static const char* const kMods[] = { "CrimsonRoute.asi" };
        for (const char* m : kMods) if (GetModuleHandleA(m)) return m;
        return nullptr;
    }

    static bool LogHookTarget(const char* what, void* addr, bool stack = false, bool allowProxy = false)
    {
        char mod[64];
        OwningModule(addr, mod, sizeof mod);

        // First bytes, so a detour that was already there is on the record even
        // when this mod goes on to hook over the top of it.
        uint8_t b[16] = {};
        char bytes[64] = "";
        const char* detour = nullptr;
        if (addr && !IsBadReadPtr(addr, sizeof b))
        {
            memcpy(b, addr, sizeof b);
            int w = 0;
            for (int i = 0; i < 8; ++i) w += snprintf(bytes + w, sizeof bytes - w, "%02X ", b[i]);
            detour = ExistingDetour(b);
        }

        if (detour)
        {
            char into[96];
            DescribeDetour(b, addr, into, sizeof into);
            if (stack)
            {
                // Stacking is what every overlay does and what this mod
                // avoided; here it is the only way to draw without handing
                // another mod the wrapper. Ours runs first and calls theirs.
                LOG("[hook] %s @ %p in %s starts with %s (%s)%s: another mod detoured it first; stacking this mod's detour on top, so ours runs and then theirs",
                    what, addr, mod, detour, bytes, into);
            }
            else
            {
                LOG("[hook] %s @ %p in %s starts with %s (%s)%s: another mod detoured it first, so this one is left alone",
                    what, addr, mod, detour, bytes, into);
                return false;
            }
        }
        if (!IsSystemOwner(mod))
        {
            // A proxy does not have to be named after the library it replaces.
            // The stacking allowance below was written for OptiScaler, DLSS
            // Enabler and ReShade installed as dxgi.dll, and it sits behind this
            // test, which knows three names. susemi325's 1.6.14 log on 12
            // September 2026 has OptiScaler loaded as VERSION.dll with Present
            // and Present1 both inside it, so the call was refused here, a check
            // earlier than the one the retry relaxes. The retry ran, said it was
            // trying with a proxy allowed, and changed nothing: three mods deep
            // and the menu still dark, with the reason sitting in the log in
            // plain words that named the wrong rule.
            //
            // allowProxy is set for the present calls and nothing else, so
            // ExecuteCommandLists is still refused by name here, which is where
            // issue 34 was earned.
            if (!allowProxy)
            {
                LOG("[hook] %s @ %p in %s (%s), which is not a system DLL: another mod owns it, so this one is left alone", what, addr, mod, bytes);
                return false;
            }
            LOG("[hook] %s @ %p in %s (%s) belongs to another mod outright, and it owns every present call there is: "
                "stacking on it, because the alternative is a menu that never draws", what, addr, mod, bytes);
            return true;
        }
        // Right name, wrong place. A dxgi.dll that lives beside the game is a
        // proxy some other mod installed, and detouring it is detouring them.
        // D3D12Core.dll is the Agility SDK runtime, which a game ships in its
        // own folder on purpose, so the game-folder rule does not apply to it.
        // The detour check above has already ruled out another mod's jump on
        // it. Judged by place alone, ExecuteCommandLists was refused on
        // LuxDragon's machine and the queue had no fallback.
        if (!ModuleInSystemDir(addr) && _stricmp(mod, "d3d12core.dll") != 0)
        {
            // Unless there is nothing else left. OptiScaler, DLSS Enabler and
            // ReShade replace dxgi.dll outright, so the game's swapchain is
            // their object and every present call in its vtable is their code.
            // Refusing all of them leaves this mod with no way to draw at all,
            // which is what happened here on 11 September 2026: an OptiScaler
            // dxgi.dll went in beside the game, Crimson Route was loaded so the
            // wrapper was off, and the menu went silent on a plugin that
            // otherwise loaded and looted. Stacking on a proxy's present is the
            // same move this mod already makes on Steam's overlay: ours runs,
            // then theirs, then the real library. Issue 34's refusal stands
            // where it was earned, on anything that is not a present call.
            if (!allowProxy)
            {
                LOG("[hook] %s @ %p in %s (%s), a module named like a system DLL but living in the game folder, "
                    "so it is another mod's proxy: left alone", what, addr, mod, bytes);
                return false;
            }
            LOG("[hook] %s @ %p in %s (%s) is another mod's proxy in the game folder, and it owns every present "
                "call there is: stacking on it, because the alternative is a menu that never draws", what, addr, mod, bytes);
            return true;
        }
        LOG("[hook] %s @ %p in %s (%s)", what, addr, mod, bytes);
        return true;
    }

    // --- COM wrapper: draw the overlay before Streamline interpolates --------
    // Forwards every IDXGISwapChain4 method to the real (Streamline-proxy)
    // swapchain, intercepting Present/Present1 to draw the overlay into the
    // writable game-facing back buffer first, and ResizeBuffers* to rebuild RTVs.
    class WrappedIDXGISwapChain final : public IDXGISwapChain4
    {
    public:
        explicit WrappedIDXGISwapChain(IDXGISwapChain4* inner) : m_inner(inner)
        {
            InterlockedIncrement(&g_liveWrappers);
        }

        // IUnknown ----------------------------------------------------------
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == __uuidof(IUnknown)               ||
                riid == __uuidof(IDXGIObject)            ||
                riid == __uuidof(IDXGIDeviceSubObject)   ||
                riid == __uuidof(IDXGISwapChain)         ||
                riid == __uuidof(IDXGISwapChain1)        ||
                riid == __uuidof(IDXGISwapChain2)        ||
                riid == __uuidof(IDXGISwapChain3)        ||
                riid == __uuidof(IDXGISwapChain4))
            {
                AddRef();
                *ppv = static_cast<IDXGISwapChain4*>(this);
                return S_OK;
            }
            return m_inner->QueryInterface(riid, ppv);
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG r = InterlockedDecrement(&m_ref);
            if (r == 0)
            {
                IDXGISwapChain4* inner = m_inner;
                const bool ours = (g_swapChain == static_cast<IDXGISwapChain3*>(inner));

                // Let go of the back buffers before the swapchain goes.
                //
                // This is the release the wrapper never did. CreateRenderTargets
                // calls GetBuffer for every back buffer and keeps the resources,
                // so while our views exist the swapchain has outstanding
                // references to its own buffers. The game reaching zero on its
                // reference does not end that. Every other place that touches the
                // buffers retires the GPU first and drops the views:
                // ReconcileSwapChain does it, PreResizeCleanup does it. Teardown
                // was the one path that did neither, and teardown is where the
                // game dies when frame generation is toggled.
                //
                // Only when this wrapper owns the chain the views describe. A
                // second wrapper being released must not tear down the live one's
                // resources.
                if (ours)
                {
                    WaitForOverlayIdle();     // our last submit may still be in flight
                    CleanupRenderTargets();
                    ReleaseOffscreenTarget();
                    g_swapChain = nullptr;    // and forget the identity: the
                                              // allocator can hand this address to
                                              // the next chain, and ReconcileSwapChain
                                              // compares by pointer first, so a
                                              // recycled one would read as unchanged.
                }
                InterlockedDecrement(&g_liveWrappers);
                delete this;
                inner->Release();
            }
            return r;
        }

        // IDXGIObject -------------------------------------------------------
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override { return m_inner->SetPrivateData(n, s, d); }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* p) override { return m_inner->SetPrivateDataInterface(n, p); }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override { return m_inner->GetPrivateData(n, s, d); }
        HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override { return m_inner->GetParent(riid, pp); }

        // IDXGIDeviceSubObject ---------------------------------------------
        HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** pp) override { return m_inner->GetDevice(riid, pp); }

        // IDXGISwapChain ----------------------------------------------------
        HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override
        {
            const bool drew = RenderOverlay(m_inner, true);
            const HRESULT hr = m_inner->Present(syncInterval, flags);
            PostPresentDeviceCheck(drew);
            return hr;
        }
        HRESULT STDMETHODCALLTYPE GetBuffer(UINT i, REFIID riid, void** pp) override { return m_inner->GetBuffer(i, riid, pp); }
        HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fs, IDXGIOutput* t) override { return m_inner->SetFullscreenState(fs, t); }
        HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fs, IDXGIOutput** t) override { return m_inner->GetFullscreenState(fs, t); }
        HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override { return m_inner->GetDesc(d); }
        HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT bc, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override
        {
            PreResizeCleanup();
            const HRESULT hr = m_inner->ResizeBuffers(bc, w, h, f, fl);
            PostResizeRebuild(m_inner);
            return hr;
        }
        HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* p) override { return m_inner->ResizeTarget(p); }
        HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** pp) override { return m_inner->GetContainingOutput(pp); }
        HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* p) override { return m_inner->GetFrameStatistics(p); }
        HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* p) override { return m_inner->GetLastPresentCount(p); }

        // IDXGISwapChain1 ---------------------------------------------------
        HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* p) override { return m_inner->GetDesc1(p); }
        HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* p) override { return m_inner->GetFullscreenDesc(p); }
        HRESULT STDMETHODCALLTYPE GetHwnd(HWND* p) override { return m_inner->GetHwnd(p); }
        HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** pp) override { return m_inner->GetCoreWindow(riid, pp); }
        HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) override
        {
            const bool drew = RenderOverlay(m_inner, true);
            const HRESULT hr = m_inner->Present1(syncInterval, flags, pp);
            PostPresentDeviceCheck(drew);
            return hr;
        }
        BOOL    STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return m_inner->IsTemporaryMonoSupported(); }
        HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** pp) override { return m_inner->GetRestrictToOutput(pp); }
        HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* p) override { return m_inner->SetBackgroundColor(p); }
        HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* p) override { return m_inner->GetBackgroundColor(p); }
        HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return m_inner->SetRotation(r); }
        HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* p) override { return m_inner->GetRotation(p); }

        // IDXGISwapChain2 ---------------------------------------------------
        HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { return m_inner->SetSourceSize(w, h); }
        HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { return m_inner->GetSourceSize(w, h); }
        HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT m) override { return m_inner->SetMaximumFrameLatency(m); }
        HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* m) override { return m_inner->GetMaximumFrameLatency(m); }
        HANDLE  STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return m_inner->GetFrameLatencyWaitableObject(); }
        HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* p) override { return m_inner->SetMatrixTransform(p); }
        HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* p) override { return m_inner->GetMatrixTransform(p); }

        // IDXGISwapChain3 ---------------------------------------------------
        UINT    STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return m_inner->GetCurrentBackBufferIndex(); }
        HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override { return m_inner->CheckColorSpaceSupport(c, s); }
        HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override
        {
            const HRESULT hr = m_inner->SetColorSpace1(c);
            if (SUCCEEDED(hr)) OnColorSpaceChanged(c);
            return hr;
        }
        HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT bc, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* nodeMask, IUnknown* const* pQueues) override
        {
            PreResizeCleanup();
            const HRESULT hr = m_inner->ResizeBuffers1(bc, w, h, f, fl, nodeMask, pQueues);
            PostResizeRebuild(m_inner);
            return hr;
        }

        // IDXGISwapChain4 ---------------------------------------------------
        HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT s, void* d) override { return m_inner->SetHDRMetaData(t, s, d); }

    private:
        IDXGISwapChain4* m_inner;
        LONG             m_ref = 1;
    };

    // Replace *pp (the real swapchain the game just created) with a wrapper that
    // owns it, so every Present routes through us first. hwnd is the window the
    // swapchain was created for - used to ignore auxiliary windows.
    // A probe is not the game. Crimson Route creates a 100 by 100 chain on a
    // window of its own to read vtables, OptiScaler does the same, and 1.6.5
    // took the first chain it saw as the game's: detours were read off it,
    // the queue was pinned to it, the main window was locked to Route's, and
    // Route was handed our wrapper back as its own probe. The game's real
    // chains then arrived on another window and were refused as auxiliary.
    // Nothing drew, Route saw no presents, and the menu key had nowhere to be
    // read. LuxDragon's 1.6.6 log, issue #47. The old guard let exactly 100
    // through. Nothing playable is under 256 on a side.
    static bool IsProbeChain(UINT w, UINT h)
    {
        return w < 256 || h < 256;
    }

    static void WrapSwapChain(IDXGISwapChain1** pp, HWND hwnd)
    {
        if (!pp || !*pp) return;

        // Skip tiny probe/overlay swapchains (matches OptiScaler) - never wrap our
        // own or a capability-probe surface.
        DXGI_SWAP_CHAIN_DESC1 d1 = {};
        if (SUCCEEDED((*pp)->GetDesc1(&d1)) && IsProbeChain(d1.Width, d1.Height))
            return;

        // Once we have locked onto the game's main window, ignore swapchains on any
        // other window: those are auxiliary surfaces (the engine/Streamline create
        // them) and wrapping them buys nothing while risking a fight with SL's own
        // bookkeeping.
        if (g_wrappedHwnd && hwnd && hwnd != g_wrappedHwnd)
            return;

        IDXGISwapChain4* inner = nullptr;
        if (FAILED((*pp)->QueryInterface(IID_PPV_ARGS(&inner))) || !inner)
            return; // need full IDXGISwapChain4 to wrap safely; else leave native

        // Transfer the caller's ref to the wrapper: QI added a ref (inner), drop
        // the raw ref, hand back the wrapper (ref=1) which owns inner.
        (*pp)->Release();
        auto* wrapper = new WrappedIDXGISwapChain(inner);
        *pp = static_cast<IDXGISwapChain1*>(wrapper);

        if (hwnd) g_wrappedHwnd = hwnd;

        // Log the layer on EVERY wrap - the pre-FG chain and the FG-active re-wrap
        // can be different objects. inner->Present in sl.*/nvngx/amd*/xess = a FG
        // proxy (correct, pre-interpolation, writable). dxgi.dll = native chain.
        // BufferCount jumping (e.g. to 6) + Flags reveal when MFG has taken over.
        void** innerVt = *reinterpret_cast<void***>(inner);
        LogHookedModule("Wrapped swapchain Present", innerVt[8]);

        if (!g_wrapperActive)
        {
            g_wrapperActive = true;
            LOG_OK("Swapchain wrapped - overlay composites before Frame Generation (%ux%u, %u buffers, flags 0x%X).",
                   d1.Width, d1.Height, d1.BufferCount, d1.Flags);
        }
        else
        {
            // A later recreation (FG toggle, resolution/settings change): expected.
            LOG("Swapchain re-wrapped after recreation (%ux%u, %u buffers, flags 0x%X).",
                d1.Width, d1.Height, d1.BufferCount, d1.Flags);
        }
    }

    // Our replacement for the factory's CreateSwapChainForHwnd vtable slot. Calls
    // the real slot (saved), then wraps the result. Not a MinHook detour - see
    // InstallSwapChainCreationPatch.
    // The four detour targets, read off the game's own objects. Slot numbers
    // are the interface layout, which is fixed: Present is IDXGISwapChain[8],
    // ResizeBuffers [13], SetColorSpace1 is IDXGISwapChain3[38], and
    // ExecuteCommandLists is ID3D12CommandQueue[10]. Each still goes through
    // LogHookTarget, so anything already detoured, owned by another module, or
    // sitting in a proxy that borrows a system DLL's name is left alone.
    // The detour targets, read off the game's own objects. Slot numbers are the
    // interface layout, which is fixed: Present is IDXGISwapChain[8], Present1
    // [22], ResizeBuffers [13], SetColorSpace1 is IDXGISwapChain3[38],
    // ResizeBuffers1 [39], and ExecuteCommandLists is ID3D12CommandQueue[10].
    //
    // They are cached because a later pass may fill in what the first one
    // refused. What is cached is six function addresses inside modules, never a
    // swapchain pointer: holding a reference to the chain is what kept it from
    // dying in issue #30, and a raw one would dangle the moment the game
    // replaced it.
    struct HookSlot
    {
        const char* name;
        void*       addr;
        void*       detour;
        void**      orig;
        bool        mayStack;   // another mod's jump may be stacked on
        bool        mayProxy;   // a game-folder proxy may be stacked on as a last resort
        bool        draws;      // installing this one gives the overlay a way in
        bool        installed;
    };
    static HookSlot g_targets[6] = {};
    static bool g_targetsRead = false;
    static bool g_proxyPassDone = false;

    static bool AnyDrawPathHooked()
    {
        for (const HookSlot& t : g_targets)
            if (t.installed && t.draws) return true;
        return false;
    }

    // One pass over whatever is still uninstalled. Returns how many it added.
    static int InstallPass(bool stack, bool allowProxy, bool wrapperWillDraw = true)
    {
        int added = 0, skipped = 0;
        for (HookSlot& t : g_targets)
        {
            if (t.installed || !t.addr) { if (!t.installed) ++skipped; continue; }
            if (!LogHookTarget(t.name, t.addr, stack && t.mayStack, allowProxy && t.mayProxy)) { ++skipped; continue; }
            if (MH_CreateHook(t.addr, t.detour, t.orig) != MH_OK) { LOG_ERR("MH_CreateHook failed for %s.", t.name); ++skipped; continue; }
            t.installed = true;
            ++added;
        }
        if (added && MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
            LOG_ERR("MH_EnableHook failed; the DirectX detours are not active.");
        if (skipped && !allowProxy && wrapperWillDraw)
            LOG("%d of 6 DirectX functions were left to whoever hooked them first. The overlay comes from the wrapped swapchain instead; if that does not happen it will not draw, which is better than two mods fighting over one function.", skipped);
        else if (skipped && !allowProxy)
            LOG("%d of 6 DirectX functions belong to something else and the wrapper is off.", skipped);
        return added;
    }

    // Nothing is drawing and nothing is hooked that could: try again with a
    // game-folder proxy allowed. Says what it is doing either way, because a
    // silent overlay with no explanation is the report this exists to prevent.
    static void EscalateToProxy(const char* why)
    {
        if (g_proxyPassDone || !g_targetsRead) return;
        g_proxyPassDone = true;
        LOG("[overlay] %s. Trying again with a present call in another mod's proxy allowed.", why);
        if (InstallPass(true, true) == 0)
            LOG_ERR("[overlay] there is still nothing this mod can hook to draw the menu. The [hook] lines above name who owns each call. "
                    "Looting is unaffected; the menu needs one of those mods out of the folder, or WrapSwapChain=1 with no swapchain-keying overlay loaded.");
    }

    // Started once, when the targets are first read. Every path that can draw
    // counts a frame, so a count still at zero means no path exists, whatever
    // the reason: the hooks were all refused, or the wrapper was expected and
    // never happened because the chain failed its interface check or arrived on
    // another window. A machine can be slow to its first frame (twenty seconds
    // on a 5060 Ti), so this waits and then acts on the count rather than on
    // the clock alone.
    static DWORD WINAPI OverlayBlindWatch(LPVOID)
    {
        for (int i = 0; i < 90 && g_presentSeen.load(std::memory_order_relaxed) == 0; ++i)
            Sleep(200);
        if (g_presentSeen.load(std::memory_order_relaxed) == 0 && !g_wrapperActive && !AnyDrawPathHooked())
            EscalateToProxy("eighteen seconds and no frame has reached this mod, with nothing hooked that could bring one");
        return 0;
    }

    static void InstallDetoursFrom(IDXGISwapChain1* chain, IUnknown* queueUnk, bool stack)
    {
        if (g_targetsRead || !chain) return;
        g_targetsRead = true;

        void** scVt = *reinterpret_cast<void***>(chain);
        void* presentAddr  = scVt[8];
        void* present1Addr = scVt[22];
        void* resizeAddr   = scVt[13];
        void* colorSpaceAddr = nullptr;
        void* resize1Addr    = nullptr;
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3)
        {
            colorSpaceAddr = (*reinterpret_cast<void***>(sc3))[38];
            resize1Addr    = (*reinterpret_cast<void***>(sc3))[39];
            sc3->Release();
        }
        void* execAddr = nullptr;
        ID3D12CommandQueue* q = nullptr;
        if (queueUnk && SUCCEEDED(queueUnk->QueryInterface(IID_PPV_ARGS(&q))) && q)
        {
            execAddr = (*reinterpret_cast<void***>(q))[10];
            q->Release();
        }

        // ExecuteCommandLists is never stacked and never taken from a proxy:
        // the queue comes from the creation pin, Trinity sits on that function,
        // and a proxy's copy of it was the hang in issue #34.
        const HookSlot targets[6] = {
            { "Present",             presentAddr,    reinterpret_cast<void*>(&hkPresent),             reinterpret_cast<void**>(&oPresent),             true,  true,  true,  false },
            { "Present1",            present1Addr,   reinterpret_cast<void*>(&hkPresent1),            reinterpret_cast<void**>(&oPresent1),            true,  true,  true,  false },
            { "ResizeBuffers",       resizeAddr,     reinterpret_cast<void*>(&hkResizeBuffers),       reinterpret_cast<void**>(&oResizeBuffers),       true,  true,  false, false },
            { "ResizeBuffers1",      resize1Addr,    reinterpret_cast<void*>(&hkResizeBuffers1),      reinterpret_cast<void**>(&oResizeBuffers1),      true,  true,  false, false },
            { "ExecuteCommandLists", execAddr,       reinterpret_cast<void*>(&hkExecuteCommandLists), reinterpret_cast<void**>(&oExecuteCommandLists), false, false, false, false },
            { "SetColorSpace1",      colorSpaceAddr, reinterpret_cast<void*>(&hkSetColorSpace1),      reinterpret_cast<void**>(&oSetColorSpace1),      true,  true,  false, false },
        };
        for (int i = 0; i < 6; ++i) g_targets[i] = targets[i];

        // Whether anything at all can draw is knowable here, without waiting:
        // the wrapper is off when a keying overlay is loaded or the ini says
        // so, and the present calls are about to be refused or taken.
        const bool willWrap = Settings::Get().wrapSwapChain && !stack;
        InstallPass(stack, false, willWrap);

        if (!willWrap && !AnyDrawPathHooked())
            EscalateToProxy("every present call belongs to another mod's proxy in the game folder and the swapchain wrapper is off, so nothing can draw the menu");

        CloseHandle(CreateThread(nullptr, 0, &OverlayBlindWatch, nullptr, 0, nullptr));
    }

    static HRESULT STDMETHODCALLTYPE hkFactoryCreateSwapChainForHwnd(
        IDXGIFactory2* self, IUnknown* device, HWND hwnd,
        const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc,
        IDXGIOutput* restrictOut, IDXGISwapChain1** ppSwapChain)
    {
        // Only the outermost (game-issued) creation is wrapped. If we are already
        // inside a creation on this thread, this is Streamline recreating the chain
        // through the same patched slot during an FG toggle - let it complete
        // untouched, otherwise we recurse into the interposer mid-rebuild.
        CreateGuard guard;

        // Without a wrapper, nothing lets go of the back buffers when the game
        // replaces its swapchain. The wrapper's destructor does that on the
        // wrapped path, at the game's last Release. On the stacked path the
        // views stayed on the old chain's buffers, so the chain could not
        // finish dying, and the replacement the game then asked for on the
        // same window failed, which is the null pointer the game reads when
        // frame generation or a display setting is applied. The first hint
        // that this creation is a replacement is this call, so the release
        // has to happen here, before the original runs. A full-size chain on
        // another window is another mod's, and is left alone.
        if (!guard.wasNested && !g_wrapperActive && g_imguiReady && g_swapChain &&
            desc && !IsProbeChain(desc->Width, desc->Height) &&
            (!g_gameHwnd || hwnd == g_gameHwnd))
        {
            LOG("[hook] the game is replacing its %ux%u swapchain with a %ux%u one and there is no wrapper to let go of the old buffers: releasing them before the new chain is made",
                g_scWidth, g_scHeight, desc->Width, desc->Height);
            WaitForOverlayIdle();
            CleanupRenderTargets();
            ReleaseOffscreenTarget();
            g_swapChain = nullptr;
        }

        const HRESULT hr = oFactoryCreateSwapChainForHwnd(self, device, hwnd, desc, fsDesc, restrictOut, ppSwapChain);
        if (!guard.wasNested && FAILED(hr))
            LOG_ERR("[hook] CreateSwapChainForHwnd failed (0x%08X) for a %ux%u chain on window %p; if the game dies next, this is why",
                    static_cast<unsigned>(hr), desc ? desc->Width : 0u, desc ? desc->Height : 0u, static_cast<void*>(hwnd));
        if (guard.wasNested || FAILED(hr))
            return hr;

        // The game's real chain and queue, the first time they exist. Tiny
        // chains are probes and not the game's window; skip those the way
        // WrapSwapChain does.
        if (desc && IsProbeChain(desc->Width, desc->Height))
        {
            LOG("[hook] a %ux%u swapchain on window %p (%s) is another mod's probe, not the game's: no detours, no queue, no wrapper",
                desc->Width, desc->Height, static_cast<void*>(hwnd), hwnd && IsWindowVisible(hwnd) ? "visible" : "hidden");
            return hr;
        }
        if (!g_gameHwnd && hwnd) g_gameHwnd = hwnd;
        // Who owns Present on the chain the game is about to use, decided once.
        // Steam's overlay and the frame generation interposer coexist with the
        // wrapper. Anything else that detoured Present first is assumed to key
        // its own work on the object it is handed, which Crimson Route does:
        // above this mod at the factory it keyed the wrapper, met the real
        // chain in its detour, and drew nothing. For such a mod the wrapper is
        // skipped and this mod's detours stack on top of its instead, ours
        // first, theirs next, dxgi last. The overlay then draws where it drew
        // before the wrapper existed, after frame generation. Issue #47.
        static bool s_decided = false, s_stack = false;
        if (!s_decided && ppSwapChain && *ppSwapChain)
        {
            s_decided = true;
            void** vt = *reinterpret_cast<void***>(*ppSwapChain);
            char owner[64] = "";
            if (DetourOwner(vt[8], owner, sizeof owner) && !BenignPresentOwner(owner))
            {
                s_stack = true;
                LOG("[hook] %s detoured Present before this mod and would be handed the wrapper: not wrapping; stacking this mod's detours on top of its instead", owner);
            }
            else if (const char* keying = KeyingOverlayLoaded())
            {
                s_stack = true;
                LOG("[hook] %s is loaded and keys its work on the swapchain object it is handed: not wrapping; stacking this mod's detours instead", keying);
            }
        }
        if (ppSwapChain && *ppSwapChain)
            InstallDetoursFrom(*ppSwapChain, device, s_stack);

        // Wrap the first swapchain and never a replacement.
        //
        // Measured on a 5060 Ti that reproduces this reliably. Six sessions,
        // every one of them wrapping a replacement chain, every one of them
        // dead within a couple of minutes of a frame generation toggle. A
        // build that refuses to wrap at all survived repeated toggles in both
        // directions, before and after loading a save. The first wrap never
        // killed anything; the second always did.
        //
        // What the game does when it dies is read a null pointer, twice, in
        // its own code: once loading an interface out of an object and calling
        // through its vtable, once taking a getter's result and reading it at
        // +0x30. The first of those is null-checked twenty bytes away on
        // another path, so the game knows that pointer can be absent and
        // assumes it cannot be there. Something it looks up during the rebuild
        // is not found, and handing it our object in place of the real one is
        // the obvious way to make a lookup keyed on that pointer miss.
        //
        // An earlier attempt gated this on a wrapper still being alive, which
        // never fired: the game releases its swapchain before creating the
        // replacement, so by then ours is gone. Whether one is alive is not
        // the question. Whether we have already wrapped one is.
        //
        // The cost is the menu. After a toggle it stops drawing until the game
        // is restarted, because the chain it drew through is gone and we are
        // not taking the new one. That is the trade, and a menu that stops
        // beats a game that stops.
        // Replacements are wrapped, the same as the first one.
        //
        // Two builds refused them, on the theory that handing the game our
        // proxy in place of its own swapchain was what killed it. That was
        // wrong, and the test that settled it wrapped every replacement while
        // taking no GPU resources at all: five recreations across repeated
        // frame generation toggles, no crash. The proxy is not the problem.
        //
        // What was killing it is that the render target views held references
        // to back buffers of a chain the game was destroying. Those are
        // released in the wrapper's destructor now. See the comment there.
        // A full-size chain on some other window, once the game's is known, is
        // another mod's surface. WrapSwapChain already leaves those alone, but
        // the queue pin below did not: every creation re-pinned the present
        // queue, authoritative, so a chain created by an overlay mod on its own
        // window moved this mod's submits onto that mod's queue while the
        // wrapper stayed on the game's chain. Nothing about that queue is ours
        // to submit on. Left alone entirely, and said once per window.
        if (g_wrappedHwnd && hwnd && hwnd != g_wrappedHwnd)
        {
            static HWND s_said = nullptr;
            if (s_said != hwnd)
            {
                s_said = hwnd;
                LOG("[hook] a %ux%u swapchain on window %p, which is not the game's %p: another mod's surface, left alone",
                    desc ? desc->Width : 0u, desc ? desc->Height : 0u, static_cast<void*>(hwnd), static_cast<void*>(g_wrappedHwnd));
            }
            return hr;
        }
        if (g_wrapperActive)
            LOG("Swapchain replaced; wrapping the new one.");

        // For D3D12 the first argument is the swapchain's present command
        // queue - the queue that owns the back buffers. Pin it as the queue
        // we submit overlay work on, so we never submit on Streamline's MFG
        // pacer queue (which is rejected). Re-pins on each recreation too.
        ID3D12CommandQueue* pq = nullptr;
        if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&pq))) && pq)
        {
            PublishPresentQueue(pq, true);
            pq->Release();
        }
        if (Settings::Get().wrapSwapChain && !s_stack)
            WrapSwapChain(ppSwapChain, hwnd);
        return hr;
    }

    // VirtualProtect-patch one vtable slot. Reentrancy-safe (no MinHook, no thread
    // suspend), so it is safe to leave installed while the game runs.
    static bool PatchVtableSlot(void** vtable, int index, void* newFn, void** origOut)
    {
        DWORD old = 0;
        if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
            return false;
        *origOut = vtable[index];
        vtable[index] = newFn;
        VirtualProtect(&vtable[index], sizeof(void*), old, &old);
        return true;
    }

    // Patch the factory CLASS's CreateSwapChainForHwnd slot (slot 15) so every
    // swapchain the game creates comes back wrapped. We read the vtable from a
    // dummy factory of the same class the game will use (a Streamline proxy factory
    // when FG is present, since we go through the same DXGI entry points). No
    // export detour, no reentrant MinHook -> cannot deadlock startup or fight
    // Streamline's interposer. The patched vtable lives in the DLL and persists
    // after the dummy factory is released.
    static void InstallSwapChainCreationPatch()
    {
        IDXGIFactory2* factory = nullptr;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || !factory)
        {
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
            {
                LOG_ERR("FG: no factory to patch - overlay-over-FrameGen disabled.");
                return;
            }
        }

        // The function is detoured, never the slot. Until 1.6.9 this replaced
        // the vtable pointer with a function in this DLL, and Steam's overlay
        // refuses to hook any factory whose create slot "points to another
        // module": whenever Steam looked at the factory after this patch and
        // after its own hook reset, it skipped the factory the game then made
        // its swapchain through, never hooked Present, and never drew. Found
        // on 10 September 2026 with HookDX12=0 as the control: Steam's overlay
        // came back the moment this patch was absent. An inline detour on
        // dxgi's own function leaves the slot in dxgi.dll, Steam hooks the
        // slot above us, Streamline and Route patch the slot above us too, and
        // this mod is the innermost caller either way, which is the ordering
        // every working run has had (see the issue #47 note). Stacking on a
        // detour Route already put on the function is the same arrangement as
        // Present, ours first and theirs next.
        void** vt = *reinterpret_cast<void***>(factory);
        void* fn = vt[15];
        if (LogHookTarget("Factory CreateSwapChainForHwnd", fn, true) &&
            MH_CreateHook(fn, reinterpret_cast<void*>(&hkFactoryCreateSwapChainForHwnd),
                          reinterpret_cast<void**>(&oFactoryCreateSwapChainForHwnd)) == MH_OK &&
            MH_EnableHook(fn) == MH_OK)
        {
            LOG("FG: CreateSwapChainForHwnd detoured in place - the factory slot still points into dxgi.dll, so Steam's overlay keeps its hooks.");
        }
        else
        {
            // The slot patch is the fallback, with the Steam cost named.
            LOG_ERR("FG: could not detour CreateSwapChainForHwnd in place; replacing the vtable slot instead, which Steam's overlay may refuse to hook over.");
            if (PatchVtableSlot(vt, 15, reinterpret_cast<void*>(&hkFactoryCreateSwapChainForHwnd),
                                reinterpret_cast<void**>(&oFactoryCreateSwapChainForHwnd)))
                LOG("FG: CreateSwapChainForHwnd slot patched.");
            else
                LOG_ERR("FG: VirtualProtect of CreateSwapChainForHwnd slot failed.");
        }

        factory->Release();
    }

    // Arm DRED so a later device removal is diagnosable. MUST run before the game
    // creates its D3D12 device - we do, because the ASI loader (winmm.dll) injects
    // us at process start, well ahead of the engine's renderer init. Global process
    // setting: applies to the device the game subsequently creates. Cheap; the
    // breadcrumb ring adds negligible overhead. See DumpDred.
    static void EnableDredIfAvailable()
    {
        ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))) && dred)
        {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->Release();
            LOG("DRED armed - a device removal will report GPU breadcrumbs + page faults.");
        }
        else
        {
            LOG("DRED unavailable on this system - device-removed causes stay opaque.");
        }
    }

    // --- Bootstrap: dummy device to locate the vtable slots -----------------
    static bool GetVTableAddresses(void*& presentAddr, void*& resizeAddr, void*& execAddr, void*& colorSpaceAddr)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MasterLooterDummyWnd";
        RegisterClassExW(&wc);

        HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                  0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd)
        {
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
            return false;
        }

        bool ok = false;
        ID3D12Device*              device  = nullptr;
        ID3D12CommandQueue*        queue   = nullptr;
        ID3D12CommandAllocator*    alloc   = nullptr;
        ID3D12GraphicsCommandList* list    = nullptr;
        IDXGIFactory4*             factory = nullptr;
        IDXGISwapChain1*           swap1   = nullptr;
        IDXGISwapChain3*           swap3   = nullptr;

        do
        {
            if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
                break;

            D3D12_COMMAND_QUEUE_DESC qd = {};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
                break;

            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))))
                break;
            if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))))
                break;

            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                break;

            DXGI_SWAP_CHAIN_DESC1 scd = {};
            scd.BufferCount      = 2;
            scd.Width            = 100;
            scd.Height           = 100;
            scd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
            scd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd.SampleDesc.Count = 1;
            scd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            scd.AlphaMode        = DXGI_ALPHA_MODE_UNSPECIFIED;

            if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &swap1)))
                break;
            if (FAILED(swap1->QueryInterface(IID_PPV_ARGS(&swap3))))
                break;

            // vtable[8]  IDXGISwapChain::Present
            // vtable[13] IDXGISwapChain::ResizeBuffers
            // vtable[38] IDXGISwapChain3::SetColorSpace1
            void** swapVtbl = *reinterpret_cast<void***>(swap3);
            presentAddr    = swapVtbl[8];
            resizeAddr     = swapVtbl[13];
            colorSpaceAddr = swapVtbl[38];

            // vtable[10] ID3D12CommandQueue::ExecuteCommandLists
            void** queueVtbl = *reinterpret_cast<void***>(queue);
            execAddr = queueVtbl[10];

            ok = true;
        } while (false);

        if (swap3)   swap3->Release();
        if (swap1)   swap1->Release();
        if (factory) factory->Release();
        if (list)    list->Release();
        if (alloc)   alloc->Release();
        if (queue)   queue->Release();
        if (device)  device->Release();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return ok;
    }

    bool InstallDX12Hooks()
    {
        // No throwaway device, ever. This used to create a D3D12 device, a
        // command queue and a swapchain purely to read vtable slots, then
        // release them. ReShade redirects every D3D12CreateDevice, and RenoDX
        // attaches to the first device it sees, which was ours: on the test
        // machine its only OnInitDevice of the whole run hooked that device,
        // we released it, and it never re-attached to the game's real one.
        // Its injections then ran against nothing and the game died at
        // CrimsonDesert.exe+0x30314D2, which is issue 34 exactly.
        //
        // The vtable slots are read instead from the game's own swapchain and
        // queue the first time it creates them, inside the factory hook, and
        // the detours go in then. Same addresses on a clean machine, because
        // vtables are per class; on a machine with a proxy in front, the
        // proxy's own vtable, which the ownership check then refuses to touch.
        if (Settings::Get().enableDred) EnableDredIfAvailable();
        else LOG("DRED not armed (EnableDred=0). Set EnableDred=1 to report GPU breadcrumbs on a device removal.");

        InitializeCriticalSection(&g_queueLock);
        g_queueLockReady = true;

        // DLSS-G / Frame Generation: patch the factory's CreateSwapChainForHwnd
        // slot so the game's swapchain comes back wrapped, letting the overlay
        // draw into the writable game-facing buffer before Streamline interpolates.
        // Best-effort: if it fails the native present hook still draws (non-FG).
        //
        // This is the part that reaches furthest into the process, since the
        // patch goes into a vtable other overlay mods use as well. Anyone whose
        // game will not start alongside another overlay can turn it off in the
        // ini without launching the game, and keep everything else.
        // Always. The factory hook is where the game's real swapchain and its
        // queue first appear, and that is now the only place the detour
        // targets can be learned from. Whether the chain is then wrapped is a
        // separate decision, made inside the hook from WrapSwapChain.
        InstallSwapChainCreationPatch();
        if (!Settings::Get().wrapSwapChain)
            LOG("WrapSwapChain=0: the swapchain is left alone. The overlay draws through the present hook, which does not show under DLSS frame generation.");

        char exePath[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        const char* exeName = strrchr(exePath, '\\');
        exeName = exeName ? exeName + 1 : exePath;
        LOG("DX12 hooks installed (%s, pid %lu).", exeName, GetCurrentProcessId());
        return true;
    }

    void RemoveDX12Hooks()
    {
        MH_DisableHook(MH_ALL_HOOKS);
        input::Shutdown();
        hooks::RemoveXInputHooks();

        if (g_imguiReady)
        {
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            g_imguiReady = false;
        }

        CleanupRenderTargets();
        for (auto& f : g_frames)
            if (f.commandAllocator) { f.commandAllocator->Release(); f.commandAllocator = nullptr; }
        g_frames.clear();
        g_swapChain   = nullptr;
        g_bufferCount = 0;
        g_scWidth     = 0;
        g_scHeight    = 0;
        g_scFormat    = DXGI_FORMAT_UNKNOWN;

        ReleaseOffscreenTarget();
        if (g_compositePSO)     { g_compositePSO->Release();     g_compositePSO     = nullptr; }
        if (g_compositeRootSig) { g_compositeRootSig->Release(); g_compositeRootSig = nullptr; }
        g_compositeFormat = DXGI_FORMAT_UNKNOWN;
        g_colorSpace      = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

        if (g_queueLockReady)
        {
            EnterCriticalSection(&g_queueLock);
            if (g_presentQueue) { g_presentQueue->Release(); g_presentQueue = nullptr; }
            g_presentQueuePinned = false;
            LeaveCriticalSection(&g_queueLock);
            g_queueLockReady = false;
            DeleteCriticalSection(&g_queueLock);
        }

        if (g_fenceEvent)  { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
        if (g_fence)       { g_fence->Release();       g_fence = nullptr; }
        if (g_commandList) { g_commandList->Release(); g_commandList = nullptr; }
        if (g_srvHeap)     { g_srvHeap->Release();     g_srvHeap = nullptr; }
        if (g_rtvHeap)     { g_rtvHeap->Release();     g_rtvHeap = nullptr; }
        if (g_device)      { g_device->Release();      g_device = nullptr; }
    }
}
