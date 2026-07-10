# tea — tiny econometric assistant
# Build configuration with strict warnings and per-platform paths.
#
# Targets:
#   make            -> release build (-O2, all warnings as errors)
#   make debug      -> ASan + UBSan build at /tmp/tea-debug for runtime checks
#   make release    -> -O3 -DNDEBUG (no debug symbols)
#   make test       -> build + run regression suite (tests/regression/*.do)
#   make smoke      -> build + run tests/demo.do as a quick smoke test
#   make check-deps -> verify all external dependency headers are reachable
#   make showpaths  -> print discovered library paths (for debugging build env)
#   make clean      -> remove all build artefacts

CC       ?= cc
UNAME_S  := $(shell uname -s)

# ---- warnings ------------------------------------------------------------
# Strict-but-practical. Every warning is a build failure; suppressions are
# documented individually.
#
# Justified suppressions:
#   -Wno-misleading-indentation : dense single-line style trips the heuristic;
#       verified no actual control-flow bugs.
#   -Wno-format-truncation      : all snprintf into name[33]/format[33] are
#       intentional bounded copies (Stata identifiers are <= 32 chars).
#   -Wno-unused-parameter       : many command handlers receive a Cmd* they
#       don't fully use; consistent signature matters more than per-handler
#       cleanup.
#   -Wno-unused-result          : a handful of fread/system/etc calls where
#       the rc is genuinely ignorable; auditing these every time adds noise
#       without value.
#
# Not enabled by default (would surface real issues but produce a lot of
# noise on the existing codebase; consider for a future cleanup pass):
#   -Wconversion -Wsign-conversion -Wpedantic -Wcast-align
WARNINGS = -Wall -Wextra -Werror \
           -Wshadow \
           -Wmissing-prototypes \
           -Wstrict-prototypes \
           -Wold-style-definition \
           -Wundef \
           -Wpointer-arith \
           -Wwrite-strings \
           -Wno-misleading-indentation \
           -Wno-format-truncation \
           -Wno-unused-parameter \
           -Wno-unused-result

# ---- hardening (runtime safety) ------------------------------------------
# These add small runtime checks that catch buffer overflows and stack
# corruption early.  Cheap; should be on for every build.
#
# _FORTIFY_SOURCE requires optimisation, so we only apply it to release.
# Stack protector is independent of optimisation level.
HARDEN_BASE    = -fstack-protector-strong
HARDEN_RELEASE = -D_FORTIFY_SOURCE=2

BASE_CFLAGS = -std=c17 -O2 $(WARNINGS) $(HARDEN_BASE) $(HARDEN_RELEASE)

# -MMD -MP: header-dependency tracking so editing a header rebuilds every
#   TU that includes it (avoids stale-object ABI mismatches).
DEPFLAGS = -MMD -MP

# ---- per-platform include and library paths ------------------------------
ifeq ($(UNAME_S),Darwin)
  BREW := $(shell command -v brew 2>/dev/null)
  ifneq ($(BREW),)
    READLINE_PREFIX ?= $(shell brew --prefix readline 2>/dev/null)
    OPENBLAS_PREFIX ?= $(shell brew --prefix openblas 2>/dev/null)
    LAPACK_PREFIX   ?= $(shell brew --prefix lapack   2>/dev/null)
    GSL_PREFIX      ?= $(shell brew --prefix gsl      2>/dev/null)
    READSTAT_PREFIX ?= $(shell brew --prefix readstat 2>/dev/null)
  else
    READLINE_PREFIX ?= /opt/homebrew/opt/readline
    OPENBLAS_PREFIX ?= /opt/homebrew/opt/openblas
    LAPACK_PREFIX   ?= /opt/homebrew/opt/lapack
    GSL_PREFIX      ?= /opt/homebrew/opt/gsl
    READSTAT_PREFIX ?= /opt/homebrew/opt/readstat
  endif
  PLATFORM_CFLAGS  = -I$(READLINE_PREFIX)/include \
                     -I$(OPENBLAS_PREFIX)/include \
                     -I$(LAPACK_PREFIX)/include \
                     -I$(GSL_PREFIX)/include \
                     -I$(READSTAT_PREFIX)/include
  PLATFORM_LDFLAGS = -L$(READLINE_PREFIX)/lib \
                     -L$(OPENBLAS_PREFIX)/lib \
                     -L$(LAPACK_PREFIX)/lib \
                     -L$(GSL_PREFIX)/lib \
                     -L$(READSTAT_PREFIX)/lib
else
  PLATFORM_CFLAGS  =
  PLATFORM_LDFLAGS =
endif

CFLAGS  = $(BASE_CFLAGS) $(PLATFORM_CFLAGS)
LDFLAGS = $(PLATFORM_LDFLAGS) -llapacke -lopenblas -lgsl -lreadline -lreadstat -lm

