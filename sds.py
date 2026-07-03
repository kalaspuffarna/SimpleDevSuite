#!/usr/bin/env python3
"""SDS - SimpleDevSuite"""
import fcntl
import os
import pty
import re
import shutil
import signal
import struct
import subprocess
import sys
import termios
import time
import tomllib
from pathlib import Path
from typing import Optional

import fitz  # PyMuPDF
import pyte
from PIL import Image as PILImage
from rich.segment import Segment
from rich.style import Style
from rich.text import Text
from textual.app import App, ComposeResult
from textual.widget import Widget
from textual.widgets import Static, Tree, TextArea, Button, Label, Input
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual import events
from textual.binding import Binding
from textual.strip import Strip
from textual.widgets.tree import TreeNode
from textual.widgets.text_area import Selection
from textual.screen import ModalScreen
from textual_image.widget import AutoImage
from textual_image.renderable.sixel import Image as SixelRenderable
from textual_image.renderable.tgp import Image as TGPRenderable
from textual_image._terminal import get_cell_size
from rich.markup import escape

TERMINAL_TAB_PREFIX = "\x00terminal\x00"

# Set SDS_DEBUG_TERM_LOG=/some/path to dump every raw byte the embedded
# terminal reads from its pty (before any of our SGR rewriting) to that file,
# for diagnosing terminal-emulation bugs offline.
_TERM_DEBUG_LOG = os.environ.get("SDS_DEBUG_TERM_LOG")

def _is_terminal_tab(path: str) -> bool:
    return path.startswith(TERMINAL_TAB_PREFIX)


def _dir_prefix(path: str) -> str:
    """`path` normalized to end in exactly one "/", for prefix-matching
    everything nested under it (`p.startswith(_dir_prefix(path))`)."""
    return path.rstrip("/") + "/"


# ── Configuration ──────────────────────────────────────────────────────────
# Built-in defaults, overridable by $XDG_CONFIG_HOME/sds/config (or
# ~/.config/sds/config), a TOML file with no extension. See
# default_config.toml (shipped alongside this script) for a fully
# documented copy of every option below.
DEFAULT_CONFIG: dict = {
    "theme": "default",
    "custom_theme": {
        "accent": "#f5a623", "bg": "#000000", "fg": "#ffffff",
        "muted": "#444444", "bg_alt": "#1a1a1a", "error": "#d23c3d",
    },
    "icons": {"style": "nerdfont"},
    "pdf": {"mode": "auto"},
    "keybinds": {
        "quit": "alt+escape", "save": "ctrl+s", "find": "ctrl+f",
        "new_entry": "ctrl+n", "close_tab": "ctrl+w", "leader": "ctrl+k",
        "tree_up": "alt+up", "tree_down": "alt+down",
        "tree_collapse": "alt+left", "tree_expand": "alt+right",
        "tree_open": "alt+enter", "tree_rename": "alt+insert",
        "tree_delete": "alt+delete",
        "tab_prev": "alt+shift+left", "tab_next": "alt+shift+right",
    },
}


def _config_path() -> Path:
    xdg = os.environ.get("XDG_CONFIG_HOME")
    base = Path(xdg) if xdg else Path.home() / ".config"
    return base / "sds" / "config"


def _merge_config(defaults: dict, overrides: dict) -> dict:
    """Shallow merge: only keys already present in `defaults` can be
    overridden — unknown sections/keys in the user's file are silently
    ignored rather than erroring, so old configs stay valid across upgrades
    that add new keys. Most top-level keys are sections (dicts merged
    key-by-key), but a few (like `theme`) are plain scalars that get
    replaced outright."""
    merged = {
        key: dict(value) if isinstance(value, dict) else value
        for key, value in defaults.items()
    }
    for key, value in overrides.items():
        if key not in merged:
            continue
        if isinstance(merged[key], dict) and isinstance(value, dict):
            for sub_key, sub_value in value.items():
                if sub_key in merged[key]:
                    merged[key][sub_key] = sub_value
        elif not isinstance(merged[key], dict) and not isinstance(value, dict):
            merged[key] = value
    return merged


def _load_config() -> dict:
    path = _config_path()
    if not path.is_file():
        return _merge_config(DEFAULT_CONFIG, {})
    try:
        with path.open("rb") as f:
            data = tomllib.load(f)
    except (tomllib.TOMLDecodeError, OSError) as exc:
        print(f"sds: warning: failed to read config at {path}: {exc}", file=sys.stderr)
        data = {}
    return _merge_config(DEFAULT_CONFIG, data)


CONFIG = _load_config()

# Fallback used only if themes.toml is ever missing/unreadable — kept in
# sync with themes.toml's own [default] table.
_BUILTIN_DEFAULT_THEME = {
    "accent": "#f5a623", "bg": "#000000", "fg": "#ffffff",
    "muted": "#444444", "bg_alt": "#1a1a1a", "error": "#d23c3d",
}


def _load_themes() -> dict:
    themes = {"default": _BUILTIN_DEFAULT_THEME}
    path = Path(__file__).resolve().parent / "themes.toml"
    try:
        with path.open("rb") as f:
            themes.update(tomllib.load(f))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        print(f"sds: warning: failed to read themes.toml: {exc}", file=sys.stderr)
    return themes


THEMES = _load_themes()


def _resolve_theme_colors() -> dict:
    """`theme = "custom"` reads colors straight from the config's own
    [custom_theme] section; any other name looks up a preset in
    themes.toml (falling back to "default" with a warning if unknown)."""
    name = CONFIG["theme"]
    if name == "custom":
        return CONFIG["custom_theme"]
    if name in THEMES:
        return THEMES[name]
    print(f"sds: warning: unknown theme {name!r}, falling back to 'default'", file=sys.stderr)
    return THEMES["default"]


_theme_colors = _resolve_theme_colors()
ACCENT = _theme_colors["accent"]
BG     = _theme_colors["bg"]
FG     = _theme_colors["fg"]
MUTED  = _theme_colors["muted"]
BG_ALT = _theme_colors["bg_alt"]
ERROR  = _theme_colors["error"]
GIT_UNTRACKED = ERROR
GIT_MODIFIED  = "#e5c07b"
GIT_STAGED    = "#98c379"

# Whether this terminal supports a real image protocol (Kitty graphics or
# Sixel) rather than just a half-block/unicode approximation. AutoImage
# resolves `_Renderable` once at import time (textual_image/renderable,
# priority: Sixel > TGP/Kitty > half-cell > unicode) -- this is the same
# capability check textual_image already did, just read back out.
PDF_IMAGE_CAPABLE = AutoImage._Renderable in (TGPRenderable, SixelRenderable)


def _resolve_pdf_mode() -> str:
    configured = CONFIG["pdf"]["mode"]
    if configured in ("image", "text"):
        return configured
    return "image" if PDF_IMAGE_CAPABLE else "text"


PDF_MODE = _resolve_pdf_mode()

_NERDFONT_ICON = {
    "python":      "\ue606",
    "js":          "\ue60c",
    "ts":          "\ue628",
    "rust":        "\ue7a8",
    "go":          "\ue627",
    "html":        "\ue736",
    "css":         "\ue749",
    "json":        "\ue60b",
    "md":          "\ue73e",
    "bash":        "\ue615",
    "c":           "\ue61e",
    "cpp":         "\ue61d",
    "java":        "\ue738",
    "file":        "\uf15b",
    "folder":      "\uf07b",
    "folder_open": "\uf07c",
    "toml":        "\uf669",
    "yaml":        "\uf481",
    "xml":         "\uf72d",
    "sql":         "\uf1c0",
    "terminal":    "\uf120",
    "pdf":         "\uf1c1",
    "clock":            "\uf017",
    "battery_full":     "\uf240",
    "battery_high":     "\uf241",
    "battery_medium":   "\uf242",
    "battery_low":      "\uf243",
    "battery_empty":    "\uf244",
    "battery_charging": "\uf0e7",
}

_ASCII_ICON = {
    "python": "PY", "js": "JS", "ts": "TS", "rust": "RS", "go": "GO",
    "html": "HTM", "css": "CSS", "json": "JSN", "md": "MD", "bash": "SH",
    "c": "C", "cpp": "C++", "java": "JAV", "file": "-", "folder": "+",
    "folder_open": "~", "toml": "TML", "yaml": "YML", "xml": "XML",
    "sql": "SQL", "terminal": ">_", "pdf": "PDF",
    "clock": "", "battery_full": "", "battery_high": "", "battery_medium": "",
    "battery_low": "", "battery_empty": "", "battery_charging": "*",
}

_NONE_ICON = {k: "" for k in _NERDFONT_ICON}

I = {"nerdfont": _NERDFONT_ICON, "ascii": _ASCII_ICON, "none": _NONE_ICON}.get(
    CONFIG["icons"]["style"], _NERDFONT_ICON
)

EXT_ICON = {
    ".py":   I["python"], ".js":  I["js"],   ".ts":   I["ts"],
    ".html": I["html"],   ".htm": I["html"], ".css":  I["css"],
    ".json": I["json"],   ".md":  I["md"],   ".markdown": I["md"],
    ".sh":   I["bash"],   ".bash":I["bash"], ".zsh":  I["bash"],
    ".rs":   I["rust"],   ".go":  I["go"],   ".java": I["java"],
    ".c":    I["c"],      ".h":   I["c"],
    ".cpp":  I["cpp"],    ".cc":  I["cpp"],  ".cxx":  I["cpp"],
    ".toml": I["toml"],   ".yaml":I["yaml"], ".yml":  I["yaml"],
    ".xml":  I["xml"],    ".sql": I["sql"],
    ".pdf":  I["pdf"],
}

EXT_LANG = {
    ".py": "python",   ".js": "javascript", ".ts": "javascript",
    ".html": "html",   ".htm": "html",      ".css": "css",
    ".json": "json",   ".md": "markdown",   ".markdown": "markdown",
    ".sh": "bash",     ".bash": "bash",     ".zsh": "bash",
    ".rs": "rust",     ".go": "go",         ".java": "java",
    ".sql": "sql",     ".toml": "toml",
    ".yaml": "yaml",   ".yml": "yaml",      ".xml": "xml",
}

COMMENT_TOKEN = {
    ".py": "#",  ".sh": "#",  ".bash": "#", ".zsh": "#",
    ".yaml": "#", ".yml": "#", ".toml": "#",
    ".js": "//", ".ts": "//", ".java": "//",
    ".c": "//",  ".h": "//",  ".cpp": "//", ".cc": "//", ".cxx": "//",
    ".rs": "//", ".go": "//",
    ".sql": "--",
}


