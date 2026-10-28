/* Spider-Man (2000) PC, Treyarch port, SpideyPC.exe linked 2001-08-15.
 *
 * The game finds Media.pkr in its current directory (PCMOVIE_Init in spidey-decomp), so no CD
 * drive is needed. Saves go to "save\" in the game folder.
 */
#ifndef RC_PROFILE_H
#define RC_PROFILE_H

#define PROFILE_EXE        "SpideyPC.exe"
#define PROFILE_GUEST_ROOT "C:\\Spider-Man"
#define PROFILE_GUEST_EXE  "SpideyPC.exe"
#define PROFILE_TITLE      "Spider-Man"

/* Registry values the game reads. None are known to be required yet; RC_LOG=1 lists every lookup. */
#define PROFILE_REGISTRY

#endif
