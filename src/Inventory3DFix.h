#pragma once
// Inventory3DManager VR Fix — Standalone F4SE Plugin
//
// Fixes the vanilla FO4VR bug where 3D item previews in the Pipboy inventory
// are invisible. BSLightingShader items (weapons, armor, food, junk) are
// missing because the forward render path skips shader groups 1+2.
//
// Fix: Inject BSLightingShader rendering into the forward path, then write
// alpha=1 to item pixels so they survive the game's alpha-blend compositing.

#include "RE/Fallout.h"
#include <d3d11.h>

namespace inv3d
{
    class Inventory3DFix
    {
    public:
        static Inventory3DFix& GetSingleton()
        {
            static Inventory3DFix instance;
            return instance;
        }

        void Install();

    private:
        Inventory3DFix() = default;
        ~Inventory3DFix() = default;
        Inventory3DFix(const Inventory3DFix&) = delete;
        Inventory3DFix& operator=(const Inventory3DFix&) = delete;

        // ── Per-renderer render hook ──
        // Intercepts FUN_140b02e30: orchestrates mode 16 for inventory renderers.
        static void HookPerRendererRender(uintptr_t renderer, uintptr_t renderData);
        using PerRendererRender_t = void(*)(uintptr_t renderer, uintptr_t renderData);
        static inline PerRendererRender_t g_originalPerRendererRender = nullptr;

        // ── Composite hook ──
        // Intercepts the composite CALL inside the per-renderer render function.
        // Applies alpha fixup + content-aware blit for BSLightingShader visibility.
        static void HookComposite(int srcRT, int dstRT, char useEffect11);
        using Composite_t = void(*)(int srcRT, int dstRT, char useEffect11);
        static inline Composite_t g_originalComposite = nullptr;

        // Flag: when true, HookComposite runs D3D11 alpha fixup
        static inline bool s_doAlphaFixup = false;

        // ── Forward render pass hook ──
        // After the normal forward render (BSEffectShader), additionally renders
        // BSLightingShader groups 0, 1, 2 and writes alpha=1 to item pixels.
        static void HookForwardRenderPass(uintptr_t accumulator, uintptr_t context);
        using ForwardRenderPass_t = void(*)(uintptr_t accumulator, uintptr_t context);
        static inline ForwardRenderPass_t g_originalForwardRenderPass = nullptr;

        // FUN_14281e400 — renders a specific shader accumulator group
        using RenderShaderGroup_t = void(*)(uintptr_t accumulator, uint32_t groupID,
                                            uintptr_t context, uint8_t flag);
        static inline RenderShaderGroup_t g_renderShaderGroup = nullptr;

        // Flag: when true, HookForwardRenderPass adds BSLightingShader groups
        static inline bool s_addLightingGroups = false;

        // ── Scene graph traversal hook ──
        // Sets accumulator+0xf669=true and f688=0x19 before traversal so
        // BSLightingShader geometry registers into groups 1+2.
        static void HookSceneTraversal(uintptr_t camera, uintptr_t sceneNode,
                                       uintptr_t cullingProcess, uint8_t flag);
        using SceneTraversal_t = void(*)(uintptr_t camera, uintptr_t sceneNode,
                                         uintptr_t cullingProcess, uint8_t flag);
        static inline SceneTraversal_t g_originalSceneTraversal = nullptr;

        // Accumulator pointer stored by HookPerRendererRender for use by traversal hook
        static inline uintptr_t g_currentAccumulator = 0;

        // ── Renderer layout offsets (Ghidra-verified, F4VR 1.2.72) ──
        static constexpr int OFF_RENDERER_ENABLED    = 0x5D;
        static constexpr int OFF_RENDERER_VR_MODE    = 0x78;
        static constexpr int OFF_RENDERER_OFFSCR_3D  = 0x98;
        static constexpr int OFF_RENDERER_FIELD_C0   = 0xC0;
        static constexpr int OFF_RENDERER_FIELD_C8   = 0xC8;
        static constexpr int OFF_RENDERER_NAME       = 0x220;

        // ── D3D11 resources for alpha fixup ──
        static inline bool s_d3dInitialized = false;
        static inline bool s_d3dInitFailed = false;
        static inline ID3D11VertexShader* s_alphaVS = nullptr;
        static inline ID3D11PixelShader* s_contentAwareBlitPS = nullptr;
        static inline ID3D11BlendState* s_alphaBlendState = nullptr;    // alpha-only write
        static inline ID3D11BlendState* s_overwriteBlendState = nullptr; // full overwrite
        static inline ID3D11DepthStencilState* s_alphaDepthState = nullptr;
        static inline ID3D11RasterizerState* s_alphaRastState = nullptr;
        static inline ID3D11Buffer* s_dimsCB = nullptr;

        // Staging texture: copy of source RT used as SRV input for fullscreen draws.
        // Recreated whenever source dimensions or format change.
        static inline ID3D11Texture2D* s_stagingTex = nullptr;
        static inline ID3D11ShaderResourceView* s_stagingSRV = nullptr;
        static inline UINT s_stagingWidth = 0;
        static inline UINT s_stagingHeight = 0;
        static inline DXGI_FORMAT s_stagingFormat = DXGI_FORMAT_UNKNOWN;

    public:
        // BSGraphics::RendererData singleton (VR offset 0x60F3CE8)
        //   +0x48 = ID3D11Device*, +0x50 = ID3D11DeviceContext*
        //   +0x0A58 = RenderTarget array (each 0x30: +0x00=Tex2D*, +0x10=RTV*, +0x18=SRV*)
        static constexpr uintptr_t BSGFX_RENDERER_DATA = 0x60F3CE8;

    private:

        static bool InitD3D11Resources();
        static void DoAlphaWriteToCurrentRTV();
        static void DoAlphaWriteToSrc(int srcRT);
        static void DoContentAwareBlit(int srcRT, int dstRT);
    };
}