def _battery_status() -> Optional[tuple[int, bool]]:
    """(percent, charging) from the first /sys/class/power_supply/BAT*
    found, or None on a desktop with no battery."""
    try:
        bat_dirs = sorted(Path("/sys/class/power_supply").glob("BAT*"))
    except OSError:
        return None
    if not bat_dirs:
        return None
    bat = bat_dirs[0]
    try:
        percent = int((bat / "capacity").read_text().strip())
        status = (bat / "status").read_text().strip().lower()
    except (OSError, ValueError):
        return None
    return percent, status == "charging"


def _battery_icon(percent: int, charging: bool) -> str:
    if charging:
        return I["battery_charging"]
    if percent >= 90:
        return I["battery_full"]
    if percent >= 65:
        return I["battery_high"]
    if percent >= 40:
        return I["battery_medium"]
    if percent >= 15:
        return I["battery_low"]
    return I["battery_empty"]


CSS = f"""
Screen {{
    background: {BG};
}}

#tab-bar {{
    height: 2;
    background: {BG};
    color: {FG};
    border-bottom: solid {ACCENT};
    padding: 0 1;
    overflow: hidden;
}}

#main-area {{
    height: 1fr;
}}

#file-tree-panel {{
    width: 28;
    border-right: solid {ACCENT};
    background: {BG};
}}

#tree-label {{
    height: 1;
    background: {ACCENT};
    color: {BG};
    padding: 0 1;
    text-style: bold;
}}

Tree {{
    background: {BG};
    color: {FG};
    scrollbar-color: {ACCENT} {BG};
    scrollbar-size: 1 1;
}}

Tree > .tree--cursor {{
    background: {ACCENT};
    color: {BG};
}}

#editor-panel {{
    background: {BG};
    height: 1fr;
    width: 1fr;
}}

#welcome {{
    color: {ACCENT};
    width: 1fr;
    height: 1fr;
    content-align: center middle;
    text-align: left;
    padding: 0 4;
}}

SDSEditor {{
    height: 1fr;
    width: 1fr;
    background: {BG};
}}

SDSEditor > .text-area--gutter {{
    background: {BG_ALT};
    color: {MUTED};
}}

SDSEditor > .text-area--cursor-line {{
    background: {BG_ALT};
}}

SDSEditor > .text-area--cursor {{
    background: {ACCENT};
    color: {BG};
}}

SDSEditor > .text-area--selection {{
    background: #3a3000;
}}

ConfirmScreen {{
    align: center middle;
    background: rgba(0,0,0,0.8);
}}

#confirm-box {{
    width: 46;
    height: auto;
    background: {BG};
    border: solid {ACCENT};
    padding: 1 2;
    align: center middle;
    layout: vertical;
}}

#confirm-label {{
    color: {FG};
    text-align: center;
    width: 1fr;
    height: auto;
    margin-bottom: 1;
}}

#confirm-buttons {{
    layout: horizontal;
    align: center middle;
    width: 1fr;
    height: auto;
}}

HelpScreen {{
    align: center middle;
    background: rgba(0,0,0,0.8);
}}

#help-box {{
    width: 70;
    height: 80%;
    background: {BG};
    border: solid {ACCENT};
    padding: 1 2;
}}

#help-scroll {{
    height: 1fr;
}}

#help-body {{
    color: {FG};
}}

#help-hint {{
    color: {MUTED};
    height: 1;
    margin-top: 1;
}}

Button {{
    background: {BG};
    color: {FG};
    border: solid {MUTED};
    margin: 0 1;
    min-width: 12;
    height: 3;
}}

Button:focus {{
    background: {ACCENT};
    color: {BG};
    border: solid {ACCENT};
}}

Button.-primary {{
    background: {ACCENT};
    color: {BG};
    border: solid {ACCENT};
}}

#bottom-bar {{
    dock: bottom;
    height: auto;
}}

#status-row {{
    height: 1;
    background: {BG_ALT};
}}

#status-bar {{
    height: 1;
    background: {BG_ALT};
    color: {MUTED};
    padding: 0 1;
    width: 1fr;
}}

#system-info {{
    height: 1;
    background: {BG_ALT};
    color: {ACCENT};
    padding: 0 1;
    width: auto;
}}

#search-bar {{
    height: 4;
    background: {BG_ALT};
    border-top: solid {ACCENT};
    padding: 0 1;
    display: none;
}}

#search-input {{
    width: 1fr;
    border: solid {MUTED};
}}

#search-input:focus {{
    border: solid {ACCENT};
}}

#search-info {{
    width: auto;
    min-width: 9;
    color: {MUTED};
    padding: 0 1;
    content-align: right middle;
}}

SDSTerminal {{
    height: 1fr;
    width: 1fr;
    background: #000000;
    border: solid {MUTED};
}}

SDSTerminal:focus {{
    border: solid {ACCENT};
}}

SDSPdfViewer {{
    height: 1fr;
    width: 1fr;
    background: {BG};
    align: center middle;
}}

#pdf-image {{
    /* Both must be "auto" (not 1fr) so the image is scaled to fit while
       preserving the page's aspect ratio, instead of being stretched to
       exactly fill the container. */
    width: auto;
    height: auto;
}}

#pdf-text {{
    width: 1fr;
    height: 1fr;
    background: {BG};
    color: {FG};
}}

#pdf-status {{
    dock: bottom;
    height: 1;
    background: {BG_ALT};
    color: {MUTED};
    padding: 0 1;
    content-align: center middle;
}}

NewEntryScreen {{
    align: center middle;
    background: rgba(0,0,0,0.8);
}}

#newentry-box {{
    width: 60;
    height: auto;
    background: {BG};
    border: solid {ACCENT};
    padding: 1 2;
}}

#newentry-label {{
    color: {FG};
    width: 1fr;
    height: auto;
    margin-bottom: 1;
}}

#newentry-input {{
    width: 1fr;
    border: solid {MUTED};
    margin-bottom: 1;
}}

#newentry-input:focus {{
    border: solid {ACCENT};
}}

#newentry-hint {{
    color: {MUTED};
    width: 1fr;
    height: auto;
}}
"""


def _location_to_offset(text: str, loc: tuple[int, int]) -> int:
    row, col = loc
    lines = text.split("\n")
    return sum(len(line) + 1 for line in lines[:row]) + col


def _offset_to_location(text: str, offset: int) -> tuple[int, int]:
    row = text.count("\n", 0, offset)
    line_start = text.rfind("\n", 0, offset) + 1
    return row, offset - line_start


def _pretty_key(name: str) -> str:
    """A configured key name (e.g. "ctrl+shift+s") formatted for display
    (e.g. "Ctrl+Shift+S") — reads live from CONFIG so hint text never goes
    stale after a rebind."""
    key = CONFIG["keybinds"][name]
    return "+".join(part.capitalize() for part in key.split("+"))


def _help_text() -> str:
    k = _pretty_key
    return (
        "  SDS — SimpleDevSuite\n\n"
        "  General\n"
        f"    this help screen       {k('leader')} then H\n"
        f"    new file/dir           {k('new_entry')}\n"
        f"    find in file           {k('find')}\n"
        f"    close tab              {k('close_tab')}\n"
        f"    save                   {k('save')}\n"
        f"    quit                   {k('quit')}\n\n"
        "  File tree\n"
        f"    navigate tree           {k('tree_up')}/{k('tree_down')}\n"
        f"    collapse / expand dir   {k('tree_collapse')}/{k('tree_expand')}\n"
        f"    open file               {k('tree_open')}\n"
        f"    rename file/dir         {k('tree_rename')}\n"
        f"    delete file/dir         {k('tree_delete')}\n"
        f"    switch tabs             {k('tab_prev')}/{k('tab_next')}\n\n"
        "  Editor\n"
        "    select all                      Ctrl+A\n"
        f"    comment / uncomment selection   {k('leader')} then C\n\n"
        "  Terminal\n"
        f"    new terminal                          {k('leader')} then T\n"
        "    scrollback (mouse wheel also works)   Shift+PgUp/PgDn\n"
        "    close the terminal                    type 'exit'\n"
        "    select text                           click + drag\n"
        "    copy selection                        Ctrl+Shift+C\n"
        "    paste                                 Ctrl+Shift+V\n\n"
        "  PDF viewer\n"
        "    next / previous page     PageUp/PageDown\n"
        "    zoom (image mode only)   +/-\n"
    )


class SDSEditor(TextArea):
    # TextArea inserts printable characters itself before a Key event ever
    # bubbles up to the App, so the leader chord's follow-up key (c/t/h) has
    # to be intercepted here — stopping it later at the App level is too
    # late to stop the character from being typed.
    def on_key(self, event: events.Key) -> None:
        app = self.app
        if hasattr(app, "_resolve_chord") and app._resolve_chord(event.key):
            event.stop()
            event.prevent_default()
            return

        # TextArea's own binding for ctrl+a is "cursor to line start" (an
        # Emacs/readline convention); override it with the far more common
        # "select all" meaning, same as most editors.
        if event.key == "ctrl+a":
            self.action_select_all()
            event.stop()
            event.prevent_default()


# pyte's ANSI color names use "brown" for what everyone else calls "yellow"
# (an old aixterm-era convention), and pyte 0.8.2 has its own typo for
# bright magenta ("bfightmagenta") — neither is a name Rich recognizes.
_PYTE_COLOR_FIXUPS = {
    "brown": "yellow",
    "brightbrown": "bright_yellow",
    "bfightmagenta": "bright_magenta",
}


def _pyte_color(name: str) -> Optional[str]:
    if not name or name == "default":
        return None
    if name in _PYTE_COLOR_FIXUPS:
        name = _PYTE_COLOR_FIXUPS[name]
    if len(name) == 6 and all(ch in "0123456789abcdefABCDEF" for ch in name):
        return f"#{name}"
    if name.startswith("bright") and len(name) > 6 and not name.startswith("bright_"):
        return f"bright_{name[6:]}"
    return name


def _pyte_char_style(c) -> Style:
    fg = _pyte_color(c.fg)
    bg = _pyte_color(c.bg)
    if c.reverse:
        fg, bg = bg, fg
    return Style(
        color=fg, bgcolor=bg,
        bold=c.bold, italic=c.italics,
        underline=c.underscore, strike=c.strikethrough,
    )


