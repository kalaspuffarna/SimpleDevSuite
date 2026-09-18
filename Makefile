# sds — SimpleDevSuite
#
#   make                 build ./sds (tree-sitter support if the library is found)
#   make TREESITTER=0    build without tree-sitter even if it is installed
#   make clean
#
# Sources are picked up from src/ automatically, so adding a feature is just a
# matter of dropping new .c files into a directory there.

CC      ?= cc
CFLAGS  ?= -O2 -Wall
LDLIBS  := -lncursesw -lutil -lz

TREESITTER ?= $(shell pkg-config --exists tree-sitter 2>/dev/null && echo 1 || echo 0)
ifeq ($(TREESITTER),1)
CPPFLAGS += -DSDS_TREESITTER $(shell pkg-config --cflags tree-sitter)
LDLIBS   += $(shell pkg-config --libs tree-sitter) -ldl
endif

BUILD := build/ts$(TREESITTER)
SRCS  := $(shell find src -name '*.c' | sort)
OBJS  := $(SRCS:src/%.c=$(BUILD)/%.o)

# ./sds is shared by both variants, so relink it whenever the variant or the
# flags change, not only when an object is newer than it.
CONFIG := build/config
CONFIG_NOW := $(TREESITTER) $(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)
$(shell mkdir -p build; [ "$$(cat $(CONFIG) 2>/dev/null)" = '$(CONFIG_NOW)' ] || echo '$(CONFIG_NOW)' > $(CONFIG))

sds: $(OBJS) $(CONFIG)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -rf build sds

.PHONY: clean
-include $(OBJS:.o=.d)
