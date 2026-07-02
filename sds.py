#!/usr/bin/env python3
"""SDS - SimpleDevSuite"""

import fcntl
import os
import pty
import shutil
import signal
import struct
import sys
import termios
from pathlib import Path
from typing import Optional

import fitz  # PyMuPDF
import pyte
from PIL import Image as PILImage
from rich.segment import Segment
from rich.style import Style
from textual.app import App, ComposeResult
from textual.widget import Widget
from textual.widgets import Static, Tree, TextArea, Button, Label, Input
from textual.containers import Horizontal, Vertical
from textual import events
from textual.binding import Binding
from textual.strip import Strip
from textual.widgets.tree import TreeNode
from textual.widgets.text_area import Selection
from textual.screen import ModalScreen
from textual_image.widget import AutoImage
from textual_image.renderable.halfcell import Image as HalfcellRenderable
from textual_image.renderable.sixel import Image as SixelRenderable
from textual_image.renderable.tgp import Image as TGPRenderable
from textual_image.renderable.unicode import Image as UnicodeRenderable
from textual_image._terminal import get_cell_size
from rich.markup import escape

TERMINAL_TAB_PREFIX = "\x00terminal\x00"

# The renderer AutoImage picked at import time is whatever this terminal's
# graphics capability actually resolved to — put it first in the cycle list
# under the "Auto" label so cycling starts from what auto-detection chose.
_RENDER_MODES = [
    ("Auto", AutoImage._Renderable),
    ("TGP/Kitty", TGPRenderable),
    ("Sixel", SixelRenderable),
    ("Halfcell", HalfcellRenderable),
    ("Unicode", UnicodeRenderable),
]


def _is_terminal_tab(path: str) -> bool:
    return path.startswith(TERMINAL_TAB_PREFIX)

YELLOW = "#f5a623"
BLACK  = "#000000"
WHITE  = "#ffffff"
GREY   = "#444444"
DGREY  = "#1a1a1a"
RED    = "#d23c3d"

I = {
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
}

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