# Standard xterm-ish key -> byte-sequence mapping (normal, non-application cursor mode).
_TERM_KEYS = {
    "enter": b"\r", "tab": b"\t", "shift+tab": b"\x1b[Z",
    "escape": b"\x1b", "backspace": b"\x7f", "delete": b"\x1b[3~",
    "up": b"\x1b[A", "down": b"\x1b[B", "right": b"\x1b[C", "left": b"\x1b[D",
    "home": b"\x1b[H", "end": b"\x1b[F",
    "pageup": b"\x1b[5~", "pagedown": b"\x1b[6~", "insert": b"\x1b[2~",
    "f1": b"\x1bOP", "f2": b"\x1bOQ", "f3": b"\x1bOR", "f4": b"\x1bOS",
    "f5": b"\x1b[15~", "f6": b"\x1b[17~", "f7": b"\x1b[18~", "f8": b"\x1b[19~",
    "f9": b"\x1b[20~", "f10": b"\x1b[21~", "f11": b"\x1b[23~", "f12": b"\x1b[24~",
}


# pyte 0.8.2's CSI parser (see its Stream._csi implementation) has no concept
# of colon-separated SGR sub-parameters (ITU-T T.416, e.g. `CSI 4:3 m` for a
# curly underline or `CSI 38:2::r:g:bm` for a colon-form truecolor). The
# moment it hits a `:` it treats that as the sequence's final byte, no-ops,
# and aborts — dumping everything after the colon onto the screen as literal
# text and, worse, silently dropping SGR resets that happen to use colon form
# (`CSI 4:0 m`), which leaves attributes like underline stuck on forever.
# Modern shells/prompts/tools that detect an advanced host terminal (e.g. via
# TERM_PROGRAM=ghostty, which we pass through from the real environment) emit
# these routinely, so we rewrite them into the plain semicolon form pyte
# understands before they ever reach it.
_CSI_COLON_SGR_RE = re.compile(rb"\x1b\[([0-9:;]*)m")


def _desub_sgr(data: bytes) -> bytes:
    """Rewrite colon sub-parameter SGR sequences into pyte-compatible
    semicolon form, dropping the handful of sub-styles (curly/dotted
    underline, underline color) pyte has no representation for anyway."""
    if b":" not in data:
        return data

    def repl(m: "re.Match[bytes]") -> bytes:
        params = m.group(1).split(b";")
        out: list[bytes] = []
        for param in params:
            if b":" not in param:
                out.append(param)
                continue
            sub = param.split(b":")
            head = sub[0]
            if head == b"4":
                # Underline style sub-param: `:0` (or blank) means "none".
                out.append(b"24" if len(sub) > 1 and sub[1] in (b"", b"0") else b"4")
            elif head in (b"38", b"48"):
                mode = sub[1] if len(sub) > 1 else b""
                if mode == b"5" and len(sub) > 2:
                    out.extend([head, b"5", sub[2]])
                elif mode == b"2":
                    # Colorspace-id form has 3 components after the mode
                    # (colorspace, r, g, b); the common form has just 2.
                    rgb = sub[-3:] if len(sub) >= 5 else sub[2:5]
                    if len(rgb) == 3:
                        out.extend([head, b"2", *rgb])
                # Unrecognized 38/48 sub-forms are dropped entirely.
            # Everything else (58 = underline color, and any other unknown
            # colon code) is dropped entirely rather than risk mis-parsing.
        if not out:
            # Emitting a bare `CSI m` would reset all attributes, which is
            # not what "we dropped an unsupported sub-code" should do.
            return b""
        return b"\x1b[" + b";".join(out) + b"m"

    return _CSI_COLON_SGR_RE.sub(repl, data)


# ── Pty-backed subprocess — spawns a real shell attached to a pseudo-terminal ─
class PtyProcess:
    def __init__(self, cwd: str, cols: int, rows: int):
        self.master_fd, slave_fd = pty.openpty()
        self._resize_fd(rows, cols)
        shell = os.environ.get("SHELL", "/bin/bash")
        self.shell_name = os.path.basename(shell)
        env = dict(os.environ, TERM="xterm-256color")
        self._popen = subprocess.Popen(
            [shell],
            stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
            cwd=cwd, env=env, preexec_fn=os.setsid, close_fds=True,
        )
        os.close(slave_fd)
        os.set_blocking(self.master_fd, False)

    def foreground_command(self) -> Optional[str]:
        """Best-effort name of whatever's currently running in the
        foreground of this pty (e.g. 'vim', 'npm') — used to name the tab
        after what it's running/ran last, the same way real terminal
        emulators derive their tab titles."""
        try:
            pgid = os.tcgetpgrp(self.master_fd)
            with open(f"/proc/{pgid}/comm") as f:
                return f.read().strip()
        except (OSError, ValueError):
            return None

    def _resize_fd(self, rows: int, cols: int) -> None:
        try:
            fcntl.ioctl(self.master_fd, termios.TIOCSWINSZ,
                        struct.pack("HHHH", rows, cols, 0, 0))
        except OSError:
            pass

    def resize(self, rows: int, cols: int) -> None:
        self._resize_fd(rows, cols)
        try:
            os.killpg(os.getpgid(self._popen.pid), signal.SIGWINCH)
        except (ProcessLookupError, OSError):
            pass

    def write(self, data: bytes) -> None:
        try:
            os.write(self.master_fd, data)
        except OSError:
            pass

    def read(self) -> bytes:
        try:
            return os.read(self.master_fd, 65536)
        except (BlockingIOError, OSError):
            return b""

    def is_alive(self) -> bool:
        return self._popen.poll() is None

    def kill(self) -> None:
        try:
            os.killpg(os.getpgid(self._popen.pid), signal.SIGKILL)
        except (ProcessLookupError, OSError):
            pass
        try:
            os.close(self.master_fd)
        except OSError:
            pass


