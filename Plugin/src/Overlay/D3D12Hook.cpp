#include "PCH.h"
#include "Overlay/D3D12Hook.h"
#include "Overlay/DebugOverlay.h"
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <chrono>
#include <string>

// Forward declarations
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


namespace SAFCompanion::Overlay
{
    // ── D3D12 / DXGI hook state ──────────────────────────────────────────
    struct FrameContext {
        ID3D12CommandAllocator* commandAllocator = nullptr;
        ID3D12Resource*         mainRenderTargetResource = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE mainRenderTargetDescriptor{};
    };

    static std::vector<FrameContext> g_frameContext;
    static UINT                      g_frameBufferCount = 0;

    static ID3D12DescriptorHeap*     g_pd3dSrvDescHeap = nullptr;
    static ID3D12DescriptorHeap*     g_pd3dRtvDescHeap = nullptr;
    static ID3D12CommandQueue*     g_pd3dCommandQueue = nullptr;
    static ID3D12GraphicsCommandList* g_pd3dCommandList = nullptr;

    static HWND                      g_window = nullptr;
    static WNDPROC                   g_originalWndProc = nullptr;

    static uint64_t*                 g_methodsTable = nullptr;
    static bool                      g_initialized = false;
    std::atomic<bool>                g_overlayVisible{ false };

    using PresentFn             = long (__fastcall*)(IDXGISwapChain3*, UINT, UINT);
    using ExecuteCommandListsFn = void (__fastcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList*);
    using ResizeBuffersFn       = long (__fastcall*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    static PresentFn             g_originalPresent = nullptr;
    static ExecuteCommandListsFn g_originalExecuteCommandLists = nullptr;
    static ResizeBuffersFn       g_originalResizeBuffers = nullptr;

    // ── Helper: safe COM release ─────────────────────────────────────────
    template<typename T>
    static void SafeRelease(T*& res) {
        if (res) { res->Release(); res = nullptr; }
    }

    // ── WndProc ──────────────────────────────────────────────────────────
    // NOTE: Starfield uses Raw Input, so WM_KEYDOWN is NOT reliable for hotkeys.
    // All hotkey polling is done via GetAsyncKeyState in HookPresent instead.
    LRESULT WINAPI HookWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (g_overlayVisible && g_initialized) {
            if (::ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
                return 1;
            }

            // Block game input when overlay is visible
            switch (msg) {
                case WM_MOUSEMOVE:
                case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                case WM_CHAR:
                case WM_INPUT:
                    return 0;
            }
        }
        return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
    }

    // ── Reset ImGui / D3D12 state ────────────────────────────────────────
    static void ResetState()
    {
        if (g_initialized) {
            g_initialized = false;
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        SafeRelease(g_pd3dCommandQueue);
        g_frameContext.clear();
        SafeRelease(g_pd3dCommandList);
        SafeRelease(g_pd3dRtvDescHeap);
        SafeRelease(g_pd3dSrvDescHeap);
    }

    // ── Hook: Present ──────────────────────────────────────────────────
    static long __fastcall HookPresent(IDXGISwapChain3* pSwapChain, UINT syncInterval, UINT flags)
    {
        static int callCount = 0;
        if (callCount == 0) {
            callCount++;
        }
        
        if (!g_pd3dCommandQueue) {
            return g_originalPresent(pSwapChain, syncInterval, flags);
        }

        if (!g_initialized) {
            ID3D12Device* pDevice = nullptr;
            if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&pDevice))) {
                return g_originalPresent(pSwapChain, syncInterval, flags);
            }

