/* Trampolines that run the engine's original Font::MapChar / bidi::get_char_type after the port
 * has hooked them (text_cache.c). Each holds the original's first four instructions (overwritten
 * by the hook; checked against the image before hooking, see *_head) and then jumps to
 * original+16. x16 is scratch (IP0) in AAPCS64. */
    .text
    .global text_orig_map_char
    .type text_orig_map_char, %function
    .global text_orig_map_char_head
text_orig_map_char:
text_orig_map_char_head:
    ldr  w9, [x0, #0x38]          /* engine +0xd021a0 */
    cmp  w1, #0x80
    mov  w8, #0x20ac
    csel w8, w8, w1, eq
    adrp x16, text_orig_map_char_cont
    ldr  x16, [x16, :lo12:text_orig_map_char_cont]
    br   x16
    .size text_orig_map_char, .-text_orig_map_char

    .global text_orig_bidi_char_type
    .type text_orig_bidi_char_type, %function
    .global text_orig_bidi_head
text_orig_bidi_char_type:
text_orig_bidi_head:
    mov  w8, w0                    /* engine +0xc6c6f4 */
    mov  w9, #0x269
    mov  w12, #-1
    mov  w0, #0x12
    adrp x16, text_orig_bidi_cont
    ldr  x16, [x16, :lo12:text_orig_bidi_cont]
    br   x16
    .size text_orig_bidi_char_type, .-text_orig_bidi_char_type