CSS = f"""
Screen {{
    background: {BLACK};
}}

#tab-bar {{
    height: 2;
    background: {BLACK};
    color: {WHITE};
    border-bottom: solid {YELLOW};
    padding: 0 1;
    overflow: hidden;
}}

#main-area {{
    height: 1fr;
}}

#file-tree-panel {{
    width: 28;
    border-right: solid {YELLOW};
    background: {BLACK};
}}

#tree-label {{
    height: 1;
    background: {YELLOW};
    color: {BLACK};
    padding: 0 1;
    text-style: bold;
}}

Tree {{
    background: {BLACK};
    color: {WHITE};
    scrollbar-color: {YELLOW} {BLACK};
    scrollbar-size: 1 1;
}}

Tree > .tree--cursor {{
    background: {YELLOW};
    color: {BLACK};
}}

#editor-panel {{
    background: {BLACK};
    height: 1fr;
    width: 1fr;
}}

#welcome {{
    color: {YELLOW};
    width: 1fr;
    height: 1fr;
    content-align: center middle;
    text-align: left;
    padding: 0 4;
}}

SDSEditor {{
    height: 1fr;
    width: 1fr;
    background: {BLACK};
}}

SDSEditor > .text-area--gutter {{
    background: {DGREY};
    color: {GREY};
}}

SDSEditor > .text-area--cursor-line {{
    background: {DGREY};
}}

SDSEditor > .text-area--cursor {{
    background: {YELLOW};
    color: {BLACK};
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
    background: {BLACK};
    border: solid {YELLOW};
    padding: 1 2;
    align: center middle;
    layout: vertical;
}}

#confirm-label {{
    color: {WHITE};
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

Button {{
    background: {BLACK};
    color: {WHITE};
    border: solid {GREY};
    margin: 0 1;
    min-width: 12;
    height: 3;
}}

Button:focus {{
    background: {YELLOW};
    color: {BLACK};
    border: solid {YELLOW};
}}

Button.-primary {{
    background: {YELLOW};
    color: {BLACK};
    border: solid {YELLOW};
}}

#bottom-bar {{
    dock: bottom;
    height: auto;
}}

#status-bar {{
    height: 1;
    background: {DGREY};
    color: {GREY};
    padding: 0 1;
}}

#search-bar {{
    height: 4;
    background: {DGREY};
    border-top: solid {YELLOW};
    padding: 0 1;
    display: none;
}}

#search-input {{
    width: 1fr;
    border: solid {GREY};
}}

#search-input:focus {{
    border: solid {YELLOW};
}}

#search-info {{
    width: auto;
    min-width: 9;
    color: {GREY};
    padding: 0 1;
    content-align: right middle;
}}

SDSTerminal {{
    height: 1fr;
    width: 1fr;
    background: #000000;
    border: solid {GREY};
}}

SDSTerminal:focus {{
    border: solid {YELLOW};
}}

SDSPdfViewer {{
    height: 1fr;
    width: 1fr;
    background: {BLACK};
    align: center middle;
}}

#pdf-image {{
    /* Both must be "auto" (not 1fr) so the image is scaled to fit while
       preserving the page's aspect ratio, instead of being stretched to
       exactly fill the container. */
    width: auto;
    height: auto;
}}

#pdf-status {{
    dock: bottom;
    height: 1;
    background: {DGREY};
    color: {GREY};
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
    background: {BLACK};
    border: solid {YELLOW};
    padding: 1 2;
}}

#newentry-label {{
    color: {WHITE};
    width: 1fr;
    height: auto;
    margin-bottom: 1;
}}

#newentry-input {{
    width: 1fr;
    border: solid {GREY};
    margin-bottom: 1;
}}

#newentry-input:focus {{
    border: solid {YELLOW};
}}

#newentry-hint {{
    color: {GREY};
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


class SDSEditor(TextArea):
    # TextArea inserts printable characters itself before a Key event ever
    # bubbles up to the App, so the Ctrl+K leader's follow-up key (c/t) has
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


# ── Pty-backed subprocess — spawns a real shell attached to a pseudo-terminal ─
class PtyProcess:
    def __init__(self, cwd: str, cols: int, rows: int):
        import subprocess
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

    def on_mount(self) -> None:
        cols = max(self.size.width, 10)
        rows = max(self.size.height, 3)
        self.vt = pyte.HistoryScreen(cols, rows, history=5000)
        self.vtstream = pyte.ByteStream(self.vt)
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
        on every poll tick as a cheap, self-healing safety net."""
        cols = max(self.size.width, 10)
        rows = max(self.size.height, 3)
        if (cols, rows) != (self.vt.columns, self.vt.lines):
            self.vt.resize(rows, cols)
            if self.pty is not None:
                self.pty.resize(rows, cols)

    def _poll(self) -> None:
        if self.pty is None or self._dead:
            return
        self._sync_size()
        self._update_title()
        data = self.pty.read()
        if data:
            self.vtstream.feed(data)
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

    # ── rendering ────────────────────────────────────────────────────────
    def _display_line(self, y: int) -> dict:
        vt = self.vt
        if self._scroll_offset == 0:
            return vt.buffer.get(y, {})
        history = vt.history.top
        total_history = len(history)
        start = total_history - self._scroll_offset
        idx = start + y
        if idx < 0:
            return {}
        if idx < total_history:
            return history[idx]
        return vt.buffer.get(idx - total_history, {})

    def render_line(self, y: int) -> Strip:
        if self.vt is None:
            return Strip.blank(self.size.width)
        width = self.size.width
        line = self._display_line(y)
        cursor_x = None
        if (self._scroll_offset == 0 and not self.vt.cursor.hidden
                and self.vt.cursor.y == y):
            cursor_x = self.vt.cursor.x

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
        if key in ("alt+shift+left", "alt+shift+right", "alt+shift+w"):
            # Ctrl+W itself is deliberately forwarded to the shell (readline
            # delete-word) rather than closing the tab — the tab closes
            # naturally once the shell exits (see `_dead` handling below).
            # Alt+Shift+W is left exempted here as a harmless bonus shortcut
            # for terminals/WMs that happen to let it through unmolested,
            # but it's not reliable in general (commonly intercepted by the
            # OS/WM before Textual ever sees it), so it isn't advertised.
            return

        # The terminal forwards nearly everything to the shell, which would
        # otherwise swallow the app's Ctrl+K leader (new terminal / comment)
        # before it ever reaches the App — same tradeoff SDSEditor makes for
        # the same reason. This is the one thing that stays reserved.
        app = self.app
        if getattr(app, "_pending_chord", None) == "ctrl+k":
            if hasattr(app, "_resolve_chord") and app._resolve_chord(key):
                event.stop(); event.prevent_default(); return
            # unresolved: chord already cleared by _resolve_chord; fall
            # through so this keystroke still reaches the shell normally
        elif key == "ctrl+k" and hasattr(app, "_arm_leader"):
            app._arm_leader()
            event.stop(); event.prevent_default(); return

        if key == "shift+pageup":
            self._scroll(max(self.size.height - 1, 1))
            event.stop(); event.prevent_default(); return
        if key == "shift+pagedown":
            self._scroll(-max(self.size.height - 1, 1))
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
        self.render_mode_index = 0

    def compose(self) -> ComposeResult:
        yield AutoImage(id="pdf-image")
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
        pix = page.get_pixmap(matrix=fitz.Matrix(self.zoom, self.zoom))
        pil_mode = "RGBA" if pix.alpha else "RGB"
        img = PILImage.frombytes(pil_mode, (pix.width, pix.height), pix.samples)
        self.query_one("#pdf-image", AutoImage).image = img
        self._update_status()

    def _update_status(self) -> None:
        if not self.doc:
            return
        mode_name = _RENDER_MODES[self.render_mode_index][0]
        try:
            cell = get_cell_size()
            cell_info = f"  |  cell: {cell.width}x{cell.height}px"
        except Exception:
            cell_info = ""
        self.query_one("#pdf-status", Static).update(
            f"Page {self.page_index + 1}/{self.doc.page_count}  |  "
            f"PageUp/PageDown: navigate  +/-: zoom  m: render mode ({mode_name}){cell_info}"
        )

    def cycle_render_mode(self) -> None:
        self.render_mode_index = (self.render_mode_index + 1) % len(_RENDER_MODES)
        mode_name, renderer_cls = _RENDER_MODES[self.render_mode_index]
        img_widget = self.query_one("#pdf-image", AutoImage)
        img_widget._Renderable = renderer_cls
        img_widget.refresh(layout=True)
        self._update_status()
        self.notify(f"PDF render mode: {mode_name}", timeout=2)

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
        elif char in ("+", "="):
            self.zoom_in(); event.stop(); event.prevent_default()
        elif char == "-":
            self.zoom_out(); event.stop(); event.prevent_default()
        elif char == "m":
            self.cycle_render_mode(); event.stop(); event.prevent_default()


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
                "Alt+Up/Down: tree  Alt+Left/Right: dir  Alt+Enter: open  "
                "Alt+Insert: rename  Alt+Delete: delete  "
                "Alt+Shift+Left/Right: tabs  Ctrl+N: new  Ctrl+F: find  "
                "Ctrl+K then C/T: comment / terminal  "
                "Ctrl+W: close  Ctrl+S: save  Ctrl+Q: quit"
            )
            return
        name = Path(filepath).name
        mod  = "  [unsaved]" if modified else ""
        icon = EXT_ICON.get(Path(filepath).suffix.lower(), I["file"])
        self.update(
            f"{icon} {name}{mod}  |  "
            f"Ln {row+1} Col {col+1}  |  "
            f"{lang or 'text'}  |  "
            f"Ctrl+F: find  Ctrl+A: select all  Ctrl+K then C/T: comment / terminal  "
            f"Ctrl+S: save  Ctrl+W: close  Ctrl+Q: quit"
        )

    def set_terminal_info(self) -> None:
        self.update(
            f"{I['terminal']} Terminal  |  Shift+PgUp/PgDn or wheel: scrollback  |  "
            f"type 'exit' to close  |  Alt+Shift+Left/Right: switch tabs  |  Ctrl+Q: quit"
        )

    def set_pdf_info(self, name: str) -> None:
        self.update(
            f"{I['pdf']} {name}  |  PageUp/PageDown: page  +/-: zoom  m: render mode  |  "
            f"Ctrl+W: close  Ctrl+Q: quit"
        )