# ── Terminal panel — a real pty-backed terminal emulator with scrollback ─────
class SDSTerminal(Widget):
    can_focus = True

    def __init__(self, cwd: str, tab_id: str, **kwargs):
        super().__init__(**kwargs)
        self.cwd = cwd
        self.tab_id = tab_id
        self.title_cmd = "Terminal"
        self.pty: Optional[PtyProcess] = None
        self.vt: Optional["pyte.HistoryScreen"] = None
        self.vtstream: Optional["pyte.ByteStream"] = None
        self._scroll_offset = 0
        self._dead = False
        self._poll_timer = None
        self._poll_tick = 0
        # Mouse-drag text selection, for copy — each endpoint is an absolute
        # (row, col) pair, where row is measured from the top of scrollback
        # history so the marked region stays put even if the user scrolls
        # mid-drag (see `_abs_row`).
        self._sel_start: Optional[tuple[int, int]] = None
        self._sel_end: Optional[tuple[int, int]] = None
        self._selecting = False

    def on_mount(self) -> None:
        cols = max(self.size.width, 10)
        rows = max(self.size.height, 3)
        self.vt = pyte.HistoryScreen(cols, rows, history=5000)
        self.vtstream = pyte.ByteStream(self.vt)
        # pyte answers DA/DSR/CPR queries (device attributes, status, cursor
        # position — `report_device_attributes` etc. in pyte/screens.py) by
        # calling `write_process_input`, which is a no-op unless overridden.
        # Left unwired, every capability probe a child process sends just
        # vanishes: apps that use these probes to detect what the terminal
        # can render (e.g. Unicode box-drawing vs. plain-ASCII fallback) get
        # no answer and assume the worst. Route the reply back down the pty
        # like a real terminal would.
        self.vt.write_process_input = lambda data: (
            self.pty.write(data.encode()) if self.pty is not None else None
        )
        try:
            self.pty = PtyProcess(self.cwd, cols, rows)
        except Exception as exc:
            self.notify(f"Failed to start terminal: {exc}", severity="error")
            return
        self._poll_timer = self.set_interval(1 / 30, self._poll)

    def _sync_size(self) -> None:
        """Keep pyte's screen dimensions locked to the widget's actual
        rendered content size. Resize events can be missed or fire with a
        stale size during the very first layout pass, so this is re-checked
        on every poll tick as a cheap, self-healing safety net.

        A widget with `display = False` (e.g. a terminal tab that's
        currently switched away from) is excluded from layout entirely, so
        `self.size` collapses to (0, 0) rather than reporting its last real
        size. Treating that as a genuine resize down to the 10x3 floor
        would make pyte permanently drop every cell past column 10 (see
        `Screen.resize`), which can never be recovered when the tab is
        switched back to — so skip syncing while there's no real size to
        sync to.
        """
        if self.size.width == 0 or self.size.height == 0:
            return
        cols = max(self.size.width, 10)
        rows = max(self.size.height, 3)
        if (cols, rows) != (self.vt.columns, self.vt.lines):
            self.vt.resize(rows, cols)
            if self.pty is not None:
                self.pty.resize(rows, cols)

    # A hidden (switched-away-from) terminal tab still needs to keep
    # draining its pty so scrollback doesn't fall behind, but nobody can see
    # it — so it doesn't need the full 30Hz cadence. Every 6th tick (~5Hz)
    # is enough to keep it reasonably current without paying for
    # _update_title()'s syscalls (tcgetpgrp + a /proc/<pgid>/comm open) and
    # the read/feed/refresh cycle 30x/sec for a screen nobody's watching.
    _HIDDEN_POLL_DIVISOR = 6

    def _poll(self) -> None:
        if self.pty is None or self._dead:
            return
        self._poll_tick += 1
        if not self.display and self._poll_tick % self._HIDDEN_POLL_DIVISOR:
            return
        self._sync_size()
        self._update_title()
        data = self.pty.read()
        if data:
            if _TERM_DEBUG_LOG:
                with open(_TERM_DEBUG_LOG, "ab") as f:
                    f.write(data)
            self.vtstream.feed(_desub_sgr(data))
            self.refresh()
        elif not self.pty.is_alive():
            self._dead = True
            self.vtstream.feed(b"\r\n[process exited - press any key to close]\r\n")
            self.refresh()

    def _update_title(self) -> None:
        if self.pty is None:
            return
        name = self.pty.foreground_command()
        if name and name != self.pty.shell_name and name != self.title_cmd:
            self.title_cmd = name
            app = self.app
            if hasattr(app, "_update_tab_bar"):
                app._update_tab_bar()

    def on_resize(self, event: events.Resize) -> None:
        if self.vt is None:
            return
        self._sync_size()
        self.refresh()

    def kill(self) -> None:
        if self._poll_timer is not None:
            self._poll_timer.stop()
            self._poll_timer = None
        if self.pty is not None:
            self.pty.kill()
            self.pty = None

    def on_unmount(self) -> None:
        self.kill()

    # ── scrollback ───────────────────────────────────────────────────────
    def _scroll(self, delta: int) -> None:
        if self.vt is None:
            return
        max_offset = len(self.vt.history.top)
        new_offset = max(0, min(max_offset, self._scroll_offset + delta))
        if new_offset != self._scroll_offset:
            self._scroll_offset = new_offset
            self.refresh()

    def on_mouse_scroll_up(self, event) -> None:
        self._scroll(3)
        event.stop()

    def on_mouse_scroll_down(self, event) -> None:
        self._scroll(-3)
        event.stop()

    # ── mouse-drag text selection ───────────────────────────────────────
    def _abs_row(self, y: int) -> int:
        """Row `y` on screen, expressed as an absolute index counting from
        the top of scrollback history — stable across scrolling, unlike a
        bare screen row."""
        return len(self.vt.history.top) - self._scroll_offset + y

    def _line_at_abs(self, idx: int) -> dict:
        vt = self.vt
        history = vt.history.top
        total_history = len(history)
        if idx < 0:
            return {}
        if idx < total_history:
            return history[idx]
        return vt.buffer.get(idx - total_history, {})

    def _clamp_mouse(self, event) -> tuple[int, int]:
        x = max(0, min(self.size.width - 1, event.x))
        y = max(0, min(self.size.height - 1, event.y))
        return x, y

    def on_mouse_down(self, event) -> None:
        if self.vt is None or event.button != 1:
            return
        x, y = self._clamp_mouse(event)
        self._sel_start = self._sel_end = (self._abs_row(y), x)
        self._selecting = True
        self.capture_mouse()
        self.refresh()
        event.stop()

    def on_mouse_move(self, event) -> None:
        if not self._selecting or self.vt is None:
            return
        x, y = self._clamp_mouse(event)
        self._sel_end = (self._abs_row(y), x)
        self.refresh()
        event.stop()

    def on_mouse_up(self, event) -> None:
        if not self._selecting:
            return
        self._selecting = False
        self.release_mouse()
        if self._sel_start == self._sel_end:
            # A plain click with no drag — clear rather than keep a
            # zero-width "selection" marked.
            self._sel_start = self._sel_end = None
        self.refresh()
        event.stop()

    def _row_text(self, idx: int, c0: int, c1: int) -> str:
        line = self._line_at_abs(idx)
        c1 = min(c1, self.vt.columns - 1)
        chars = [
            (c.data if (c := line.get(x)) and c.data else " ")
            for x in range(c0, c1 + 1)
        ]
        return "".join(chars).rstrip()

    def _extract_selection(self) -> Optional[str]:
        if self.vt is None or self._sel_start is None or self._sel_end is None:
            return None
        lo, hi = sorted((self._sel_start, self._sel_end))
        if lo == hi:
            return None
        if lo[0] == hi[0]:
            return self._row_text(lo[0], lo[1], hi[1])
        width = self.vt.columns
        lines = [self._row_text(lo[0], lo[1], width - 1)]
        lines.extend(self._row_text(idx, 0, width - 1) for idx in range(lo[0] + 1, hi[0]))
        lines.append(self._row_text(hi[0], 0, hi[1]))
        return "\n".join(lines)

    def _copy_selection(self) -> None:
        text = self._extract_selection()
        if not text:
            return
        self.app.copy_to_clipboard(text)
        self.notify("Copied selection", timeout=1.5)

    def _paste_clipboard(self) -> None:
        text = self.app.clipboard
        if not text or self.pty is None or self._dead:
            return
        self._scroll_offset = 0
        self.pty.write(b"\x1b[200~" + text.encode("utf-8", errors="ignore") + b"\x1b[201~")

    # ── rendering ────────────────────────────────────────────────────────
    def _display_line(self, y: int) -> dict:
        return self._line_at_abs(self._abs_row(y))

    def _selected_range(self, abs_row: int) -> Optional[tuple[int, int]]:
        """Column range (inclusive) selected on absolute row `abs_row`, if
        any part of the current selection covers it."""
        if self._sel_start is None or self._sel_end is None:
            return None
        lo, hi = sorted((self._sel_start, self._sel_end))
        if lo == hi or not (lo[0] <= abs_row <= hi[0]):
            return None
        width = self.vt.columns
        c0 = lo[1] if abs_row == lo[0] else 0
        c1 = hi[1] if abs_row == hi[0] else width - 1
        return c0, c1

    def render_line(self, y: int) -> Strip:
        if self.vt is None:
            return Strip.blank(self.size.width)
        width = self.size.width
        abs_row = self._abs_row(y)
        line = self._line_at_abs(abs_row)
        cursor_x = None
        if (self._scroll_offset == 0 and not self.vt.cursor.hidden
                and self.vt.cursor.y == y):
            cursor_x = self.vt.cursor.x
        sel_range = self._selected_range(abs_row)

        segments = []
        cur_style: Optional[Style] = None
        cur_text: list[str] = []

        def flush():
            if cur_text:
                segments.append(Segment("".join(cur_text), cur_style))

        for x in range(width):
            c = line.get(x)
            data = (c.data if c and c.data else " ")
            try:
                style = _pyte_char_style(c) if c else Style()
            except Exception:
                style = Style()
            if x == cursor_x:
                style = style + Style(reverse=True)
            if sel_range is not None and sel_range[0] <= x <= sel_range[1]:
                style = style + Style(reverse=True)
            if cur_style is not None and style == cur_style:
                cur_text.append(data)
            else:
                flush()
                cur_style = style
                cur_text = [data]
        flush()
        return Strip(segments, width)

    # ── keyboard input ───────────────────────────────────────────────────
    def _encode_key(self, event: events.Key) -> Optional[bytes]:
        key = event.key
        if key in _TERM_KEYS:
            return _TERM_KEYS[key]
        if key.startswith("ctrl+") and len(key) == 6 and key[5].isalpha():
            return bytes([ord(key[5].lower()) - ord("a") + 1])
        if key.startswith("alt+") and len(key) == 5:
            return b"\x1b" + key[4].encode()
        if event.character:
            return event.character.encode("utf-8", errors="ignore")
        return None

    def on_key(self, event: events.Key) -> None:
        key = event.key
        kb  = CONFIG["keybinds"]
        if key in (kb["close_tab"], kb["tab_prev"], kb["tab_next"], "alt+shift+w"):
            # Close-tab/tab-cycling close the tab or switch tabs (App.on_key's
            # bindings) instead of reaching the shell — same tradeoff every
            # real terminal emulator makes, sacrificing e.g. readline's
            # delete-previous-word binding on Ctrl+W for a consistent
            # tab-close shortcut. Not stopping the event here lets it bubble
            # up to the App unhandled. Alt+Shift+W is left exempted here too
            # as a harmless bonus close-tab shortcut for terminals/WMs that
            # happen to let it through unmolested, but it's not reliable in
            # general (commonly intercepted by the OS/WM before Textual ever
            # sees it), so it isn't advertised or configurable.
            return

        # The terminal forwards nearly everything to the shell, which would
        # otherwise swallow the app's leader chord (new terminal / comment /
        # help) before it ever reaches the App — same tradeoff SDSEditor
        # makes for the same reason. This is the one thing that stays
        # reserved.
        app = self.app
        if getattr(app, "_pending_chord", None) == kb["leader"]:
            if hasattr(app, "_resolve_chord") and app._resolve_chord(key):
                event.stop(); event.prevent_default(); return
            # unresolved: chord already cleared by _resolve_chord; fall
            # through so this keystroke still reaches the shell normally
        elif key == kb["leader"] and hasattr(app, "_arm_leader"):
            app._arm_leader()
            event.stop(); event.prevent_default(); return

        if key == "shift+pageup":
            self._scroll(max(self.size.height - 1, 1))
            event.stop(); event.prevent_default(); return
        if key == "shift+pagedown":
            self._scroll(-max(self.size.height - 1, 1))
            event.stop(); event.prevent_default(); return

        if key == "ctrl+shift+c":
            self._copy_selection()
            event.stop(); event.prevent_default(); return
        if key == "ctrl+shift+v":
            self._paste_clipboard()
            event.stop(); event.prevent_default(); return

        if self._dead:
            app = self.app
            if hasattr(app, "_close_path_silently"):
                app._close_path_silently(self.tab_id)
            event.stop(); event.prevent_default(); return

        data = self._encode_key(event)
        if data is None:
            return
        self._scroll_offset = 0
        self._sel_start = self._sel_end = None
        if self.pty is not None:
            self.pty.write(data)
        event.stop()
        event.prevent_default()

    def on_paste(self, event: events.Paste) -> None:
        # Textual delivers a terminal paste (bracketed paste from the real
        # terminal emulator) as its own event, not as a run of Key events —
        # forward it as one chunk, itself wrapped in bracketed-paste markers
        # so the shell/program inside treats it as a paste too (no
        # per-line auto-indent/auto-submit surprises).
        if self.pty is None or self._dead:
            return
        self._scroll_offset = 0
        data = event.text.encode("utf-8", errors="ignore")
        self.pty.write(b"\x1b[200~" + data + b"\x1b[201~")
        event.stop()


