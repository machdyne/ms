# Machdyne Scheme (LIX edition) -- Makefile
#
# Common targets:
#   make            build the desktop REPL binary (./ms)
#   make test       build and run the full test suite
#   make run        build and start the REPL
#   make small      build a size-optimized binary with a small static heap
#                   (useful for sanity-checking an MCU-style configuration
#                   on your desktop before cross-compiling)
#   make readline   build with GNU readline for arrow-key editing/history
#   make asan       build with AddressSanitizer + UBSan for debugging
#   make clean      remove build artifacts
#
# Embedded (LIX) builds are not linked here since they depend on your
# target's toolchain/headers -- see README.md#embedding-on-an-mcu-lix for
# the exact compiler invocation and required headers.

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -O2
LDLIBS   := -lm
BIN      := ms

# Override on the command line, e.g.:
#   make small MS_HEAP_SIZE=256 MS_PROTECT_STACK_SIZE=128
MS_HEAP_SIZE          ?=
MS_PROTECT_STACK_SIZE ?=

# Opt-in GNU readline support for the desktop REPL (arrow-key editing,
# command history). Off by default so the default build never requires
# readline to be installed, and so the embedded/LIX build is never at
# risk of picking it up. Enable with:
#   make USE_READLINE=1
#   make readline          (shorthand for the above)
USE_READLINE ?= 0

DEFS := -DMS_HOST
ifneq ($(MS_HEAP_SIZE),)
DEFS += -DMS_HEAP_SIZE=$(MS_HEAP_SIZE)
endif
ifneq ($(MS_PROTECT_STACK_SIZE),)
DEFS += -DMS_PROTECT_STACK_SIZE=$(MS_PROTECT_STACK_SIZE)
endif
ifeq ($(USE_READLINE),1)
DEFS += -DMS_USE_READLINE
LDLIBS += -lreadline
endif

.PHONY: all run test small readline asan clean

all: $(BIN)

$(BIN): ms.c
	$(CC) $(CFLAGS) $(DEFS) -o $(BIN) ms.c $(LDLIBS)

run: $(BIN)
	./$(BIN)

test: $(BIN)
	@./run_tests.sh

small:
	$(CC) $(CFLAGS) -Os -DMS_HOST -DMS_HEAP_SIZE=2000 -DMS_PROTECT_STACK_SIZE=256 \
		-o $(BIN) ms.c $(LDLIBS)
	@echo "built $(BIN) with a 2000-cell heap and a 256-slot protect stack"

readline:
	$(MAKE) USE_READLINE=1

asan:
	$(CC) -Wall -Wextra -g -fsanitize=address,undefined -DMS_HOST \
		-o $(BIN) ms.c $(LDLIBS)

clean:
	rm -f $(BIN)
