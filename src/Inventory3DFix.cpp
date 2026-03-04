#include "Inventory3DFix.h"
#include <d3dcompiler.h>

namespace inv3d
{
    // ════════════════════════════════════════════════════════════════════════
    // VR address constants (Ghidra-verified for F4VR 1.2.72)
    // ════════════════════════════════════════════════════════════════════════

    // Inventory3DManager::UpdateModelTransform(LoadedInventoryModel&)
    static constexpr uintptr_t VR_UPDATE_MODEL_TRANSFORM = 0x1309bb0;

    // Interface3D::Renderer::GetByName(BSFixedString&)
    static constexpr uintptr_t VR_I3D_GET_BY_NAME = 0xb00270;

    // VR dimension globals (HMD per-eye resolution — set from swap chain)
    static constexpr uintptr_t VR_SCREEN_WIDTH_GLOBAL  = 0x65a2b30;  // DAT_1465a2b30
    static constexpr uintptr_t VR_SCREEN_HEIGHT_GLOBAL = 0x65a2b34;  // DAT_1465a2b34

    // Flat-screen dimension globals
    static constexpr uintptr_t FLAT_SCREEN_WIDTH_GLOBAL  = 0x3926690;  // DAT_143926690
    static constexpr uintptr_t FLAT_SCREEN_HEIGHT_GLOBAL = 0x39266a8;  // DAT_1439266a8

    // Interface3D::Renderer::Offscreen_GetRenderTargetWidth/Height
    static constexpr uintptr_t VR_GET_RT_WIDTH  = 0xb009b0;
    static constexpr uintptr_t VR_GET_RT_HEIGHT = 0xb009d0;

    // Interface3D::Renderer::GetWorldPointFromScreenPoint
    static constexpr uintptr_t VR_SCREEN_TO_WORLD = 0xb01740;

    // FUN_140b04a90 — RT index selector (returns "screen" RT based on display/vrMode)
    static constexpr uintptr_t VR_GET_SCREEN_RT = 0xb04a90;

    // FUN_140b01f10 — pre-compositing: populates VR overlay UV/position data in renderData
    // Only called when vrMode==1 inside FUN_140b02e30, but needed for VR compositing
    static constexpr uintptr_t VR_PRE_COMPOSITING = 0xb01f10;

    // nsInventory3DManager::CalcWorldBound
    static constexpr uintptr_t VR_CALC_WORLD_BOUND = 0x130b670;

    // ════════════════════════════════════════════════════════════════════════
    // Call sites for hooking
    // ════════════════════════════════════════════════════════════════════════

    // CALL UpdateModelTransform inside FinishItemLoad (FUN_141309560)
    static constexpr uintptr_t CALL_SITE_FINISH_ITEM_LOAD = 0x130995B;

    // CALL FUN_140b02e30 (per-renderer render) inside FUN_140b02c20 (render dispatch loop)
    // This is the main render loop that iterates over all Interface3D renderers.
    static constexpr uintptr_t CALL_SITE_PER_RENDERER_RENDER = 0xb02cf7;

    // CALL FUN_1427b08c0 (composite) inside FUN_140b02e30 (per-renderer render)
    // This copies the offscreen RT (0x3F) onto the screen RT (0x40) using
    // ImageSpace effect 0xF (alpha-blending composite).
    static constexpr uintptr_t CALL_SITE_COMPOSITE = 0xb03497;

    // ImageSpaceManager::Copy — raw texture copy using BSImagespaceShaderCopy
    static constexpr uintptr_t VR_ISM_COPY = 0x27b0880;

    // CALL FUN_140b038f0 (VR compositing) inside FUN_140b02e30 (per-renderer render)
    // Projects the screen RT content into VR space, also reads offscreen RT.
    static constexpr uintptr_t CALL_SITE_VR_COMPOSITING = 0xb034f9;

    // MOV byte ptr [RSP+0x30], 0x1 — sets param_7=1 for FUN_140b03d60 call
    // at 0x140b0344b inside FUN_140b02e30. The immediate value byte (0x01) is
    // at offset +4 from the instruction start. param_7 selects:
    //   0 = FULL deferred render (6 G-buffer RTs, BSLightingShader works)
    //   1 = SIMPLE forward render (BSEffectShader only — BSLightingShader fails)
    // Patching this byte from 0x01 to 0x00 enables deferred rendering for items.
    static constexpr uintptr_t PATCH_PARAM7_BYTE = 0xb0343e;

    // ── NEW: Forward render group injection ──
    // FUN_14281cc90 — forward render pass inside FUN_1427ff820 (the forward render function).
    // Renders shader groups: 0x22, 0x24, 0xB, 0xA, 3, 4, 0x11, 0x12.
    // Notably MISSING groups 1 and 2 (BSLightingShader opaque geometry).
    // CALL to FUN_14281cc90 at 0x1427ff876 inside FUN_1427ff820.
    static constexpr uintptr_t CALL_SITE_FORWARD_RENDER_PASS = 0x27ff876;

    // FUN_14281e400 — generic "render shader group" function.
    // Signature: void(BSShaderAccumulator* acc, uint groupID, context* ctx, bool flag)
    // Called by both forward and deferred paths to render specific shader groups.
    static constexpr uintptr_t VR_RENDER_SHADER_GROUP = 0x281e400;

    // FUN_1427ff370 — scene graph traversal (culling + geometry registration).
    // CALL at 0x140b04095 inside FUN_140b03d60. Traversal walks the scene graph,
    // frustum-culls geometry, and registers visible BSGeometry into the accumulator's
    // shader groups. BSLightingShader only registers into groups 1+2 when
    // accumulator+0xf669 == true (deferred mode). We hook to force f669=true.
    static constexpr uintptr_t CALL_SITE_SCENE_TRAVERSAL = 0xb04095;

