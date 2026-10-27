/*
 * Bionic ctype tables. The NDK <ctype.h> macros index tab[c+1] for c in
 * [-1,255], so each exported symbol points at a 257-entry table whose slot 0
 * is the EOF entry. Flags are generated from newlib classifiers at init.
 */
#include <ctype.h>
#include <stdint.h>

#define _B_U 0x01
#define _B_L 0x02
#define _B_D 0x04
#define _B_S 0x08
#define _B_P 0x10
#define _B_C 0x20
#define _B_X 0x40
#define _B_B 0x80

static unsigned char ctype_tab[257];
static short lower_tab[257];
static short upper_tab[257];

/* Exported object symbols the game links against. */
const unsigned char *b__ctype_ = ctype_tab;
const short *b__tolower_tab_ = lower_tab;
const short *b__toupper_tab_ = upper_tab;

void bionic_ctype_init(void) {
  for (int c = -1; c < 256; c++) {
    unsigned char f = 0;
    int i = c + 1;
    if (c >= 0) {
      if (isupper(c)) f |= _B_U;
      if (islower(c)) f |= _B_L;
      if (isdigit(c)) f |= _B_D;
      if (isspace(c)) f |= _B_S;
      if (ispunct(c)) f |= _B_P;
      if (iscntrl(c)) f |= _B_C;
      if (isxdigit(c)) f |= _B_X;
      if (c == ' ') f |= _B_B;
      lower_tab[i] = (short)tolower(c);
      upper_tab[i] = (short)toupper(c);
    } else {
      lower_tab[i] = -1;
      upper_tab[i] = -1;
    }
    ctype_tab[i] = f;
  }
}

/* Bionic also exposes these function forms; route to newlib. */
int b_isalpha(int c) { return isalpha(c); }
int b_isdigit(int c) { return isdigit(c); }
int b_isspace(int c) { return isspace(c); }
int b_isalnum(int c) { return isalnum(c); }
int b_isxdigit(int c) { return isxdigit(c); }
int b_tolower(int c) { return tolower(c); }
int b_toupper(int c) { return toupper(c); }
