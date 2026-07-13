# Inform 6 compiler, built from the sources under src/
# with LLVM-based code generation

CC      := clang
CFLAGS  := -O2 -g --std=c11 -DUNIX -Wall -Wno-unused-but-set-variable
SRCDIR  := src
BUILDDIR := build

LLVM_CFLAGS := $(shell llvm-config --cflags)
LLVM_LIBS   := $(shell llvm-config --ldflags --libs core analysis passes --system-libs)

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

all: inform6-llvm

inform6-llvm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LLVM_LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/header.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Only the LLVM modules need the LLVM headers.
$(BUILDDIR)/llvm_%.o: $(SRCDIR)/llvm_%.c $(SRCDIR)/header.h $(SRCDIR)/llvm_codegen.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

test: inform6-llvm
	tests/run-m1.sh
	tests/run-m3.sh

clean-tests:
	rm -f tests/*.ulx tests/*.z5 tests/*.log tests/inform6-llvm-dump.ll

clean: clean-tests
	rm -rf $(BUILDDIR) inform6-llvm

.PHONY: all test clean clean-tests
