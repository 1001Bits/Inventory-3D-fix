#pragma once
// Inventory3DManager VR Fix — Standalone F4SE Plugin
//
// Fixes the vanilla FO4VR bug where 3D item previews in the Pipboy inventory
// don't display correctly (some items invisible, some partly visible).
//
// Root cause: The Pipboy's 3D screen surface (worldRoot) renders to the depth
// buffer BEFORE the item model (offscr3D). The item fails depth testing because
// it sits "behind" the screen quad. Only edges extending past the quad are visible.
//
// Fix: Set clearDepthStencilOffscreen (renderer+0x62) to clear the depth buffer
// between the scene render and the offscreen 3D render, preventing occlusion.
//
// Selectable approaches (via config iInventory3DFixMode):
//   0 = Disabled
//   1 = Approach A: Force vrMode=0 during UpdateModelTransform only
//   2 = Approach B: Override camera frustum during UpdateModelTransform
//   3 = Approach C: Force flat mode + swap VR dimension globals
//   4 = Approach D: Swap VR camera pointer with flat Pipboy camera
//   5 = Diagnostics only (no fix, just logging)
//   6 = Force vrMode=0 for all inventory renderers (diagnostic)
//   7 = Force vrMode=1 for all inventory renderers (diagnostic)
//   8 = Force display_mode=0 (skip VR compositing — diagnostic)
//   9 = Skip scene render (test depth occlusion theory — diagnostic)
//  10 = Set clearDepthStencilOffscreen=true (RECOMMENDED — depth clear fix)
//  11 = Clear BOTH render target color AND depth stencil (diagnostic)
//  12 = Override offscreen RT for VR compositing (diagnostic)
//  13 = Force clear alpha=1.0 on offscreen RT (test alpha-blend composite theory)
//  14 = Combined: redirect VR compositing RT + force clear alpha=1.0
//  15 = Replace composite with raw Copy (no alpha blend) + redirect VR compositing
//  16 = D3D11 alpha fixup: after composite, force alpha=1.0 on offscreen RT via fullscreen draw

#include "RE/Fallout.h"

