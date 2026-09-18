#include "core/sds.h"
#include "input/input.h"
#include "input/input_internal.h"
#include "pdf/pdf.h"

Mouse mev;
int mouse_cfg = 1;          /* top-level `mouse = off` turns it off */

void mouse_enable(int on) {
    if (!mouse_cfg) return;
    /* 1000 = press/release, 1002 = also report motion while a button is
     * down (drag-to-select), 1006 = SGR coordinates. */
    printf(on ? "\033[?1000h\033[?1002h\033[?1006h"
              : "\033[?1006l\033[?1002l\033[?1000l");
}
/* Hold Shift and the terminal keeps the mouse for itself, so the user can
 * still select and copy text out of sds the way they always could. */
static int mouse_decode(int cb, int col, int row, int press) {
    /* Turned off in the config: swallow anything that arrives anyway rather
     * than act on it — another program may have left reporting switched on. */
    if (!mouse_cfg) return K_NONE;
    mev.y = row - 1;
    mev.x = col - 1;
    mev.shift  = (cb & 4)  != 0;
    mev.alt    = (cb & 8)  != 0;
    mev.ctrl   = (cb & 16) != 0;
    mev.motion = (cb & 32) != 0;
    mev.btn    = (cb & 64) ? MB_WHEEL_UP + (cb & 3) : (cb & 3);
    mev.press  = press;
    return K_MOUSE;
}

/* ── input ────────────────────────────────────────────────────────── */
/* One key reported through modifyOtherKeys (CSI 27;mod;code ~) or the CSI u
 * form: a code point plus xterm's modifier mask. Terminals only fall back on
 * these for combinations with no legacy encoding — Shift+Enter is the one sds
 * needs — but decode the rest too, so a terminal that reports more than it has
 * to does not silently lose bindings. */
static int mok_key(int code, int mod) {
    if (mod < 1) mod = 1;
    int shift = (mod - 1) & 1, alt = (mod - 1) & 2, ctrl = (mod - 1) & 4;
    if (code == 13 || code == 10)
        return alt ? (shift ? K_ASENTER : ALT('\n')) : '\r';
    if (code == 9)   return shift ? KEY_BTAB : '\t';
    if (code == 27)  return 27;
    if (code == 8 || code == 127) return KEY_BACKSPACE;
    if (code < 32 || code > 0x10ffff) return K_NONE;
    if (ctrl && !alt) {
        if (code == '/') return 31;             /* what the key really types */
        return CTRL(tolower(code));
    }
    if (alt && !ctrl) {
        char ch[8];
        int n = pdf_utf8((uint32_t)code, ch);
        int d = kb_digit_of(ch, n);             /* Alt+Shift+<digit> → pane N */
        if (d >= 0) return PKEY(d);
        return code < 128 ? ALT(tolower(code)) : K_NONE;
    }
    return (!alt && !ctrl && code < 128) ? code : K_NONE;
}
/* Parse the tail of a CSI sequence ncurses didn't decode itself.
 * `defmod` is the modifier to assume when the sequence carries none:
 * 3 (Alt) if we saw a doubled ESC, 0 (plain) for a bare ESC [ ... .
 * Getting this right matters: a plain Up that ncurses failed to decode
 * must stay Up, not silently become Alt+Up.                            */
static int csi_tail(int defmod) {
    int ch, mod = 0, num = 0, first = 0, nnum = 0, final = 0, mouse = 0;
    int p[3] = { 0, 0, 0 };                     /* the first three parameters */
    while ((ch = getch()) != ERR) {
        if (ch == '<' && !nnum && !num) { mouse = 1; continue; }
        if (isdigit(ch)) num = num * 10 + (ch - '0');
        else if (ch == ';') {
            if (!nnum) first = num;
            if (nnum < 3) p[nnum] = num;
            nnum++; num = 0;
        }
        else {
            final = ch;
            if (nnum >= 1) mod = num; else first = num;
            if (nnum < 3) p[nnum] = num;
            break;
        }
    }
    if (mouse)                                     /* CSI < btn ; col ; row M */
        return mouse_decode(p[0], p[1], p[2], final == 'M');
    if (final == 'u')                              /* CSI code ; mod u */
        return mok_key(first, nnum >= 1 ? p[1] : 1);
    if (final == '~' && first == 27 && nnum >= 2)  /* CSI 27 ; mod ; code ~ */
        return mok_key(p[2], p[1]);
    if (mod < 2 || mod > 8) mod = defmod;
    int alt = (mod == 3 || mod == 4);
    if (final == '~') {
        if (first == 3) return alt ? K_ADEL : KEY_DC;    /* Delete */
        if (first == 2) return alt ? K_AINS : KEY_IC;    /* Insert */
        return K_NONE;
    }
    int dir;
    switch (final) {
        case 'A': dir = D_UP;    break;
        case 'B': dir = D_DOWN;  break;
        case 'D': dir = D_LEFT;  break;
        case 'C': dir = D_RIGHT; break;
        case 'H': dir = D_HOME;  break;
        case 'F': dir = D_END;   break;
        default:  return K_NONE;
    }
    if (mod >= 2) return MK(mod, dir);
    switch (dir) {                              /* unmodified: plain keys */
        case D_UP:    return KEY_UP;
        case D_DOWN:  return KEY_DOWN;
        case D_LEFT:  return KEY_LEFT;
        case D_RIGHT: return KEY_RIGHT;
        case D_HOME:  return KEY_HOME;
        default:      return KEY_END;
    }
}
/* How getch() should wait: -1 blocks, otherwise milliseconds. The main loop
 * switches to a short poll while a terminal tab is running; dialogs always
 * block, so they go through read_key() rather than read_key_raw(). */
