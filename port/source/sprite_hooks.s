/* Entry stubs for the SpriteMgr::getNewScale / getNewOffset replacements (sprite_cache.c).
 * The engine returns the vector through the indirect-result register x8; the C functions take it
 * as their first argument (x0 is unused by the originals). */
    .text
    .global sprite_get_new_scale_hook
    .type sprite_get_new_scale_hook, %function
sprite_get_new_scale_hook:
    mov x0, x8
    b sprite_get_new_scale
    .size sprite_get_new_scale_hook, .-sprite_get_new_scale_hook

    .global sprite_get_new_offset_hook
    .type sprite_get_new_offset_hook, %function
sprite_get_new_offset_hook:
    mov x0, x8
    b sprite_get_new_offset
    .size sprite_get_new_offset_hook, .-sprite_get_new_offset_hook