# ── PDF viewer — renders real rasterized pages via textual-image ─────────────
class SDSPdfViewer(Vertical):
    can_focus = True

    def __init__(self, path: str, **kwargs):
        super().__init__(**kwargs)
        self.path = path
        self.doc: Optional["fitz.Document"] = None
        self.page_index = 0
        self.zoom = 3.0

    def compose(self) -> ComposeResult:
        if PDF_MODE == "image":
            yield AutoImage(id="pdf-image")
        else:
            yield TextArea("", id="pdf-text", read_only=True, show_line_numbers=False)
        yield Static("", id="pdf-status")

    def on_mount(self) -> None:
        try:
            self.doc = fitz.open(self.path)
        except Exception as exc:
            self.notify(f"Failed to open PDF: {exc}", severity="error")
            return
        if self.doc.page_count == 0:
            self.notify("PDF has no pages", severity="error")
            return
        self._render_page()

    def _render_page(self) -> None:
        if not self.doc:
            return
        page = self.doc[self.page_index]
        if PDF_MODE == "image":
            pix = page.get_pixmap(matrix=fitz.Matrix(self.zoom, self.zoom))
            pil_mode = "RGBA" if pix.alpha else "RGB"
            img = PILImage.frombytes(pil_mode, (pix.width, pix.height), pix.samples)
            self.query_one("#pdf-image", AutoImage).image = img
        else:
            text = page.get_text() or "[This page has no extractable text]"
            self.query_one("#pdf-text", TextArea).text = text
        self._update_status()

    def _update_status(self) -> None:
        if not self.doc:
            return
        nav = "PageUp/PageDown: navigate"
        extra = ""
        if PDF_MODE == "image":
            nav += "  +/-: zoom"
            try:
                cell = get_cell_size()
                extra = f"  |  cell: {cell.width}x{cell.height}px"
            except Exception:
                pass
        self.query_one("#pdf-status", Static).update(
            f"Page {self.page_index + 1}/{self.doc.page_count}  |  {nav}{extra}"
        )

    def next_page(self) -> None:
        if self.doc and self.page_index < self.doc.page_count - 1:
            self.page_index += 1
            self._render_page()

    def prev_page(self) -> None:
        if self.doc and self.page_index > 0:
            self.page_index -= 1
            self._render_page()

    def zoom_in(self) -> None:
        self.zoom = min(self.zoom + 0.5, 6.0)
        self._render_page()

    def zoom_out(self) -> None:
        self.zoom = max(self.zoom - 0.5, 0.5)
        self._render_page()

    def close(self) -> None:
        if self.doc is not None:
            self.doc.close()
            self.doc = None

    def on_unmount(self) -> None:
        self.close()

    def on_key(self, event: events.Key) -> None:
        key = event.key
        char = event.character
        if key in ("pagedown", "down", "right", "space"):
            self.next_page(); event.stop(); event.prevent_default()
        elif key in ("pageup", "up", "left"):
            self.prev_page(); event.stop(); event.prevent_default()
        elif char in ("+", "=") and PDF_MODE == "image":
            self.zoom_in(); event.stop(); event.prevent_default()
        elif char == "-" and PDF_MODE == "image":
            self.zoom_out(); event.stop(); event.prevent_default()


# ── Confirm dialog ────────────────────────────────────────────────────────────
class ConfirmScreen(ModalScreen):
    def __init__(self, message: str, default_yes: bool = True):
        super().__init__()
        self._message = message
        self._default_yes = default_yes

    def compose(self) -> ComposeResult:
        with Vertical(id="confirm-box"):
            yield Label(self._message, id="confirm-label")
            with Horizontal(id="confirm-buttons"):
                yield Button("Yes", id="yes", variant="primary")
                yield Button("No",  id="no")

    def on_mount(self) -> None:
        self.query_one("#yes" if self._default_yes else "#no").focus()

    def on_button_pressed(self, event: Button.Pressed) -> None:
        self.dismiss(event.button.id == "yes")

    def on_key(self, event: events.Key) -> None:
        key = event.key
        if key == "enter":
            focused = self.focused
            result = (focused is not None and
                      hasattr(focused, "id") and
                      focused.id == "yes")
            self.dismiss(result)
            event.stop()
        elif key == "escape":
            self.dismiss(False)
            event.stop()
        elif key in ("left", "shift+tab"):
            self.query_one("#yes").focus()
            event.stop()
        elif key in ("right", "tab"):
            self.query_one("#no").focus()
            event.stop()


# ── New file/directory dialog ─────────────────────────────────────────────────
class NewEntryScreen(ModalScreen):
    def __init__(self, base: str):
        super().__init__()
        self._base = base

    def compose(self) -> ComposeResult:
        with Vertical(id="newentry-box"):
            yield Label(f"New entry in {self._base}", id="newentry-label")
            yield Input(placeholder="name.py or folder/", id="newentry-input")
            yield Label(
                "End with / to create a directory  •  Enter: create  •  Esc: cancel",
                id="newentry-hint",
            )

    def on_mount(self) -> None:
        self.query_one(Input).focus()

    def on_input_submitted(self, event: Input.Submitted) -> None:
        self.dismiss(event.value.strip())
        event.stop()

    def on_key(self, event: events.Key) -> None:
        if event.key == "escape":
            self.dismiss(None)
            event.stop()


# ── Rename dialog ──────────────────────────────────────────────────────────────
class RenameScreen(ModalScreen):
    def __init__(self, old_name: str):
        super().__init__()
        self._old_name = old_name

    def compose(self) -> ComposeResult:
        with Vertical(id="newentry-box"):
            yield Label(f"Rename '{self._old_name}'", id="newentry-label")
            yield Input(value=self._old_name, id="newentry-input")
            yield Label("Enter: rename  •  Esc: cancel", id="newentry-hint")

    def on_mount(self) -> None:
        inp = self.query_one(Input)
        inp.focus()
        inp.action_select_all()

    def on_input_submitted(self, event: Input.Submitted) -> None:
        self.dismiss(event.value.strip())
        event.stop()

    def on_key(self, event: events.Key) -> None:
        if event.key == "escape":
            self.dismiss(None)
            event.stop()


# ── Help screen — a keyboard cheat sheet for every window type ───────────────
class HelpScreen(ModalScreen):
    def compose(self) -> ComposeResult:
        with Vertical(id="help-box"):
            with VerticalScroll(id="help-scroll"):
                yield Static(_help_text(), id="help-body")
            yield Label("Esc: close", id="help-hint")

    def on_key(self, event: events.Key) -> None:
        if event.key == "escape":
            self.dismiss()
            event.stop()


# ── Tab bar — markup=False so [[ ]] are literal characters ───────────────────
class TabBar(Static):
    def __init__(self, **kwargs):
        # markup=False prevents Rich from interpreting [ ] as tags
        super().__init__("", markup=False, **kwargs)
        self._files:    list[str] = []
        self._active:   int       = -1
        self._modified: set[int]  = set()

    def refresh_tabs(self, files: list[str], active: int, modified: set[int]) -> None:
        self._files    = files
        self._active   = active
        self._modified = modified
        self._repaint()

    def on_resize(self, _: events.Resize) -> None:
        self._repaint()

    def _label(self, i: int) -> str:
        f = self._files[i]
        term = getattr(self.app, "terminals", {}).get(f)
        if term is not None:
            name, icon = term.title_cmd, I["terminal"]
        else:
            name = Path(f).name
            icon = EXT_ICON.get(Path(f).suffix.lower(), I["file"])
        star = " *" if i in self._modified else ""
        return f"[[ {icon} {name}{star} ]]" if i == self._active else f"[ {icon} {name}{star} ]"

    def _window(self, lo: int, hi: int, labels: list[str], sep: str) -> str:
        """Render the tabs from lo..hi inclusive, with a "..."/"..*" marker
        on either side for any tabs that don't fit in that direction."""
        n = len(labels)
        parts = []
        if lo > 0:
            parts.append("..*" if self._modified & set(range(lo)) else "...")
        parts.extend(labels[lo:hi + 1])
        if hi < n - 1:
            parts.append("..*" if self._modified & set(range(hi + 1, n)) else "...")
        return sep.join(parts)

    def _repaint(self) -> None:
        if not self._files:
            self.update(" SDS \u2014 no files open")
            return

        sep    = "  "
        labels = [self._label(i) for i in range(len(self._files))]
        width  = self.content_size.width

        full = self._window(0, len(labels) - 1, labels, sep)
        if width <= 0 or len(full) <= width:
            self.update(full)
            return

        # Pin the active tab and spin outward like a wheel: alternately pull
        # in the tab just before and just after the visible window, one at a
        # time, checking whether the whole bar (with "..." on any side that
        # still has tabs left over) still fits. A side stops growing the
        # moment its next tab doesn't fit; once both sides are stuck (or
        # we've pulled in every tab) we're done.
        active = max(0, min(self._active, len(labels) - 1))
        lo = hi = active
        left_blocked  = lo == 0
        right_blocked = hi == len(labels) - 1
        try_left_next = True

        while not (left_blocked and right_blocked):
            grow_left = (try_left_next and not left_blocked) or right_blocked
            candidate = (lo - 1, hi) if grow_left else (lo, hi + 1)

            if len(self._window(*candidate, labels, sep)) <= width:
                lo, hi = candidate
                left_blocked  = lo == 0
                right_blocked = hi == len(labels) - 1
            elif grow_left:
                left_blocked = True
            else:
                right_blocked = True

            try_left_next = not try_left_next

        self.update(self._window(lo, hi, labels, sep))


# ── Status bar ────────────────────────────────────────────────────────────────
class StatusBar(Static):
    def __init__(self, **kwargs):
        super().__init__("", markup=False, **kwargs)

    def set_info(self, filepath: str = "", row: int = 0,
                 col: int = 0, lang: str = "", modified: bool = False) -> None:
        if not filepath:
            self.update(
                f"{_pretty_key('leader')} H: help  •  "
                f"{_pretty_key('tree_open')}: open  •  "
                f"{_pretty_key('new_entry')}: new"
            )
            return
        name = Path(filepath).name
        mod  = "  [unsaved]" if modified else ""
        icon = EXT_ICON.get(Path(filepath).suffix.lower(), I["file"])
        self.update(
            f"{icon} {name}{mod}  |  "
            f"Ln {row+1} Col {col+1}  |  "
            f"{lang or 'text'}  |  "
            f"{_pretty_key('leader')} H: help  •  "
            f"{_pretty_key('save')}: save  •  "
            f"{_pretty_key('close_tab')}: close"
        )

    def set_terminal_info(self) -> None:
        self.update(
            f"{I['terminal']} Terminal  |  "
            f"{_pretty_key('leader')} H: help  •  "
            f"{_pretty_key('close_tab')}: close  •  "
            f"Shift+PgUp/PgDn: scrollback"
        )

    def set_pdf_info(self, name: str) -> None:
        self.update(
            f"{I['pdf']} {name}  |  "
            f"{_pretty_key('leader')} H: help  •  "
            f"PageUp/PageDown: page  •  "
            f"{_pretty_key('close_tab')}: close"
        )


# ── Main app ──────────────────────────────────────────────────────────────────
def _build_bindings() -> list:
    """Textual's App has a built-in `ctrl+q -> quit` priority binding that
    an empty subclass `BINDINGS = []` does NOT clear (Textual's binding
    merge is a per-key overwrite across the MRO — an empty list contributes
    zero keys, so it never touches App's inherited entry). To actually
    neutralize Ctrl+Q and move quitting to the configured key instead, this
    class needs its own priority binding occupying the same key."""
    quit_key = CONFIG["keybinds"]["quit"]
    bindings = []
    if quit_key != "ctrl+q":
        bindings.append(Binding("ctrl+q", "noop", show=False, priority=True))
    bindings.append(Binding(quit_key, "quit", show=False, priority=True))
    return bindings


