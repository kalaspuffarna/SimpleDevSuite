#!/bin/bash
# Build and install sds into ~/.local (the build itself is `make`, see Makefile).
#
# tree-sitter is optional. When the library is present sds uses it for syntax
# highlighting (grammars are loaded at runtime — see `sds --fetch-grammar`);
# otherwise it falls back to its own keyword lexer and everything still works.
#
# zlib is required: the PDF viewer needs it to inflate page streams.
#
# To see rendered PDF pages rather than extracted text you also need one of
# mupdf-tools, poppler or ghostscript, and a terminal that speaks the kitty
# graphics protocol (kitty, ghostty, WezTerm, iTerm2). Without them sds shows
# the extracted text and says what is missing.
set -e

# Work in the repo, not in whatever directory this was called from.
cd "$(dirname "$0")"

BIN="$HOME/.local/bin"
CBIN="$HOME/.local/c_bin"

build() {
    if pkg-config --exists tree-sitter 2>/dev/null; then
        echo "building with tree-sitter support"
    else
        echo "tree-sitter not found — building with the built-in lexer"
        echo "  (install it and re-run to enable tree-sitter highlighting)"
    fi
    make -j"$(nproc)" sds
}

build
mkdir -p "$BIN" "$CBIN"
mv sds "$CBIN/"

cat > "$BIN/sds" <<EOF
#!/bin/bash
exec $CBIN/sds "\$@"
EOF

cat > "$BIN/sds_update" <<'EOF'
#!/bin/bash
set -e
currentdir=$(pwd)
cd /tmp
rm -rf /tmp/SimpleDevSuite
git clone git@github.com:kalaspuffarna/SimpleDevSuite.git
cd SimpleDevSuite
make -j"$(nproc)" sds
mkdir -p "$HOME/.local/c_bin"
mv sds "$HOME/.local/c_bin/sds"
cd "$currentdir"
rm -rf /tmp/SimpleDevSuite
EOF

chmod +x "$BIN/sds" "$BIN/sds_update"
echo "installed $CBIN/sds  (launcher: $BIN/sds)"
