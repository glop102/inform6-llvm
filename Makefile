# Inform 6 compiler, built from the sources under src/

CC      := clang
CFLAGS  := -O2 -g --std=c11 -DUNIX -Wall -Wno-unused-but-set-variable
SRCDIR  := src
BUILDDIR := build

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

all: inform6-llvm

inform6-llvm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/header.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) inform6-llvm

.PHONY: all clean
