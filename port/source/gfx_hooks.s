/* Trampolines for gfx_settings.c: each runs the original function's first four instructions (copied
 * from the engine, position independent; gfx_settings.c compares them with the engine before hooking)
 * and continues at the original + 16. */
    .text
    .align 4

/* pig::scene::SceneMgr::Render */
    .global gfx_scene_orig
    .type gfx_scene_orig, %function
    .global gfx_scene_orig_head
gfx_scene_orig:
gfx_scene_orig_head:
    str  d8, [sp, #-80]!           /* engine +0x1252294 */
    stp  x24, x23, [sp, #16]
    stp  x22, x21, [sp, #32]
    stp  x20, x19, [sp, #48]
    adrp x16, gfx_scene_orig_cont
    ldr  x16, [x16, :lo12:gfx_scene_orig_cont]
    br   x16
    .size gfx_scene_orig, .-gfx_scene_orig

/* pig::scene::Model::PreRender */
    .global gfx_prerender_orig
    .type gfx_prerender_orig, %function
    .global gfx_prerender_orig_head
gfx_prerender_orig:
gfx_prerender_orig_head:
    sub  sp, sp, #0x70             /* engine +0x124b1d8 */
    str  d8, [sp, #16]
    stp  x26, x25, [sp, #32]
    stp  x24, x23, [sp, #48]
    adrp x16, gfx_prerender_orig_cont
    ldr  x16, [x16, :lo12:gfx_prerender_orig_cont]
    br   x16
    .size gfx_prerender_orig, .-gfx_prerender_orig

/* GS_GamePlay::UpdatePostFx */
    .global gfx_postfx_orig
    .type gfx_postfx_orig, %function
    .global gfx_postfx_orig_head
gfx_postfx_orig:
gfx_postfx_orig_head:
    sub  sp, sp, #0x50             /* engine +0xc38b0c */
    stp  x22, x21, [sp, #32]
    stp  x20, x19, [sp, #48]
    stp  x29, x30, [sp, #64]
    adrp x16, gfx_postfx_orig_cont
    ldr  x16, [x16, :lo12:gfx_postfx_orig_cont]
    br   x16
    .size gfx_postfx_orig, .-gfx_postfx_orig

/* ShadowRenderable::Render */
    .global gfx_shadow_orig
    .type gfx_shadow_orig, %function
    .global gfx_shadow_orig_head
gfx_shadow_orig:
gfx_shadow_orig_head:
    str  x19, [sp, #-32]!          /* engine +0xc3130c */
    stp  x29, x30, [sp, #16]
    add  x29, sp, #0x10
    mov  x19, x0
    adrp x16, gfx_shadow_orig_cont
    ldr  x16, [x16, :lo12:gfx_shadow_orig_cont]
    br   x16
    .size gfx_shadow_orig, .-gfx_shadow_orig
