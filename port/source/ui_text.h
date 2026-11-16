#pragma once
#include <stdint.h>

/* Invalid or truncated UTF-8 consumes one byte and draws a replacement glyph. */
static unsigned ui_next_cp(const unsigned char **pp) {
  const unsigned char *p = *pp;
  unsigned a = *p, cp;
  int n;
  if (a < 0x80) { if (a) *pp = p + 1; return a; }
  if (a >= 0xc2 && a <= 0xdf) { cp = a & 31; n = 2; }
  else if (a >= 0xe0 && a <= 0xef) { cp = a & 15; n = 3; }
  else if (a >= 0xf0 && a <= 0xf4) { cp = a & 7; n = 4; }
  else { *pp = p + 1; return 0xfffd; }
  for (int i = 1; i < n; i++) {
    if ((p[i] & 0xc0) != 0x80) { *pp = p + 1; return 0xfffd; }
    cp = (cp << 6) | (p[i] & 63);
  }
  if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
      cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
    *pp = p + 1; return 0xfffd;
  }
  *pp = p + n;
  return cp;
}
