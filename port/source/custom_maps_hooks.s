/* Trampolines for custom_maps.c: each runs the original function's first four instructions (copied
 * from the engine, position independent; custom_maps.c compares them with the engine before hooking)
 * and continues at the original + 16. */
    .text
    .align 4

/* pig::stream::ZipStreamFactory::CreateStream(pig::String const&) */
    .global cm_zip_orig
    .type cm_zip_orig, %function
    .global cm_zip_orig_head
cm_zip_orig:
cm_zip_orig_head:
    sub  sp, sp, #0x120            /* engine +0x125d9c4 */
    stp  x28, x23, [sp, #224]
    stp  x22, x21, [sp, #240]
    stp  x20, x19, [sp, #256]
    adrp x16, cm_zip_orig_cont
    ldr  x16, [x16, :lo12:cm_zip_orig_cont]
    br   x16
    .size cm_zip_orig, .-cm_zip_orig

/* CMatchingLocal::AddServer(int, CNetworkId&, int, CRoomAttributes&) */
    .global cm_addserver_orig
    .type cm_addserver_orig, %function
    .global cm_addserver_orig_head
cm_addserver_orig:
cm_addserver_orig_head:
    stp  x28, x25, [sp, #-80]!     /* engine +0xd27804 */
    stp  x24, x23, [sp, #16]
    stp  x22, x21, [sp, #32]
    stp  x20, x19, [sp, #48]
    adrp x16, cm_addserver_orig_cont
    ldr  x16, [x16, :lo12:cm_addserver_orig_cont]
    br   x16
    .size cm_addserver_orig, .-cm_addserver_orig

/* Menu_MP_CreateHost::InitUI() */
    .global cm_initui_orig
    .type cm_initui_orig, %function
    .global cm_initui_orig_head
cm_initui_orig:
cm_initui_orig_head:
    sub  sp, sp, #0xe0             /* engine +0xba1728 */
    str  x21, [sp, #176]
    stp  x20, x19, [sp, #192]
    stp  x29, x30, [sp, #208]
    adrp x16, cm_initui_orig_cont
    ldr  x16, [x16, :lo12:cm_initui_orig_cont]
    br   x16
    .size cm_initui_orig, .-cm_initui_orig

/* Menu_MP_CreateHost::onMapSelected(int) */
    .global cm_mapsel_orig
    .type cm_mapsel_orig, %function
    .global cm_mapsel_orig_head
cm_mapsel_orig:
cm_mapsel_orig_head:
    str  x23, [sp, #-64]!          /* engine +0xba24ac */
    stp  x22, x21, [sp, #16]
    stp  x20, x19, [sp, #32]
    stp  x29, x30, [sp, #48]
    adrp x16, cm_mapsel_orig_cont
    ldr  x16, [x16, :lo12:cm_mapsel_orig_cont]
    br   x16
    .size cm_mapsel_orig, .-cm_mapsel_orig
