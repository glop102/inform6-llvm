# Inform 6 compiler, built from the sources under src/
# with LLVM-based code generation

CC      := clang
CFLAGS  := -O2 -g --std=c11 -DUNIX -Wall -Wno-unused-but-set-variable
SRCDIR  := src
BUILDDIR := build

LLVM_CFLAGS := $(shell llvm-config --cflags)
LLVM_LIBS   := $(shell llvm-config --ldflags --libs core analysis --system-libs)

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

all: inform6-llvm

inform6-llvm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LLVM_LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/header.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Only the LLVM codegen module needs the LLVM headers.
$(BUILDDIR)/llvm_codegen.o: $(SRCDIR)/llvm_codegen.c $(SRCDIR)/header.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) inform6-llvm

.PHONY: all clean
