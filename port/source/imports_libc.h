#pragma once
#include <stdint.h>

extern uint8_t fake_sF[3][152];

/* Points TPIDR_EL0 at a fresh bionic-style TLS block (stack guard slot).
 * Call on every thread before it runs engine code. */
void bionic_tls_init_thread(void);