class SDS(App):
    CSS   = CSS
    TITLE = "SDS"
    BINDINGS = _build_bindings()

    def __init__(self, start_dir: str = "."):
        super().__init__()
        self.start_dir    = str(Path(start_dir).resolve())
        self.open_files:  list[str]           = []
        self.active_tab:  int                 = -1
        self._saved_text: dict[str, str]      = {}
        self.editors:     dict[str, SDSEditor] = {}
        self.pdf_viewers: dict[str, SDSPdfViewer] = {}
        self.terminals:   dict[str, SDSTerminal] = {}
        self._terminal_counter = 0
        self._search_matches: list[tuple[int, int]] = []
        self._search_index:   int                   = -1
        self._pending_chord:  Optional[str]          = None
        self._chord_timer                             = None
        self._git_root:       Optional[str]          = None
        self._git_statuses:   dict[str, str]         = {}
        self._git_dir_rollup: dict[str, str]         = {}
        self._tree_nodes:     dict[str, TreeNode]    = {}

    def compose(self) -> ComposeResult:
        yield TabBar(id="tab-bar")
        with Horizontal(id="main-area"):
            with Vertical(id="file-tree-panel"):
                yield Static(
                    f" {I['folder']} {Path(self.start_dir).name}",
                    id="tree-label",
                )
                yield Tree(self.start_dir, id="tree")
            with Vertical(id="editor-panel"):
                yield Static("\n" + _help_text(), id="welcome")
        with Vertical(id="bottom-bar"):
            with Horizontal(id="search-bar"):
                yield Input(
                    placeholder="Find in file... (Enter: next, Up: prev, Esc: close)",
                    id="search-input",
                )
                yield Static("", id="search-info")
            with Horizontal(id="status-row"):
                yield StatusBar(id="status-bar")
                yield Static("", id="system-info")

    def on_mount(self) -> None:
        self._update_tab_bar()
        self._git_root = self._detect_git_root()
        self._set_git_status(self._compute_git_status(), recolor=False)
        tree = self.query_one(Tree)
        self._tree_nodes[str(Path(self.start_dir).resolve())] = tree.root
        self._fill_tree(tree.root, self.start_dir)
        tree.root.expand()
        tree.focus()
        self._refresh_status()
        if self._git_root:
            self.set_interval(4.0, self._refresh_git_status)
        self._refresh_clock()
        self.set_interval(15.0, self._refresh_clock)

    def _refresh_clock(self) -> None:
        parts = [f"{I['clock']} {time.strftime('%H:%M')}"]
        battery = _battery_status()
        if battery is not None:
            percent, charging = battery
            parts.append(f"{_battery_icon(percent, charging)} {percent}%")
        self.query_one("#system-info", Static).update("  ".join(parts))

    def _detect_git_root(self) -> Optional[str]:
        try:
            result = subprocess.run(
                ["git", "rev-parse", "--show-toplevel"],
                cwd=self.start_dir, capture_output=True, text=True, timeout=2,
            )
        except (OSError, subprocess.TimeoutExpired):
            return None
        if result.returncode != 0:
            return None
        return result.stdout.strip()

    def _compute_git_status(self) -> dict[str, str]:
        """{absolute_path: 2-char porcelain status} for everything git status
        reports as changed. Empty dict if this isn't a git repo (or git
        isn't available) — the tree just renders with no colors then."""
        if not self._git_root:
            return {}
        try:
            result = subprocess.run(
                ["git", "status", "--porcelain=v1", "--untracked-files=all"],
                cwd=self._git_root, capture_output=True, text=True, timeout=2,
            )
        except (OSError, subprocess.TimeoutExpired):
            return {}
        if result.returncode != 0:
            return {}
        statuses: dict[str, str] = {}
        for line in result.stdout.splitlines():
            if len(line) < 4:
                continue
            code = line[:2]
            rel = line[3:]
            if " -> " in rel:  # renames: "R  old -> new"
                rel = rel.split(" -> ", 1)[1]
            statuses[str(Path(self._git_root) / rel)] = code
        return statuses

    @staticmethod
    def _git_color_for_code(code: str) -> str:
        if code[0] == "?" and code[1] == "?":
            return GIT_UNTRACKED
        if code[1] not in (" ", "?"):
            return GIT_MODIFIED
        return GIT_STAGED

    def _compute_git_dir_rollup(self, statuses: dict[str, str]) -> dict[str, str]:
        """{directory_path: color} — the "worst" status color found anywhere
        under that directory. Precomputed once per status snapshot instead
        of the old approach, which rescanned the *entire* status dict for
        every directory node on every recolor (O(files) per node)."""
        priority = {GIT_STAGED: 0, GIT_MODIFIED: 1, GIT_UNTRACKED: 2}
        rollup: dict[str, str] = {}
        for path, code in statuses.items():
            color = self._git_color_for_code(code)
            d = Path(path).parent
            while True:
                key = str(d)
                if key not in rollup or priority[color] > priority[rollup[key]]:
                    rollup[key] = color
                if key == self._git_root or d.parent == d:
                    break
                d = d.parent
        return rollup

    def _git_color_for(self, path: str, is_dir: bool) -> Optional[str]:
        if not self._git_statuses:
            return None
        if is_dir:
            return self._git_dir_rollup.get(path)
        code = self._git_statuses.get(path)
        return self._git_color_for_code(code) if code else None

    def _set_git_status(self, statuses: dict[str, str], recolor: bool = True) -> None:
        """Store a freshly computed git-status snapshot. Skips the rollup
        rebuild and tree recolor entirely if nothing actually changed since
        the last snapshot — the 4s poll used to unconditionally rebuild
        every tree node's label regardless."""
        if statuses == self._git_statuses:
            return
        self._git_statuses = statuses
        self._git_dir_rollup = self._compute_git_dir_rollup(statuses)
        if recolor:
            self._recolor_tree(self.query_one(Tree).root)

    def _refresh_git_status_worker(self) -> None:
        statuses = self._compute_git_status()
        self.call_from_thread(self._set_git_status, statuses)

    def _refresh_git_status(self) -> None:
        """Recompute git status off the event loop — `git status` on a big
        repo can take real time, and this runs on a recurring 4s timer plus
        after every save, so it must never block keystrokes/rendering."""
        self.run_worker(
            self._refresh_git_status_worker, thread=True, exclusive=True,
            group="git-status",
        )

    def _recolor_tree(self, node: TreeNode) -> None:
        if node.data:
            path = Path(node.data)
            node.set_label(self._tree_label(path, path.is_dir()))
        for child in node.children:
            self._recolor_tree(child)

    def _tree_label(self, path: Path, is_dir: bool) -> Text:
        icon = I["folder"] if is_dir else EXT_ICON.get(path.suffix.lower(), I["file"])
        label = Text(f"{icon} {path.name}")
        color = self._git_color_for(str(path), is_dir)
        if color:
            label.stylize(color)
        return label

    def _fill_tree(self, node: TreeNode, path: str) -> None:
        try:
            entries = sorted(
                Path(path).iterdir(),
                key=lambda p: (not p.is_dir(), p.name.lower()),
            )
            for e in entries:
                if e.is_dir():
                    child = node.add(self._tree_label(e, True), data=str(e), allow_expand=True)
                else:
                    child = node.add_leaf(self._tree_label(e, False), data=str(e))
                self._tree_nodes[str(e.resolve())] = child
        except PermissionError:
            pass

    def on_tree_node_expanded(self, event: Tree.NodeExpanded) -> None:
        node = event.node
        if node.data and not node.children:
            self._fill_tree(node, node.data)

    def _find_node(self, path: str) -> Optional[TreeNode]:
        return self._tree_nodes.get(str(Path(path).resolve()))

    def _forget_subtree(self, node: TreeNode) -> None:
        """Drop every path->node entry for a node and its (already-loaded)
        descendants — used before an in-place refresh discards them, so the
        lookup dict never points at removed nodes."""
        if node.data:
            self._tree_nodes.pop(str(Path(node.data).resolve()), None)
        for child in node.children:
            self._forget_subtree(child)

    def _refresh_dir(self, base: str, select_name: Optional[str] = None) -> None:
        node = self._find_node(base)
        if node is None:
            return
        was_root = node.parent is None
        for child in node.children:
            self._forget_subtree(child)
        node.remove_children()
        self._fill_tree(node, base)
        tree = self.query_one(Tree)
        if not was_root:
            node.expand()
        if select_name:
            for child in node.children:
                if child.data and Path(child.data).name == select_name:
                    tree.select_node(child)
                    tree.scroll_to_node(child)
                    break

    def _open_new_entry(self) -> None:
        tree = self.query_one(Tree)
        node = tree.cursor_node
        base = self.start_dir
        if node is not None and node.data:
            p = Path(node.data)
            base = str(p if p.is_dir() else p.parent)

        def handle(name: Optional[str]) -> None:
            if name:
                self._create_entry(base, name)

        self.push_screen(NewEntryScreen(base), handle)

    def _create_entry(self, base: str, name: str) -> None:
        is_dir = name.endswith("/")
        rel = name.strip("/")
        if not rel:
            self.notify("Name cannot be empty", severity="error")
            return

        target = Path(base) / rel
        if target.exists():
            self.notify(f"{target.name} already exists", severity="error")
            return

        try:
            if is_dir:
                target.mkdir(parents=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.touch()
        except Exception as exc:
            self.notify(f"Error: {exc}", severity="error")
            return

        if self._git_root:
            self._set_git_status(self._compute_git_status(), recolor=False)
        self._refresh_dir(base, select_name=rel.split("/", 1)[0])
        if not is_dir:
            self._open_file(str(target))
        self.notify(f"Created {target.name}", timeout=1)

    def _remap_open_path(self, old: str, new: str) -> None:
        """After a rename, point any open tabs under `old` at `new` instead."""
        old_prefix = _dir_prefix(old)
        for i, p in enumerate(self.open_files):
            if _is_terminal_tab(p):
                continue
            if p == old:
                remapped = new
            elif p.startswith(old_prefix):
                remapped = _dir_prefix(new) + p[len(old_prefix):]
            else:
                continue
            self.open_files[i] = remapped
            if p in self.editors:
                self.editors[remapped] = self.editors.pop(p)
                self._saved_text[remapped] = self._saved_text.pop(p)
            if p in self.pdf_viewers:
                self.pdf_viewers[remapped] = self.pdf_viewers.pop(p)

    def _rename_selected(self) -> None:
        tree = self.query_one(Tree)
        node = tree.cursor_node
        if node is None or not node.data:
            return
        old_path = Path(node.data)

        def handle(new_name: Optional[str]) -> None:
            if new_name and new_name != old_path.name:
                self._rename_entry(old_path, new_name)

        self.push_screen(RenameScreen(old_path.name), handle)

    def _rename_entry(self, old_path: Path, new_name: str) -> None:
        new_name = new_name.strip().strip("/")
        if not new_name:
            self.notify("Name cannot be empty", severity="error")
            return
        new_path = old_path.parent / new_name
        if new_path.exists():
            self.notify(f"{new_name} already exists", severity="error")
            return
        try:
            old_path.rename(new_path)
        except Exception as exc:
            self.notify(f"Rename failed: {exc}", severity="error")
            return

        self._remap_open_path(str(old_path), str(new_path))
        if self._git_root:
            self._set_git_status(self._compute_git_status(), recolor=False)
        self._refresh_dir(str(old_path.parent), select_name=new_name)
        self._update_tab_bar()
        self._refresh_status()
        self.notify(f"Renamed to {new_name}", timeout=1)

    def _delete_selected(self) -> None:
        tree = self.query_one(Tree)
        node = tree.cursor_node
        if node is None or not node.data:
            return
        target = Path(node.data)
        kind = "directory" if target.is_dir() else "file"

        def handle(confirmed: bool) -> None:
            if confirmed:
                self._delete_entry(target)

        self.push_screen(
            ConfirmScreen(
                f"Do you really want to delete {kind} '{target.name}'? "
                f"This cannot be undone.",
                default_yes=False,
            ),
            handle,
        )

    def _delete_entry(self, target: Path) -> None:
        try:
            if target.is_dir():
                shutil.rmtree(target)
            else:
                target.unlink()
        except Exception as exc:
            self.notify(f"Delete failed: {exc}", severity="error")
            return

        prefix = _dir_prefix(str(target))
        for p in list(self.open_files):
            if not _is_terminal_tab(p) and (p == str(target) or p.startswith(prefix)):
                self._close_path_silently(p)

        if self._git_root:
            self._set_git_status(self._compute_git_status(), recolor=False)
        self._refresh_dir(str(target.parent))
        self.notify(f"Deleted {target.name}", timeout=1)

    def _panels(self) -> list:
        widgets: list = list(self.editors.values())
        widgets.extend(self.pdf_viewers.values())
        widgets.extend(self.terminals.values())
        return widgets

    def _hide_all_panels(self) -> None:
        for w in self._panels():
            w.display = False

    def _remove_welcome(self) -> None:
        try:
            self.query_one("#welcome").remove()
        except Exception:
            pass

    def _panel_dict_for(self, path: str) -> Optional[dict]:
        """Whichever of editors/pdf_viewers/terminals currently owns `path`."""
        for panels in (self.terminals, self.pdf_viewers, self.editors):
            if path in panels:
                return panels
        return None

    def _show_panel(self, path: str) -> None:
        """Hide every panel, then show and focus the one already open for
        `path`. A no-op if nothing is open for `path` — checked before
        hiding anything, so a bad path never blanks the current view."""
        panels = self._panel_dict_for(path)
        if panels is None:
            return
        self._hide_all_panels()
        panel = panels[path]
        panel.display = True
        panel.focus()

    def _open_terminal(self) -> None:
        """Always opens a brand new terminal tab, so you can have several
        running side by side — switch between them with Alt+Shift+Left/Right."""
        self._remove_welcome()

        self._hide_all_panels()

        n = self._terminal_counter
        self._terminal_counter += 1
        tab_id = f"{TERMINAL_TAB_PREFIX}{n}"
        term = SDSTerminal(self.start_dir, tab_id, id=f"terminal-panel-{n}")
        self.terminals[tab_id] = term
        self.query_one("#editor-panel").mount(term)
        self.open_files.append(tab_id)

        term.display = True
        self.active_tab = self.open_files.index(tab_id)

        self._update_tab_bar()
        self._refresh_status()
        self._sync_search()
        self.call_after_refresh(term.focus)

    def _open_file(self, filepath: str) -> None:
        if Path(filepath).suffix.lower() == ".pdf":
            self._open_pdf(filepath)
            return

        panel = self.query_one("#editor-panel")
        self._remove_welcome()

        self._hide_all_panels()

        if filepath not in self.editors:
            try:
                text = Path(filepath).read_text(errors="replace")
            except Exception as exc:
                text = f"# Error: {exc}\n"
            lang = EXT_LANG.get(Path(filepath).suffix.lower())
            ed = SDSEditor.code_editor(
                text, language=lang, theme="monokai",
                id=f"ed_{len(self.editors)}",
            )
            self.editors[filepath] = ed
            self._saved_text[filepath] = text
            panel.mount(ed)
        else:
            self.editors[filepath].display = True

        if filepath not in self.open_files:
            self.open_files.append(filepath)
        self.active_tab = self.open_files.index(filepath)

        self._update_tab_bar()
        self._refresh_status()
        self._sync_search()
        self.editors[filepath].focus()

    def _open_pdf(self, filepath: str) -> None:
        panel = self.query_one("#editor-panel")
        self._remove_welcome()

        self._hide_all_panels()

        if filepath not in self.pdf_viewers:
            viewer = SDSPdfViewer(filepath, id=f"pdf_{len(self.pdf_viewers)}")
            self.pdf_viewers[filepath] = viewer
            panel.mount(viewer)
        else:
            self.pdf_viewers[filepath].display = True

        if filepath not in self.open_files:
            self.open_files.append(filepath)
        self.active_tab = self.open_files.index(filepath)

        self._update_tab_bar()
        self._refresh_status()
        self._sync_search()
        self.pdf_viewers[filepath].focus()

    def _active_editor(self) -> Optional[SDSEditor]:
        p = self._active_path()
        return self.editors.get(p) if p else None

    def _active_path(self) -> Optional[str]:
        if self.active_tab < 0 or not self.open_files:
            return None
        return self.open_files[self.active_tab]

    def _is_modified(self, path: str) -> bool:
        """Whether `path`'s current editor content actually differs from
        what's on disk — computed from content rather than a dirty flag, so
        e.g. typing a character and then undoing it isn't flagged as a
        change, and a stray edit event from some other widget can never
        mark the wrong tab as modified."""
        ed = self.editors.get(path)
        return ed is not None and ed.text != self._saved_text.get(path)

    def _modified_indices(self) -> set[int]:
        return {i for i, p in enumerate(self.open_files) if self._is_modified(p)}

    def _update_tab_bar(self) -> None:
        self.query_one(TabBar).refresh_tabs(
            self.open_files, self.active_tab, self._modified_indices(),
        )

    def _refresh_status(self) -> None:
        sb   = self.query_one(StatusBar)
        path = self._active_path()
        if not path:
            sb.set_info()
            return
        if path in self.terminals:
            sb.set_terminal_info()
            return
        if path in self.pdf_viewers:
            sb.set_pdf_info(Path(path).name)
            return
        ed = self._active_editor()
        if ed:
            r, c = ed.cursor_location
            lang = EXT_LANG.get(Path(path).suffix.lower(), "")
            sb.set_info(path, r, c, lang, self._is_modified(path))

    def on_text_area_changed(self, event: TextArea.Changed) -> None:
        # TextArea is no longer exclusive to SDSEditor — the PDF viewer's
        # text-mode fallback (#pdf-text) also uses one, and setting its
        # `.text` on every page render/switch would otherwise fire this too.
        # Harmless now regardless (modified-ness is computed from content,
        # not set here), but skip the refresh for anything that isn't the
        # active editor so a background PDF re-render doesn't repaint chrome.
        if self.active_tab >= 0 and event.text_area is self._active_editor():
            self._update_tab_bar()
            self._refresh_status()

    def on_text_area_selection_changed(self, _: TextArea.SelectionChanged) -> None:
        self._refresh_status()

    def _save_current(self) -> None:
        path = self._active_path()
        ed   = self._active_editor()
        if not path or not ed:
            return
        try:
            Path(path).write_text(ed.text)
            self._saved_text[path] = ed.text
            self._update_tab_bar()
            self._refresh_status()
            if self._git_root:
                self._refresh_git_status()
            self.notify(f"Saved {Path(path).name}", timeout=1)
        except Exception as exc:
            self.notify(f"Save failed: {exc}", severity="error")

    def _save_all(self) -> None:
        for idx in sorted(self._modified_indices()):
            path = self.open_files[idx]
            ed = self.editors.get(path)
            if ed is None:
                continue
            try:
                Path(path).write_text(ed.text)
                self._saved_text[path] = ed.text
            except Exception as exc:
                self.notify(f"Save failed for {Path(path).name}: {exc}", severity="error")
        self._update_tab_bar()
        self._refresh_status()
        if self._git_root:
            self._refresh_git_status()

    def _do_quit(self) -> None:
        for term in self.terminals.values():
            term.kill()
        self.exit()

    def action_noop(self) -> None:
        """Bound to Ctrl+Q when it's not the configured quit key, so it's a
        true no-op instead of falling through to anything else."""
        pass

    async def action_quit(self) -> None:
        if len(self.screen_stack) > 1:
            return

        modified = self._modified_indices()
        if modified:
            n = len(modified)

            def handle(save: bool) -> None:
                if save:
                    self._save_all()
                self._do_quit()

            self.push_screen(
                ConfirmScreen(f"Unsaved changes in {n} file{'s' if n != 1 else ''}. Save before quitting?"),
                handle,
            )
            return

        # No unsaved changes, but still confirm — a stray Ctrl+Q shouldn't
        # silently kill every open terminal and close the app.
        def handle_quit(confirmed: bool) -> None:
            if confirmed:
                self._do_quit()

        self.push_screen(ConfirmScreen("Quit SDS?", default_yes=False), handle_quit)

    def _close_tab(self) -> None:
        if not self.open_files or self.active_tab < 0:
            return
        path = self._active_path()
        if path and self._is_modified(path):
            def handle(save: bool) -> None:
                if save:
                    self._save_current()
                self._do_close()
            self.push_screen(ConfirmScreen("Save changes before closing?"), handle)
        else:
            self._do_close()

    def _do_close(self, idx: Optional[int] = None) -> None:
        if idx is None:
            idx = self.active_tab
        if idx < 0 or idx >= len(self.open_files):
            return
        closing_active = idx == self.active_tab

        path = self.open_files.pop(idx)
        if path in self.terminals:
            term = self.terminals.pop(path)
            term.kill()
            term.remove()
        elif path in self.pdf_viewers:
            viewer = self.pdf_viewers.pop(path)
            viewer.close()
            viewer.remove()
        elif path in self.editors:
            self.editors.pop(path).remove()
            self._saved_text.pop(path, None)

        if closing_active:
            if self.open_files:
                self.active_tab = min(idx, len(self.open_files) - 1)
                self._show_panel(self.open_files[self.active_tab])
            else:
                self.active_tab = -1
                self.query_one(Tree).focus()
        elif idx < self.active_tab:
            self.active_tab -= 1

        self._update_tab_bar()
        self._refresh_status()
        self._sync_search()

    def _close_path_silently(self, path: str) -> None:
        """Close a tab (no save prompt) if it's open — used when its file/dir
        is deleted or renamed out from under it."""
        if path in self.open_files:
            self._do_close(self.open_files.index(path))

    # ── Search in file ──────────────────────────────────────────────────────
    def _sync_search(self) -> None:
        bar = self.query_one("#search-bar")
        if not bar.display:
            return
        if not self._active_editor():
            self._close_search()
            return
        self._run_search(self.query_one("#search-input", Input).value)

    def _toggle_search(self) -> None:
        ed = self._active_editor()
        if not ed:
            self.notify("Open a file to search", timeout=1)
            return
        bar = self.query_one("#search-bar")
        inp = self.query_one("#search-input", Input)
        if bar.display:
            self._search_next(1)
            inp.focus()
        else:
            bar.display = True
            inp.value = ""
            self._search_matches = []
            self._search_index = -1
            self._update_search_info()
            inp.focus()
            self.refresh(layout=True)

    def _close_search(self) -> None:
        bar = self.query_one("#search-bar")
        bar.display = False
        self._search_matches = []
        self._search_index = -1
        ed = self._active_editor()
        if ed:
            ed.focus()
        self.refresh(layout=True)

    def _run_search(self, query: str) -> None:
        ed = self._active_editor()
        if not ed or not query:
            self._search_matches = []
            self._search_index = -1
            self._update_search_info()
            return

        text = ed.text
        lower_text, lower_query = text.lower(), query.lower()
        matches = []
        start = 0
        while True:
            idx = lower_text.find(lower_query, start)
            if idx == -1:
                break
            matches.append((idx, idx + len(query)))
            start = idx + len(query)

        self._search_matches = matches
        if matches:
            cur_offset = _location_to_offset(text, ed.cursor_location)
            self._search_index = next(
                (i for i, (s, _) in enumerate(matches) if s >= cur_offset), 0
            )
            self._select_match()
        else:
            self._search_index = -1
        self._update_search_info()

    def _select_match(self) -> None:
        ed = self._active_editor()
        if not ed or self._search_index < 0:
            return
        s, e = self._search_matches[self._search_index]
        text = ed.text
        ed.selection = Selection(
            _offset_to_location(text, s), _offset_to_location(text, e)
        )
        ed.scroll_cursor_visible()

    def _search_next(self, step: int) -> None:
        if not self._search_matches:
            return
        self._search_index = (self._search_index + step) % len(self._search_matches)
        self._select_match()
        self._update_search_info()

    def _update_search_info(self) -> None:
        info = self.query_one("#search-info", Static)
        if not self._search_matches:
            query = self.query_one("#search-input", Input).value
            info.update("No matches" if query else "")
        else:
            info.update(f"{self._search_index + 1}/{len(self._search_matches)}")

    def on_input_changed(self, event: Input.Changed) -> None:
        if event.input.id == "search-input":
            self._run_search(event.value)

    def on_input_submitted(self, event: Input.Submitted) -> None:
        if event.input.id == "search-input":
            self._search_next(1)
            event.stop()

    def _switch_to_terminal(self, tab_id: str) -> None:
        """Switch to an existing terminal tab — unlike _open_terminal(),
        this never creates a new one (used for tab-cycling)."""
        if tab_id not in self.terminals:
            return
        self._remove_welcome()
        self._show_panel(tab_id)
        self.active_tab = self.open_files.index(tab_id)
        self._update_tab_bar()
        self._refresh_status()
        self._sync_search()

    def _show_tab(self, path: str) -> None:
        if path in self.terminals:
            self._switch_to_terminal(path)
        else:
            self._open_file(path)

    # ── Ctrl+K leader key: c = toggle comment, t = terminal ─────────────────
    def _arm_leader(self) -> None:
        self._pending_chord = CONFIG["keybinds"]["leader"]
        if self._chord_timer:
            self._chord_timer.stop()
        self._chord_timer = self.set_timer(2.5, self._clear_chord)
        self.query_one(StatusBar).update(
            f"{_pretty_key('leader')} …  c: comment/uncomment   t: terminal   "
            "h: help   (any other key cancels)"
        )

    def _clear_chord(self) -> None:
        self._pending_chord = None
        self._chord_timer = None
        self._refresh_status()

    def _resolve_chord(self, key: str) -> bool:
        """Resolve a pending leader chord with the given follow-up key.
        Returns True if the key was consumed (caller should stop the event)."""
        if self._pending_chord != CONFIG["keybinds"]["leader"]:
            return False
        self._pending_chord = None
        if self._chord_timer:
            self._chord_timer.stop()
            self._chord_timer = None
        if isinstance(self.focused, Input):
            self._refresh_status()
            return False
        if key == "c":
            self._toggle_comment_selection()
            self._refresh_status()
            return True
        if key == "t":
            self._open_terminal()
            return True
        if key == "h":
            self.push_screen(HelpScreen())
            return True
        self._refresh_status()
        return False

    def _toggle_comment_selection(self) -> None:
        ed   = self._active_editor()
        path = self._active_path()
        if not ed or not path:
            return
        token = COMMENT_TOKEN.get(Path(path).suffix.lower())
        if not token:
            self.notify("No line-comment syntax for this file type", severity="error")
            return

        sel = ed.selection
        start_row = min(sel.start[0], sel.end[0])
        end_row   = max(sel.start[0], sel.end[0])
        end_col   = sel.end[1] if sel.end[0] >= sel.start[0] else sel.start[1]
        if end_row > start_row and end_col == 0:
            end_row -= 1

        lines = ed.text.split("\n")
        target = [lines[r] for r in range(start_row, end_row + 1) if r < len(lines)]
        non_blank = [l for l in target if l.strip()]
        all_commented = bool(non_blank) and all(
            l.lstrip().startswith(token) for l in non_blank
        )

        n = 0
        if all_commented:
            for row in range(start_row, end_row + 1):
                if row >= len(lines):
                    continue
                line = lines[row]
                stripped = line.lstrip()
                if not stripped.startswith(token):
                    continue
                indent = len(line) - len(stripped)
                rest = stripped[len(token):]
                if rest.startswith(" "):
                    rest = rest[1:]
                remove = len(line) - (indent + len(rest))
                ed.delete((row, indent), (row, indent + remove))
                n += 1
            self.notify(f"Uncommented {n} line{'s' if n != 1 else ''}", timeout=1)
        else:
            for row in range(start_row, end_row + 1):
                ed.insert(f"{token} ", (row, 0))
                n += 1
            self.notify(f"Commented {n} line{'s' if n != 1 else ''}", timeout=1)

    def on_key(self, event: events.Key) -> None:
        key = event.key
        kb  = CONFIG["keybinds"]

        if len(self.screen_stack) > 1:
            return

        if key == kb["leader"] and not isinstance(self.focused, Input):
            self._arm_leader()
            event.stop(); event.prevent_default(); return

        # If not consumed, _resolve_chord already cleared the chord and
        # refreshed the status bar; the key falls through normally below.
        if self._pending_chord == kb["leader"] and self._resolve_chord(key):
            event.stop(); event.prevent_default(); return

        if key == kb["find"]:
            self._toggle_search(); event.stop(); return

        if key == kb["new_entry"]:
            self._open_new_entry(); event.stop(); return

        search_bar = self.query_one("#search-bar")
        if search_bar.display:
            search_input = self.query_one("#search-input", Input)
            if key == "escape":
                self._close_search(); event.stop(); return
            if self.focused is search_input:
                if key == "down":
                    self._search_next(1); event.stop(); return
                if key == "up":
                    self._search_next(-1); event.stop(); return

        if key == kb["save"]:
            self._save_current(); event.stop(); return

        if key in (kb["close_tab"], "alt+shift+w"):
            self._close_tab(); event.stop(); return

        if key in (kb["tab_prev"], kb["tab_next"]):
            if self.open_files:
                delta = -1 if key == kb["tab_prev"] else 1
                self.active_tab = (self.active_tab + delta) % len(self.open_files)
                self._show_tab(self.open_files[self.active_tab])
            event.stop(); return

        tree = self.query_one(Tree)
        ed   = self._active_editor()

        if key in (kb["tree_up"], kb["tree_down"]):
            if key == kb["tree_up"]:
                tree.action_cursor_up()
            else:
                tree.action_cursor_down()
            if ed: ed.focus()
            event.stop(); return

        if key == kb["tree_collapse"]:
            node = tree.cursor_node
            if node:
                try:
                    node.collapse()
                except Exception:
                    pass
            if ed: ed.focus()
            event.stop(); return

        if key == kb["tree_expand"]:
            node = tree.cursor_node
            if node and node.data:
                try:
                    if Path(node.data).is_dir():
                        node.expand()
                        if not node.children:
                            self._fill_tree(node, node.data)
                    else:
                        self.notify("Select a directory to expand", timeout=1)
                except Exception:
                    self.notify("Cannot expand", timeout=1)
            if ed: ed.focus()
            event.stop(); return

        if key == kb["tree_open"]:
            node = tree.cursor_node
            if node and node.data:
                try:
                    if Path(node.data).is_file():
                        self._open_file(node.data)
                    else:
                        self.notify("Select a file to open", timeout=1)
                except Exception as exc:
                    self.notify(f"Error: {exc}", timeout=2)
            event.stop(); return

        if key == kb["tree_rename"]:
            self._rename_selected()
            if ed: ed.focus()
            event.stop(); return

        if key == kb["tree_delete"]:
            self._delete_selected()
            if ed: ed.focus()
            event.stop(); event.prevent_default(); return


def main():
    start = sys.argv[1] if len(sys.argv) > 1 else "."
    SDS(start_dir=start).run()


if __name__ == "__main__":
    main()