SRC     = $(wildcard src/*.c)
OBJ     = $(SRC:.c=.o)
DEP     = $(OBJ:.o=.d)
BIN     = tea

$(BIN): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

-include $(DEP)

# ---- debug target: ASan + UBSan, no optimisation -------------------------
# Builds a separate binary at tea-debug so the release tea/ stays unaffected.
# Use this for any reproducer that might hint at memory corruption.
DEBUG_CFLAGS  = -std=c17 -O0 -g3 -fno-omit-frame-pointer \
                $(WARNINGS) $(HARDEN_BASE) \
                -fsanitize=address,undefined \
                $(PLATFORM_CFLAGS)
DEBUG_LDFLAGS = -fsanitize=address,undefined $(LDFLAGS)

debug:
	$(CC) $(DEBUG_CFLAGS) $(SRC) -o tea-debug $(DEBUG_LDFLAGS)
	@echo "built ./tea-debug — run with the same args as tea"

# ---- release target: -O3, _FORTIFY_SOURCE on, no debug symbols ----------
release: clean
	$(MAKE) BASE_CFLAGS="-std=c17 -O3 -DNDEBUG $(WARNINGS) $(HARDEN_BASE) $(HARDEN_RELEASE)" $(BIN)

clean:
	rm -f src/*.o src/*.d src/*.gch $(BIN) tea-debug

test: $(BIN)
	@./tests/regression/run_tests.sh

# Quick sanity check: build and run the original demo do-file.
smoke: $(BIN)
	./$(BIN) tests/demo.do

# check-deps: verifies that each external dependency's header is reachable
# with the current CFLAGS.  Catches the common "I forgot to apt install
# libfoo-dev" failure mode with a clear diagnosis instead of a wall of
# preprocessor errors.
check-deps:
	@echo "checking external dependencies..."
	@for hdr in readline/readline.h cblas.h lapacke.h gsl/gsl_cdf.h readstat.h; do \
	    printf "  %-24s " "$$hdr"; \
	    if echo "#include <$$hdr>" | $(CC) $(CFLAGS) -E -x c - >/dev/null 2>&1; then \
	        echo "OK"; \
	    else \
	        echo "MISSING"; \
	        missing=1; \
	    fi; \
	done; \
	if [ "$$missing" = "1" ]; then \
	    echo ""; \
	    echo "Some headers were not found.  Install the dev packages:"; \
	    echo "  Debian/Ubuntu: apt install libreadline-dev libopenblas-dev \\"; \
	    echo "                            liblapacke-dev libgsl-dev libreadstat-dev"; \
	    echo "  macOS (brew):  brew install readline openblas lapack gsl readstat"; \
	    exit 1; \
	fi

# convenience: print discovered prefixes for debugging build env
showpaths:
	@echo "UNAME_S          = $(UNAME_S)"
	@echo "READLINE_PREFIX  = $(READLINE_PREFIX)"
	@echo "OPENBLAS_PREFIX  = $(OPENBLAS_PREFIX)"
	@echo "LAPACK_PREFIX    = $(LAPACK_PREFIX)"
	@echo "GSL_PREFIX       = $(GSL_PREFIX)"
	@echo "READSTAT_PREFIX  = $(READSTAT_PREFIX)"
	@echo "CFLAGS           = $(CFLAGS)"
	@echo "LDFLAGS          = $(LDFLAGS)"

.PHONY: clean test smoke check-deps showpaths debug release

# ---- WebAssembly build (browser demo) -------------------------------------
# Requires emcc and the prebuilt WASM static libs (reference CLAPACK stack,
# GSL, readstat).  Point WASM_LIBS at the directory holding:
#   liblapacke.a liblapack.a libcblas.a libblas.a libf2c.a libgsl.a libreadstat.a
WASM_LIBS ?= /home/claude/wasm-libs
WASM_INC  ?= -Iwasm/include \
             -I/home/claude/gsl/build-wasm \
             -I/home/claude/readstat/src
WASM_SRC  = $(filter-out src/main.c,$(SRC))

wasm: web/tea.js

web/tea.js: $(WASM_SRC)
	@mkdir -p web
	emcc -std=c17 -O2 $(WASM_INC) $(WASM_SRC) \
	  $(WASM_LIBS)/liblapack.a $(WASM_LIBS)/libblas.a $(WASM_LIBS)/libf2c.a \
	  $(WASM_LIBS)/libgsl.a $(WASM_LIBS)/libreadstat.a \
	  -sEXPORTED_FUNCTIONS=_tea_web_init,_tea_web_exec,_tea_web_version,_tea_web_run_dofile,_malloc,_free \
	  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,FS,NODEFS \
	  -sALLOW_MEMORY_GROWTH=1 -sMODULARIZE=1 -sEXPORT_NAME=createTea \
	  -sNO_EXIT_RUNTIME=1 -sFORCE_FILESYSTEM=1 -lnodefs.js \
	  -o web/tea.js

wasm-clean:
	rm -rf web/tea.js web/tea.wasm
