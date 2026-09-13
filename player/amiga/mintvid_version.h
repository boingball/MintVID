#ifndef MINTVID_VERSION_H
#define MINTVID_VERSION_H

/* Human-facing semantic version and AmigaOS Version-command identity. Amiga
 * $VER strings conventionally use version.revision rather than three-part
 * semantic versions; MINTVID_VERSION retains the complete release number
 * shown by the GUIs. */
#define MINTVID_VERSION       "1.3.0"
#define MINTVID_AMIGA_VERSION "1.3"
#define MINTVID_VERSION_DATE  "12.9.2026"

#if defined(__GNUC__)
#define MINTVID_VERSION_USED __attribute__((used))
#else
#define MINTVID_VERSION_USED
#endif

#define MINTVID_DECLARE_VERSION(symbol, program) \
    const char symbol[] MINTVID_VERSION_USED = \
        "\0$VER: " program " " MINTVID_AMIGA_VERSION \
        " (" MINTVID_VERSION_DATE ")"

#endif
