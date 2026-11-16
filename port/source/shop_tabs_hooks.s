/* Wrapper for Menu_IGM_Inventory::MakeLogicalTabNodeTable (shop_tabs.c). The function is static
 * and returns a std::vector through x8 (option in w1). The wrapper runs the original (its first
 * four position-independent instructions here, then a jump to original+16) and then calls
 * shop_tabs_fixup(vector). */
    .text
    .global shop_tabs_make_logical_hook
    .type shop_tabs_make_logical_hook, %function
shop_tabs_make_logical_hook:
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    str  x8, [sp, #16]
    bl   shop_tabs_orig
    ldr  x0, [sp, #16]
    bl   shop_tabs_fixup
    ldr  x8, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
    .size shop_tabs_make_logical_hook, .-shop_tabs_make_logical_hook

    .global shop_tabs_orig
    .type shop_tabs_orig, %function
    .global shop_tabs_orig_head
shop_tabs_orig:
shop_tabs_orig_head:
    sub  sp, sp, #0x50             /* engine +0xb74bd0 */
    stp  x20, x19, [sp, #0x30]
    stp  x29, x30, [sp, #0x40]
    add  x29, sp, #0x40
    adrp x16, shop_tabs_orig_cont
    ldr  x16, [x16, :lo12:shop_tabs_orig_cont]
    br   x16
    .size shop_tabs_orig, .-shop_tabs_orig

/* Original Menu_IGM_Inventory::ResetInventory (timed by shop_reset_inventory in shop_tabs.c). */
    .global shop_reset_orig
    .type shop_reset_orig, %function
    .global shop_reset_orig_head
shop_reset_orig:
shop_reset_orig_head:
    sub  sp, sp, #0xb0             /* engine +0xb787c4 */
    stp  x26, x25, [sp, #0x60]
    stp  x24, x23, [sp, #0x70]
    stp  x22, x21, [sp, #0x80]
    adrp x16, shop_reset_orig_cont
    ldr  x16, [x16, :lo12:shop_reset_orig_cont]
    br   x16
    .size shop_reset_orig, .-shop_reset_orig
