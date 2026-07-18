# Inform 6 compiler, built from the sources under src/
# with LLVM-based code generation

CC      := clang
CFLAGS  := -O2 -g --std=c11 -DUNIX -Wall -Wno-unused-but-set-variable
SRCDIR  := src
BUILDDIR := build

# LLVM is optional. llvm-config (the tool LLVM ships to report its install's
# include/link flags) doubles as the detector: if it's found, build the real
# pipeline; otherwise build the stub, which compiles everything classically.
# Override with WITH_LLVM=0/1 and LLVM_CONFIG=/path/to/llvm-config.
LLVM_CONFIG ?= llvm-config
WITH_LLVM ?= $(shell command -v $(LLVM_CONFIG) >/dev/null 2>&1 && echo 1 || echo 0)

ifeq ($(WITH_LLVM),1)
LLVM_CFLAGS := $(shell $(LLVM_CONFIG) --cflags)
LLVM_LIBS   := $(shell $(LLVM_CONFIG) --ldflags --libs core analysis passes --system-libs)
EXCLUDED    := $(SRCDIR)/llvm_stub.c
else
LLVM_CFLAGS :=
LLVM_LIBS   :=
EXCLUDED    := $(SRCDIR)/llvm_codegen.c $(SRCDIR)/llvm_lower.c
$(info LLVM not found ($(LLVM_CONFIG)); building without the LLVM pipeline)
endif

SRCS := $(filter-out $(EXCLUDED),$(wildcard $(SRCDIR)/*.c))
OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

all: inform6-llvm

inform6-llvm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LLVM_LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/header.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Only the real LLVM modules need the LLVM headers.
$(BUILDDIR)/llvm_codegen.o $(BUILDDIR)/llvm_lower.o: \
    $(BUILDDIR)/llvm_%.o: $(SRCDIR)/llvm_%.c $(SRCDIR)/header.h $(SRCDIR)/llvm_codegen.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

test:
	nix run .#tests

bench:
	nix run .#benchmarks

clean-tests:
	rm -f tests/*.ulx tests/*.z5 tests/*.log tests/inform6-llvm-dump.ll tests/life.opcodes.tsv

clean: clean-tests
	rm -rf $(BUILDDIR) inform6-llvm

.PHONY: all test bench clean clean-tests
