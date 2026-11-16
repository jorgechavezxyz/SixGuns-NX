/* LAN: entry stub for the CUdp::GetLocalNetworkId replacement (net.c).
 * The engine returns the CNetworkId through the indirect-result register x8;
 * pass it to the C function as its second argument. */
    .text
    .global net_udp_local_network_id_hook
    .type net_udp_local_network_id_hook, %function
net_udp_local_network_id_hook:
    mov x1, x8
    b net_udp_local_network_id
    .size net_udp_local_network_id_hook, .-net_udp_local_network_id_hook

/* Original GS_GamePlay::StartDying (net.c start_dying_hook): its first four instructions, then a jump
 * to original+16. */
    .text
    .global start_dying_orig
    .type start_dying_orig, %function
    .global start_dying_orig_head
start_dying_orig:
start_dying_orig_head:
    sub  sp, sp, #0x40             /* engine +0xc3d08c */
    stp  x20, x19, [sp, #32]
    stp  x29, x30, [sp, #48]
    add  x29, sp, #0x30
    adrp x16, start_dying_orig_cont
    ldr  x16, [x16, :lo12:start_dying_orig_cont]
    br   x16
    .size start_dying_orig, .-start_dying_orig

/* Original Multiplayer::RevivePlayer(bool) (net.c revive_hook). */
    .text
    .global revive_orig
    .type revive_orig, %function
    .global revive_orig_head
revive_orig:
revive_orig_head:
    stp  x20, x19, [sp, #-32]!     /* engine +0xc183f0 */
    stp  x29, x30, [sp, #16]
    add  x29, sp, #0x10
    mov  w19, w1
    adrp x16, revive_orig_cont
    ldr  x16, [x16, :lo12:revive_orig_cont]
    br   x16
    .size revive_orig, .-revive_orig

/* Original Player::GetArmor(int) (net.c armor_hook, LAN armor cap). */
    .text
    .global armor_orig
    .type armor_orig, %function
    .global armor_orig_head
armor_orig:
armor_orig_head:
    str  x25, [sp, #-80]!          /* engine +0xa28f44 */
    stp  x24, x23, [sp, #16]
    stp  x22, x21, [sp, #32]
    stp  x20, x19, [sp, #48]
    adrp x16, armor_orig_cont
    ldr  x16, [x16, :lo12:armor_orig_cont]
    br   x16
    .size armor_orig, .-armor_orig

/* Original PlayerCtrl::UpdateFreeCamera (net.c free_cam_hook). */
    .text
    .global free_cam_orig
    .type free_cam_orig, %function
    .global free_cam_orig_head
free_cam_orig:
free_cam_orig_head:
    sub  sp, sp, #0x150            /* engine +0xa3078c */
    stp  d11, d10, [sp, #208]
    stp  d9, d8, [sp, #224]
    stp  x28, x27, [sp, #240]
    adrp x16, free_cam_orig_cont
    ldr  x16, [x16, :lo12:free_cam_orig_cont]
    br   x16
    .size free_cam_orig, .-free_cam_orig