    // ════════════════════════════════════════════════════════════════════════
    // Installation
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::Install(int fixMode)
    {
        s_fixMode = fixMode;

        if (s_fixMode == 0) {
            spdlog::info("[Inv3D] Fix disabled (iInventory3DFixMode=0)");
            return;
        }

        spdlog::info("[Inv3D] Installing Inventory3DManager fix (mode={})", s_fixMode);

        // Initialize function pointers
        g_I3DGetByName = reinterpret_cast<I3DGetByName_t>(REL::Offset(VR_I3D_GET_BY_NAME).address());
        g_getScreenRT = reinterpret_cast<GetScreenRT_t>(REL::Offset(VR_GET_SCREEN_RT).address());
        g_preCompositing = reinterpret_cast<PreCompositing_t>(REL::Offset(VR_PRE_COMPOSITING).address());

        auto& trampoline = F4SE::GetTrampoline();

        // Hook 1: UpdateModelTransform (for approaches 1-4 and diagnostics)
        {
            REL::Relocation<std::uintptr_t> callSite{ REL::Offset(CALL_SITE_FINISH_ITEM_LOAD) };
            auto* callByte = reinterpret_cast<uint8_t*>(callSite.address());
            if (*callByte != 0xE8) {
                spdlog::error("[Inv3D] UpdateModelTransform call site at {:X} is not E8 (found {:02X})",
                    callSite.address(), *callByte);
                return;
            }
            g_originalUpdateModelTransform = reinterpret_cast<UpdateModelTransform_t>(
                trampoline.write_call<5>(callSite.address(), &HookUpdateModelTransform));
            spdlog::info("[Inv3D] Hooked UpdateModelTransform call at {:X} (mode={})",
                callSite.address(), s_fixMode);
        }

        // Hook 2: Per-renderer render (for approaches 5-16)
        if (s_fixMode >= 5 && s_fixMode <= 16) {
            REL::Relocation<std::uintptr_t> callSite{ REL::Offset(CALL_SITE_PER_RENDERER_RENDER) };
            auto* callByte = reinterpret_cast<uint8_t*>(callSite.address());
            if (*callByte != 0xE8) {
                spdlog::error("[Inv3D] Per-renderer render call site at {:X} is not E8 (found {:02X})",
                    callSite.address(), *callByte);
                return;
            }
            g_originalPerRendererRender = reinterpret_cast<PerRendererRender_t>(
                trampoline.write_call<5>(callSite.address(), &HookPerRendererRender));
            spdlog::info("[Inv3D] Hooked per-renderer render call at {:X} (mode {})",
                callSite.address(), s_fixMode);
        }

        // Hook 3: Composite (for modes 14-16)
        // Mode 14: passthrough (diagnostic), Mode 15: raw copy, Mode 16: alpha fixup after
        if (s_fixMode >= 14 && s_fixMode <= 16) {
            g_imageSpcCopy = reinterpret_cast<ISMCopy_t>(REL::Offset(VR_ISM_COPY).address());
            spdlog::info("[Inv3D] ImageSpaceManager::Copy at {:X}", reinterpret_cast<uintptr_t>(g_imageSpcCopy));

            REL::Relocation<std::uintptr_t> callSite{ REL::Offset(CALL_SITE_COMPOSITE) };
            auto* callByte = reinterpret_cast<uint8_t*>(callSite.address());
            if (*callByte != 0xE8) {
                spdlog::error("[Inv3D] Composite call site at {:X} is not E8 (found {:02X})",
                    callSite.address(), *callByte);
                return;
            }
            g_originalComposite = reinterpret_cast<Composite_t>(
                trampoline.write_call<5>(callSite.address(), &HookComposite));
            spdlog::info("[Inv3D] Hooked composite call at {:X} (mode {})",
                callSite.address(), s_fixMode);
        }

        // Hook 4: VR compositing (diagnostic only — NOT used for fix)
        // Only installed for mode 14 as a passthrough diagnostic.
        if (s_fixMode == 14) {
            REL::Relocation<std::uintptr_t> callSite{ REL::Offset(CALL_SITE_VR_COMPOSITING) };
            auto* callByte = reinterpret_cast<uint8_t*>(callSite.address());
            if (*callByte != 0xE8) {
                spdlog::error("[Inv3D] VR compositing call site at {:X} is not E8 (found {:02X})",
                    callSite.address(), *callByte);
                return;
            }
            g_originalVRCompositing = reinterpret_cast<VRCompositing_t>(
                trampoline.write_call<5>(callSite.address(), &HookVRCompositing));
            spdlog::info("[Inv3D] Hooked VR compositing call at {:X} (mode {})",
                callSite.address(), s_fixMode);
        }

        // Hook 5: Forward render pass (mode 16) — inject BSLightingShader groups
        // The forward render (param_7=1) skips BSLightingShader groups 1 and 2.
        // This hook adds them after the normal forward render, making opaque items visible.
        if (s_fixMode == 16) {
            g_renderShaderGroup = reinterpret_cast<RenderShaderGroup_t>(
                REL::Offset(VR_RENDER_SHADER_GROUP).address());
            spdlog::info("[Inv3D] RenderShaderGroup at {:X}",
                reinterpret_cast<uintptr_t>(g_renderShaderGroup));

            REL::Relocation<std::uintptr_t> callSite{ REL::Offset(CALL_SITE_FORWARD_RENDER_PASS) };
            auto* callByte = reinterpret_cast<uint8_t*>(callSite.address());
            if (*callByte != 0xE8) {
                spdlog::error("[Inv3D] Forward render pass call site at {:X} is not E8 (found {:02X})",
                    callSite.address(), *callByte);
                return;
            }
            g_originalForwardRenderPass = reinterpret_cast<ForwardRenderPass_t>(
                trampoline.write_call<5>(callSite.address(), &HookForwardRenderPass));
            spdlog::info("[Inv3D] Hooked forward render pass at {:X} (mode {})",
                callSite.address(), s_fixMode);

            // Hook 6: Scene graph traversal — force f669=true before traversal
            // FUN_140b03d60 sets acc+0xf669=false (forward mode), then calls
            // FUN_1427ff370 (traversal). We intercept the traversal call and
            // set f669=true so BSLightingShader geometry registers into groups 1+2.
            REL::Relocation<std::uintptr_t> travCallSite{ REL::Offset(CALL_SITE_SCENE_TRAVERSAL) };
            auto* travCallByte = reinterpret_cast<uint8_t*>(travCallSite.address());
            if (*travCallByte != 0xE8) {
                spdlog::error("[Inv3D] Traversal call site at {:X} is not E8 (found {:02X})",
                    travCallSite.address(), *travCallByte);
                return;
            }
            g_originalSceneTraversal = reinterpret_cast<SceneTraversal_t>(
                trampoline.write_call<5>(travCallSite.address(), &HookSceneTraversal));
            spdlog::info("[Inv3D] Hooked scene traversal at {:X} (mode {})",
                travCallSite.address(), s_fixMode);

            // NOTE: VirtualProtect for param_7 byte is done lazily in
            // HookPerRendererRender, because Install() runs before config loads
            // (onModLoaded → default mode, not mode 16). The runtime hook checks
            // s_fixMode and does VirtualProtect on first call when mode 16 is active.
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Hook: Per-renderer render (Approach E)
    //
    // Intercepts FUN_140b02e30 for the PipboyMenu renderer and forces vrMode=0
    // during the ENTIRE render call. This ensures the offscreen 3D rendering
    // uses the same flat parameters (depth stencil 3, RT 0x3F, Pipboy camera)
    // as the position calculation in UpdateModelTransform.
    //
    // Without this fix, vrMode may be 1/2 during rendering, causing:
    // - Different depth stencil target (1 vs 3) → depth buffer conflict
    // - Different render targets (0x37/0x44 vs 0x3F) → wrong RT
    // - Different camera (Native vs Pipboy) → wrong projection
    // ════════════════════════════════════════════════════════════════════════

    // Check if a renderer name is one used by Inventory3DManager for 3D item previews
    static bool IsInventoryRenderer(const char* name)
    {
        if (!name || name[0] == '\0') return false;
        // Known Inventory3DManager renderer names (from Ghidra + runtime logs)
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
    // Scene graph helpers for mode 16: walk NiNode tree, disable envmap
    // materials before deferred render to prevent null texture crashes.
    // ════════════════════════════════════════════════════════════════════════

    // Basic pointer validity: in user-space range and properly aligned.
    // All C++ heap objects on x64 are at least 8-byte aligned.
    static bool IsValidPtr(uintptr_t p) {
        return p > 0x10000 && p < 0x7FFFFFFFFFFF && (p & 0x7) == 0;
    }

    // Check if pointer is within the game exe's loaded image (vtable/rdata range).
    // Must use runtime base — ASLR randomizes it (NOT always 0x140000000).
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
    static int s_walkLogCount = 0;

    // kEnvMap = bit 7 of BSShaderProperty::EShaderPropertyFlag
    static constexpr uint64_t kEnvMapBit = 1ULL << 7;

    // Max recursion depth for scene graph walk (prevents stack overflow on corrupt trees)
    static constexpr int MAX_WALK_DEPTH = 16;

    // Recursively walk scene graph from obj.
    // IMPORTANT: NiNode and BSGeometry checks are MUTUALLY EXCLUSIVE.
    // NiNode (VR) has padding at +0x120-0x160, then children at +0x160.
    // BSGeometry has modelBound at +0x120, properties at +0x130.
    // Reading +0x130 on a NiNode reads PADDING (garbage) → crash.
    // So: check for NiNode FIRST. If it has valid children, recurse.
    // Only fall through to BSGeometry property check if NOT a NiNode.
    static void WalkSceneGraph(uintptr_t obj, int depth = 0) {
        if (!IsValidPtr(obj) || s_numShaderSaves >= MAX_SHADER_SAVES || depth > MAX_WALK_DEPTH)
            return;

        // Validate object's own vtable — every NiAVObject subclass has an exe-range vtable at +0x0
        uintptr_t objVtbl = *reinterpret_cast<uintptr_t*>(obj);
        if (!IsExePtr(objVtbl))
            return;

        // ── NiNode path: children NiTObjectArray at +0x160 (VR) ──
        // NiTArray layout: vtable(+0), _data(+8), _capacity(+10), _size(+14)
        // NiNode/BSFadeNode/ShadowSceneNode all have this.
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
            return;  // This is a NiNode — do NOT read properties at +0x130
        }

        // ── BSGeometry path: check both properties[0] and properties[1] ──
        // VR shifts BSGeometry members by +0x40 from non-VR layout:
        //   properties[0] at +0x170 (non-VR: +0x130) — typically NiAlphaProperty
        //   properties[1] at +0x178 (non-VR: +0x138) — typically BSShaderProperty
        // We check both slots but use strict validation to avoid modifying NiAlphaProperty:
        //   1. Alpha float at +0x28: BSShaderProperty has 0.0-1.0, NiAlphaProperty has garbage
        //   2. Flags at +0x30: BSShaderProperty flags fit in 40 bits (upper 24 bits = 0),
        //      NiAlphaProperty "flags" are unrelated data (pointers, ASCII → upper bits nonzero)
        static constexpr int VR_PROP_OFFSETS[] = { 0x170, 0x178 };

        for (int propOff : VR_PROP_OFFSETS) {
            uintptr_t prop = *reinterpret_cast<uintptr_t*>(obj + propOff);
            if (!IsValidPtr(prop)) continue;
            uintptr_t propVtbl = *reinterpret_cast<uintptr_t*>(prop);
            if (!IsExePtr(propVtbl)) continue;

            // Check 1: BSShaderProperty::alpha at +0x28 must be a valid float (0.0-1.0)
            float alpha = *reinterpret_cast<float*>(prop + 0x28);
            if (alpha < -0.1f || alpha > 2.0f) continue;

            // Check 2: BSShaderProperty::flags at +0x30 must fit in 40 bits.
            // Valid flags are a bitfield (e.g. 0x0000008180400291).
            // NiAlphaProperty has unrelated data here (pointers like 0x7FF700B54DB8).
            uint64_t flags = *reinterpret_cast<uint64_t*>(prop + 0x30);
            if ((flags >> 40) != 0) continue;  // Not BSShaderProperty — skip

            if (s_walkLogCount < 20) {
                auto* nameBS = reinterpret_cast<RE::BSFixedString*>(obj + 0x10);
                const char* nm = (nameBS && nameBS->c_str()) ? nameBS->c_str() : "(null)";
                uint32_t techID = *reinterpret_cast<uint32_t*>(prop + 0xD8);
                spdlog::info("[Inv3D-Walk] '{}' +0x{:X} prop={:X} flags=0x{:X} tech=0x{:X} envmap={}",
                    nm, propOff, prop, flags, techID, (flags & kEnvMapBit) ? 1 : 0);
                s_walkLogCount++;
            }

            if ((flags & kEnvMapBit) && s_numShaderSaves < MAX_SHADER_SAVES) {
                auto& ss = s_shaderSaves[s_numShaderSaves++];
                ss.shaderPropAddr = prop;
                ss.originalFlags = flags;
                ss.originalTechID = *reinterpret_cast<uint32_t*>(prop + 0xD8);

                *reinterpret_cast<uint64_t*>(prop + 0x30) = flags & ~kEnvMapBit;
                *reinterpret_cast<uint32_t*>(prop + 0xD8) = ss.originalTechID & ~0x3Fu;

                if (s_walkLogCount < 25) {
                    spdlog::info("[Inv3D-Walk] DISABLED envmap: flags 0x{:X}→0x{:X} tech 0x{:X}→0x{:X}",
                        flags, flags & ~kEnvMapBit, ss.originalTechID, ss.originalTechID & ~0x3Fu);
                    s_walkLogCount++;
                }
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

    // SEH-safe walk wrapper — catches access violations from bad pointers in scene graph.
    // Must be a separate __declspec(noinline) function because __try/__except cannot
    // coexist with C++ objects that have destructors in the same function.
    __declspec(noinline) static bool TryProtectedWalk(uintptr_t obj)
    {
        __try {
            WalkSceneGraph(obj, 0);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SEH-safe render wrapper
    using RawRenderFunc = void(*)(uintptr_t, uintptr_t);
    __declspec(noinline) static bool TryProtectedRender(
        RawRenderFunc func, uintptr_t renderer, uintptr_t renderData)
    {
        __try {
            func(renderer, renderData);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SEH-safe wrapper for rendering shader groups (BSLightingShader forward injection)
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

    void Inventory3DFix::HookPerRendererRender(uintptr_t renderer, uintptr_t renderData)
    {
        auto& self = GetSingleton();

        // Check if this renderer is used by Inventory3DManager
        auto* namePtr = reinterpret_cast<RE::BSFixedString*>(renderer + OFF_RENDERER_NAME);
        const char* rendererName = (namePtr && namePtr->c_str()) ? namePtr->c_str() : "";
        bool isInventory = IsInventoryRenderer(rendererName);

        if (isInventory) {
            auto* vrModePtr = reinterpret_cast<int*>(renderer + OFF_RENDERER_VR_MODE);
            int savedVrMode = *vrModePtr;

            // Read key state
            int display   = *reinterpret_cast<int*>(renderer + OFF_RENDERER_DISPLAY);
            int renderSel = *reinterpret_cast<int*>(renderer + OFF_RENDERER_RENDER_SEL);
            auto offscr3D = *reinterpret_cast<uintptr_t*>(renderer + OFF_RENDERER_OFFSCR_3D);
            auto screen3D = *reinterpret_cast<uintptr_t*>(renderer + OFF_RENDERER_SCREEN_3D);
            auto field_C0 = *reinterpret_cast<uintptr_t*>(renderer + OFF_RENDERER_FIELD_C0);
            auto field_C8 = *reinterpret_cast<uint8_t*>(renderer + OFF_RENDERER_FIELD_C8);
            int rtOff     = *reinterpret_cast<int*>(renderer + OFF_RENDERER_RT_OFFSCR);
            int rtScr     = *reinterpret_cast<int*>(renderer + OFF_RENDERER_RT_SCREEN);
            auto enabled  = *reinterpret_cast<uint8_t*>(renderer + OFF_RENDERER_ENABLED);

            // Additional diagnostic fields
            int field_74  = *reinterpret_cast<int*>(renderer + OFF_RENDERER_FIELD_74);
            auto field_d8 = *reinterpret_cast<uint32_t*>(renderer + OFF_RENDERER_FIELD_D8);
            int screenRT  = g_getScreenRT ? g_getScreenRT(renderer) : -999;

            // Read overlay-related BSFixedStrings
            auto* overlayA = reinterpret_cast<RE::BSFixedString*>(renderer + OFF_RENDERER_OVERLAY_A);
            auto* overlayB = reinterpret_cast<RE::BSFixedString*>(renderer + OFF_RENDERER_OVERLAY_B);
            const char* ovrA = (overlayA && overlayA->c_str()) ? overlayA->c_str() : "(null)";
            const char* ovrB = (overlayB && overlayB->c_str()) ? overlayB->c_str() : "(null)";

            // Compute bVar5 (same logic as FUN_140b02e30) — determines if offscreen 3D renders
            bool bVar5 = (enabled != 0) && (offscr3D != 0 || (field_C0 != 0 && field_C8 != 0));

            // Log offscreen 3D model LOCAL and WORLD positions at render time
            // NiAVObject: +0x60=local.translate, +0xA0=world.translate
            if (bVar5 && offscr3D != 0 && self._renderLogCount < MAX_RENDER_LOG) {
                float olx = *reinterpret_cast<float*>(offscr3D + 0x60);
                float oly = *reinterpret_cast<float*>(offscr3D + 0x64);
                float olz = *reinterpret_cast<float*>(offscr3D + 0x68);
                float owx = *reinterpret_cast<float*>(offscr3D + 0xA0);
                float owy = *reinterpret_cast<float*>(offscr3D + 0xA4);
                float owz = *reinterpret_cast<float*>(offscr3D + 0xA8);
                // Camera: Pipboy camera LOCAL and WORLD positions
                auto camPip = *reinterpret_cast<uintptr_t*>(renderer + OFF_RENDERER_CAM_PIPBOY);
                float clx = 0, cly = 0, clz = 0, cwx = 0, cwy = 0, cwz = 0;
                if (camPip > 0x10000) {
                    clx = *reinterpret_cast<float*>(camPip + 0x60);
                    cly = *reinterpret_cast<float*>(camPip + 0x64);
                    clz = *reinterpret_cast<float*>(camPip + 0x68);
                    cwx = *reinterpret_cast<float*>(camPip + 0xA0);
                    cwy = *reinterpret_cast<float*>(camPip + 0xA4);
                    cwz = *reinterpret_cast<float*>(camPip + 0xA8);
                }
                spdlog::info("[Inv3D] '{}' RENDER: offscr3D LOCAL=({:.2f},{:.2f},{:.2f}) "
                    "WORLD=({:.2f},{:.2f},{:.2f})",
                    rendererName, olx, oly, olz, owx, owy, owz);
                spdlog::info("[Inv3D] '{}' RENDER: cam LOCAL=({:.2f},{:.2f},{:.2f}) "
                    "WORLD=({:.2f},{:.2f},{:.2f}) delta_world=({:.2f},{:.2f},{:.2f})",
                    rendererName, clx, cly, clz, cwx, cwy, cwz,
                    owx-cwx, owy-cwy, owz-cwz);
                // Also log camera world rotation row 1 (look direction hint)
                if (camPip > 0x10000) {
                    // NiTransform world at +0x70, NiMatrix3 at +0x70
                    // Row 0: +0x70..+0x7F, Row 1: +0x80..+0x8F, Row 2: +0x90..+0x9F
                    float r0x = *reinterpret_cast<float*>(camPip + 0x70);
                    float r0y = *reinterpret_cast<float*>(camPip + 0x74);
                    float r0z = *reinterpret_cast<float*>(camPip + 0x78);
                    float r1x = *reinterpret_cast<float*>(camPip + 0x80);
                    float r1y = *reinterpret_cast<float*>(camPip + 0x84);
                    float r1z = *reinterpret_cast<float*>(camPip + 0x88);
                    float r2x = *reinterpret_cast<float*>(camPip + 0x90);
                    float r2y = *reinterpret_cast<float*>(camPip + 0x94);
                    float r2z = *reinterpret_cast<float*>(camPip + 0x98);
                    spdlog::info("[Inv3D] '{}' cam worldRot: row0=({:.4f},{:.4f},{:.4f}) "
                        "row1=({:.4f},{:.4f},{:.4f}) row2=({:.4f},{:.4f},{:.4f})",
                        rendererName, r0x, r0y, r0z, r1x, r1y, r1z, r2x, r2y, r2z);
                }
            }

            // Log render state — when there's 3D content, non-zero vrMode, or first few frames
            bool shouldLog = (self._renderLogCount < 3) ||
                             (bVar5 && self._renderLogCount < MAX_RENDER_LOG) ||
                             (savedVrMode != 0 && self._renderLogCount < MAX_RENDER_LOG);

            if (shouldLog) {
                spdlog::info("[Inv3D] '{}' RENDER: vrMode={} display={} renderSel={} "
                    "screenRT=0x{:X} field_74={} field_d8=0x{:X} "
                    "offscr3D={:X} screen3D={:X} fC0={:X} fC8={} rtOff={} rtScr={} "
                    "enabled={} bVar5={} mode={}",
                    rendererName, savedVrMode, display, renderSel,
                    screenRT, field_74, field_d8,
                    offscr3D, screen3D, field_C0, field_C8, rtOff, rtScr,
                    enabled, bVar5, s_fixMode);

                self._renderLogCount++;
            }

            // ── MODE 5: Diagnostics only (no vrMode change) ──
            if (s_fixMode == 5) {
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
            }
            // ── MODE 6: Force vrMode=0 during render for all inventory renderers ──
            // This ensures the offscreen 3D rendering uses flat parameters (Pipboy camera,
            // flat dimensions, RT 0x3f/0x40, depth stencil 3) matching the position
            // computed by UpdateModelTransform (which also forces vrMode=0 in this mode).
            else if (s_fixMode == 6) {
                if (savedVrMode != 0) {
                    if (shouldLog) {
                        spdlog::info("[Inv3D-F] Forcing vrMode {} -> 0 for '{}' render", savedVrMode, rendererName);
                    }
                    *vrModePtr = 0;
                }
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
                if (savedVrMode != 0) {
                    *vrModePtr = savedVrMode;
                }
            }
            // ── MODE 7: Force vrMode=1 (full VR rendering pipeline) ──
            else if (s_fixMode == 7) {
                if (savedVrMode != 1) {
                    if (shouldLog) {
                        spdlog::info("[Inv3D-G] Forcing vrMode {} -> 1 for '{}' render", savedVrMode, rendererName);
                    }
                    *vrModePtr = 1;
                }
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
                if (savedVrMode != 1) {
                    *vrModePtr = savedVrMode;
                }
            }
            // ── MODE 8: Force display_mode=0 (skip VR compositing entirely) ──
            // Diagnostic: tests if the VR compositing step causes the occlusion.
            // Content is rendered to flat RTs but NOT projected into VR.
            else if (s_fixMode == 8) {
                auto* displayPtr = reinterpret_cast<int*>(renderer + OFF_RENDERER_DISPLAY);
                int savedDisplay = *displayPtr;
                if (savedDisplay != 0) {
                    if (shouldLog) {
                        spdlog::info("[Inv3D-H] Forcing display {} -> 0 for '{}' (skip VR compositing)", savedDisplay, rendererName);
                    }
                    *displayPtr = 0;
                }
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
                if (savedDisplay != 0) {
                    *displayPtr = savedDisplay;
                }
            }
            // ── MODE 9: Skip scene render (renderSel=0) to test depth occlusion ──
            else if (s_fixMode == 9) {
                auto* renderSelPtr = reinterpret_cast<int*>(renderer + OFF_RENDERER_RENDER_SEL);
                int savedRenderSel = *renderSelPtr;
                if (savedRenderSel != 0) {
                    if (shouldLog) {
                        spdlog::info("[Inv3D-I] Forcing renderSel {} -> 0 for '{}' (skip scene render, test depth occlusion)",
                            savedRenderSel, rendererName);
                    }
                    *renderSelPtr = 0;
                }
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
                if (savedRenderSel != 0) {
                    *renderSelPtr = savedRenderSel;
                }
            }
            // ── MODE 10: Set clearDepthStencilOffscreen=true ──
            else if (s_fixMode == 10) {
                auto* clearDSPtr = reinterpret_cast<uint8_t*>(renderer + OFF_RENDERER_CLEAR_DS);
                uint8_t savedClearDS = *clearDSPtr;
                if (shouldLog) {
                    // Log byte values at +0x5E through +0x6A for layout analysis
                    uint8_t b5E = *reinterpret_cast<uint8_t*>(renderer + 0x5E);
                    uint8_t b5F = *reinterpret_cast<uint8_t*>(renderer + 0x5F);
                    uint8_t b60 = *reinterpret_cast<uint8_t*>(renderer + 0x60);
                    uint8_t b61 = *reinterpret_cast<uint8_t*>(renderer + 0x61);
                    uint8_t b62 = *reinterpret_cast<uint8_t*>(renderer + 0x62);
                    uint8_t b63 = *reinterpret_cast<uint8_t*>(renderer + 0x63);
                    uint8_t b64 = *reinterpret_cast<uint8_t*>(renderer + 0x64);
                    uint8_t b68 = *reinterpret_cast<uint8_t*>(renderer + 0x68);
                    spdlog::info("[Inv3D-J] '{}' bytes: +5E={} +5F={} +60={} +61={} +62={} +63={} +64={} +68={}",
                        rendererName, b5E, b5F, b60, b61, b62, b63, b64, b68);
                }
                if (savedClearDS == 0) {
                    *clearDSPtr = 1;
                }
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
                if (savedClearDS == 0) {
                    *clearDSPtr = savedClearDS;
                }
            }
            // ── MODE 11: Force clearRTOffscreen + clearDepthStencilOffscreen ──
            else if (s_fixMode == 11) {
                auto* clearRTPtr = reinterpret_cast<uint8_t*>(renderer + OFF_RENDERER_CLEAR_RT);
                auto* clearDSPtr = reinterpret_cast<uint8_t*>(renderer + OFF_RENDERER_CLEAR_DS);
                uint8_t savedClearRT = *clearRTPtr;
                uint8_t savedClearDS = *clearDSPtr;
                if (shouldLog) {
                    spdlog::info("[Inv3D-K] '{}' clearRT={} clearDS={}, forcing both to 1", rendererName, savedClearRT, savedClearDS);
                }
                *clearRTPtr = 1;
                *clearDSPtr = 1;
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
                *clearRTPtr = savedClearRT;
                *clearDSPtr = savedClearDS;
            }
            // ── MODE 12: Override offscreen RT for VR compositing ──
            // Theory: VR compositing (effect 0x42) reads BOTH screenRT (0x40) and
            // offscreenRT (0x3F). The offscreenRT still has the 3D item after the
            // composite copies to 0x40. The shader may use RT 0x3F as a MASK,
            // cutting holes in the display where item geometry exists.
            // Fix: Set rtOverride (+0x1D8) to the screen RT so VR compositing reads
            // the screen RT for both inputs, preventing masking. The offscreen 3D
            // render still uses RT 0x3F (hardcoded as local_res10 for vrMode=0).
            else if (s_fixMode == 12) {
                auto* rtOverridePtr = reinterpret_cast<int*>(renderer + OFF_RENDERER_RT_OFFSCR);
                int savedRtOverride = *rtOverridePtr;
                // Get the screen RT index the same way the game does
                int screenRT = g_getScreenRT ? g_getScreenRT(renderer) : 0x40;
                if (shouldLog) {
                    spdlog::info("[Inv3D-L] '{}' rtOverride={} -> screenRT=0x{:X} (redirect VR compositing input)",
                        rendererName, savedRtOverride, screenRT);
                }
                *rtOverridePtr = screenRT;
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
                *rtOverridePtr = savedRtOverride;
            }
            // ── MODE 13: Force clear alpha=1.0 on offscreen RT ──
            // Theory: The composite (ImageSpace effect 0xF) uses alpha blending.
            // Opaque items (BSLightingShader) don't write alpha or write alpha=0,
            // so the composite blends them to transparent. Nuka Cherry (BSEffectShader
            // glass) writes alpha>0 → visible. Setting clear alpha=1 means all
            // unwritten pixels have alpha=1, and if the render state's WriteMask
            // excludes alpha, item pixels ALSO keep alpha=1 from the clear → visible.
            else if (s_fixMode == 13) {
                auto* clearAlphaPtr = reinterpret_cast<float*>(renderer + OFF_RENDERER_CLEAR_A);
                float savedClearAlpha = *clearAlphaPtr;
                // Also log the full clear color for diagnostics
                if (shouldLog) {
                    float cr = *reinterpret_cast<float*>(renderer + OFF_RENDERER_CLEAR_R);
                    float cg = *reinterpret_cast<float*>(renderer + OFF_RENDERER_CLEAR_G);
                    float cb = *reinterpret_cast<float*>(renderer + OFF_RENDERER_CLEAR_B);
                    spdlog::info("[Inv3D-M] '{}' clearColor=({:.3f},{:.3f},{:.3f},{:.3f}) -> forcing alpha=1.0",
                        rendererName, cr, cg, cb, savedClearAlpha);
                }
                *clearAlphaPtr = 1.0f;
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
                *clearAlphaPtr = savedClearAlpha;
            }
            // ── MODE 14: Diagnostic passthrough — verify all hooks work ──
            // Pure passthrough: all hooks installed but nothing modified.
            // If the Pipboy displays correctly (glass items visible, opaque invisible),
            // the hook infrastructure is verified.
            else if (s_fixMode == 14) {
                if (shouldLog) {
                    spdlog::info("[Inv3D-N] '{}' mode14: diagnostic passthrough (no modifications)",
                        rendererName);
                }
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
            }
            // ── MODE 15: Replace composite with raw Copy ──
            // Hooks the composite CALL to use ImageSpaceManager::Copy instead of
            // the alpha-blending effect 0xF. Copy does a raw overwrite (no alpha),
            // so items' RGB is directly on the screen RT. VR compositing is NOT
            // modified — it reads the raw offscreen RT (alpha=0 for opaque items)
            // and the screen RT (now containing items from the Copy).
            // Expected result: items visible on black background, Pipboy surface gone.
            else if (s_fixMode == 15) {
                if (shouldLog) {
                    spdlog::info("[Inv3D-O] '{}' mode15: composite replaced with Copy",
                        rendererName);
                }
                s_replaceComposite = true;
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
                s_replaceComposite = false;
            }
            // ── MODE 16: Forward render injection ──
            //
            // The forward render path (param_7=1) ONLY renders BSEffectShader
            // (group 10), skipping BSLightingShader groups 1+2 entirely.
            // Opaque items (weapons, armor, food) are invisible.
            //
            // Fix: Keep the forward path but inject BSLightingShader rendering:
            //   1. Set g_currentAccumulator from renderer+0x1B0
            //   2. Walk scene graph to disable envmap (prevent null texture crashes)
            //   3. Enable s_addLightingGroups → HookSceneTraversal sets f669=true
            //      so BSLightingShader geometry registers into groups 1+2
            //   4. Enable s_doAlphaFixup → HookComposite applies alpha fixup
            //      on offscreen RT before alpha-blending composite
            //   5. HookForwardRenderPass renders groups 0,1,2 after normal pass
            //
            // This avoids the deferred path crash (param_7=0 → null resource refs)
            // while making opaque items render via forward BSLightingShader.
            else if (s_fixMode == 16) {
                // Get BSShaderAccumulator from renderer+0x1B0
                // (Ghidra-verified: FUN_140b02e30 reads renderer+0x1B0 as accumulator)
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

                static int s_m16LogCount = 0;
                if (s_m16LogCount < 10) {
                    spdlog::info("[Inv3D-P] '{}' mode16: fwd inject acc={:X} "
                        "offscr3D={:X} vrMode={} enabled={} bVar5={}",
                        rendererName, acc, offscr3D, savedVrMode, enabled, bVar5);
                    s_m16LogCount++;
                }

                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }

                // Restore shader states (envmap flags etc.)
                RestoreShaderStates();

                s_addLightingGroups = false;
                s_doAlphaFixup = false;
                g_currentAccumulator = 0;
            }
            else {
                if (g_originalPerRendererRender) {
                    g_originalPerRendererRender(renderer, renderData);
                }
            }
        } else {
            // Not an inventory renderer — call original directly
            if (g_originalPerRendererRender) {
                g_originalPerRendererRender(renderer, renderData);
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Hook: UpdateModelTransform — dispatches to selected approach
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::HookUpdateModelTransform(uintptr_t inv3dMgr, uintptr_t loadedModel)
    {
        auto& self = GetSingleton();

        if (self._logCount < MAX_LOG_ENTRIES) {
            spdlog::info("[Inv3D] Hook fired! inv3dMgr={:X} loadedModel={:X} mode={}",
                inv3dMgr, loadedModel, s_fixMode);

            // ── PRE-TRANSFORM: Log model state BEFORE UpdateModelTransform ──
            uintptr_t preModelObj = *reinterpret_cast<uintptr_t*>(loadedModel + 0x10);
            float preBound = *reinterpret_cast<float*>(loadedModel + 0x28);
            if (preModelObj > 0x10000) {
                float px = *reinterpret_cast<float*>(preModelObj + 0x60);
                float py = *reinterpret_cast<float*>(preModelObj + 0x64);
                float pz = *reinterpret_cast<float*>(preModelObj + 0x68);
                float ps = *reinterpret_cast<float*>(preModelObj + 0x6C);

                // Try to read NiObjectNET name (BSFixedString at modelObj+0x10)
                const char* modelName = "(unknown)";
                auto* bsName = reinterpret_cast<RE::BSFixedString*>(preModelObj + 0x10);
                if (bsName && bsName->c_str() && bsName->c_str()[0] != '\0') {
                    modelName = bsName->c_str();
                }

                spdlog::info("[Inv3D]   PRE model({:X}): name='{}' pos=({:.4f},{:.4f},{:.4f}) scale={:.4f} "
                    "loadedModel+0x28={:.4f}",
                    preModelObj, modelName, px, py, pz, ps, preBound);

                // Dump loadedModel struct at key offsets (first 0x30 bytes and around 0x88)
                // loadedModel+0x00: likely vtable or smart pointer
                // loadedModel+0x08: some field
                // loadedModel+0x10: NiAVObject* (already known)
                // loadedModel+0x18: unknown
                // loadedModel+0x20: unknown
                // loadedModel+0x28: depth/bound radius
                // loadedModel+0x88: secondary NiNode* (used by UpdateModelTransform)
                auto u00 = *reinterpret_cast<uintptr_t*>(loadedModel + 0x00);
                auto u08 = *reinterpret_cast<uintptr_t*>(loadedModel + 0x08);
                auto u18 = *reinterpret_cast<uintptr_t*>(loadedModel + 0x18);
                auto u20 = *reinterpret_cast<uintptr_t*>(loadedModel + 0x20);
                auto u30 = *reinterpret_cast<float*>(loadedModel + 0x30);
                auto u88 = *reinterpret_cast<uintptr_t*>(loadedModel + 0x88);
                spdlog::info("[Inv3D]   loadedModel: +0={:X} +8={:X} +18={:X} +20={:X} "
                    "+30={:.4f} +88={:X}",
                    u00, u08, u18, u20, u30, u88);
            } else {
                spdlog::info("[Inv3D]   PRE modelObj=NULL, loadedModel+0x28={:.4f}", preBound);
            }

            // Dump renderer state
            void* renderer = self.GetInventoryRenderer(inv3dMgr);
            if (renderer) {
                auto b = reinterpret_cast<uintptr_t>(renderer);
                int vrMode = *reinterpret_cast<int*>(b + OFF_RENDERER_VR_MODE);
                int display = *reinterpret_cast<int*>(b + OFF_RENDERER_DISPLAY);
                int rtOff = *reinterpret_cast<int*>(b + OFF_RENDERER_RT_OFFSCR);
                int rtScr = *reinterpret_cast<int*>(b + OFF_RENDERER_RT_SCREEN);
                auto camPip = *reinterpret_cast<uintptr_t*>(b + OFF_RENDERER_CAM_PIPBOY);
                auto camNat = *reinterpret_cast<uintptr_t*>(b + OFF_RENDERER_CAM_NATIVE);
                auto offscr3D = *reinterpret_cast<uintptr_t*>(b + OFF_RENDERER_OFFSCR_3D);
                auto* namePtr = reinterpret_cast<RE::BSFixedString*>(inv3dMgr + OFF_INV3D_RENDERER_NAME);

                spdlog::info("[Inv3D]   renderer='{}' vrMode={} display={} rtOff={} rtScr={} "
                    "offscr3D={:X} camPip={:X} camNat={:X}",
                    namePtr ? namePtr->c_str() : "?", vrMode, display, rtOff, rtScr,
                    offscr3D, camPip, camNat);

                // Dump GetWidth/GetHeight results
                using GetDim_t = uint32_t(*)(void*);
                auto getWidth = reinterpret_cast<GetDim_t>(REL::Offset(VR_GET_RT_WIDTH).address());
                auto getHeight = reinterpret_cast<GetDim_t>(REL::Offset(VR_GET_RT_HEIGHT).address());
                uint32_t w = getWidth(renderer);
                uint32_t h = getHeight(renderer);
                spdlog::info("[Inv3D]   GetWidth={} GetHeight={}", w, h);

                // Dump Inv3DManager screen position fields
                float posX = *reinterpret_cast<float*>(inv3dMgr + 0x14);
                float posY = *reinterpret_cast<float*>(inv3dMgr + 0x18);
                float posZ = *reinterpret_cast<float*>(inv3dMgr + 0x1c);
                uint8_t flags = *reinterpret_cast<uint8_t*>(inv3dMgr + 0x10);
                spdlog::info("[Inv3D]   inv3dMgr flags=0x{:02X} screenPos=({:.4f}, {:.4f}, {:.4f})",
                    flags, posX, posY, posZ);
            }
        }

        // ── Post-call diagnostics: log model world position and bound changes ──
        auto logPostTransform = [&]() {
            if (self._logCount >= MAX_LOG_ENTRIES) return;

            // Read the NiAVObject* from loadedModel+0x10
            uintptr_t modelObj = *reinterpret_cast<uintptr_t*>(loadedModel + 0x10);
            float postBound = *reinterpret_cast<float*>(loadedModel + 0x28);
            if (modelObj > 0x10000) {
                float mx = *reinterpret_cast<float*>(modelObj + 0x60);
                float my = *reinterpret_cast<float*>(modelObj + 0x64);
                float mz = *reinterpret_cast<float*>(modelObj + 0x68);
                float mscale = *reinterpret_cast<float*>(modelObj + 0x6C);
                spdlog::info("[Inv3D]   POST model({:X}): pos=({:.4f},{:.4f},{:.4f}) scale={:.4f} "
                    "bound={:.4f}",
                    modelObj, mx, my, mz, mscale, postBound);
            }

            // Compare with renderer's offscr3D
            void* rend = self.GetInventoryRenderer(inv3dMgr);
            if (rend) {
                auto rb = reinterpret_cast<uintptr_t>(rend);
                uintptr_t offscr3D = *reinterpret_cast<uintptr_t*>(rb + OFF_RENDERER_OFFSCR_3D);
                if (offscr3D > 0x10000 && offscr3D != modelObj) {
                    float ox = *reinterpret_cast<float*>(offscr3D + 0x60);
                    float oy = *reinterpret_cast<float*>(offscr3D + 0x64);
                    float oz = *reinterpret_cast<float*>(offscr3D + 0x68);
                    spdlog::info("[Inv3D]   offscr3D({:X}) pos=({:.4f},{:.4f},{:.4f})",
                        offscr3D, ox, oy, oz);
                }
            }
        };

        // For mode 5, just log + call original (diagnostics only)
        if (s_fixMode == 5) {
            if (g_originalUpdateModelTransform) {
                g_originalUpdateModelTransform(inv3dMgr, loadedModel);
            }
            logPostTransform();
            return;
        }

        // For mode 6, force vrMode=0 during UpdateModelTransform
        // (same as Approach A but combined with render-time fix)
        if (s_fixMode == 6) {
            self.ApproachA_ForceFlat(inv3dMgr, loadedModel);
            logPostTransform();
            return;
        }

        // For mode 7, force vrMode=1 during position computation too
        // (must match the vrMode used during rendering for correct camera)
        if (s_fixMode == 7) {
            void* renderer = self.GetInventoryRenderer(inv3dMgr);
            int savedVrMode = 0;
            if (renderer) {
                auto rendBase = reinterpret_cast<uintptr_t>(renderer);
                auto* vrModePtr = reinterpret_cast<int*>(rendBase + OFF_RENDERER_VR_MODE);
                savedVrMode = *vrModePtr;
                if (savedVrMode != 1) {
                    *vrModePtr = 1;
                    if (self._logCount < MAX_LOG_ENTRIES) {
                        spdlog::info("[Inv3D-G] Forcing vrMode {} -> 1 for UpdateModelTransform", savedVrMode);
                    }
                }
            }
            if (g_originalUpdateModelTransform) {
                g_originalUpdateModelTransform(inv3dMgr, loadedModel);
            }
            if (renderer && savedVrMode != 1) {
                *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(renderer) + OFF_RENDERER_VR_MODE) = savedVrMode;
            }
            logPostTransform();
            return;
        }

        // For mode 16: deferred render — no vrMode changes needed.
        // Let the game compute positions naturally (vrMode=0 for flat Pipboy).
        // The deferred render patch handles the rendering pipeline.
        if (s_fixMode == 16) {
            if (g_originalUpdateModelTransform) {
                g_originalUpdateModelTransform(inv3dMgr, loadedModel);
            }
            logPostTransform();
            return;
        }

        // For modes 8-15, just call original + diagnostics (render hook handles the fix)
        if (s_fixMode >= 8 && s_fixMode <= 15) {
            if (g_originalUpdateModelTransform) {
                g_originalUpdateModelTransform(inv3dMgr, loadedModel);
            }
            logPostTransform();
            return;
        }

        switch (s_fixMode) {
        case 1:
            self.ApproachA_ForceFlat(inv3dMgr, loadedModel);
            break;
        case 2:
            self.ApproachB_OverrideFrustum(inv3dMgr, loadedModel);
            break;
        case 3:
            self.ApproachC_RecomputePosition(inv3dMgr, loadedModel);
            break;
        case 4:
            self.ApproachD_SwapCamera(inv3dMgr, loadedModel);
            break;
        default:
            if (g_originalUpdateModelTransform) {
                g_originalUpdateModelTransform(inv3dMgr, loadedModel);
            }
            break;
        }
        logPostTransform();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Hook: Scene graph traversal — force f669=true for BSLightingShader registration
    //
    // FUN_140b03d60 sets accumulator+0xf669=false (forward mode) right before
    // calling FUN_1427ff370 (scene graph traversal). When f669=false, the
    // traversal does NOT register BSLightingShader geometry into groups 1+2.
    // This hook overrides f669=true so BSLightingShader items get accumulated,
    // then HookForwardRenderPass renders those groups after the normal forward pass.
    // Only active when s_addLightingGroups=true (PipboyMenu item rendering).
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::HookSceneTraversal(uintptr_t camera, uintptr_t sceneNode,
                                             uintptr_t cullingProcess, uint8_t flag)
    {
        if (s_addLightingGroups) {
            // Get accumulator: prefer g_currentAccumulator (set by HookPerRendererRender),
            // fallback to extracting from BSCullingProcess+0x190 (offset 400 decimal,
            // Ghidra-verified: FUN_141d4d9c0 stores accumulator at cullingProcess+400)
            uintptr_t acc = g_currentAccumulator;
            if (acc == 0 && cullingProcess != 0) {
                acc = *reinterpret_cast<uintptr_t*>(cullingProcess + 0x190);
            }

            if (acc != 0) {
                // Force f669=true AND f688=0x19 for BSLightingShader registration.
                // FUN_140b03d60 sets f669=false, f688=0 when param_7=1 (forward path).
                // BSLightingShader registration into groups 1+2 requires BOTH:
                //   f669=true  (enables deferred geometry registration)
                //   f688=0x19  (deferred technique index — 25 decimal)
                // Without f688=0x19, all geometry goes to group 10 only.
                *reinterpret_cast<bool*>(acc + 0xf669) = true;
                *reinterpret_cast<uint32_t*>(acc + 0xf688) = 0x19;

                static int s_travLogCount = 0;
                if (s_travLogCount < 5) {
                    spdlog::info("[Inv3D-TRAV] Set f669=true f688=0x19 on accumulator {:X} (from {})",
                        acc, (g_currentAccumulator != 0) ? "renderer" : "cullingProcess");
                    s_travLogCount++;
                }
            }
        }

        if (g_originalSceneTraversal) {
            g_originalSceneTraversal(camera, sceneNode, cullingProcess, flag);
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Hook: Forward render pass — inject BSLightingShader groups
    //
    // The forward render path (param_7=1) calls FUN_14281cc90 which renders
    // BSEffectShader and other forward groups (3, 4, 10, 11, etc.) but skips
    // BSLightingShader groups 1 and 2. This hook adds them so opaque items
    // (weapons, food, armor) render with their diffuse/albedo color.
    // BSLightingShader's pixel shader writes SV_Target0 = albedo to the
    // bound offscreen RT; writes to unbound G-buffer targets (SV_Target1-5)
    // are silently discarded by D3D11.
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::HookForwardRenderPass(uintptr_t accumulator, uintptr_t context)
    {
        // Diagnostic BEFORE the original: scan ALL groups (0-37) to find where geometry is
        if (s_addLightingGroups && g_renderShaderGroup) {
            static int s_fwdLogCount = 0;
            if (s_fwdLogCount < 5) {
                // Scan all possible groups to find ANY with data
                for (uint32_t gid = 0; gid < 38; gid++) {
                    uintptr_t groupBase = accumulator + gid * 0x678;
                    int totalEntries = 0;
                    for (int e = 0; e < 4; e++) {
                        uintptr_t entry = groupBase + 0x620 + e * 0x18;
                        int count = *reinterpret_cast<int*>(entry + 0x10);
                        totalEntries += count;
                    }
                    if (totalEntries != 0) {
                        spdlog::info("[Inv3D-FWD] PRE: group {} entryCount={}", gid, totalEntries);
                    }
                }
                bool f669 = *reinterpret_cast<bool*>(accumulator + 0xf669);
                int f688 = *reinterpret_cast<int*>(accumulator + 0xf688);
                uintptr_t acc10 = *reinterpret_cast<uintptr_t*>(accumulator + 0x10);
                spdlog::info("[Inv3D-FWD] PRE: f669={} f688={} acc+0x10={:X}", f669, f688, acc10);
                s_fwdLogCount++;
            }
        }

        // Normal forward render pass (BSEffectShader, water, particles, etc.)
        if (g_originalForwardRenderPass) {
            g_originalForwardRenderPass(accumulator, context);
        }

        // After the original, inject BSLightingShader groups 0, 1, 2
        if (s_addLightingGroups && g_renderShaderGroup) {
            static int s_fwdPostLogCount = 0;
            if (s_fwdPostLogCount < 5) {
                // Scan all groups AFTER normal forward render to see what's registered
                for (uint32_t gid = 0; gid < 38; gid++) {
                    uintptr_t groupBase = accumulator + gid * 0x678;
                    int totalEntries = 0;
                    for (int e = 0; e < 4; e++) {
                        uintptr_t entry = groupBase + 0x620 + e * 0x18;
                        int count = *reinterpret_cast<int*>(entry + 0x10);
                        totalEntries += count;
                    }
                    if (totalEntries != 0) {
                        spdlog::info("[Inv3D-FWD] POST: group {} entryCount={}", gid, totalEntries);
                    }
                }
                s_fwdPostLogCount++;
            }

            // Render BSLightingShader groups 0, 1, 2 (opaque geometry).
            // The injected groups render to whatever RT is currently bound — which for
            // projected mode is a TEMP RT, not the composite srcRT. Capture it after
            // rendering so DoContentAwareBlit can read from the correct texture.
            bool ok = TryRenderShaderGroups(g_renderShaderGroup, accumulator, context);
            if (!ok) {
                static int s_crashCount = 0;
                if (s_crashCount < 5) {
                    spdlog::error("[Inv3D-FWD] BSLightingShader render crashed! (count={})",
                        ++s_crashCount);
                }
            }

            // Write alpha=1 to the currently bound RT (forwardRT) for pixels
            // where RGB > threshold. For projected mode, the game will then
            // alpha-blend forwardRT → srcRT=0x70; BSLightingShader pixels now
            // have alpha=1 and survive the copy (previously filtered out at alpha=0).
            DoAlphaWriteToCurrentRTV();
        }
    }

    // Hook: Composite — replaces alpha-blending composite with raw Copy
    //
    // Called at 0xb03497 inside FUN_140b02e30. The original composite uses
    // ImageSpace effect 0xF which alpha-blends offscreen RT onto screen RT.
    // BSLightingShader writes alpha=0 → opaque items invisible.
    // ImageSpaceManager::Copy does a raw overwrite → all items visible.
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::HookComposite(int srcRT, int dstRT, char useEffect11)
    {
        // Mode 15: replace composite with raw Copy (no alpha blending)
        if (s_replaceComposite && g_imageSpcCopy) {
            auto& self = GetSingleton();
            if (self._renderLogCount < MAX_RENDER_LOG) {
                spdlog::info("[Inv3D-O] Composite replaced with Copy: src=0x{:X} dst=0x{:X}", srcRT, dstRT);
            }
            g_imageSpcCopy(srcRT, dstRT);
            return;
        }

        // Mode 16: Content-aware blit AFTER the composite.
        //
        // Previous approach (alpha write to srcRT) failed for projected mode because:
        // 1. srcRT=0x41 (projected) has different dimensions than srcRT=0x37 (wrist),
        //    causing CopyResource to fail silently → staging has stale data → no fix
        // 2. srcRT may use a format without alpha (e.g., R11G11B10_FLOAT),
        //    making alpha-write do nothing
        //
        // New approach:
        // 1. Run original composite (correctly handles BSEffectShader / Nuka Cherry)
        // 2. DoContentAwareBlit reads srcRT, writes visible pixels (RGB > threshold)
        //    directly to dstRT with overwrite — no srcRT alpha channel needed.
        //    Staging is recreated if srcRT dimensions/format change between calls.
        if (s_doAlphaFixup) {
            // Write alpha=1 to srcRT pixels where RGB>threshold BEFORE composite.
            // Required for projected mode (vrMode=1): VR compositor (effect 0x42) uses
            // srcRT alpha as a visibility mask for the projected overlay. BSLightingShader
            // writes alpha=0 → items invisible. Fix: write alpha=1 for item pixels first.
            DoAlphaWriteToSrc(srcRT);
            if (g_originalComposite) {
                g_originalComposite(srcRT, dstRT, useEffect11);
            }
            // Belt-and-suspenders: also blit items directly to dstRT (handles wrist mode
            // and any path where the composite's alpha-blend is the visibility gating step).
            DoContentAwareBlit(srcRT, dstRT);
            return;
        }

        // Call original composite (non-fixup path)
        if (g_originalComposite) {
            g_originalComposite(srcRT, dstRT, useEffect11);
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Hook: VR Compositing — redirects offscreen RT input to screen RT
    //
    // Called at 0xb034f9 inside FUN_140b02e30. VR compositing (FUN_140b038f0)
    // reads renderer+0x1D8 to get the offscreen RT. For display_mode=1
    // (PipboyMenu), effect 0x42 receives both screen RT and offscreen RT.
    // The offscreen RT's alpha (0 for opaque items) may cause masking.
    // Fix: temporarily redirect +0x1D8 to the screen RT during this call only.
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::HookVRCompositing(uintptr_t renderer, uintptr_t renderData)
    {
        if (s_redirectVRCompositing) {
            auto& self = GetSingleton();
            auto* rtOverridePtr = reinterpret_cast<int*>(renderer + OFF_RENDERER_RT_OFFSCR);
            int savedRtOverride = *rtOverridePtr;
            int screenRT = g_getScreenRT ? g_getScreenRT(renderer) : 0x40;

            if (self._renderLogCount < MAX_RENDER_LOG) {
                spdlog::info("[Inv3D] VR compositing: redirecting offscreenRT from {} to screenRT=0x{:X}",
                    savedRtOverride, screenRT);
            }

            *rtOverridePtr = screenRT;
            if (g_originalVRCompositing) {
                g_originalVRCompositing(renderer, renderData);
            }
            *rtOverridePtr = savedRtOverride;
            return;
        }

        // Default: call original
        if (g_originalVRCompositing) {
            g_originalVRCompositing(renderer, renderData);
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // D3D11 Alpha Fixup — Mode 16
    //
    // After the offscreen 3D render, BSLightingShader leaves alpha=0 on opaque
    // items. VR compositing (effect 0x42) uses this alpha as a visibility mask,
    // making opaque items invisible. This fix draws a fullscreen triangle with
    // an alpha-only write mask, setting alpha=1.0 while preserving RGB.
    // ════════════════════════════════════════════════════════════════════════

    static const char* s_alphaVS_HLSL =
        "float4 main(uint id : SV_VertexID) : SV_POSITION {\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n";

    // Simple PS: sets alpha=1 everywhere (used as fallback)
    static const char* s_alphaPS_HLSL =
        "float4 main() : SV_TARGET {\n"
        "    return float4(0, 0, 0, 1);\n"
        "}\n";

    // Content-aware blit PS: reads srcRT staging (with UV scaling for size mismatch),
    // discards background pixels (RGB <= threshold), outputs full RGBA for item pixels.
    // Writes directly to dstRT with overwrite blend — no srcRT alpha dependency.
    // Works even when srcRT has no alpha channel (e.g., R11G11B10_FLOAT).
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

    // Passthrough PS: samples a texture and outputs RGB with alpha=1
    // Used for custom compositing (additive blend onto screen RT)
    // Uses SV_POSITION to get pixel coords — works when src and dst are same size.
    // If they differ, we scale: the VS outputs UV coords that we pass through.
    static const char* s_passthroughPS_HLSL =
        "Texture2D srcTex : register(t0);\n"
        "cbuffer Constants : register(b0) {\n"
        "    float2 srcDims;\n"
        "    float2 dstDims;\n"
        "};\n"
        "float4 main(float4 pos : SV_POSITION) : SV_TARGET {\n"
        "    // Scale pixel coords from dst space to src space\n"
        "    float2 uv = pos.xy / dstDims;\n"
        "    int2 srcCoord = int2(uv * srcDims);\n"
        "    float4 c = srcTex.Load(int3(srcCoord, 0));\n"
        "    return float4(c.rgb, 1.0);\n"
        "}\n";

    bool Inventory3DFix::InitD3D11Resources()
    {
        if (s_d3dInitialized) return true;
        if (s_d3dInitFailed) return false;

        // Get BSGraphics::RendererData singleton
        uintptr_t base = REL::Module::get().base();
        auto** rendererDataPtrPtr = reinterpret_cast<uintptr_t**>(base + BSGFX_RENDERER_DATA);
        if (!rendererDataPtrPtr || !*rendererDataPtrPtr) {
            spdlog::error("[Inv3D-P] BSGraphics::RendererData singleton not available");
            s_d3dInitFailed = true;
            return false;
        }

        uintptr_t rendererData = reinterpret_cast<uintptr_t>(*rendererDataPtrPtr);
        auto* device = *reinterpret_cast<ID3D11Device**>(rendererData + 0x48);
        if (!device) {
            spdlog::error("[Inv3D-P] ID3D11Device is null");
            s_d3dInitFailed = true;
            return false;
        }

        spdlog::info("[Inv3D-P] D3D11 device found at {:X}", reinterpret_cast<uintptr_t>(device));

        // Compile vertex shader
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* errBlob = nullptr;
        HRESULT hr = D3DCompile(s_alphaVS_HLSL, strlen(s_alphaVS_HLSL), "AlphaFixupVS",
            nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob);
        if (FAILED(hr)) {
            const char* errMsg = errBlob ? (const char*)errBlob->GetBufferPointer() : "unknown";
            spdlog::error("[Inv3D-P] VS compile failed: {}", errMsg);
            if (errBlob) errBlob->Release();
            s_d3dInitFailed = true;
            return false;
        }
        if (errBlob) errBlob->Release();

        hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            nullptr, &s_alphaVS);
        vsBlob->Release();
        if (FAILED(hr)) {
            spdlog::error("[Inv3D-P] CreateVertexShader failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Compile pixel shader
        ID3DBlob* psBlob = nullptr;
        hr = D3DCompile(s_alphaPS_HLSL, strlen(s_alphaPS_HLSL), "AlphaFixupPS",
            nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob);
        if (FAILED(hr)) {
            const char* errMsg = errBlob ? (const char*)errBlob->GetBufferPointer() : "unknown";
            spdlog::error("[Inv3D-P] PS compile failed: {}", errMsg);
            if (errBlob) errBlob->Release();
            s_d3dInitFailed = true;
            return false;
        }
        if (errBlob) errBlob->Release();

        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
            nullptr, &s_alphaPS);
        psBlob->Release();
        if (FAILED(hr)) {
            spdlog::error("[Inv3D-P] CreatePixelShader (blanket) failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Compile content-aware blit pixel shader (replaces old content-aware PS)
        ID3DBlob* caBlob = nullptr;
        hr = D3DCompile(s_contentAwareBlitPS_HLSL, strlen(s_contentAwareBlitPS_HLSL), "ContentAwareBlitPS",
            nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &caBlob, &errBlob);
        if (FAILED(hr)) {
            const char* errMsg = errBlob ? (const char*)errBlob->GetBufferPointer() : "unknown";
            spdlog::error("[Inv3D-P] Content-aware blit PS compile failed: {}", errMsg);
            if (errBlob) errBlob->Release();
            s_d3dInitFailed = true;
            return false;
        }
        if (errBlob) errBlob->Release();

        hr = device->CreatePixelShader(caBlob->GetBufferPointer(), caBlob->GetBufferSize(),
            nullptr, &s_contentAwareBlitPS);
        caBlob->Release();
        if (FAILED(hr)) {
            spdlog::error("[Inv3D-P] CreatePixelShader (content-aware blit) failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        spdlog::info("[Inv3D-P] Content-aware blit PS compiled successfully");

        // Compile passthrough pixel shader (for custom compositing)
        ID3DBlob* ptBlob = nullptr;
        hr = D3DCompile(s_passthroughPS_HLSL, strlen(s_passthroughPS_HLSL), "PassthroughPS",
            nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ptBlob, &errBlob);
        if (FAILED(hr)) {
            const char* errMsg = errBlob ? (const char*)errBlob->GetBufferPointer() : "unknown";
            spdlog::error("[Inv3D-P] Passthrough PS compile failed: {}", errMsg);
            if (errBlob) errBlob->Release();
            s_d3dInitFailed = true;
            return false;
        }
        if (errBlob) errBlob->Release();

        hr = device->CreatePixelShader(ptBlob->GetBufferPointer(), ptBlob->GetBufferSize(),
            nullptr, &s_passthroughPS);
        ptBlob->Release();
        if (FAILED(hr)) {
            spdlog::error("[Inv3D-P] CreatePixelShader (passthrough) failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        spdlog::info("[Inv3D-P] Passthrough PS compiled successfully");

        // Create staging texture (matching RT 0x3F format/dims) — deferred to first DoAlphaFixup call
        // We can't create it here because we don't know the RT dimensions until the first render.
        // s_stagingTex and s_stagingSRV will be created lazily in DoAlphaFixup.

        // Create blend state — only write alpha channel
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        hr = device->CreateBlendState(&bd, &s_alphaBlendState);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D-P] CreateBlendState failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Create depth-stencil state — disable depth test/write
        D3D11_DEPTH_STENCIL_DESC dsd = {};
        dsd.DepthEnable = FALSE;
        dsd.StencilEnable = FALSE;
        hr = device->CreateDepthStencilState(&dsd, &s_alphaDepthState);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D-P] CreateDepthStencilState failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Create rasterizer state — no culling
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        hr = device->CreateRasterizerState(&rd, &s_alphaRastState);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D-P] CreateRasterizerState failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Create constant buffer for passthrough PS dimensions
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = 16;  // float4 (srcDims.xy, dstDims.xy) — must be 16-byte aligned
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&cbDesc, nullptr, &s_dimsCB);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D-P] CreateBuffer (dims CB) failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Create additive blend state — for custom compositing (src + dst)
        D3D11_BLEND_DESC abd = {};
        abd.RenderTarget[0].BlendEnable = TRUE;
        abd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        abd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        abd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        abd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        abd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        abd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        abd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&abd, &s_additiveBlendState);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D-P] CreateBlendState (additive) failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        // Create overwrite blend state — for content-aware blit (no blending, full overwrite)
        // Used by DoContentAwareBlit: PS discards below-threshold pixels, overwrites the rest.
        D3D11_BLEND_DESC obd = {};
        obd.RenderTarget[0].BlendEnable = FALSE;
        obd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&obd, &s_overwriteBlendState);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D-P] CreateBlendState (overwrite) failed: 0x{:X}", (unsigned)hr);
            s_d3dInitFailed = true;
            return false;
        }

        s_d3dInitialized = true;
        spdlog::info("[Inv3D-P] D3D11 alpha fixup resources initialized successfully");
        return true;
    }

    // DoAlphaWriteToCurrentRTV — writes alpha=1 to the currently bound RTV where RGB > threshold.
    //
    // Called from HookForwardRenderPass AFTER TryRenderShaderGroups renders BSLightingShader
    // to the forward RT (e.g., 1024x1024 RGBA8 for projected mode). The game then
    // alpha-blends that forward RT into srcRT=0x70. With alpha=0 (BSLightingShader default),
    // items are filtered out. Writing alpha=1 here makes them survive the copy.
    //
    // Only operates on formats with an alpha channel (skips R11G11B10_FLOAT etc.).
    void Inventory3DFix::DoAlphaWriteToCurrentRTV()
    {
        if (!InitD3D11Resources()) return;

        uintptr_t base = REL::Module::get().base();
        auto** rendererDataPtrPtr = reinterpret_cast<uintptr_t**>(base + BSGFX_RENDERER_DATA);
        if (!rendererDataPtrPtr || !*rendererDataPtrPtr) return;

        uintptr_t rendererData = reinterpret_cast<uintptr_t>(*rendererDataPtrPtr);
        auto* device = *reinterpret_cast<ID3D11Device**>(rendererData + 0x48);
        auto* context = *reinterpret_cast<ID3D11DeviceContext**>(rendererData + 0x50);
        if (!device || !context) return;

        // Get the currently bound RTV (and DSV so we can restore it)
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
            return;
        }

        D3D11_TEXTURE2D_DESC desc;
        currentTex->GetDesc(&desc);

        // Only operate on formats with an alpha channel
        bool hasAlpha = (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                         desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                         desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                         desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                         desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                         desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM ||
                         desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
                         desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
        if (!hasAlpha) {
            static int s_skipCount = 0;
            if (s_skipCount < 3) {
                spdlog::warn("[Inv3D-P] DoAlphaWriteToCurrentRTV: fmt={} has no alpha — skipping",
                    (int)desc.Format);
                s_skipCount++;
            }
            currentTex->Release();
            currentRTV->Release();
            return;
        }

        // Create/recreate staging if dimensions or format changed
        bool needNewStaging = !s_stagingTex ||
            s_stagingWidth  != desc.Width  ||
            s_stagingHeight != desc.Height ||
            s_stagingFormat != desc.Format;

        if (needNewStaging) {
            if (s_stagingTex) {
                s_stagingSRV->Release(); s_stagingSRV = nullptr;
                s_stagingTex->Release(); s_stagingTex = nullptr;
            }
            D3D11_TEXTURE2D_DESC stageDesc = desc;
            stageDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
            stageDesc.Usage          = D3D11_USAGE_DEFAULT;
            stageDesc.CPUAccessFlags = 0;
            stageDesc.MiscFlags      = 0;

            HRESULT hr = device->CreateTexture2D(&stageDesc, nullptr, &s_stagingTex);
            if (FAILED(hr)) {
                spdlog::error("[Inv3D-P] DoAlphaWriteToCurrentRTV: staging create failed: 0x{:X}", (unsigned)hr);
                currentTex->Release();
                currentRTV->Release();
                return;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format                    = stageDesc.Format;
            srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels       = 1;
            srvDesc.Texture2D.MostDetailedMip = 0;
            hr = device->CreateShaderResourceView(s_stagingTex, &srvDesc, &s_stagingSRV);
            if (FAILED(hr)) {
                spdlog::error("[Inv3D-P] DoAlphaWriteToCurrentRTV: staging SRV failed: 0x{:X}", (unsigned)hr);
                s_stagingTex->Release(); s_stagingTex = nullptr;
                currentTex->Release();
                currentRTV->Release();
                return;
            }
            s_stagingWidth  = desc.Width;
            s_stagingHeight = desc.Height;
            s_stagingFormat = desc.Format;
            spdlog::info("[Inv3D-P] ForwardRT staging: {}x{} fmt={}", desc.Width, desc.Height, (int)desc.Format);
        }

        // Copy current RT → staging (so we can read it while writing back to the RTV)
        context->CopyResource(s_stagingTex, currentTex);

        // Update dims constant buffer (src=dst=forwardRT dimensions)
        if (s_dimsCB) {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(context->Map(s_dimsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                float dims[4] = {
                    static_cast<float>(desc.Width),  static_cast<float>(desc.Height),
                    static_cast<float>(desc.Width),  static_cast<float>(desc.Height)
                };
                memcpy(mapped.pData, dims, sizeof(dims));
                context->Unmap(s_dimsCB, 0);
            }
        }

        // Save D3D11 state
        ID3D11BlendState* savedBlendState = nullptr;
        FLOAT savedBlendFactor[4];
        UINT savedSampleMask;
        context->OMGetBlendState(&savedBlendState, savedBlendFactor, &savedSampleMask);

        ID3D11DepthStencilState* savedDepthState = nullptr;
        UINT savedStencilRef;
        context->OMGetDepthStencilState(&savedDepthState, &savedStencilRef);

        ID3D11RasterizerState* savedRastState = nullptr;
        context->RSGetState(&savedRastState);

        D3D11_VIEWPORT savedVP;
        UINT numVP = 1;
        context->RSGetViewports(&numVP, &savedVP);

        ID3D11VertexShader* savedVS = nullptr;
        context->VSGetShader(&savedVS, nullptr, nullptr);
        ID3D11PixelShader* savedPS = nullptr;
        context->PSGetShader(&savedPS, nullptr, nullptr);
        ID3D11InputLayout* savedIL = nullptr;
        context->IAGetInputLayout(&savedIL);
        D3D11_PRIMITIVE_TOPOLOGY savedTopo;
        context->IAGetPrimitiveTopology(&savedTopo);

        ID3D11ShaderResourceView* savedPSSRV = nullptr;
        context->PSGetShaderResources(0, 1, &savedPSSRV);

        ID3D11Buffer* savedCB = nullptr;
        context->PSGetConstantBuffers(0, 1, &savedCB);

        // Bind currentRTV with no DSV (alpha-only write — s_alphaBlendState preserves RGB)
        context->OMSetRenderTargets(1, &currentRTV, nullptr);
        FLOAT blendFactor[4] = { 0, 0, 0, 0 };
        context->OMSetBlendState(s_alphaBlendState, blendFactor, 0xFFFFFFFF);
        context->OMSetDepthStencilState(s_alphaDepthState, 0);
        context->RSSetState(s_alphaRastState);

        D3D11_VIEWPORT vp = {};
        vp.Width    = static_cast<FLOAT>(desc.Width);
        vp.Height   = static_cast<FLOAT>(desc.Height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(s_alphaVS, nullptr, 0);
        context->PSSetShader(s_contentAwareBlitPS, nullptr, 0);
        context->PSSetShaderResources(0, 1, &s_stagingSRV);
        if (s_dimsCB) context->PSSetConstantBuffers(0, 1, &s_dimsCB);

        context->Draw(3, 0);

        static int s_logCount = 0;
        if (s_logCount < 5) {
            spdlog::info("[Inv3D-P] DoAlphaWriteToCurrentRTV: {}x{} fmt={}", desc.Width, desc.Height, (int)desc.Format);
            s_logCount++;
        }

        // Restore state
        context->OMSetRenderTargets(1, &currentRTV, currentDSV);  // currentRTV was the saved bound RTV
        context->OMSetBlendState(savedBlendState, savedBlendFactor, savedSampleMask);
        context->OMSetDepthStencilState(savedDepthState, savedStencilRef);
        context->RSSetState(savedRastState);
        context->RSSetViewports(1, &savedVP);
        context->IASetInputLayout(savedIL);
        context->IASetPrimitiveTopology(savedTopo);
        context->VSSetShader(savedVS, nullptr, 0);
        context->PSSetShader(savedPS, nullptr, 0);
        context->PSSetShaderResources(0, 1, &savedPSSRV);
        context->PSSetConstantBuffers(0, 1, &savedCB);

        if (savedBlendState) savedBlendState->Release();
        if (savedDepthState) savedDepthState->Release();
        if (savedRastState)  savedRastState->Release();
        if (savedVS)         savedVS->Release();
        if (savedPS)         savedPS->Release();
        if (savedIL)         savedIL->Release();
        if (savedPSSRV)      savedPSSRV->Release();
        if (savedCB)         savedCB->Release();

        currentTex->Release();
        currentRTV->Release();
        if (currentDSV) currentDSV->Release();
    }

    // DoAlphaWriteToSrc — writes alpha=1 to srcRT where RGB > threshold.
    //
    // Called BEFORE g_originalComposite so that:
    //   Wrist mode: composite's alpha-blend sees alpha=1 → items visible
    //   Projected mode: VR compositor (effect 0x42) uses srcRT alpha as a mask;
    //     writing alpha=1 for item pixels makes them visible in the projected overlay.
    //
    // Approach: copy srcRT → staging (to read from), then draw fullscreen triangle
    // with alpha-only write mask into srcRT. PS discards below-threshold pixels so
    // background stays alpha=0. Item pixels get alpha=1.
    // Only operates when srcRT has an alpha channel; logs and skips otherwise.
    void Inventory3DFix::DoAlphaWriteToSrc(int srcRT)
    {
        if (!InitD3D11Resources()) return;

        uintptr_t base = REL::Module::get().base();
        auto** rendererDataPtrPtr = reinterpret_cast<uintptr_t**>(base + BSGFX_RENDERER_DATA);
        if (!rendererDataPtrPtr || !*rendererDataPtrPtr) return;

        uintptr_t rendererData = reinterpret_cast<uintptr_t>(*rendererDataPtrPtr);
        auto* device = *reinterpret_cast<ID3D11Device**>(rendererData + 0x48);
        auto* context = *reinterpret_cast<ID3D11DeviceContext**>(rendererData + 0x50);
        if (!context || !device) return;

        uintptr_t rtArrayBase = rendererData + 0x0A58;
        uintptr_t srcEntry = rtArrayBase + static_cast<uintptr_t>(srcRT) * 0x30;
        auto* srcTexture = *reinterpret_cast<ID3D11Texture2D**>(srcEntry + 0x00);
        auto* srcView    = *reinterpret_cast<ID3D11RenderTargetView**>(srcEntry + 0x10);
        if (!srcTexture || !srcView) {
            spdlog::warn("[Inv3D-P] DoAlphaWriteToSrc: srcRT 0x{:X} missing texture or RTV", srcRT);
            return;
        }

        D3D11_TEXTURE2D_DESC srcDesc;
        srcTexture->GetDesc(&srcDesc);

        // Only operate on formats with an alpha channel — otherwise alpha write does nothing.
        bool hasAlpha = (srcDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                         srcDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                         srcDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                         srcDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                         srcDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                         srcDesc.Format == DXGI_FORMAT_R16G16B16A16_UNORM ||
                         srcDesc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
                         srcDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
        static int s_noAlphaCount = 0;
        if (!hasAlpha) {
            if (s_noAlphaCount < 3) {
                spdlog::warn("[Inv3D-P] DoAlphaWriteToSrc: srcRT 0x{:X} fmt={} has no alpha channel — skipping",
                    srcRT, (int)srcDesc.Format);
                s_noAlphaCount++;
            }
            return;
        }

        // Create/recreate staging if srcRT dimensions or format changed
        bool needNewStaging = !s_stagingTex ||
            s_stagingWidth  != srcDesc.Width  ||
            s_stagingHeight != srcDesc.Height ||
            s_stagingFormat != srcDesc.Format;

        if (needNewStaging) {
            if (s_stagingTex) {
                s_stagingSRV->Release(); s_stagingSRV = nullptr;
                s_stagingTex->Release(); s_stagingTex = nullptr;
            }
            D3D11_TEXTURE2D_DESC stageDesc = srcDesc;
            stageDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
            stageDesc.Usage          = D3D11_USAGE_DEFAULT;
            stageDesc.CPUAccessFlags = 0;
            stageDesc.MiscFlags      = 0;

            HRESULT hr = device->CreateTexture2D(&stageDesc, nullptr, &s_stagingTex);
            if (FAILED(hr)) {
                spdlog::error("[Inv3D-P] DoAlphaWriteToSrc: staging create failed: 0x{:X}", (unsigned)hr);
                return;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format                    = stageDesc.Format;
            srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels       = 1;
            srvDesc.Texture2D.MostDetailedMip = 0;
            hr = device->CreateShaderResourceView(s_stagingTex, &srvDesc, &s_stagingSRV);
            if (FAILED(hr)) {
                spdlog::error("[Inv3D-P] DoAlphaWriteToSrc: staging SRV failed: 0x{:X}", (unsigned)hr);
                s_stagingTex->Release(); s_stagingTex = nullptr;
                return;
            }
            s_stagingWidth  = srcDesc.Width;
            s_stagingHeight = srcDesc.Height;
            s_stagingFormat = srcDesc.Format;
            spdlog::info("[Inv3D-P] Alpha staging created: {}x{} fmt={} (srcRT=0x{:X})",
                srcDesc.Width, srcDesc.Height, (int)srcDesc.Format, srcRT);
        }

        // Copy srcRT → staging before binding srcRT as RTV
        context->CopyResource(s_stagingTex, srcTexture);

        // Dims CB: both src and dst are srcRT dimensions (writing back into same RT)
        if (s_dimsCB) {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(context->Map(s_dimsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                float dims[4] = {
                    static_cast<float>(srcDesc.Width),  static_cast<float>(srcDesc.Height),
                    static_cast<float>(srcDesc.Width),  static_cast<float>(srcDesc.Height)
                };
                memcpy(mapped.pData, dims, sizeof(dims));
                context->Unmap(s_dimsCB, 0);
            }
        }

        // Save D3D11 state
        ID3D11RenderTargetView* savedRTV = nullptr;
        ID3D11DepthStencilView* savedDSV = nullptr;
        context->OMGetRenderTargets(1, &savedRTV, &savedDSV);

        ID3D11BlendState* savedBlendState = nullptr;
        FLOAT savedBlendFactor[4];
        UINT savedSampleMask;
        context->OMGetBlendState(&savedBlendState, savedBlendFactor, &savedSampleMask);

        ID3D11DepthStencilState* savedDepthState = nullptr;
        UINT savedStencilRef;
        context->OMGetDepthStencilState(&savedDepthState, &savedStencilRef);

        ID3D11RasterizerState* savedRastState = nullptr;
        context->RSGetState(&savedRastState);

        D3D11_VIEWPORT savedVP;
        UINT numVP = 1;
        context->RSGetViewports(&numVP, &savedVP);

        ID3D11VertexShader* savedVS = nullptr;
        context->VSGetShader(&savedVS, nullptr, nullptr);
        ID3D11PixelShader* savedPS = nullptr;
        context->PSGetShader(&savedPS, nullptr, nullptr);
        ID3D11InputLayout* savedIL = nullptr;
        context->IAGetInputLayout(&savedIL);
        D3D11_PRIMITIVE_TOPOLOGY savedTopo;
        context->IAGetPrimitiveTopology(&savedTopo);

        ID3D11ShaderResourceView* savedPSSRV = nullptr;
        context->PSGetShaderResources(0, 1, &savedPSSRV);

        ID3D11Buffer* savedCB = nullptr;
        context->PSGetConstantBuffers(0, 1, &savedCB);

        // Set up pipeline: write alpha=1 into srcRT for pixels where staging RGB > threshold
        context->OMSetRenderTargets(1, &srcView, nullptr);
        FLOAT blendFactor[4] = { 0, 0, 0, 0 };
        context->OMSetBlendState(s_alphaBlendState, blendFactor, 0xFFFFFFFF);  // alpha-only write
        context->OMSetDepthStencilState(s_alphaDepthState, 0);
        context->RSSetState(s_alphaRastState);

        D3D11_VIEWPORT vp = {};
        vp.Width    = static_cast<FLOAT>(srcDesc.Width);
        vp.Height   = static_cast<FLOAT>(srcDesc.Height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(s_alphaVS, nullptr, 0);
        // Reuse content-aware blit PS: discards below threshold, outputs alpha=1
        // (alpha-only blend mask ensures only alpha channel is written to srcRT)
        context->PSSetShader(s_contentAwareBlitPS, nullptr, 0);
        context->PSSetShaderResources(0, 1, &s_stagingSRV);
        if (s_dimsCB) context->PSSetConstantBuffers(0, 1, &s_dimsCB);

        context->Draw(3, 0);

        static int s_alphaWriteLogCount = 0;
        if (s_alphaWriteLogCount < 10) {
            spdlog::info("[Inv3D-P] Alpha write to srcRT=0x{:X} ({}x{} fmt={})",
                srcRT, srcDesc.Width, srcDesc.Height, (int)srcDesc.Format);
            s_alphaWriteLogCount++;
        }

        // Restore state
        context->OMSetRenderTargets(1, &savedRTV, savedDSV);
        context->OMSetBlendState(savedBlendState, savedBlendFactor, savedSampleMask);
        context->OMSetDepthStencilState(savedDepthState, savedStencilRef);
        context->RSSetState(savedRastState);
        context->RSSetViewports(1, &savedVP);
        context->IASetInputLayout(savedIL);
        context->IASetPrimitiveTopology(savedTopo);
        context->VSSetShader(savedVS, nullptr, 0);
        context->PSSetShader(savedPS, nullptr, 0);
        context->PSSetShaderResources(0, 1, &savedPSSRV);
        context->PSSetConstantBuffers(0, 1, &savedCB);

        if (savedRTV)        savedRTV->Release();
        if (savedDSV)        savedDSV->Release();
        if (savedBlendState) savedBlendState->Release();
        if (savedDepthState) savedDepthState->Release();
        if (savedRastState)  savedRastState->Release();
        if (savedVS)         savedVS->Release();
        if (savedPS)         savedPS->Release();
        if (savedIL)         savedIL->Release();
        if (savedPSSRV)      savedPSSRV->Release();
        if (savedCB)         savedCB->Release();
    }

    // DoContentAwareBlit — reads srcRT and writes visible item pixels to dstRT.
    //
    // Called AFTER g_originalComposite has already run. The composite handles
    // BSEffectShader items (Nuka Cherry, glass) which write non-zero alpha.
    // BSLightingShader items write alpha=0 → invisible after composite.
    //
    // This blit reads the srcRT staging copy and for pixels where RGB > threshold
    // (i.e., something rendered there), directly writes that RGB+alpha=1 to dstRT.
    // Uses DISCARD for below-threshold pixels so background stays transparent.
    //
    // Key advantages over the previous alpha-write approach:
    // 1. Works even when srcRT has no alpha channel (e.g., R11G11B10_FLOAT)
    // 2. Staging is recreated if srcRT dimensions/format change between calls
    //    (fixes projected mode where srcRT=0x41 may differ from wrist srcRT=0x37)
    // 3. Writes to dstRT directly — no round-trip through srcRT alpha + composite
    void Inventory3DFix::DoContentAwareBlit(int srcRT, int dstRT)
    {
        if (!InitD3D11Resources()) return;

        uintptr_t base = REL::Module::get().base();
        auto** rendererDataPtrPtr = reinterpret_cast<uintptr_t**>(base + BSGFX_RENDERER_DATA);
        if (!rendererDataPtrPtr || !*rendererDataPtrPtr) return;

        uintptr_t rendererData = reinterpret_cast<uintptr_t>(*rendererDataPtrPtr);
        auto* device = *reinterpret_cast<ID3D11Device**>(rendererData + 0x48);
        auto* context = *reinterpret_cast<ID3D11DeviceContext**>(rendererData + 0x50);
        if (!context || !device) return;

        uintptr_t rtArrayBase = rendererData + 0x0A58;

        // Get srcRT texture (read source)
        uintptr_t srcEntry = rtArrayBase + static_cast<uintptr_t>(srcRT) * 0x30;
        auto* srcTexture = *reinterpret_cast<ID3D11Texture2D**>(srcEntry + 0x00);
        if (!srcTexture) {
            spdlog::warn("[Inv3D-P] srcRT 0x{:X} has null texture", srcRT);
            return;
        }
        D3D11_TEXTURE2D_DESC srcDesc;
        srcTexture->GetDesc(&srcDesc);

        // Get dstRT view (write target)
        uintptr_t dstEntry = rtArrayBase + static_cast<uintptr_t>(dstRT) * 0x30;
        auto* dstTexture = *reinterpret_cast<ID3D11Texture2D**>(dstEntry + 0x00);
        auto* dstView    = *reinterpret_cast<ID3D11RenderTargetView**>(dstEntry + 0x10);
        if (!dstView || !dstTexture) {
            spdlog::warn("[Inv3D-P] dstRT 0x{:X} has null rtView or texture", dstRT);
            return;
        }
        D3D11_TEXTURE2D_DESC dstDesc;
        dstTexture->GetDesc(&dstDesc);

        // Read directly from srcRT. After DoAlphaWriteToCurrentRTV runs during
        // HookForwardRenderPass, BSLightingShader pixels in the forward RT have
        // alpha=1, so the game's alpha-blend copy into srcRT now includes them.
        ID3D11Texture2D* readTex = srcTexture;

        D3D11_TEXTURE2D_DESC readDesc;
        readTex->GetDesc(&readDesc);

        // ── Create/recreate staging texture if read source dimensions or format changed ──
        bool needNewStaging = !s_stagingTex ||
            s_stagingWidth  != readDesc.Width  ||
            s_stagingHeight != readDesc.Height ||
            s_stagingFormat != readDesc.Format;

        if (needNewStaging) {
            if (s_stagingTex) {
                s_stagingSRV->Release(); s_stagingSRV = nullptr;
                s_stagingTex->Release(); s_stagingTex = nullptr;
            }
            D3D11_TEXTURE2D_DESC stageDesc = readDesc;
            stageDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
            stageDesc.Usage          = D3D11_USAGE_DEFAULT;
            stageDesc.CPUAccessFlags = 0;
            stageDesc.MiscFlags      = 0;

            HRESULT hr = device->CreateTexture2D(&stageDesc, nullptr, &s_stagingTex);
            if (FAILED(hr)) {
                spdlog::error("[Inv3D-P] Failed to create staging texture: 0x{:X}", (unsigned)hr);
                return;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format                    = stageDesc.Format;
            srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels       = 1;
            srvDesc.Texture2D.MostDetailedMip = 0;
            hr = device->CreateShaderResourceView(s_stagingTex, &srvDesc, &s_stagingSRV);
            if (FAILED(hr)) {
                spdlog::error("[Inv3D-P] Failed to create staging SRV: 0x{:X}", (unsigned)hr);
                s_stagingTex->Release(); s_stagingTex = nullptr;
                return;
            }
            s_stagingWidth  = readDesc.Width;
            s_stagingHeight = readDesc.Height;
            s_stagingFormat = readDesc.Format;
            spdlog::info("[Inv3D-P] Staging created: {}x{} fmt={}",
                readDesc.Width, readDesc.Height, (int)readDesc.Format);
        }

        // Copy read source → staging (BSLightingShader content is in readTex)
        context->CopyResource(s_stagingTex, readTex);

        // Update dims constant buffer: srcDims=readTex size, dstDims=dstRT size
        if (s_dimsCB) {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(context->Map(s_dimsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                float dims[4] = {
                    static_cast<float>(readDesc.Width),  static_cast<float>(readDesc.Height),
                    static_cast<float>(dstDesc.Width),   static_cast<float>(dstDesc.Height)
                };
                memcpy(mapped.pData, dims, sizeof(dims));
                context->Unmap(s_dimsCB, 0);
            }
        }

        // Save D3D11 state
        ID3D11RenderTargetView* savedRTV = nullptr;
        ID3D11DepthStencilView* savedDSV = nullptr;
        context->OMGetRenderTargets(1, &savedRTV, &savedDSV);

        ID3D11BlendState* savedBlendState = nullptr;
        FLOAT savedBlendFactor[4];
        UINT savedSampleMask;
        context->OMGetBlendState(&savedBlendState, savedBlendFactor, &savedSampleMask);

        ID3D11DepthStencilState* savedDepthState = nullptr;
        UINT savedStencilRef;
        context->OMGetDepthStencilState(&savedDepthState, &savedStencilRef);

        ID3D11RasterizerState* savedRastState = nullptr;
        context->RSGetState(&savedRastState);

        D3D11_VIEWPORT savedVP;
        UINT numVP = 1;
        context->RSGetViewports(&numVP, &savedVP);

        ID3D11VertexShader* savedVS = nullptr;
        context->VSGetShader(&savedVS, nullptr, nullptr);
        ID3D11PixelShader* savedPS = nullptr;
        context->PSGetShader(&savedPS, nullptr, nullptr);
        ID3D11InputLayout* savedIL = nullptr;
        context->IAGetInputLayout(&savedIL);
        D3D11_PRIMITIVE_TOPOLOGY savedTopo;
        context->IAGetPrimitiveTopology(&savedTopo);

        ID3D11ShaderResourceView* savedPSSRV = nullptr;
        context->PSGetShaderResources(0, 1, &savedPSSRV);

        ID3D11Buffer* savedCB = nullptr;
        context->PSGetConstantBuffers(0, 1, &savedCB);

        // Set up pipeline: draw fullscreen triangle into dstRT
        context->OMSetRenderTargets(1, &dstView, nullptr);
        FLOAT blendFactor[4] = { 0, 0, 0, 0 };
        context->OMSetBlendState(s_overwriteBlendState, blendFactor, 0xFFFFFFFF);
        context->OMSetDepthStencilState(s_alphaDepthState, 0);
        context->RSSetState(s_alphaRastState);

        // Viewport over the DESTINATION RT
        D3D11_VIEWPORT vp = {};
        vp.Width    = static_cast<FLOAT>(dstDesc.Width);
        vp.Height   = static_cast<FLOAT>(dstDesc.Height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(s_alphaVS, nullptr, 0);
        context->PSSetShader(s_contentAwareBlitPS, nullptr, 0);
        context->PSSetShaderResources(0, 1, &s_stagingSRV);
        if (s_dimsCB) context->PSSetConstantBuffers(0, 1, &s_dimsCB);

        context->Draw(3, 0);

        static int s_blitLogCount = 0;
        if (s_blitLogCount < 10) {
            spdlog::info("[Inv3D-P] Content-aware blit: srcRT=0x{:X} ({}x{}) -> dstRT=0x{:X} ({}x{})",
                srcRT, srcDesc.Width, srcDesc.Height,
                dstRT, dstDesc.Width, dstDesc.Height);
            s_blitLogCount++;
        }

        // Restore state
        context->OMSetRenderTargets(1, &savedRTV, savedDSV);
        context->OMSetBlendState(savedBlendState, savedBlendFactor, savedSampleMask);
        context->OMSetDepthStencilState(savedDepthState, savedStencilRef);
        context->RSSetState(savedRastState);
        context->RSSetViewports(1, &savedVP);
        context->IASetInputLayout(savedIL);
        context->IASetPrimitiveTopology(savedTopo);
        context->VSSetShader(savedVS, nullptr, 0);
        context->PSSetShader(savedPS, nullptr, 0);
        context->PSSetShaderResources(0, 1, &savedPSSRV);
        context->PSSetConstantBuffers(0, 1, &savedCB);

        if (savedRTV)        savedRTV->Release();
        if (savedDSV)        savedDSV->Release();
        if (savedBlendState) savedBlendState->Release();
        if (savedDepthState) savedDepthState->Release();
        if (savedRastState)  savedRastState->Release();
        if (savedVS)         savedVS->Release();
        if (savedPS)         savedPS->Release();
        if (savedIL)         savedIL->Release();
        if (savedPSSRV)      savedPSSRV->Release();
        if (savedCB)         savedCB->Release();
    }

    // ════════════════════════════════════════════════════════════════════════
    // DiagnosticReadback — One-shot CPU readback of RT to inspect pixel values
    //
    // Creates a CPU-readable staging texture, copies the RT to it, maps it,
    // and logs sample pixel values. This tells us definitively whether items
    // have rendered any RGB content to the offscreen RT.
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::DiagnosticReadback(int rtIndex)
    {
        uintptr_t base = REL::Module::get().base();
        auto** rendererDataPtrPtr = reinterpret_cast<uintptr_t**>(base + BSGFX_RENDERER_DATA);
        if (!rendererDataPtrPtr || !*rendererDataPtrPtr) return;

        uintptr_t rendererData = reinterpret_cast<uintptr_t>(*rendererDataPtrPtr);
        auto* device = *reinterpret_cast<ID3D11Device**>(rendererData + 0x48);
        auto* context = *reinterpret_cast<ID3D11DeviceContext**>(rendererData + 0x50);
        if (!device || !context) return;

        uintptr_t rtArrayBase = rendererData + 0x0A58;
        uintptr_t rtEntry = rtArrayBase + static_cast<uintptr_t>(rtIndex) * 0x30;
        auto* rtTexture = *reinterpret_cast<ID3D11Texture2D**>(rtEntry + 0x00);
        if (!rtTexture) return;

        D3D11_TEXTURE2D_DESC texDesc;
        rtTexture->GetDesc(&texDesc);

        // Create CPU-readable staging texture
        if (!s_cpuStagingTex) {
            D3D11_TEXTURE2D_DESC stageDesc = texDesc;
            stageDesc.BindFlags = 0;
            stageDesc.Usage = D3D11_USAGE_STAGING;
            stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stageDesc.MiscFlags = 0;

            HRESULT hr = device->CreateTexture2D(&stageDesc, nullptr, &s_cpuStagingTex);
            if (FAILED(hr)) {
                spdlog::error("[Inv3D-DIAG] Failed to create CPU staging texture: 0x{:X}", (unsigned)hr);
                return;
            }
        }

        // Copy RT → CPU staging
        context->CopyResource(s_cpuStagingTex, rtTexture);

        // Map and read pixels
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = context->Map(s_cpuStagingTex, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            spdlog::error("[Inv3D-DIAG] Map failed: 0x{:X}", (unsigned)hr);
            return;
        }

        spdlog::info("[Inv3D-DIAG] RT 0x{:X} readback: {}x{} fmt={} rowPitch={}",
            rtIndex, texDesc.Width, texDesc.Height, (int)texDesc.Format, mapped.RowPitch);

        // Scan entire RT for non-zero pixels
        uint32_t nonZeroCount = 0;
        uint32_t nonZeroAlphaCount = 0;
        float maxR = 0, maxG = 0, maxB = 0, maxA = 0;
        uint32_t maxPixelX = 0, maxPixelY = 0;

        auto* pixels = reinterpret_cast<uint8_t*>(mapped.pData);
        uint32_t bytesPerPixel = 4;  // R8G8B8A8

        for (uint32_t y = 0; y < texDesc.Height; y++) {
            auto* row = pixels + y * mapped.RowPitch;
            for (uint32_t x = 0; x < texDesc.Width; x++) {
                uint8_t r = row[x * bytesPerPixel + 0];
                uint8_t g = row[x * bytesPerPixel + 1];
                uint8_t b = row[x * bytesPerPixel + 2];
                uint8_t a = row[x * bytesPerPixel + 3];

                if (r > 0 || g > 0 || b > 0) {
                    nonZeroCount++;
                    float fr = r / 255.0f;
                    float fg = g / 255.0f;
                    float fb = b / 255.0f;
                    float brightness = fr > fg ? (fr > fb ? fr : fb) : (fg > fb ? fg : fb);
                    float curMax = maxR > maxG ? (maxR > maxB ? maxR : maxB) : (maxG > maxB ? maxG : maxB);
                    if (brightness > curMax) {
                        maxR = fr; maxG = fg; maxB = fb; maxA = a / 255.0f;
                        maxPixelX = x; maxPixelY = y;
                    }
                }
                if (a > 0) nonZeroAlphaCount++;
            }
        }

        spdlog::info("[Inv3D-DIAG] Non-zero RGB pixels: {} / {} ({:.2f}%)",
            nonZeroCount, texDesc.Width * texDesc.Height,
            100.0f * nonZeroCount / (texDesc.Width * texDesc.Height));
        spdlog::info("[Inv3D-DIAG] Non-zero alpha pixels: {} / {} ({:.2f}%)",
            nonZeroAlphaCount, texDesc.Width * texDesc.Height,
            100.0f * nonZeroAlphaCount / (texDesc.Width * texDesc.Height));
        spdlog::info("[Inv3D-DIAG] Brightest pixel at ({},{}) RGBA=({:.4f},{:.4f},{:.4f},{:.4f})",
            maxPixelX, maxPixelY, maxR, maxG, maxB, maxA);

        // Sample a grid of 5x5 points
        spdlog::info("[Inv3D-DIAG] Sample grid (5x5):");
        for (int gy = 0; gy < 5; gy++) {
            for (int gx = 0; gx < 5; gx++) {
                uint32_t sx = (texDesc.Width * (gx * 2 + 1)) / 10;
                uint32_t sy = (texDesc.Height * (gy * 2 + 1)) / 10;
                auto* row = pixels + sy * mapped.RowPitch;
                uint8_t r = row[sx * bytesPerPixel + 0];
                uint8_t g = row[sx * bytesPerPixel + 1];
                uint8_t b = row[sx * bytesPerPixel + 2];
                uint8_t a = row[sx * bytesPerPixel + 3];
                if (r > 0 || g > 0 || b > 0 || a > 0) {
                    spdlog::info("[Inv3D-DIAG]   ({},{}) RGBA=({},{},{},{})",
                        sx, sy, r, g, b, a);
                }
            }
        }

        context->Unmap(s_cpuStagingTex, 0);
        spdlog::info("[Inv3D-DIAG] Readback complete");
    }

    // ════════════════════════════════════════════════════════════════════════
    // DoAdditiveComposite — Custom compositing via additive blend
    //
    // Instead of the game's alpha-based composite (effect 0xF), we read from
    // the offscreen RT (srcRT) and ADD its RGB onto the screen RT (dstRT).
    // Additive blend: dst.rgb += src.rgb. Black pixels (0,0,0) add nothing.
    // This completely bypasses the alpha=0 problem.
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::DoAdditiveComposite(int srcRT, int dstRT)
    {
        if (!InitD3D11Resources()) return;

        uintptr_t base = REL::Module::get().base();
        auto** rendererDataPtrPtr = reinterpret_cast<uintptr_t**>(base + BSGFX_RENDERER_DATA);
        if (!rendererDataPtrPtr || !*rendererDataPtrPtr) return;

        uintptr_t rendererData = reinterpret_cast<uintptr_t>(*rendererDataPtrPtr);
        auto* device = *reinterpret_cast<ID3D11Device**>(rendererData + 0x48);
        auto* context = *reinterpret_cast<ID3D11DeviceContext**>(rendererData + 0x50);
        if (!context || !device) return;

        // Get source RT (offscreen 3D) texture
        uintptr_t rtArrayBase = rendererData + 0x0A58;
        uintptr_t srcEntry = rtArrayBase + static_cast<uintptr_t>(srcRT) * 0x30;
        auto* srcTexture = *reinterpret_cast<ID3D11Texture2D**>(srcEntry + 0x00);

        // Get destination RT (screen) RTV and texture for viewport dims
        uintptr_t dstEntry = rtArrayBase + static_cast<uintptr_t>(dstRT) * 0x30;
        auto* dstTexture = *reinterpret_cast<ID3D11Texture2D**>(dstEntry + 0x00);
        auto* dstRTV = *reinterpret_cast<ID3D11RenderTargetView**>(dstEntry + 0x10);

        if (!srcTexture || !dstRTV || !dstTexture) {
            spdlog::warn("[Inv3D-P] Additive composite: null resource src=0x{:X} dst=0x{:X}", srcRT, dstRT);
            if (g_originalComposite) {
                g_originalComposite(srcRT, dstRT, 0);
            }
            return;
        }

        D3D11_TEXTURE2D_DESC srcDesc;
        srcTexture->GetDesc(&srcDesc);

        D3D11_TEXTURE2D_DESC dstDesc;
        dstTexture->GetDesc(&dstDesc);

        // ── Copy srcRT to staging texture to avoid resource hazard ──
        // RT 0x3F may still be bound as render target; reading its SRV would
        // return black. Copy to our staging texture first.
        if (!s_stagingTex) {
            D3D11_TEXTURE2D_DESC stageDesc = srcDesc;
            stageDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            stageDesc.Usage = D3D11_USAGE_DEFAULT;
            stageDesc.CPUAccessFlags = 0;
            stageDesc.MiscFlags = 0;

            HRESULT hr = device->CreateTexture2D(&stageDesc, nullptr, &s_stagingTex);
            if (FAILED(hr)) {
                spdlog::error("[Inv3D-P] Failed to create staging texture: 0x{:X}", (unsigned)hr);
                return;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = stageDesc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            srvDesc.Texture2D.MostDetailedMip = 0;

            hr = device->CreateShaderResourceView(s_stagingTex, &srvDesc, &s_stagingSRV);
            if (FAILED(hr)) {
                spdlog::error("[Inv3D-P] Failed to create staging SRV: 0x{:X}", (unsigned)hr);
                s_stagingTex->Release();
                s_stagingTex = nullptr;
                return;
            }

            spdlog::info("[Inv3D-P] Staging texture created: {}x{} fmt={}",
                stageDesc.Width, stageDesc.Height, (int)stageDesc.Format);
        }

        // Copy srcRT → staging (GPU-side, fast, avoids hazard)
        context->CopyResource(s_stagingTex, srcTexture);

        // Update constant buffer with src/dst dimensions
        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            HRESULT hr = context->Map(s_dimsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr)) {
                float* data = reinterpret_cast<float*>(mapped.pData);
                data[0] = static_cast<float>(srcDesc.Width);
                data[1] = static_cast<float>(srcDesc.Height);
                data[2] = static_cast<float>(dstDesc.Width);
                data[3] = static_cast<float>(dstDesc.Height);
                context->Unmap(s_dimsCB, 0);
            }
        }

        // Save D3D11 state
        ID3D11RenderTargetView* savedRTV = nullptr;
        ID3D11DepthStencilView* savedDSV = nullptr;
        context->OMGetRenderTargets(1, &savedRTV, &savedDSV);

        ID3D11BlendState* savedBlendState = nullptr;
        FLOAT savedBlendFactor[4];
        UINT savedSampleMask;
        context->OMGetBlendState(&savedBlendState, savedBlendFactor, &savedSampleMask);

        ID3D11DepthStencilState* savedDepthState = nullptr;
        UINT savedStencilRef;
        context->OMGetDepthStencilState(&savedDepthState, &savedStencilRef);

        ID3D11RasterizerState* savedRastState = nullptr;
        context->RSGetState(&savedRastState);

        D3D11_VIEWPORT savedVP;
        UINT numVP = 1;
        context->RSGetViewports(&numVP, &savedVP);

        ID3D11VertexShader* savedVS = nullptr;
        context->VSGetShader(&savedVS, nullptr, nullptr);
        ID3D11PixelShader* savedPS = nullptr;
        context->PSGetShader(&savedPS, nullptr, nullptr);
        ID3D11InputLayout* savedIL = nullptr;
        context->IAGetInputLayout(&savedIL);
        D3D11_PRIMITIVE_TOPOLOGY savedTopo;
        context->IAGetPrimitiveTopology(&savedTopo);

        ID3D11ShaderResourceView* savedPSSRV = nullptr;
        context->PSGetShaderResources(0, 1, &savedPSSRV);

        ID3D11Buffer* savedPSCB = nullptr;
        context->PSGetConstantBuffers(0, 1, &savedPSCB);

        // Set up additive composite pipeline
        // Bind dstRT as render target, staging copy as SRV input
        context->OMSetRenderTargets(1, &dstRTV, nullptr);
        FLOAT blendFactor[4] = { 0, 0, 0, 0 };
        context->OMSetBlendState(s_additiveBlendState, blendFactor, 0xFFFFFFFF);
        context->OMSetDepthStencilState(s_alphaDepthState, 0);
        context->RSSetState(s_alphaRastState);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<FLOAT>(dstDesc.Width);
        vp.Height = static_cast<FLOAT>(dstDesc.Height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(s_alphaVS, nullptr, 0);
        context->PSSetShader(s_passthroughPS, nullptr, 0);
        context->PSSetShaderResources(0, 1, &s_stagingSRV);  // staging copy, no hazard
        context->PSSetConstantBuffers(0, 1, &s_dimsCB);

        // Draw fullscreen triangle: reads staging copy, adds to screen RT
        context->Draw(3, 0);

        auto& self = GetSingleton();
        if (self._renderLogCount < MAX_RENDER_LOG) {
            spdlog::info("[Inv3D-P] Additive composite: src=0x{:X}({}x{}) → dst=0x{:X}({}x{}) via staging",
                srcRT, srcDesc.Width, srcDesc.Height,
                dstRT, dstDesc.Width, dstDesc.Height);
            self._renderLogCount++;
        }

        // Restore D3D11 state
        context->OMSetRenderTargets(1, &savedRTV, savedDSV);
        context->OMSetBlendState(savedBlendState, savedBlendFactor, savedSampleMask);
        context->OMSetDepthStencilState(savedDepthState, savedStencilRef);
        context->RSSetState(savedRastState);
        context->RSSetViewports(1, &savedVP);
        context->IASetInputLayout(savedIL);
        context->IASetPrimitiveTopology(savedTopo);
        context->VSSetShader(savedVS, nullptr, 0);
        context->PSSetShader(savedPS, nullptr, 0);
        context->PSSetShaderResources(0, 1, &savedPSSRV);
        context->PSSetConstantBuffers(0, 1, &savedPSCB);

        if (savedRTV) savedRTV->Release();
        if (savedDSV) savedDSV->Release();
        if (savedBlendState) savedBlendState->Release();
        if (savedDepthState) savedDepthState->Release();
        if (savedRastState) savedRastState->Release();
        if (savedVS) savedVS->Release();
        if (savedPS) savedPS->Release();
        if (savedIL) savedIL->Release();
        if (savedPSSRV) savedPSSRV->Release();
        if (savedPSCB) savedPSCB->Release();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Helper: Get the Interface3D::Renderer for an Inventory3DManager
    // ════════════════════════════════════════════════════════════════════════

    void* Inventory3DFix::GetInventoryRenderer(uintptr_t inv3dMgr)
    {
        if (!g_I3DGetByName || !inv3dMgr) return nullptr;

        auto* namePtr = reinterpret_cast<RE::BSFixedString*>(inv3dMgr + OFF_INV3D_RENDERER_NAME);
        if (!namePtr || !namePtr->c_str() || namePtr->c_str()[0] == '\0') return nullptr;

        return g_I3DGetByName(*namePtr);
    }

    // ════════════════════════════════════════════════════════════════════════
    // APPROACH A: Force flat VR mode during transform update
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::ApproachA_ForceFlat(uintptr_t inv3dMgr, uintptr_t loadedModel)
    {
        void* renderer = GetInventoryRenderer(inv3dMgr);
        int savedVrMode = 0;

        if (renderer) {
            auto rendBase = reinterpret_cast<uintptr_t>(renderer);
            auto* vrModePtr = reinterpret_cast<int*>(rendBase + OFF_RENDERER_VR_MODE);
            savedVrMode = *vrModePtr;

            if (savedVrMode != 0) {
                *vrModePtr = 0;
                if (_logCount < MAX_LOG_ENTRIES) {
                    spdlog::info("[Inv3D-A] Forcing vrMode=0 (was {})", savedVrMode);
                    _logCount++;
                }
            }
        }

        if (g_originalUpdateModelTransform) {
            g_originalUpdateModelTransform(inv3dMgr, loadedModel);
        }

        if (renderer && savedVrMode != 0) {
            *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(renderer) + OFF_RENDERER_VR_MODE) = savedVrMode;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // APPROACH B: Override camera frustum
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::ApproachB_OverrideFrustum(uintptr_t inv3dMgr, uintptr_t loadedModel)
    {
        void* renderer = GetInventoryRenderer(inv3dMgr);
        uintptr_t nativeCam = 0;
        float savedFrustum[6] = {};
        bool frustumSaved = false;

        if (renderer) {
            auto rendBase = reinterpret_cast<uintptr_t>(renderer);
            int vrMode = *reinterpret_cast<int*>(rendBase + OFF_RENDERER_VR_MODE);

            if (vrMode != 0) {
                nativeCam = *reinterpret_cast<uintptr_t*>(rendBase + OFF_RENDERER_CAM_NATIVE);
                uintptr_t pipboyCam = *reinterpret_cast<uintptr_t*>(rendBase + OFF_RENDERER_CAM_PIPBOY);

                if (nativeCam > 0x10000 && pipboyCam > 0x10000) {
                    auto* pipboyFrustumPtr = *reinterpret_cast<float**>(pipboyCam + OFF_CAM_FRUSTUM_PTR);
                    auto* nativeFrustumPtr = *reinterpret_cast<float**>(nativeCam + OFF_CAM_FRUSTUM_PTR);

                    if (pipboyFrustumPtr && nativeFrustumPtr) {
                        std::memcpy(savedFrustum, nativeFrustumPtr, sizeof(savedFrustum));
                        frustumSaved = true;
                        std::memcpy(nativeFrustumPtr, pipboyFrustumPtr, sizeof(savedFrustum));

                        if (_logCount < MAX_LOG_ENTRIES) {
                            spdlog::info("[Inv3D-B] Overriding native cam frustum with pipboy cam");
                            _logCount++;
                        }
                    }
                }
            } else {
                if (_logCount < MAX_LOG_ENTRIES) {
                    spdlog::info("[Inv3D-B] vrMode already 0, skipping");
                    _logCount++;
                }
            }
        }

        if (g_originalUpdateModelTransform) {
            g_originalUpdateModelTransform(inv3dMgr, loadedModel);
        }

        if (frustumSaved && nativeCam > 0x10000) {
            auto* nativeFrustumPtr = *reinterpret_cast<float**>(nativeCam + OFF_CAM_FRUSTUM_PTR);
            if (nativeFrustumPtr) {
                std::memcpy(nativeFrustumPtr, savedFrustum, sizeof(savedFrustum));
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // APPROACH C: Force flat mode + swap VR dimension globals
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::ApproachC_RecomputePosition(uintptr_t inv3dMgr, uintptr_t loadedModel)
    {
        void* renderer = GetInventoryRenderer(inv3dMgr);
        int savedVrMode = 0;
        uint32_t savedVrWidth = 0, savedVrHeight = 0;
        bool dimsSwapped = false;

        if (renderer) {
            auto rendBase = reinterpret_cast<uintptr_t>(renderer);
            auto* vrModePtr = reinterpret_cast<int*>(rendBase + OFF_RENDERER_VR_MODE);
            savedVrMode = *vrModePtr;

            if (savedVrMode != 0) {
                *vrModePtr = 0;

                uintptr_t base = REL::Module::get().base();
                auto* vrWidthPtr  = reinterpret_cast<uint32_t*>(base + VR_SCREEN_WIDTH_GLOBAL);
                auto* vrHeightPtr = reinterpret_cast<uint32_t*>(base + VR_SCREEN_HEIGHT_GLOBAL);
                auto* flatWidthPtr  = reinterpret_cast<uint32_t*>(base + FLAT_SCREEN_WIDTH_GLOBAL);
                auto* flatHeightPtr = reinterpret_cast<uint32_t*>(base + FLAT_SCREEN_HEIGHT_GLOBAL);

                savedVrWidth  = *vrWidthPtr;
                savedVrHeight = *vrHeightPtr;
                *vrWidthPtr  = *flatWidthPtr;
                *vrHeightPtr = *flatHeightPtr;
                dimsSwapped = true;

                if (_logCount < MAX_LOG_ENTRIES) {
                    spdlog::info("[Inv3D-C] Forced flat mode + swapped VR dims {}x{} -> {}x{}",
                        savedVrWidth, savedVrHeight, *flatWidthPtr, *flatHeightPtr);
                    _logCount++;
                }
            }
        }

        if (g_originalUpdateModelTransform) {
            g_originalUpdateModelTransform(inv3dMgr, loadedModel);
        }

        if (renderer && savedVrMode != 0) {
            *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(renderer) + OFF_RENDERER_VR_MODE) = savedVrMode;
        }
        if (dimsSwapped) {
            uintptr_t base = REL::Module::get().base();
            *reinterpret_cast<uint32_t*>(base + VR_SCREEN_WIDTH_GLOBAL)  = savedVrWidth;
            *reinterpret_cast<uint32_t*>(base + VR_SCREEN_HEIGHT_GLOBAL) = savedVrHeight;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // APPROACH D: Swap camera pointer
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::ApproachD_SwapCamera(uintptr_t inv3dMgr, uintptr_t loadedModel)
    {
        void* renderer = GetInventoryRenderer(inv3dMgr);
        uintptr_t savedNativeCam = 0;
        bool cameraSwapped = false;

        if (renderer) {
            auto rendBase = reinterpret_cast<uintptr_t>(renderer);
            int vrMode = *reinterpret_cast<int*>(rendBase + OFF_RENDERER_VR_MODE);

            if (vrMode != 0) {
                auto* nativeCamPtr = reinterpret_cast<uintptr_t*>(rendBase + OFF_RENDERER_CAM_NATIVE);
                auto* pipboyCamPtr = reinterpret_cast<uintptr_t*>(rendBase + OFF_RENDERER_CAM_PIPBOY);

                savedNativeCam = *nativeCamPtr;
                uintptr_t pipboyCam = *pipboyCamPtr;

                if (pipboyCam > 0x10000 && savedNativeCam > 0x10000) {
                    *nativeCamPtr = pipboyCam;
                    cameraSwapped = true;

                    if (_logCount < MAX_LOG_ENTRIES) {
                        spdlog::info("[Inv3D-D] Swapped native cam ({:X}) with pipboy cam ({:X})",
                            savedNativeCam, pipboyCam);
                        _logCount++;
                    }
                }
            }
        }

        if (g_originalUpdateModelTransform) {
            g_originalUpdateModelTransform(inv3dMgr, loadedModel);
        }

        if (cameraSwapped && renderer) {
            *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(renderer) + OFF_RENDERER_CAM_NATIVE) = savedNativeCam;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Per-frame update: diagnostics only (no state forcing)
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::OnFrameUpdate()
    {
        if (s_fixMode == 0) return;
        if (!g_I3DGetByName) return;

        if (!_frameUpdateVerified) {
            _frameUpdateVerified = true;
            spdlog::info("[Inv3D] OnFrameUpdate running — per-frame hook is active");
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Diagnostics: log renderer state for debugging
    // ════════════════════════════════════════════════════════════════════════

    void Inventory3DFix::LogRendererState()
    {
        if (!g_I3DGetByName) {
            g_I3DGetByName = reinterpret_cast<I3DGetByName_t>(REL::Offset(VR_I3D_GET_BY_NAME).address());
        }

        static const char* rendererNames[] = {
            "PipboyMenu", "InventoryMenu", "ContainerMenu",
            "ExamineMenu", "BarterMenu", "WorkshopMenu",
            "CookingMenu", "PowerArmorMenu"
        };

        uintptr_t base = REL::Module::get().base();
        auto vrWidth  = *reinterpret_cast<uint32_t*>(base + VR_SCREEN_WIDTH_GLOBAL);
        auto vrHeight = *reinterpret_cast<uint32_t*>(base + VR_SCREEN_HEIGHT_GLOBAL);
        auto flatWidth  = *reinterpret_cast<uint32_t*>(base + FLAT_SCREEN_WIDTH_GLOBAL);
        auto flatHeight = *reinterpret_cast<uint32_t*>(base + FLAT_SCREEN_HEIGHT_GLOBAL);

        spdlog::info("[Inv3D-Diag] VR dims: {}x{}, Flat dims: {}x{}",
            vrWidth, vrHeight, flatWidth, flatHeight);

        for (const char* name : rendererNames) {
            RE::BSFixedString bsName(name);
            void* rend = g_I3DGetByName(bsName);
            if (!rend) continue;

            auto b = reinterpret_cast<uintptr_t>(rend);
            auto active   = *reinterpret_cast<uint8_t*>(b + OFF_RENDERER_ACTIVE);
            auto enabled  = *reinterpret_cast<uint8_t*>(b + OFF_RENDERER_ENABLED);
            auto display  = *reinterpret_cast<int32_t*>(b + OFF_RENDERER_DISPLAY);
            auto vrMode   = *reinterpret_cast<int32_t*>(b + OFF_RENDERER_VR_MODE);
            auto offscr3D = *reinterpret_cast<uintptr_t*>(b + OFF_RENDERER_OFFSCR_3D);
            auto rtOff    = *reinterpret_cast<int32_t*>(b + OFF_RENDERER_RT_OFFSCR);
            auto rtScr    = *reinterpret_cast<int32_t*>(b + OFF_RENDERER_RT_SCREEN);
            auto camPip   = *reinterpret_cast<uintptr_t*>(b + OFF_RENDERER_CAM_PIPBOY);
            auto camNat   = *reinterpret_cast<uintptr_t*>(b + OFF_RENDERER_CAM_NATIVE);
            auto camWld   = *reinterpret_cast<uintptr_t*>(b + OFF_RENDERER_CAM_WORLD);
            auto renderSel = *reinterpret_cast<int32_t*>(b + OFF_RENDERER_RENDER_SEL);
            auto screen3D  = *reinterpret_cast<uintptr_t*>(b + OFF_RENDERER_SCREEN_3D);
            auto field_C0  = *reinterpret_cast<uintptr_t*>(b + OFF_RENDERER_FIELD_C0);
            auto field_C8  = *reinterpret_cast<uint8_t*>(b + OFF_RENDERER_FIELD_C8);

            spdlog::info("[Inv3D-Diag] '{}': active={} enabled={} display={} vrMode={} renderSel={} "
                "offscr3D={:X} screen3D={:X} fC0={:X} fC8={} rtOff={} rtScr={} "
                "camPipboy={:X} camNative={:X} camWorld={:X}",
                name, active, enabled, display, vrMode, renderSel,
                offscr3D, screen3D, field_C0, field_C8, rtOff, rtScr,
                camPip, camNat, camWld);
        }
    }
}