            // Get window from swapchain
            DXGI_SWAP_CHAIN_DESC desc{};
            pSwapChain->GetDesc(&desc);
            g_window = desc.OutputWindow;
            if (g_window && !g_originalWndProc) {
                g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_window, GWLP_WNDPROC, (LONG_PTR)HookWndProc);
            }

            g_frameBufferCount = desc.BufferCount;
            g_frameContext.resize(g_frameBufferCount);

            // Create SRV heap for ImGui
            {
                D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
                heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                heapDesc.NumDescriptors = g_frameBufferCount;
                heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                if (pDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_pd3dSrvDescHeap)) != S_OK) {
                    pDevice->Release();
                    return g_originalPresent(pSwapChain, syncInterval, flags);
                }
            }

            // Create RTV heap for render targets
            {
                D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
                heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                heapDesc.NumDescriptors = g_frameBufferCount;
                heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                heapDesc.NodeMask       = 1;

                if (pDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_pd3dRtvDescHeap)) != S_OK) {
                    pDevice->Release();
                    return g_originalPresent(pSwapChain, syncInterval, flags);
                }

                UINT rtvDescriptorSize = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                for (UINT i = 0; i < g_frameBufferCount; i++) {
                    g_frameContext[i].mainRenderTargetDescriptor = rtvHandle;
                    pSwapChain->GetBuffer(i, IID_PPV_ARGS(&g_frameContext[i].mainRenderTargetResource));
                    pDevice->CreateRenderTargetView(g_frameContext[i].mainRenderTargetResource, nullptr, rtvHandle);
                    rtvHandle.ptr += rtvDescriptorSize;
                }
            }

            // Create command allocators + command list
            {
                for (UINT i = 0; i < g_frameBufferCount; i++) {
                    if (pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frameContext[i].commandAllocator)) != S_OK) {
                        pDevice->Release();
                        return g_originalPresent(pSwapChain, syncInterval, flags);
                    }
                }

                if (pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frameContext[0].commandAllocator, nullptr, IID_PPV_ARGS(&g_pd3dCommandList)) != S_OK || g_pd3dCommandList->Close() != S_OK) {
                    pDevice->Release();
                    return g_originalPresent(pSwapChain, syncInterval, flags);
                }
            }

            // Init ImGui
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

            ImGui::StyleColorsDark();

            ImGui_ImplWin32_Init(g_window);
            ImGui::SetNextWindowSize(ImVec2(550, 500), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            
            ImGui_ImplDX12_Init(pDevice, g_frameBufferCount,
                DXGI_FORMAT_R8G8B8A8_UNORM, g_pd3dSrvDescHeap,
                g_pd3dSrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                g_pd3dSrvDescHeap->GetGPUDescriptorHandleForHeapStart());

            g_initialized = true;
            pDevice->Release();
        }

        // Render overlay only when visible
        if (g_overlayVisible.load()) {
            static bool once = true;
            if (once) {
                once = false;
            }
            ImGui_ImplWin32_NewFrame();
            ImGui_ImplDX12_NewFrame();
            ImGui::NewFrame();

            RenderDebugOverlay();

            FrameContext& frameCtx = g_frameContext[pSwapChain->GetCurrentBackBufferIndex()];
            frameCtx.commandAllocator->Reset();

            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource   = frameCtx.mainRenderTargetResource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;

            g_pd3dCommandList->Reset(frameCtx.commandAllocator, nullptr);
            g_pd3dCommandList->ResourceBarrier(1, &barrier);
            g_pd3dCommandList->OMSetRenderTargets(1, &frameCtx.mainRenderTargetDescriptor, FALSE, nullptr);
            g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);

            ImGui::Render();
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            g_pd3dCommandList->ResourceBarrier(1, &barrier);
            g_pd3dCommandList->Close();

            g_pd3dCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**)&g_pd3dCommandList);
        }

        return g_originalPresent(pSwapChain, syncInterval, flags);
    }

    // ── Hook: ExecuteCommandLists ────────────────────────────────────────
    static void __fastcall HookExecuteCommandLists(ID3D12CommandQueue* queue, UINT numCommandLists, ID3D12CommandList* ppCommandLists)
    {
        if (!g_pd3dCommandQueue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_pd3dCommandQueue = queue;
            g_pd3dCommandQueue->AddRef();
        }
        g_originalExecuteCommandLists(queue, numCommandLists, ppCommandLists);
    }

    // ── Hook: ResizeBuffers ────────────────────────────────────────────
    static long __fastcall HookResizeBuffers(IDXGISwapChain3* pSwapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags)
    {
        ResetState();
        return g_originalResizeBuffers(pSwapChain, bufferCount, width, height, newFormat, swapChainFlags);
    }

    // ── Build method table from fake D3D12 objects ───────────────────────
    static bool BuildMethodTable()
    {
        WNDCLASSEX wc = {};
        wc.cbSize        = sizeof(WNDCLASSEX);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = DefWindowProc;
        wc.hInstance     = GetModuleHandle(nullptr);
        wc.lpszClassName = L"SAFCFakeWindow";
        RegisterClassEx(&wc);

        HWND fakeWindow = CreateWindow(wc.lpszClassName, L"SAFC Fake", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

        HMODULE libDXGI  = GetModuleHandle(L"dxgi.dll");
        HMODULE libD3D12 = GetModuleHandle(L"d3d12.dll");
        if (!libDXGI || !libD3D12) {
            DestroyWindow(fakeWindow);
            UnregisterClass(wc.lpszClassName, wc.hInstance);
            return false;
        }

        auto pfnCreateDXGIFactory = (decltype(&CreateDXGIFactory))GetProcAddress(libDXGI, "CreateDXGIFactory");
        auto pfnD3D12CreateDevice = (decltype(&D3D12CreateDevice))GetProcAddress(libD3D12, "D3D12CreateDevice");
        if (!pfnCreateDXGIFactory || !pfnD3D12CreateDevice) {
            DestroyWindow(fakeWindow);
            UnregisterClass(wc.lpszClassName, wc.hInstance);
            return false;
        }

        IDXGIFactory* factory = nullptr;
        if (FAILED(pfnCreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory))) {
            DestroyWindow(fakeWindow);
            UnregisterClass(wc.lpszClassName, wc.hInstance);
            return false;
        }

        IDXGIAdapter* adapter = nullptr;
        if (factory->EnumAdapters(0, &adapter) == DXGI_ERROR_NOT_FOUND) {
            factory->Release();
            DestroyWindow(fakeWindow);
            UnregisterClass(wc.lpszClassName, wc.hInstance);
            return false;
        }

        ID3D12Device* device = nullptr;
        if (FAILED(pfnD3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&device))) {
            adapter->Release();
            factory->Release();
            DestroyWindow(fakeWindow);
            UnregisterClass(wc.lpszClassName, wc.hInstance);
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = 0;
        queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        ID3D12CommandQueue* commandQueue = nullptr;
        if (FAILED(device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), (void**)&commandQueue))) {
            device->Release();
            adapter->Release();
            factory->Release();
            DestroyWindow(fakeWindow);
            UnregisterClass(wc.lpszClassName, wc.hInstance);
            return false;
        }

        ID3D12CommandAllocator* commandAllocator = nullptr;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&commandAllocator))) {
            commandQueue->Release();
            device->Release();
            adapter->Release();
            factory->Release();
            DestroyWindow(fakeWindow);
            UnregisterClass(wc.lpszClassName, wc.hInstance);
            return false;
        }

        ID3D12GraphicsCommandList* commandList = nullptr;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&commandList))) {
            commandAllocator->Release();
            commandQueue->Release();
            device->Release();
            adapter->Release();
            factory->Release();
            DestroyWindow(fakeWindow);
            UnregisterClass(wc.lpszClassName, wc.hInstance);
            return false;
        }

        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        swapChainDesc.BufferDesc.Width  = 100;
        swapChainDesc.BufferDesc.Height = 100;
        swapChainDesc.BufferDesc.RefreshRate.Numerator   = 60;
        swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.SampleDesc.Count  = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = 2;
        swapChainDesc.OutputWindow = fakeWindow;
        swapChainDesc.Windowed = TRUE;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

        IDXGISwapChain* swapChain = nullptr;
        if (FAILED(factory->CreateSwapChain(commandQueue, &swapChainDesc, &swapChain))) {
            commandList->Release();
            commandAllocator->Release();
            commandQueue->Release();
            device->Release();
            adapter->Release();
            factory->Release();
            DestroyWindow(fakeWindow);
            UnregisterClass(wc.lpszClassName, wc.hInstance);
            return false;
        }

        // Copy vtables
        g_methodsTable = (uint64_t*)calloc(150, sizeof(uint64_t));
        memcpy(g_methodsTable,     *(uint64_t**)(void*)device,          44 * sizeof(uint64_t));
        memcpy(g_methodsTable+44,  *(uint64_t**)(void*)commandQueue,   19 * sizeof(uint64_t));
        memcpy(g_methodsTable+63,  *(uint64_t**)(void*)commandAllocator, 9 * sizeof(uint64_t));
        memcpy(g_methodsTable+72,  *(uint64_t**)(void*)commandList,    60 * sizeof(uint64_t));
        memcpy(g_methodsTable+132, *(uint64_t**)(void*)swapChain,      18 * sizeof(uint64_t));

        // Cleanup fake objects
        swapChain->Release();
        commandList->Release();
        commandAllocator->Release();
        commandQueue->Release();
        device->Release();
        adapter->Release();
        factory->Release();
        DestroyWindow(fakeWindow);
        UnregisterClass(wc.lpszClassName, wc.hInstance);

        return true;
    }

    // ── Public API ───────────────────────────────────────────────────────
    bool InitD3D12Hook()
    {
        
        if (!BuildMethodTable()) {
            return false;
        }

        if (MH_Initialize() != MH_OK) {
            return false;
        }

        // Hook indices from combined method table:
        // Device=44, Queue=19, Allocator=9, CommandList=60, SwapChain=18
        // ExecuteCommandLists = 44 + 10 = 54
        // Present = 132 + 8 = 140
        // ResizeBuffers = 132 + 13 = 145

        if (MH_CreateHook((void*)g_methodsTable[54],  HookExecuteCommandLists, (void**)&g_originalExecuteCommandLists) != MH_OK ||
            MH_EnableHook((void*)g_methodsTable[54]) != MH_OK) {
            return false;
        }

        if (MH_CreateHook((void*)g_methodsTable[140], HookPresent, (void**)&g_originalPresent) != MH_OK ||
            MH_EnableHook((void*)g_methodsTable[140]) != MH_OK) {
            return false;
        }

        if (MH_CreateHook((void*)g_methodsTable[145], HookResizeBuffers, (void**)&g_originalResizeBuffers) != MH_OK ||
            MH_EnableHook((void*)g_methodsTable[145]) != MH_OK) {
            return false;
        }

        return true;
    }

    void ShutdownD3D12Hook()
    {
        if (g_window && g_originalWndProc) {
            SetWindowLongPtr(g_window, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
        }

        MH_DisableHook((void*)g_methodsTable[54]);
        MH_DisableHook((void*)g_methodsTable[140]);
        MH_DisableHook((void*)g_methodsTable[145]);
        MH_RemoveHook((void*)g_methodsTable[54]);
        MH_RemoveHook((void*)g_methodsTable[140]);
        MH_RemoveHook((void*)g_methodsTable[145]);
        MH_Uninitialize();

        ResetState();
        ImGui::DestroyContext();

        free(g_methodsTable);
        g_methodsTable = nullptr;
    }

    bool IsOverlayVisible() { return g_overlayVisible.load(); }
    void SetOverlayVisible(bool visible) { g_overlayVisible.store(visible); }
    void ToggleOverlay() 
    { 
        bool newState = !g_overlayVisible.load();
        g_overlayVisible.store(newState); 


        if (g_initialized) {
            ImGuiIO& io = ImGui::GetIO();
            io.MouseDrawCursor = newState;
            if (!newState) {
                io.ClearInputKeys();
                io.ClearInputMouse();
            }
        } else {
        }
    }
}