// D3D11 types for alpha fixup (mode 16)
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

        // Install hooks — pass fix mode from config (0=disabled, 1-7=approach)
        void Install(int fixMode);

        // Per-frame update: diagnostics
        void OnFrameUpdate();

        // Runtime diagnostics — dump renderer state for debugging
        void LogRendererState();

    private:
        Inventory3DFix() = default;
        ~Inventory3DFix() = default;
        Inventory3DFix(const Inventory3DFix&) = delete;
        Inventory3DFix& operator=(const Inventory3DFix&) = delete;

        // Active fix mode (stored at Install time)
        static inline int s_fixMode = 5;

        // ── UpdateModelTransform hook (for approaches 1-4 and diagnostics) ──
        static void HookUpdateModelTransform(uintptr_t inv3dMgr, uintptr_t loadedModel);
        using UpdateModelTransform_t = void(*)(uintptr_t inv3dMgr, uintptr_t loadedModel);
        static inline UpdateModelTransform_t g_originalUpdateModelTransform = nullptr;

        // ── Per-renderer render hook (for approaches 5-7) ──
        // Intercepts FUN_140b02e30 which handles depth stencil, RT acquisition,
        // offscreen 3D rendering, Scaleform UI, and VR compositing.
        static void HookPerRendererRender(uintptr_t renderer, uintptr_t renderData);
        using PerRendererRender_t = void(*)(uintptr_t renderer, uintptr_t renderData);
        static inline PerRendererRender_t g_originalPerRendererRender = nullptr;

        // ── Composite hook (for mode 15 — replaces alpha-blending composite) ──
        // Intercepts the CALL to FUN_1427b08c0 inside the per-renderer render function.
        // The composite copies offscreen RT → screen RT using ImageSpace effect 0xF.
        static void HookComposite(int srcRT, int dstRT, char useEffect11);
        using Composite_t = void(*)(int srcRT, int dstRT, char useEffect11);
        static inline Composite_t g_originalComposite = nullptr;

        // Flag: when true, HookComposite replaces with raw Copy instead of alpha blend
        static inline bool s_replaceComposite = false;

        // Flag: when true, HookComposite runs D3D11 alpha fixup after original composite
        static inline bool s_doAlphaFixup = false;

        // ImageSpaceManager::Copy — raw texture copy (no alpha blending)
        using ISMCopy_t = void(*)(int srcRT, int dstRT);
        static inline ISMCopy_t g_imageSpcCopy = nullptr;

        // ── VR compositing hook (for modes 14-15 — redirects offscreen RT input) ──
        // Intercepts the CALL to FUN_140b038f0 inside the per-renderer render function.
        // VR compositing reads renderer+0x1D8 for the offscreen RT. Redirecting it
        // to the screen RT prevents the offscreen RT's alpha from being used as a mask.
        static void HookVRCompositing(uintptr_t renderer, uintptr_t renderData);
        using VRCompositing_t = void(*)(uintptr_t renderer, uintptr_t renderData);
        static inline VRCompositing_t g_originalVRCompositing = nullptr;

        // Flag: when true, HookVRCompositing redirects offscreen RT to screen RT
        static inline bool s_redirectVRCompositing = false;

        // ── Forward render pass hook (mode 16 — injects BSLightingShader groups) ──
        // Intercepts the CALL to FUN_14281cc90 inside FUN_1427ff820 (forward render).
        // After the normal forward render (BSEffectShader), additionally renders
        // BSLightingShader groups 1 and 2 which the forward path normally skips.
        static void HookForwardRenderPass(uintptr_t accumulator, uintptr_t context);
        using ForwardRenderPass_t = void(*)(uintptr_t accumulator, uintptr_t context);
        static inline ForwardRenderPass_t g_originalForwardRenderPass = nullptr;

        // FUN_14281e400 — renders a specific shader accumulator group
        using RenderShaderGroup_t = void(*)(uintptr_t accumulator, uint32_t groupID,
                                            uintptr_t context, uint8_t flag);
        static inline RenderShaderGroup_t g_renderShaderGroup = nullptr;

        // Flag: when true, HookForwardRenderPass adds BSLightingShader groups 1+2
        static inline bool s_addLightingGroups = false;

        // ── Scene graph traversal hook (mode 16 — forces f669=true) ──
        // Intercepts the CALL to FUN_1427ff370 inside FUN_140b03d60.
        // Sets accumulator+0xf669=true before traversal so BSLightingShader
        // geometry registers into groups 1+2 (normally skipped in forward mode).
        static void HookSceneTraversal(uintptr_t camera, uintptr_t sceneNode,
                                       uintptr_t cullingProcess, uint8_t flag);
        using SceneTraversal_t = void(*)(uintptr_t camera, uintptr_t sceneNode,
                                         uintptr_t cullingProcess, uint8_t flag);
        static inline SceneTraversal_t g_originalSceneTraversal = nullptr;

        // Accumulator pointer stored by HookPerRendererRender for use by traversal hook
        static inline uintptr_t g_currentAccumulator = 0;

        // ── Helper function pointers for RT selection and VR overlay population ──
        // FUN_140b04a90 — returns the "screen" RT index based on display_mode, vrMode, etc.
        using GetScreenRT_t = int(*)(uintptr_t);
        static inline GetScreenRT_t g_getScreenRT = nullptr;

        // FUN_140b01f10 — populates VR overlay position/UV data in renderData.
        // Normally only called when vrMode==1; missing for vrMode==0 → stale overlay data.
        using PreCompositing_t = void(*)(uintptr_t, uintptr_t);
        static inline PreCompositing_t g_preCompositing = nullptr;

        // Approach implementations (1-4, used in UpdateModelTransform hook)
        void ApproachA_ForceFlat(uintptr_t inv3dMgr, uintptr_t loadedModel);
        void ApproachB_OverrideFrustum(uintptr_t inv3dMgr, uintptr_t loadedModel);
        void ApproachC_RecomputePosition(uintptr_t inv3dMgr, uintptr_t loadedModel);
        void ApproachD_SwapCamera(uintptr_t inv3dMgr, uintptr_t loadedModel);

        // Helpers
        void* GetInventoryRenderer(uintptr_t inv3dMgr);

        // Interface3D helper function pointers (lazy-init)
        using I3DGetByName_t = void* (*)(const RE::BSFixedString&);
        static inline I3DGetByName_t g_I3DGetByName = nullptr;

        // Cached flat-screen camera frustum values (captured once)
        bool    _flatFrustumCaptured = false;
        float   _flatFrustumLeft     = 0.0f;
        float   _flatFrustumRight    = 0.0f;
        float   _flatFrustumTop      = 0.0f;
        float   _flatFrustumBottom   = 0.0f;
        float   _flatFrustumNear     = 0.0f;
        float   _flatFrustumFar      = 0.0f;

        // Logging throttle
        int _logCount = 0;
        static constexpr int MAX_LOG_ENTRIES = 30;

        // Render hook log throttle (separate from UpdateModelTransform)
        int _renderLogCount = 0;
        static constexpr int MAX_RENDER_LOG = 10;

        // Per-frame state
        bool _frameUpdateVerified = false;

        // ── Renderer layout offsets (from Ghidra analysis of F4VR 1.2.72) ──
        static constexpr int OFF_RENDERER_CLEAR_R     = 0x48;   // float - clear color red
        static constexpr int OFF_RENDERER_CLEAR_G     = 0x4C;   // float - clear color green
        static constexpr int OFF_RENDERER_CLEAR_B     = 0x50;   // float - clear color blue
        static constexpr int OFF_RENDERER_CLEAR_A     = 0x54;   // float - clear color alpha
        static constexpr int OFF_RENDERER_ACTIVE     = 0x5C;   // bool - renderer active
        static constexpr int OFF_RENDERER_ENABLED    = 0x5D;   // bool - rendering enabled
        static constexpr int OFF_RENDERER_CLEAR_RT    = 0x60;   // bool - clearRTOffscreen (clear render target for offscreen 3D)
        static constexpr int OFF_RENDERER_CLEAR_DS    = 0x62;   // bool - clearDepthStencilOffscreen (clear depth between scene & offscr3D)
        static constexpr int OFF_RENDERER_CAMERA_SEL = 0x68;   // bool - use far camera
        static constexpr int OFF_RENDERER_DISPLAY    = 0x70;   // int  - display mode
        static constexpr int OFF_RENDERER_VR_MODE    = 0x78;   // int  - 0=flat, 1=VR proj, 2=VR non-proj, 3=VR3
        static constexpr int OFF_RENDERER_RENDER_SEL = 0x7C;   // int  - 0=none, 1=worldRoot, 2+=screen3D
        static constexpr int OFF_RENDERER_WORLD_ROOT = 0x88;   // NiNode* - world attached element root
        static constexpr int OFF_RENDERER_SCREEN_3D  = 0x90;   // NiAVObject* - screen attached 3D
        static constexpr int OFF_RENDERER_OFFSCR_3D  = 0x98;   // NiAVObject* - offscreen 3D
        static constexpr int OFF_RENDERER_FIELD_74   = 0x74;   // int   - additional compositing flag (triggers effect 0xC)
        static constexpr int OFF_RENDERER_FIELD_C0   = 0xC0;   // void* - secondary 3D object (used in bVar5/RT selection)
        static constexpr int OFF_RENDERER_FIELD_C8   = 0xC8;   // bool  - secondary 3D active (used in bVar5/RT selection)
        static constexpr int OFF_RENDERER_FIELD_D8   = 0xD8;   // uint  - VR compositing parameter (passed as global)
        static constexpr int OFF_RENDERER_CAM_PIPBOY = 0x1B8;  // NiCamera* - Pipboy camera (flat)
        static constexpr int OFF_RENDERER_CAM_WORLD  = 0x1C0;  // NiCamera* - World Space UI camera
        static constexpr int OFF_RENDERER_CAM_NATIVE = 0x1C8;  // NiCamera* - Native camera
        static constexpr int OFF_RENDERER_CAM_FAR    = 0x1D0;  // NiCamera* - Native/Far camera
        static constexpr int OFF_RENDERER_RT_OFFSCR  = 0x1D8;  // int  - offscreen RT override (-1=auto)
        static constexpr int OFF_RENDERER_RT_SCREEN  = 0x1DC;  // int  - screen RT override (-1=auto)
        static constexpr int OFF_RENDERER_NAME       = 0x220;  // BSFixedString - renderer name
        static constexpr int OFF_RENDERER_OVERLAY_A  = 0x238;  // BSFixedString - overlay/parent renderer name A
        static constexpr int OFF_RENDERER_OVERLAY_B  = 0x240;  // BSFixedString - overlay/parent renderer name B

        // NiCamera frustum offsets (from NiFrustum struct at camera+0x200)
        static constexpr int OFF_CAM_FRUSTUM_PTR     = 0x200;  // float* - pointer to NiFrustum {left, right, top, bottom, near, far}
        static constexpr int OFF_CAM_VIEWPORT_LEFT   = 0x214;  // float
        static constexpr int OFF_CAM_VIEWPORT_RIGHT  = 0x218;  // float
        static constexpr int OFF_CAM_VIEWPORT_TOP    = 0x21C;  // float
        static constexpr int OFF_CAM_VIEWPORT_BOTTOM = 0x220;  // float

        // Inventory3DManager offsets
        static constexpr int OFF_INV3D_RENDERER_NAME = 0xC0;   // BSFixedString - renderer name for GetByName

        // ── D3D11 alpha fixup resources (mode 16) ──
        // Draws a fullscreen triangle with alpha-only write mask to set alpha=1.0
        // on the offscreen RT after the 3D render, preserving RGB values.
        static inline bool s_d3dInitialized = false;
        static inline bool s_d3dInitFailed = false;
        static inline ID3D11VertexShader* s_alphaVS = nullptr;
        static inline ID3D11PixelShader* s_alphaPS = nullptr;            // blanket alpha=1
        static inline ID3D11PixelShader* s_contentAwarePS = nullptr;     // content-aware alpha
        static inline ID3D11BlendState* s_alphaBlendState = nullptr;
        static inline ID3D11DepthStencilState* s_alphaDepthState = nullptr;
        static inline ID3D11RasterizerState* s_alphaRastState = nullptr;

        // Staging texture: copy of offscreen RT used as input for content-aware PS
        static inline ID3D11Texture2D* s_stagingTex = nullptr;
        static inline ID3D11ShaderResourceView* s_stagingSRV = nullptr;

        // CPU-readable staging texture for one-shot diagnostic readback
        static inline ID3D11Texture2D* s_cpuStagingTex = nullptr;
        static inline int s_diagnosticCount = 0;     // counts composite calls
        static inline bool s_diagnosticDone = false;  // set after final diagnostic

        // Additive blend state for custom compositing
        static inline ID3D11BlendState* s_additiveBlendState = nullptr;

        // Passthrough PS: samples texture and outputs as-is
        static inline ID3D11PixelShader* s_passthroughPS = nullptr;

        // Constant buffer for passthrough PS (src/dst dimensions)
        static inline ID3D11Buffer* s_dimsCB = nullptr;

        // BSGraphics::RendererData layout (from CommonLibF4)
        // Singleton at VR offset 0x60F3CE8 (pointer-to-pointer)
        //   +0x48 = ID3D11Device*
        //   +0x50 = ID3D11DeviceContext*
        //   +0x0A58 = RenderTarget[101] array (each 0x30 bytes)
        //     RT[N]+0x00 = ID3D11Texture2D*
        //     RT[N]+0x10 = ID3D11RenderTargetView*
        //     RT[N]+0x18 = ID3D11ShaderResourceView*
        static constexpr uintptr_t BSGFX_RENDERER_DATA = 0x60F3CE8;

        static bool InitD3D11Resources();
        static void DoAlphaFixup(int rtIndex);
        static void DoAdditiveComposite(int srcRT, int dstRT);
        static void DiagnosticReadback(int rtIndex);
    };
}
