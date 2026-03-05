#include "Inventory3DFix.h"
#include <d3dcompiler.h>

namespace inv3d
{
    // ════════════════════════════════════════════════════════════════════════
    // VR address constants (Ghidra-verified for F4VR 1.2.72)
    // ════════════════════════════════════════════════════════════════════════

    // CALL FUN_140b02e30 (per-renderer render) inside FUN_140b02c20
    static constexpr uintptr_t CALL_SITE_PER_RENDERER_RENDER = 0xb02cf7;

    // CALL FUN_1427b08c0 (composite) inside FUN_140b02e30
    static constexpr uintptr_t CALL_SITE_COMPOSITE = 0xb03497;

    // CALL FUN_14281cc90 (forward render pass) inside FUN_1427ff820
    static constexpr uintptr_t CALL_SITE_FORWARD_RENDER_PASS = 0x27ff876;

    // FUN_14281e400 — generic "render shader group" function
    static constexpr uintptr_t VR_RENDER_SHADER_GROUP = 0x281e400;

    // CALL FUN_1427ff370 (scene traversal) inside FUN_140b03d60
    static constexpr uintptr_t CALL_SITE_SCENE_TRAVERSAL = 0xb04095;

    // ════════════════════════════════════════════════════════════════════════
    // Installation
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::Install()
    {
        spdlog::info("[Inv3D] Installing Inventory3DManager fix");

        auto& trampoline = F4SE::GetTrampoline();

        // Hook 1: Per-renderer render
        {
            REL::Relocation<std::uintptr_t> callSite{ REL::Offset(CALL_SITE_PER_RENDERER_RENDER) };
            auto* callByte = reinterpret_cast<uint8_t*>(callSite.address());
            if (*callByte != 0xE8) {
                spdlog::error("[Inv3D] Per-renderer render call site at {:X} is not E8 (found {:02X})",
                    callSite.address(), *callByte);
                return;
            }
            g_originalPerRendererRender = reinterpret_cast<PerRendererRender_t>(
                trampoline.write_call<5>(callSite.address(), &HookPerRendererRender));
            spdlog::info("[Inv3D] Hooked per-renderer render at {:X}", callSite.address());
        }

        // Hook 2: Composite
        {
            REL::Relocation<std::uintptr_t> callSite{ REL::Offset(CALL_SITE_COMPOSITE) };
            auto* callByte = reinterpret_cast<uint8_t*>(callSite.address());
            if (*callByte != 0xE8) {
                spdlog::error("[Inv3D] Composite call site at {:X} is not E8 (found {:02X})",
                    callSite.address(), *callByte);
                return;
            }
            g_originalComposite = reinterpret_cast<Composite_t>(
                trampoline.write_call<5>(callSite.address(), &HookComposite));
            spdlog::info("[Inv3D] Hooked composite at {:X}", callSite.address());
        }

        // Hook 3: Forward render pass — inject BSLightingShader groups
        {
            g_renderShaderGroup = reinterpret_cast<RenderShaderGroup_t>(
                REL::Offset(VR_RENDER_SHADER_GROUP).address());

            REL::Relocation<std::uintptr_t> callSite{ REL::Offset(CALL_SITE_FORWARD_RENDER_PASS) };
            auto* callByte = reinterpret_cast<uint8_t*>(callSite.address());
            if (*callByte != 0xE8) {
                spdlog::error("[Inv3D] Forward render pass call site at {:X} is not E8 (found {:02X})",
                    callSite.address(), *callByte);
                return;
            }
            g_originalForwardRenderPass = reinterpret_cast<ForwardRenderPass_t>(
                trampoline.write_call<5>(callSite.address(), &HookForwardRenderPass));
            spdlog::info("[Inv3D] Hooked forward render pass at {:X}", callSite.address());
        }

        // Hook 4: Scene graph traversal — force f669=true for BSLightingShader registration
        {
            REL::Relocation<std::uintptr_t> callSite{ REL::Offset(CALL_SITE_SCENE_TRAVERSAL) };
            auto* callByte = reinterpret_cast<uint8_t*>(callSite.address());
            if (*callByte != 0xE8) {
                spdlog::error("[Inv3D] Traversal call site at {:X} is not E8 (found {:02X})",
                    callSite.address(), *callByte);
                return;
            }
            g_originalSceneTraversal = reinterpret_cast<SceneTraversal_t>(
                trampoline.write_call<5>(callSite.address(), &HookSceneTraversal));
            spdlog::info("[Inv3D] Hooked scene traversal at {:X}", callSite.address());
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Inventory renderer check
    // ════════════════════════════════════════════════════════════════════════

    static bool IsInventoryRenderer(const char* name)
    {
        if (!name || name[0] == '\0') return false;
        static const char* inventoryRenderers[] = {
            "PipboyMenu", "WSExamineMenuModel",
            "InventoryMenu", "ContainerMenu", "ExamineMenu",
            "BarterMenu", "WorkshopMenu", "CookingMenu", "PowerArmorMenu"
        };
        for (const char* rn : inventoryRenderers) {
            if (std::strcmp(name, rn) == 0) return true;
        }
        return false;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Scene graph helpers: walk NiNode tree, disable envmap materials before
    // deferred render to prevent null texture crashes in BSLightingShader.
    // ════════════════════════════════════════════════════════════════════════

    static bool IsValidPtr(uintptr_t p) {
        return p > 0x10000 && p < 0x7FFFFFFFFFFF && (p & 0x7) == 0;
    }

    static bool IsExePtr(uintptr_t p) {
        static uintptr_t s_exeBase = 0;
        static uintptr_t s_exeEnd = 0;
        if (s_exeBase == 0) {
            HMODULE hExe = GetModuleHandle(NULL);
            s_exeBase = reinterpret_cast<uintptr_t>(hExe);
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(s_exeBase);
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(s_exeBase + dos->e_lfanew);
            s_exeEnd = s_exeBase + nt->OptionalHeader.SizeOfImage;
        }
        return p >= s_exeBase && p < s_exeEnd;
    }

    struct SavedShaderState {
        uintptr_t shaderPropAddr;
        uint64_t  originalFlags;
        uint32_t  originalTechID;
    };

    static constexpr int MAX_SHADER_SAVES = 32;
    static SavedShaderState s_shaderSaves[MAX_SHADER_SAVES];
    static int s_numShaderSaves = 0;
    static constexpr uint64_t kEnvMapBit = 1ULL << 7;
    static constexpr int MAX_WALK_DEPTH = 16;

    // Recursively walk scene graph from obj.
    // NiNode and BSGeometry checks are MUTUALLY EXCLUSIVE:
    // NiNode has children at +0x160, BSGeometry has properties at +0x170/+0x178.
    static void WalkSceneGraph(uintptr_t obj, int depth = 0) {
        if (!IsValidPtr(obj) || s_numShaderSaves >= MAX_SHADER_SAVES || depth > MAX_WALK_DEPTH)
            return;

        uintptr_t objVtbl = *reinterpret_cast<uintptr_t*>(obj);
        if (!IsExePtr(objVtbl))
            return;

        // NiNode path: children NiTObjectArray at +0x160 (VR)
        uintptr_t childArrayVtbl = *reinterpret_cast<uintptr_t*>(obj + 0x160);
        if (IsExePtr(childArrayVtbl)) {
            uintptr_t childData = *reinterpret_cast<uintptr_t*>(obj + 0x168);
            uint16_t  childSize = *reinterpret_cast<uint16_t*>(obj + 0x174);
            if (IsValidPtr(childData) && childSize > 0 && childSize < 256) {
                for (uint16_t i = 0; i < childSize; i++) {
                    uintptr_t child = *reinterpret_cast<uintptr_t*>(childData + i * 8);
                    if (IsValidPtr(child)) {
                        WalkSceneGraph(child, depth + 1);
                    }
                }
            }
            return;  // NiNode — do NOT read properties at +0x170
        }

        // BSGeometry path: check properties[0] and properties[1] at +0x170/+0x178 (VR)
        static constexpr int VR_PROP_OFFSETS[] = { 0x170, 0x178 };

        for (int propOff : VR_PROP_OFFSETS) {
            uintptr_t prop = *reinterpret_cast<uintptr_t*>(obj + propOff);
            if (!IsValidPtr(prop)) continue;
            uintptr_t propVtbl = *reinterpret_cast<uintptr_t*>(prop);
            if (!IsExePtr(propVtbl)) continue;

            // Validate: BSShaderProperty::alpha at +0x28 must be reasonable
            float alpha = *reinterpret_cast<float*>(prop + 0x28);
            if (alpha < -0.1f || alpha > 2.0f) continue;

            // Validate: BSShaderProperty::flags at +0x30 must fit in 40 bits
            uint64_t flags = *reinterpret_cast<uint64_t*>(prop + 0x30);
            if ((flags >> 40) != 0) continue;

            if (flags & kEnvMapBit) {
                auto& ss = s_shaderSaves[s_numShaderSaves++];
                ss.shaderPropAddr = prop;
                ss.originalFlags = flags;
                ss.originalTechID = *reinterpret_cast<uint32_t*>(prop + 0xD8);
                // Clear envmap bit to prevent null cubemap texture crash
                *reinterpret_cast<uint64_t*>(prop + 0x30) = flags & ~kEnvMapBit;
            }
        }
    }

    static void RestoreShaderStates() {
        for (int i = 0; i < s_numShaderSaves; i++) {
            auto& ss = s_shaderSaves[i];
            *reinterpret_cast<uint64_t*>(ss.shaderPropAddr + 0x30) = ss.originalFlags;
            *reinterpret_cast<uint32_t*>(ss.shaderPropAddr + 0xD8) = ss.originalTechID;
        }
        s_numShaderSaves = 0;
    }

    __declspec(noinline) static bool TryProtectedWalk(uintptr_t obj)
    {
        __try {
            WalkSceneGraph(obj, 0);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    using RenderShaderGroupFunc = void(*)(uintptr_t, uint32_t, uintptr_t, uint8_t);
    __declspec(noinline) static bool TryRenderShaderGroups(
        RenderShaderGroupFunc func, uintptr_t accumulator, uintptr_t context)
    {
        __try {
            func(accumulator, 0, context, 0);
            func(accumulator, 1, context, 0);
            func(accumulator, 2, context, 0);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Hook: Per-renderer render
    //
    // For inventory renderers: sets up accumulator, walks scene graph to
    // disable envmap, enables BSLightingShader injection + alpha fixup,
    // calls original, restores state.
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::HookPerRendererRender(uintptr_t renderer, uintptr_t renderData)
    {
        auto* namePtr = reinterpret_cast<RE::BSFixedString*>(renderer + OFF_RENDERER_NAME);
        const char* rendererName = (namePtr && namePtr->c_str()) ? namePtr->c_str() : "";

        if (!IsInventoryRenderer(rendererName)) {
            if (g_originalPerRendererRender) {
                g_originalPerRendererRender(renderer, renderData);
            }
            return;
        }

        auto offscr3D = *reinterpret_cast<uintptr_t*>(renderer + OFF_RENDERER_OFFSCR_3D);
        auto enabled  = *reinterpret_cast<uint8_t*>(renderer + OFF_RENDERER_ENABLED);
        auto field_C0 = *reinterpret_cast<uintptr_t*>(renderer + OFF_RENDERER_FIELD_C0);
        auto field_C8 = *reinterpret_cast<uint8_t*>(renderer + OFF_RENDERER_FIELD_C8);
        bool bVar5 = (enabled != 0) && (offscr3D != 0 || (field_C0 != 0 && field_C8 != 0));

        // Get BSShaderAccumulator from renderer+0x1B0
        uintptr_t acc = *reinterpret_cast<uintptr_t*>(renderer + 0x1B0);
        g_currentAccumulator = acc;
        s_addLightingGroups = true;
        s_doAlphaFixup = true;

        // Walk scene graph to disable envmap materials (prevent crashes
        // from null cubemap textures in BSLightingShader forward mode)
        if (bVar5 && offscr3D != 0) {
            s_numShaderSaves = 0;
            TryProtectedWalk(offscr3D);
        }

        if (g_originalPerRendererRender) {
            g_originalPerRendererRender(renderer, renderData);
        }

        RestoreShaderStates();

        s_addLightingGroups = false;
        s_doAlphaFixup = false;
        g_currentAccumulator = 0;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Hook: Scene graph traversal — force f669=true for BSLightingShader
    //
    // FUN_140b03d60 sets f669=false, f688=0 (forward mode) before traversal.
    // We override to f669=true, f688=0x19 so BSLightingShader geometry
    // registers into groups 1+2 instead of only group 10.
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::HookSceneTraversal(uintptr_t camera, uintptr_t sceneNode,
                                             uintptr_t cullingProcess, uint8_t flag)
    {
        if (s_addLightingGroups) {
            uintptr_t acc = g_currentAccumulator;
            if (acc == 0 && cullingProcess != 0) {
                acc = *reinterpret_cast<uintptr_t*>(cullingProcess + 0x190);
            }

            if (acc != 0) {
                *reinterpret_cast<bool*>(acc + 0xf669) = true;
                *reinterpret_cast<uint32_t*>(acc + 0xf688) = 0x19;
            }
        }

        if (g_originalSceneTraversal) {
            g_originalSceneTraversal(camera, sceneNode, cullingProcess, flag);
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Hook: Forward render pass — inject BSLightingShader groups + alpha fix
    //
    // The forward render path only renders BSEffectShader (group 10).
    // After the original pass, we render groups 0, 1, 2 (BSLightingShader)
    // then write alpha=1 to item pixels on the forward RT.
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::HookForwardRenderPass(uintptr_t accumulator, uintptr_t context)
    {
        if (g_originalForwardRenderPass) {
            g_originalForwardRenderPass(accumulator, context);
        }

        if (s_addLightingGroups && g_renderShaderGroup) {
            bool ok = TryRenderShaderGroups(g_renderShaderGroup, accumulator, context);
            if (!ok) {
                static int s_crashCount = 0;
                if (s_crashCount < 5) {
                    spdlog::error("[Inv3D] BSLightingShader render crashed! (count={})",
                        ++s_crashCount);
                }
            }

            // Write alpha=1 to the currently bound RT (forwardRT) for pixels
            // where RGB > threshold. For projected mode, the game will then
            // alpha-blend forwardRT -> srcRT; items now survive the copy.
            DoAlphaWriteToCurrentRTV();
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Hook: Composite — alpha fixup before + content-aware blit after
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::HookComposite(int srcRT, int dstRT, char useEffect11)
    {
        if (s_doAlphaFixup) {
            DoAlphaWriteToSrc(srcRT);
            if (g_originalComposite) {
                g_originalComposite(srcRT, dstRT, useEffect11);
            }
            DoContentAwareBlit(srcRT, dstRT);
            return;
        }

        if (g_originalComposite) {
            g_originalComposite(srcRT, dstRT, useEffect11);
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // D3D11 Resources and Alpha Fixup
    // ════════════════════════════════════════════════════════════════════════

    static const char* s_alphaVS_HLSL =
        "float4 main(uint id : SV_VertexID) : SV_POSITION {\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n";

    // Content-aware blit PS: reads source texture, discards background pixels
    // (RGB <= threshold), outputs full RGBA for item pixels.
    static const char* s_contentAwareBlitPS_HLSL =
        "Texture2D srcTex : register(t0);\n"
        "cbuffer Constants : register(b0) {\n"
        "    float2 srcDims;\n"
        "    float2 dstDims;\n"
        "};\n"
        "float4 main(float4 pos : SV_POSITION) : SV_TARGET {\n"
        "    float2 uv = pos.xy / dstDims;\n"
        "    int2 srcCoord = int2(uv * srcDims);\n"
        "    float4 c = srcTex.Load(int3(srcCoord, 0));\n"
        "    float maxC = max(c.r, max(c.g, c.b));\n"
        "    if (maxC <= 0.001) discard;\n"
        "    return float4(c.rgb, 1.0);\n"
        "}\n";

    bool Inventory3DFix::InitD3D11Resources()
    {
        if (s_d3dInitialized) return true;
        if (s_d3dInitFailed) return false;

        uintptr_t base = REL::Module::get().base();
        auto** rendererDataPtrPtr = reinterpret_cast<uintptr_t**>(base + BSGFX_RENDERER_DATA);
        if (!rendererDataPtrPtr || !*rendererDataPtrPtr) {
            spdlog::error("[Inv3D] BSGraphics::RendererData singleton not available");
            s_d3dInitFailed = true;
            return false;
        }

        uintptr_t rendererData = reinterpret_cast<uintptr_t>(*rendererDataPtrPtr);
        auto* device = *reinterpret_cast<ID3D11Device**>(rendererData + 0x48);
        if (!device) {
            spdlog::error("[Inv3D] ID3D11Device is null");
            s_d3dInitFailed = true;
            return false;
        }

        // Compile vertex shader
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* errBlob = nullptr;
        HRESULT hr = D3DCompile(s_alphaVS_HLSL, strlen(s_alphaVS_HLSL), "AlphaFixupVS",
            nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob);
        if (FAILED(hr)) {
            const char* errMsg = errBlob ? (const char*)errBlob->GetBufferPointer() : "unknown";
            spdlog::error("[Inv3D] VS compile failed: {}", errMsg);
            if (errBlob) errBlob->Release();
            s_d3dInitFailed = true;
            return false;
        }
        if (errBlob) errBlob->Release();

        hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            nullptr, &s_alphaVS);
        vsBlob->Release();
        if (FAILED(hr)) {
            spdlog::error("[Inv3D] CreateVertexShader failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Compile content-aware blit pixel shader
        ID3DBlob* caBlob = nullptr;
        hr = D3DCompile(s_contentAwareBlitPS_HLSL, strlen(s_contentAwareBlitPS_HLSL), "ContentAwareBlitPS",
            nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &caBlob, &errBlob);
        if (FAILED(hr)) {
            const char* errMsg = errBlob ? (const char*)errBlob->GetBufferPointer() : "unknown";
            spdlog::error("[Inv3D] Content-aware blit PS compile failed: {}", errMsg);
            if (errBlob) errBlob->Release();
            s_d3dInitFailed = true;
            return false;
        }
        if (errBlob) errBlob->Release();

        hr = device->CreatePixelShader(caBlob->GetBufferPointer(), caBlob->GetBufferSize(),
            nullptr, &s_contentAwareBlitPS);
        caBlob->Release();
        if (FAILED(hr)) {
            spdlog::error("[Inv3D] CreatePixelShader (content-aware blit) failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Alpha-only blend state (write mask = alpha only)
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        hr = device->CreateBlendState(&bd, &s_alphaBlendState);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D] CreateBlendState (alpha) failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Overwrite blend state (no blending, full write)
        D3D11_BLEND_DESC obd = {};
        obd.RenderTarget[0].BlendEnable = FALSE;
        obd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&obd, &s_overwriteBlendState);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D] CreateBlendState (overwrite) failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Depth-stencil state — disable depth
        D3D11_DEPTH_STENCIL_DESC dsd = {};
        dsd.DepthEnable = FALSE;
        dsd.StencilEnable = FALSE;
        hr = device->CreateDepthStencilState(&dsd, &s_alphaDepthState);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D] CreateDepthStencilState failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Rasterizer state — no culling
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        hr = device->CreateRasterizerState(&rd, &s_alphaRastState);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D] CreateRasterizerState failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Constant buffer for src/dst dimensions
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = 16;  // float4: srcDims.xy, dstDims.xy
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&cbDesc, nullptr, &s_dimsCB);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D] CreateBuffer (dims CB) failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        s_d3dInitialized = true;
        spdlog::info("[Inv3D] D3D11 resources initialized");
        return true;
    }

    // ════════════════════════════════════════════════════════════════════════
    // D3D11 state save/restore helpers
    // ════════════════════════════════════════════════════════════════════════

    struct SavedD3D11State {
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        ID3D11BlendState* blendState = nullptr;
        FLOAT blendFactor[4] = {};
        UINT sampleMask = 0;
        ID3D11DepthStencilState* depthState = nullptr;
        UINT stencilRef = 0;
        ID3D11RasterizerState* rastState = nullptr;
        D3D11_VIEWPORT viewport = {};
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ID3D11InputLayout* il = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY topology = {};
        ID3D11ShaderResourceView* psSRV = nullptr;
        ID3D11Buffer* psCB = nullptr;

        void Save(ID3D11DeviceContext* ctx) {
            ctx->OMGetRenderTargets(1, &rtv, &dsv);
            ctx->OMGetBlendState(&blendState, blendFactor, &sampleMask);
            ctx->OMGetDepthStencilState(&depthState, &stencilRef);
            ctx->RSGetState(&rastState);
            UINT numVP = 1;
            ctx->RSGetViewports(&numVP, &viewport);
            ctx->VSGetShader(&vs, nullptr, nullptr);
            ctx->PSGetShader(&ps, nullptr, nullptr);
            ctx->IAGetInputLayout(&il);
            ctx->IAGetPrimitiveTopology(&topology);
            ctx->PSGetShaderResources(0, 1, &psSRV);
            ctx->PSGetConstantBuffers(0, 1, &psCB);
        }

        void Restore(ID3D11DeviceContext* ctx) {
            ctx->OMSetRenderTargets(1, &rtv, dsv);
            ctx->OMSetBlendState(blendState, blendFactor, sampleMask);
            ctx->OMSetDepthStencilState(depthState, stencilRef);
            ctx->RSSetState(rastState);
            ctx->RSSetViewports(1, &viewport);
            ctx->IASetInputLayout(il);
            ctx->IASetPrimitiveTopology(topology);
            ctx->VSSetShader(vs, nullptr, 0);
            ctx->PSSetShader(ps, nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &psSRV);
            ctx->PSSetConstantBuffers(0, 1, &psCB);

            if (rtv) rtv->Release();
            if (dsv) dsv->Release();
            if (blendState) blendState->Release();
            if (depthState) depthState->Release();
            if (rastState) rastState->Release();
            if (vs) vs->Release();
            if (ps) ps->Release();
            if (il) il->Release();
            if (psSRV) psSRV->Release();
            if (psCB) psCB->Release();
        }
    };

    // Helper: ensure staging texture matches given dimensions/format
    static bool EnsureStaging(ID3D11Device* device,
        ID3D11Texture2D*& stagingTex, ID3D11ShaderResourceView*& stagingSRV,
        UINT& stagingW, UINT& stagingH, DXGI_FORMAT& stagingFmt,
        const D3D11_TEXTURE2D_DESC& desc)
    {
        if (stagingTex && stagingW == desc.Width && stagingH == desc.Height && stagingFmt == desc.Format)
            return true;

        if (stagingTex) {
            stagingSRV->Release(); stagingSRV = nullptr;
            stagingTex->Release(); stagingTex = nullptr;
        }
        D3D11_TEXTURE2D_DESC stageDesc = desc;
        stageDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        stageDesc.Usage          = D3D11_USAGE_DEFAULT;
        stageDesc.CPUAccessFlags = 0;
        stageDesc.MiscFlags      = 0;

        HRESULT hr = device->CreateTexture2D(&stageDesc, nullptr, &stagingTex);
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                    = stageDesc.Format;
        srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels       = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        hr = device->CreateShaderResourceView(stagingTex, &srvDesc, &stagingSRV);
        if (FAILED(hr)) {
            stagingTex->Release(); stagingTex = nullptr;
            return false;
        }
        stagingW = desc.Width;
        stagingH = desc.Height;
        stagingFmt = desc.Format;
        return true;
    }

    static bool HasAlphaChannel(DXGI_FORMAT fmt) {
        return fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
               fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
               fmt == DXGI_FORMAT_B8G8R8A8_UNORM ||
               fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
               fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ||
               fmt == DXGI_FORMAT_R16G16B16A16_UNORM ||
               fmt == DXGI_FORMAT_R32G32B32A32_FLOAT ||
               fmt == DXGI_FORMAT_R10G10B10A2_UNORM;
    }

    // Helper: set up fullscreen triangle draw state
    static void SetupFullscreenDraw(ID3D11DeviceContext* context,
        ID3D11RenderTargetView* rtv, ID3D11BlendState* blendState,
        ID3D11DepthStencilState* depthState, ID3D11RasterizerState* rastState,
        ID3D11VertexShader* vs, ID3D11PixelShader* ps,
        ID3D11ShaderResourceView* srv, ID3D11Buffer* cb,
        UINT width, UINT height)
    {
        context->OMSetRenderTargets(1, &rtv, nullptr);
        FLOAT blendFactor[4] = { 0, 0, 0, 0 };
        context->OMSetBlendState(blendState, blendFactor, 0xFFFFFFFF);
        context->OMSetDepthStencilState(depthState, 0);
        context->RSSetState(rastState);

        D3D11_VIEWPORT vp = {};
        vp.Width    = static_cast<FLOAT>(width);
        vp.Height   = static_cast<FLOAT>(height);
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->PSSetShaderResources(0, 1, &srv);
        if (cb) context->PSSetConstantBuffers(0, 1, &cb);
    }

    // Helper: update dims constant buffer
    static void UpdateDimsCB(ID3D11DeviceContext* context, ID3D11Buffer* cb,
        UINT srcW, UINT srcH, UINT dstW, UINT dstH)
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            float dims[4] = {
                static_cast<float>(srcW), static_cast<float>(srcH),
                static_cast<float>(dstW), static_cast<float>(dstH)
            };
            memcpy(mapped.pData, dims, sizeof(dims));
            context->Unmap(cb, 0);
        }
    }

    // Get BSGraphics device context, or nullptr
    static ID3D11DeviceContext* GetGameDeviceContext() {
        uintptr_t base = REL::Module::get().base();
        auto** rdPtrPtr = reinterpret_cast<uintptr_t**>(base + Inventory3DFix::BSGFX_RENDERER_DATA);
        if (!rdPtrPtr || !*rdPtrPtr) return nullptr;
        return *reinterpret_cast<ID3D11DeviceContext**>(reinterpret_cast<uintptr_t>(*rdPtrPtr) + 0x50);
    }

    static ID3D11Device* GetGameDevice() {
        uintptr_t base = REL::Module::get().base();
        auto** rdPtrPtr = reinterpret_cast<uintptr_t**>(base + Inventory3DFix::BSGFX_RENDERER_DATA);
        if (!rdPtrPtr || !*rdPtrPtr) return nullptr;
        return *reinterpret_cast<ID3D11Device**>(reinterpret_cast<uintptr_t>(*rdPtrPtr) + 0x48);
    }

    // Get RT entry from BSGraphics RT array
    struct RTEntry {
        ID3D11Texture2D* texture;
        ID3D11RenderTargetView* rtv;
        ID3D11ShaderResourceView* srv;
    };

    static RTEntry GetRTEntry(int rtIndex) {
        uintptr_t base = REL::Module::get().base();
        auto** rdPtrPtr = reinterpret_cast<uintptr_t**>(base + Inventory3DFix::BSGFX_RENDERER_DATA);
        if (!rdPtrPtr || !*rdPtrPtr) return { nullptr, nullptr, nullptr };
        uintptr_t rendererData = reinterpret_cast<uintptr_t>(*rdPtrPtr);
        uintptr_t entry = rendererData + 0x0A58 + static_cast<uintptr_t>(rtIndex) * 0x30;
        return {
            *reinterpret_cast<ID3D11Texture2D**>(entry + 0x00),
            *reinterpret_cast<ID3D11RenderTargetView**>(entry + 0x10),
            *reinterpret_cast<ID3D11ShaderResourceView**>(entry + 0x18)
        };
    }

    // ════════════════════════════════════════════════════════════════════════
    // DoAlphaWriteToCurrentRTV — writes alpha=1 to currently bound forward RT
    //
    // Called after TryRenderShaderGroups. For projected mode the game
    // alpha-blends forwardRT -> srcRT; writing alpha=1 ensures BSLightingShader
    // pixels survive that copy.
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::DoAlphaWriteToCurrentRTV()
    {
        if (!InitD3D11Resources()) return;

        auto* device = GetGameDevice();
        auto* context = GetGameDeviceContext();
        if (!device || !context) return;

        ID3D11RenderTargetView* currentRTV = nullptr;
        ID3D11DepthStencilView* currentDSV = nullptr;
        context->OMGetRenderTargets(1, &currentRTV, &currentDSV);
        if (!currentRTV) {
            if (currentDSV) currentDSV->Release();
            return;
        }

        ID3D11Resource* res = nullptr;
        currentRTV->GetResource(&res);
        auto* currentTex = static_cast<ID3D11Texture2D*>(res);
        if (!currentTex) {
            currentRTV->Release();
            if (currentDSV) currentDSV->Release();
            return;
        }

        D3D11_TEXTURE2D_DESC desc;
        currentTex->GetDesc(&desc);

        if (!HasAlphaChannel(desc.Format)) {
            currentTex->Release();
            currentRTV->Release();
            if (currentDSV) currentDSV->Release();
            return;
        }

        if (!EnsureStaging(device, s_stagingTex, s_stagingSRV,
                s_stagingWidth, s_stagingHeight, s_stagingFormat, desc)) {
            currentTex->Release();
            currentRTV->Release();
            if (currentDSV) currentDSV->Release();
            return;
        }

        context->CopyResource(s_stagingTex, currentTex);
        UpdateDimsCB(context, s_dimsCB, desc.Width, desc.Height, desc.Width, desc.Height);

        SavedD3D11State saved;
        saved.Save(context);

        SetupFullscreenDraw(context, currentRTV, s_alphaBlendState,
            s_alphaDepthState, s_alphaRastState, s_alphaVS, s_contentAwareBlitPS,
            s_stagingSRV, s_dimsCB, desc.Width, desc.Height);
        context->Draw(3, 0);

        // Restore with original DSV
        context->OMSetRenderTargets(1, &currentRTV, currentDSV);
        saved.rtv = currentRTV;
        saved.dsv = currentDSV;
        saved.Restore(context);

        currentTex->Release();
    }

    // ════════════════════════════════════════════════════════════════════════
    // DoAlphaWriteToSrc — writes alpha=1 to srcRT where RGB > threshold
    //
    // For wrist mode (srcRT has alpha), this makes BSLightingShader items
    // visible through the alpha-blend composite. Skipped for formats
    // without alpha (e.g., projected mode's R11G11B10_FLOAT).
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::DoAlphaWriteToSrc(int srcRT)
    {
        if (!InitD3D11Resources()) return;

        auto* device = GetGameDevice();
        auto* context = GetGameDeviceContext();
        if (!device || !context) return;

        auto src = GetRTEntry(srcRT);
        if (!src.texture || !src.rtv) return;

        D3D11_TEXTURE2D_DESC srcDesc;
        src.texture->GetDesc(&srcDesc);

        if (!HasAlphaChannel(srcDesc.Format)) return;

        if (!EnsureStaging(device, s_stagingTex, s_stagingSRV,
                s_stagingWidth, s_stagingHeight, s_stagingFormat, srcDesc))
            return;

        context->CopyResource(s_stagingTex, src.texture);
        UpdateDimsCB(context, s_dimsCB, srcDesc.Width, srcDesc.Height, srcDesc.Width, srcDesc.Height);

        SavedD3D11State saved;
        saved.Save(context);

        SetupFullscreenDraw(context, src.rtv, s_alphaBlendState,
            s_alphaDepthState, s_alphaRastState, s_alphaVS, s_contentAwareBlitPS,
            s_stagingSRV, s_dimsCB, srcDesc.Width, srcDesc.Height);
        context->Draw(3, 0);

        saved.Restore(context);
    }

    // ════════════════════════════════════════════════════════════════════════
    // DoContentAwareBlit — reads srcRT, writes visible pixels to dstRT
    //
    // Belt-and-suspenders: reads srcRT (which now contains BSLightingShader
    // content after the alpha fix), and for pixels where RGB > threshold,
    // directly overwrites those pixels in dstRT.
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::DoContentAwareBlit(int srcRT, int dstRT)
    {
        if (!InitD3D11Resources()) return;

        auto* device = GetGameDevice();
        auto* context = GetGameDeviceContext();
        if (!device || !context) return;

        auto src = GetRTEntry(srcRT);
        auto dst = GetRTEntry(dstRT);
        if (!src.texture || !dst.texture || !dst.rtv) return;

        D3D11_TEXTURE2D_DESC srcDesc, dstDesc;
        src.texture->GetDesc(&srcDesc);
        dst.texture->GetDesc(&dstDesc);

        if (!EnsureStaging(device, s_stagingTex, s_stagingSRV,
                s_stagingWidth, s_stagingHeight, s_stagingFormat, srcDesc))
            return;

        context->CopyResource(s_stagingTex, src.texture);
        UpdateDimsCB(context, s_dimsCB, srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height);

        SavedD3D11State saved;
        saved.Save(context);

        SetupFullscreenDraw(context, dst.rtv, s_overwriteBlendState,
            s_alphaDepthState, s_alphaRastState, s_alphaVS, s_contentAwareBlitPS,
            s_stagingSRV, s_dimsCB, dstDesc.Width, dstDesc.Height);
        context->Draw(3, 0);

        saved.Restore(context);
    }
}