int g_timeout = -1;

/* The parameters of an SGR mouse report — btn ; col ; row — with the opening
 * CSI < already eaten, closed by M (press or drag) or m (release). */
static int mouse_tail(void) {
    int p[3] = { 0, 0, 0 }, np = 0, num = 0, ch;
    while ((ch = getch()) != ERR) {
        if (isdigit(ch)) { num = num * 10 + (ch - '0'); continue; }
        if (ch == ';') { if (np < 3) p[np] = num; np++; num = 0; continue; }
        if (np < 3) p[np] = num;
        if (ch != 'M' && ch != 'm') return K_NONE;
        return mouse_decode(p[0], p[1], p[2], ch == 'M');
    }
    return K_NONE;
}
int read_key_raw(void) {
    int c = getch();
    /* ncurses knows the CSI < that opens an SGR report and hands back
     * KEY_MOUSE, but it only decodes the parameters when its own mouse
     * layer is switched on — which would fight with the raw reading here.
     * So take the tail ourselves. */
    if (c == KEY_MOUSE) {
        nodelay(stdscr, TRUE);         /* a truncated report must not hang us */
        int r = mouse_tail();
        timeout(g_timeout);
        return r;
    }
    if (c == KEY_SLEFT)  return MK(2, D_LEFT);
    if (c == KEY_SRIGHT) return MK(2, D_RIGHT);
    if (c == KEY_SHOME)  return MK(2, D_HOME);
    if (c == KEY_SEND)   return MK(2, D_END);
    if (c != 27) return c;
    nodelay(stdscr, TRUE);
    int c2 = getch(), r = 27;
    if      (c2 == ERR)       r = 27;
    else if (c2 == KEY_UP)    r = MK(3, D_UP);
    else if (c2 == KEY_DOWN)  r = MK(3, D_DOWN);
    else if (c2 == KEY_LEFT)  r = MK(3, D_LEFT);
    else if (c2 == KEY_RIGHT) r = MK(3, D_RIGHT);
    else if (c2 == KEY_DC)    r = K_ADEL;
    else if (c2 == KEY_IC)    r = K_AINS;
    else if (c2 == 27) {                     /* ESC ESC [ … = Alt+<key> */
        int c3 = getch();
        r = (c3 == '[' || c3 == 'O') ? csi_tail(3) : 27;
    }
    else if (c2 == '[' || c2 == 'O') r = csi_tail(0);   /* undecoded plain key */
    else if (c2 == '\r' || c2 == '\n' || c2 == KEY_ENTER) r = ALT('\n');
    else {
        /* Alt+<char>. Collect the whole UTF-8 character first: several
         * layouts put a non-ASCII symbol on a shifted digit. */
        char ch[8];
        int n = 0;
        ch[n++] = (char)c2;
        if ((c2 & 0x80) && c2 != ERR) {
            int need = (c2 & 0xe0) == 0xc0 ? 1 : (c2 & 0xf0) == 0xe0 ? 2 :
                       (c2 & 0xf8) == 0xf0 ? 3 : 0;
            for (int i = 0; i < need; i++) {
                int cc = getch();
                if (cc == ERR || (cc & 0xc0) != 0x80) break;
                ch[n++] = (char)cc;
            }
        }
        int d = kb_digit_of(ch, n);
        if (d >= 0)      r = PKEY(d);
        else if (n == 1) r = ALT(tolower(c2));
        else             r = K_NONE;          /* Alt + some other letter */
    }
    timeout(g_timeout);            /* not nodelay(FALSE): that forces blocking */
    return r;
}
/* Blocking read, for dialogs and anything that owns the screen. */
int read_key(void) {
    int save = g_timeout;
    g_timeout = -1;
    timeout(-1);
    int c = read_key_raw();
    g_timeout = save;
    timeout(save);
    return c;
}
