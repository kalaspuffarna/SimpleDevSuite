#!/usr/bin/env python3
"""SDS - SimpleDevSuite"""

import sys
from pathlib import Path
from typing import Optional

from textual.app import App, ComposeResult
from textual.widgets import Static, Tree, TextArea, Button, Label
from textual.containers import Horizontal, Vertical
from textual import events
from textual.binding import Binding
from textual.widgets.tree import TreeNode
from textual.screen import ModalScreen
from rich.markup import escape

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
    height: 9;
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

#status-bar {{
    height: 1;
    background: {DGREY};
    color: {GREY};
    padding: 0 1;
}}
"""


class SDSEditor(TextArea):
    pass


# ── Confirm dialog ────────────────────────────────────────────────────────────
class ConfirmScreen(ModalScreen):
    def __init__(self, message: str):
        super().__init__()
        self._message = message

    def compose(self) -> ComposeResult:
        with Vertical(id="confirm-box"):
            yield Label(self._message, id="confirm-label")
            with Horizontal(id="confirm-buttons"):
                yield Button("Yes", id="yes", variant="primary")
                yield Button("No",  id="no")

    def on_mount(self) -> None:
        self.query_one("#yes").focus()

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
        f    = self._files[i]
        name = Path(f).name
        star = " *" if i in self._modified else ""
        icon = EXT_ICON.get(Path(f).suffix.lower(), I["file"])
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
                "Alt+Shift+Left/Right: tabs  Ctrl+W: close  Ctrl+S: save  Ctrl+Q: quit"
            )
            return
        name = Path(filepath).name
        mod  = "  [unsaved]" if modified else ""
        icon = EXT_ICON.get(Path(filepath).suffix.lower(), I["file"])
        self.update(
            f"{icon} {name}{mod}  |  "
            f"Ln {row+1} Col {col+1}  |  "
            f"{lang or 'text'}  |  "
            f"Ctrl+S: save  Ctrl+W: close  Ctrl+Q: quit"
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
                    f"  Alt+Shift+Left/Right  switch tabs\n"
                    f"  Ctrl+W             close tab\n"
                    f"  Ctrl+S             save\n"
                    f"  Ctrl+Q             quit\n",
                    id="welcome",
                )
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

    def _open_file(self, filepath: str) -> None:
        panel = self.query_one("#editor-panel")
        try:
            self.query_one("#welcome").remove()
        except Exception:
            pass

        for ed in self.editors.values():
            ed.display = False

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
        self.editors[filepath].focus()

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

    def _do_close(self) -> None:
        idx  = self.active_tab
        path = self.open_files.pop(idx)
        self.modified = {
            (i if i < idx else i - 1)
            for i in self.modified if i != idx
        }
        if path in self.editors:
            self.editors.pop(path).remove()

        if self.open_files:
            self.active_tab = min(idx, len(self.open_files) - 1)
            fp = self.open_files[self.active_tab]
            for ed in self.editors.values():
                ed.display = False
            self.editors[fp].display = True
            self.editors[fp].focus()
        else:
            self.active_tab = -1
            self.query_one(Tree).focus()

        self._update_tab_bar()
        self._refresh_status()

    def on_key(self, event: events.Key) -> None:
        key = event.key

        if key == "ctrl+s":
            self._save_current(); event.stop(); return

        if key == "ctrl+w":
            self._close_tab(); event.stop(); return

        if key in ("alt+shift+left", "alt+shift+right"):
            if self.open_files:
                delta = -1 if key == "alt+shift+left" else 1
                self.active_tab = (self.active_tab + delta) % len(self.open_files)
                self._open_file(self.open_files[self.active_tab])
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


def main():
    start = sys.argv[1] if len(sys.argv) > 1 else "."
    SDS(start_dir=start).run()


if __name__ == "__main__":
    main()