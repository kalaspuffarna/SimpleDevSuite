#!/bin/bash
# Build and install sds into ~/.local.
#
# tree-sitter is optional. When the library is present sds uses it for syntax
# highlighting (grammars are loaded at runtime — see `sds --fetch-grammar`);
# otherwise it falls back to its own keyword lexer and everything still works.
set -e

BIN="$HOME/.local/bin"
CBIN="$HOME/.local/c_bin"

build() {
    if pkg-config --exists tree-sitter 2>/dev/null; then
        echo "building with tree-sitter support"
        cc -O2 -Wall -DSDS_TREESITTER -o sds sds.c \
            $(pkg-config --cflags tree-sitter) -lncursesw -lutil \
            $(pkg-config --libs tree-sitter) -ldl
    else
        echo "tree-sitter not found — building with the built-in lexer"
        echo "  (install it and re-run to enable tree-sitter highlighting)"
        cc -O2 -Wall -o sds sds.c -lncursesw -lutil
    fi
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
if pkg-config --exists tree-sitter 2>/dev/null; then
    cc -O2 -Wall -DSDS_TREESITTER -o sds sds.c \
        $(pkg-config --cflags tree-sitter) -lncursesw -lutil \
        $(pkg-config --libs tree-sitter) -ldl
else
    cc -O2 -Wall -o sds sds.c -lncursesw -lutil
fi
mkdir -p "$HOME/.local/c_bin"
mv sds "$HOME/.local/c_bin/sds"
cd "$currentdir"
rm -rf /tmp/SimpleDevSuite
EOF

chmod +x "$BIN/sds" "$BIN/sds_update"
echo "installed $CBIN/sds  (launcher: $BIN/sds)"
