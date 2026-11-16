/* Trampoline running the engine's original LayerGrid::Render after the port hooked it
 * (grid_cull.c): its first four instructions (position-independent prologue, checked against
 * the image before hooking), then a jump to original+16. x16 is scratch (IP0). */
    .text
    .global grid_orig_render
    .type grid_orig_render, %function
    .global grid_orig_render_head
grid_orig_render:
grid_orig_render_head:
    sub  sp, sp, #0x70             /* engine +0xb4adf8 */
    stp  d9, d8, [sp, #0x20]
    stp  x24, x23, [sp, #0x30]
    stp  x22, x21, [sp, #0x40]
    adrp x16, grid_orig_render_cont
    ldr  x16, [x16, :lo12:grid_orig_render_cont]
    br   x16
    .size grid_orig_render, .-grid_orig_render