# ── Main app ──────────────────────────────────────────────────────────────────
class SDS(App):
    CSS   = CSS
    TITLE = "SDS"
    BINDINGS = []

    def __init__(self, start_dir: str = "."):
        super().__init__()
        self.start_dir    = str(Path(start_dir).resolve())
        self.open_files:  list[str]           = []
        self.active_tab:  int                 = -1
        self.modified:    set[int]            = set()
        self.editors:     dict[str, SDSEditor] = {}
        self.pdf_viewers: dict[str, SDSPdfViewer] = {}
        self.terminals:   dict[str, SDSTerminal] = {}
        self._terminal_counter = 0
        self._search_matches: list[tuple[int, int]] = []
        self._search_index:   int                   = -1
        self._pending_chord:  Optional[str]          = None
        self._chord_timer                             = None

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
                yield Static(
                    f"\n\n"
                    f"  SDS \u2014 SimpleDevSuite\n\n"
                    f"  Alt+Up/Down        navigate tree\n"
                    f"  Alt+Left/Right     collapse / expand dir\n"
                    f"  Alt+Enter          open file\n"
                    f"  Alt+Insert         rename file/dir\n"
                    f"  Alt+Delete         delete file/dir\n"
                    f"  Alt+Shift+Left/Right  switch tabs\n"
                    f"  Ctrl+N             new file/dir\n"
                    f"  Ctrl+F             find in file\n"
                    f"  Ctrl+A             select all\n"
                    f"  Ctrl+K then C      comment / uncomment\n"
                    f"  Ctrl+K then T      terminal\n"
                    f"  Ctrl+W             close tab\n"
                    f"  Ctrl+S             save\n"
                    f"  Ctrl+Q             quit\n",
                    id="welcome",
                )
        with Vertical(id="bottom-bar"):
            with Horizontal(id="search-bar"):
                yield Input(
                    placeholder="Find in file... (Enter: next, Up: prev, Esc: close)",
                    id="search-input",
                )
                yield Static("", id="search-info")
            yield StatusBar(id="status-bar")

    def on_mount(self) -> None:
        self._update_tab_bar()
        tree = self.query_one(Tree)
        self._fill_tree(tree.root, self.start_dir)
        tree.root.expand()
        tree.focus()
        self._refresh_status()

    def _fill_tree(self, node: TreeNode, path: str) -> None:
        try:
            entries = sorted(
                Path(path).iterdir(),
                key=lambda p: (not p.is_dir(), p.name.lower()),
            )
            for e in entries:
                if e.is_dir():
                    node.add(f"{I['folder']} {e.name}", data=str(e), allow_expand=True)
                else:
                    icon = EXT_ICON.get(e.suffix.lower(), I["file"])
                    node.add_leaf(f"{icon} {e.name}", data=str(e))
        except PermissionError:
            pass

    def on_tree_node_expanded(self, event: Tree.NodeExpanded) -> None:
        node = event.node
        if node.data and not node.children:
            self._fill_tree(node, node.data)

    def _find_node(self, node: TreeNode, path: str) -> Optional[TreeNode]:
        node_path = node.data or (self.start_dir if node.parent is None else None)
        if node_path and Path(node_path).resolve() == Path(path).resolve():
            return node
        for child in node.children:
            found = self._find_node(child, path)
            if found is not None:
                return found
        return None

    def _refresh_dir(self, base: str, select_name: Optional[str] = None) -> None:
        tree = self.query_one(Tree)
        node = self._find_node(tree.root, base)
        if node is None:
            return
        was_root = node.parent is None
        node.remove_children()
        self._fill_tree(node, base)
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

        self._refresh_dir(base, select_name=rel.split("/", 1)[0])
        if not is_dir:
            self._open_file(str(target))
        self.notify(f"Created {target.name}", timeout=1)

    def _remap_open_path(self, old: str, new: str) -> None:
        """After a rename, point any open tabs under `old` at `new` instead."""
        old_prefix = old.rstrip("/") + "/"
        for i, p in enumerate(self.open_files):
            if _is_terminal_tab(p):
                continue
            if p == old:
                remapped = new
            elif p.startswith(old_prefix):
                remapped = new.rstrip("/") + "/" + p[len(old_prefix):]
            else:
                continue
            self.open_files[i] = remapped
            if p in self.editors:
                self.editors[remapped] = self.editors.pop(p)
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

        prefix = str(target).rstrip("/") + "/"
        for p in list(self.open_files):
            if not _is_terminal_tab(p) and (p == str(target) or p.startswith(prefix)):
                self._close_path_silently(p)

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

    def _open_terminal(self) -> None:
        """Always opens a brand new terminal tab, so you can have several
        running side by side — switch between them with Alt+Shift+Left/Right."""
        try:
            self.query_one("#welcome").remove()
        except Exception:
            pass

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
        try:
            self.query_one("#welcome").remove()
        except Exception:
            pass

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
        try:
            self.query_one("#welcome").remove()
        except Exception:
            pass

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

    def _update_tab_bar(self) -> None:
        self.query_one(TabBar).refresh_tabs(
            self.open_files, self.active_tab, self.modified,
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
            sb.set_info(path, r, c, lang, self.active_tab in self.modified)

    def on_text_area_changed(self, _: TextArea.Changed) -> None:
        if self.active_tab >= 0:
            self.modified.add(self.active_tab)
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
            self.modified.discard(self.active_tab)
            self._update_tab_bar()
            self._refresh_status()
            self.notify(f"Saved {Path(path).name}", timeout=1)
        except Exception as exc:
            self.notify(f"Save failed: {exc}", severity="error")

    def _save_all(self) -> None:
        for idx in sorted(self.modified):
            path = self.open_files[idx]
            ed = self.editors.get(path)
            if ed is None:
                continue
            try:
                Path(path).write_text(ed.text)
            except Exception as exc:
                self.notify(f"Save failed for {Path(path).name}: {exc}", severity="error")
        self.modified.clear()
        self._update_tab_bar()
        self._refresh_status()

    def _do_quit(self) -> None:
        for term in self.terminals.values():
            term.kill()
        self.exit()

    async def action_quit(self) -> None:
        if len(self.screen_stack) > 1:
            return

        if self.modified:
            n = len(self.modified)

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
        if self.active_tab in self.modified:
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
        self.modified = {
            (i if i < idx else i - 1)
            for i in self.modified if i != idx
        }
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

        if closing_active:
            if self.open_files:
                self.active_tab = min(idx, len(self.open_files) - 1)
                fp = self.open_files[self.active_tab]
                self._hide_all_panels()
                if fp in self.terminals:
                    self.terminals[fp].display = True
                    self.terminals[fp].focus()
                elif fp in self.pdf_viewers:
                    self.pdf_viewers[fp].display = True
                    self.pdf_viewers[fp].focus()
                else:
                    self.editors[fp].display = True
                    self.editors[fp].focus()
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
        term = self.terminals.get(tab_id)
        if term is None:
            return
        try:
            self.query_one("#welcome").remove()
        except Exception:
            pass
        self._hide_all_panels()
        term.display = True
        self.active_tab = self.open_files.index(tab_id)
        self._update_tab_bar()
        self._refresh_status()
        self._sync_search()
        term.focus()

    def _show_tab(self, path: str) -> None:
        if path in self.terminals:
            self._switch_to_terminal(path)
        else:
            self._open_file(path)

    # ── Ctrl+K leader key: c = toggle comment, t = terminal ─────────────────
    def _arm_leader(self) -> None:
        self._pending_chord = "ctrl+k"
        if self._chord_timer:
            self._chord_timer.stop()
        self._chord_timer = self.set_timer(2.5, self._clear_chord)
        self.query_one(StatusBar).update(
            "Ctrl+K …  c: comment/uncomment   t: terminal   (any other key cancels)"
        )

    def _clear_chord(self) -> None:
        self._pending_chord = None
        self._chord_timer = None
        self._refresh_status()

    def _resolve_chord(self, key: str) -> bool:
        """Resolve a pending Ctrl+K chord with the given follow-up key.
        Returns True if the key was consumed (caller should stop the event)."""
        if self._pending_chord != "ctrl+k":
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

        if len(self.screen_stack) > 1:
            return

        if key == "ctrl+k" and not isinstance(self.focused, Input):
            self._arm_leader()
            event.stop(); event.prevent_default(); return

        # If not consumed, _resolve_chord already cleared the chord and
        # refreshed the status bar; the key falls through normally below.
        if self._pending_chord == "ctrl+k" and self._resolve_chord(key):
            event.stop(); event.prevent_default(); return

        if key == "ctrl+f":
            self._toggle_search(); event.stop(); return

        if key == "ctrl+n":
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

        if key == "ctrl+s":
            self._save_current(); event.stop(); return

        if key in ("ctrl+w", "alt+shift+w"):
            self._close_tab(); event.stop(); return

        if key in ("alt+shift+left", "alt+shift+right"):
            if self.open_files:
                delta = -1 if key == "alt+shift+left" else 1
                self.active_tab = (self.active_tab + delta) % len(self.open_files)
                self._show_tab(self.open_files[self.active_tab])
            event.stop(); return

        tree = self.query_one(Tree)
        ed   = self._active_editor()

        if key in ("alt+up", "alt+down"):
            if key == "alt+up":
                tree.action_cursor_up()
            else:
                tree.action_cursor_down()
            if ed: ed.focus()
            event.stop(); return

        if key == "alt+left":
            node = tree.cursor_node
            if node:
                try:
                    node.collapse()
                except Exception:
                    pass
            if ed: ed.focus()
            event.stop(); return

        if key == "alt+right":
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

        if key == "alt+enter":
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

        if key == "alt+insert":
            self._rename_selected()
            if ed: ed.focus()
            event.stop(); return

        if key == "alt+delete":
            self._delete_selected()
            if ed: ed.focus()
            event.stop(); event.prevent_default(); return


def main():
    start = sys.argv[1] if len(sys.argv) > 1 else "."
    SDS(start_dir=start).run()


if __name__ == "__main__":
    main()