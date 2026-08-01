VER := $(strip $(shell cat VERSION))
.DEFAULT_GOAL := tea
# src/tea_version.h is generated from VERSION with a content compare, so
# its mtime moves only when the version actually changes.  The three
# translation units that print the version include it (before interp.h,
# whose fallback yields); -MMD then makes VERSION-bump rebuilds exact:
# those units and the links, nothing else.  This replaced a -D flag that
# make could not track — `echo new > VERSION; make` used to rebuild
# nothing and ship a binary reporting the old version.
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

# ---- vendored ReadStat (no Homebrew formula exists for it) ----------------
# `make deps-readstat` clones and builds WizardMac/ReadStat into vendor/;
# when present it wins over any system/brew resolution on every platform.
ifneq ($(wildcard vendor/readstat/include/readstat.h),)
  READSTAT_PREFIX := $(abspath vendor/readstat)
  VENDOR_RS_CFLAGS := -I$(READSTAT_PREFIX)/include
  VENDOR_RS_LD     := -L$(READSTAT_PREFIX)/lib
endif

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
# -Wno-format-truncation is GCC-only; upstream clang under -Werror
# rejects unknown warning options (Apple clang merely tolerates them).
CC_IS_CLANG := $(findstring clang,$(shell $(CC) --version 2>/dev/null))
ifeq ($(CC_IS_CLANG),)
  WNO_FMT_TRUNC := -Wno-format-truncation
endif
WARNINGS = -Wall -Wextra -Werror \
           -Wshadow \
           -Wmissing-prototypes \
           -Wstrict-prototypes \
           -Wold-style-definition \
           -Wundef \
           -Wpointer-arith \
           -Wwrite-strings \
           -Wno-misleading-indentation \
           $(WNO_FMT_TRUNC) \
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
  # Verify the HEADERS exist, not just that a prefix string came back:
  # `brew --prefix FORMULA` prints the would-be opt path even for a
  # formula that is not installed (exit 0), and comes back empty in
  # other environments — either way the user's first error was a
  # mystifying "readstat.h not found" behind a garbage -I path.
  # Skipped for clean/dist-clean so housekeeping never needs the deps.
  # exempt goals that must work precisely WHEN deps are missing:
  # deps-readstat is the cure and check-deps is the diagnosis — neither
  # can be allowed to die of the disease at parse time
  ifeq ($(filter clean distclean showpaths deps-readstat check-deps,$(MAKECMDGOALS)),)
    ifeq ($(wildcard $(READLINE_PREFIX)/include/readline/readline.h),)
      $(error readline headers not found under '$(READLINE_PREFIX)' — brew install readline (or: make READLINE_PREFIX=/path))
    endif
    ifeq ($(wildcard $(OPENBLAS_PREFIX)/include/cblas.h),)
      $(error openblas headers not found under '$(OPENBLAS_PREFIX)' — brew install openblas (or: make OPENBLAS_PREFIX=/path))
    endif
    ifeq ($(wildcard $(LAPACK_PREFIX)/include/lapacke.h),)
      $(error lapack headers not found under '$(LAPACK_PREFIX)' — brew install lapack (or: make LAPACK_PREFIX=/path))
    endif
    ifeq ($(wildcard $(GSL_PREFIX)/include/gsl/gsl_cdf.h),)
      $(error gsl headers not found under '$(GSL_PREFIX)' — brew install gsl (or: make GSL_PREFIX=/path))
    endif
    ifeq ($(wildcard $(READSTAT_PREFIX)/include/readstat.h),)
      $(error readstat headers not found under '$(READSTAT_PREFIX)'. Homebrew has no readstat formula — run `make deps-readstat` once (clones and builds it into vendor/, auto-detected afterwards), or point at an existing build with `make READSTAT_PREFIX=/path`)
    endif
  endif
  READSTAT_CONF_EXTRA := LIBS=-liconv
  PLATFORM_EXTRA_LIBS := -liconv -lz
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

CFLAGS  = $(BASE_CFLAGS) $(PLATFORM_CFLAGS) $(VENDOR_RS_CFLAGS)
LDFLAGS = $(VENDOR_RS_LD) $(PLATFORM_LDFLAGS) -llapacke -lopenblas -lgsl -lreadline -lreadstat -lz $(PLATFORM_EXTRA_LIBS) -lm

SRC     = $(wildcard src/*.c)
OBJ     = $(SRC:.c=.o)
DEP     = $(OBJ:.o=.d)

src/tea_version.h: VERSION
	@printf '#define TEA_VERSION_FROM_FILE "%s"\n' "$(VER)" > $@.tmp
	@cmp -s $@.tmp $@ 2>/dev/null && rm -f $@.tmp || mv $@.tmp $@

$(OBJ): | src/tea_version.h
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
	    echo "  macOS (brew):  brew install readline openblas lapack gsl && make deps-readstat"; \
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

.PHONY: clean test smoke check-deps showpaths debug release manual docs-pdf quickstart sync-web-version dist

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

# per-object compilation with emcc's own -MMD dep tracking, so the wasm
# build is as incremental as the native one and a VERSION bump rebuilds
# exactly the units that include tea_version.h.  This retires the
# wasm-clean-before-wasm crutch (the old monolithic rule neither tracked
# VERSION nor cached objects: it under-compiled on version bumps and
# over-compiled on everything else).
WASM_OBJDIR = web/obj
WASM_OBJ = $(patsubst src/%.c,$(WASM_OBJDIR)/%.o,$(WASM_SRC))
WASM_DEP = $(WASM_OBJ:.o=.d)

$(WASM_OBJDIR)/%.o: src/%.c | src/tea_version.h
	@mkdir -p $(WASM_OBJDIR)
	emcc -std=c17 -O2 $(WASM_INC) -MMD -MP -c $< -o $@

-include $(WASM_DEP)

web/tea.js: $(WASM_OBJ)
	@mkdir -p web
	emcc -O2 $(WASM_OBJ) \
	  $(WASM_LIBS)/liblapack.a $(WASM_LIBS)/libblas.a $(WASM_LIBS)/libf2c.a \
	  $(WASM_LIBS)/libgsl.a $(WASM_LIBS)/libreadstat.a $(WASM_LIBS)/libz.a \
	  -sEXPORTED_FUNCTIONS=_tea_web_init,_tea_web_exec,_tea_web_version,_tea_web_save_memory,_tea_web_data_hash,_tea_web_run_dofile,_tea_web_complete,_malloc,_free \
	  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,FS,NODEFS \
	  -sALLOW_MEMORY_GROWTH=1 -sMODULARIZE=1 -sEXPORT_NAME=createTea \
	  -sNO_EXIT_RUNTIME=1 -sFORCE_FILESYSTEM=1 -lnodefs.js \
	  -o web/tea.js

# build ReadStat into vendor/readstat (one-time; ~a minute).  Needs
# autotools: brew install autoconf automake libtool  (macOS) or
# apt install autoconf automake libtool (Linux).
deps-readstat: vendor/readstat/include/readstat.h
vendor/readstat/include/readstat.h:
	@command -v autoreconf >/dev/null || { echo "autotools required: brew install autoconf automake libtool"; exit 1; }
	rm -rf vendor/ReadStat-src
	git clone --depth 1 https://github.com/WizardMac/ReadStat vendor/ReadStat-src
	cd vendor/ReadStat-src && ./autogen.sh && \
	  ./configure --prefix=$(abspath vendor/readstat) --enable-shared=no $(READSTAT_CONF_EXTRA) && \
	  $(MAKE) && $(MAKE) install
	@echo "== ReadStat vendored under vendor/readstat — plain 'make' will now find it"

wasm-clean:
	rm -rf web/tea.js web/tea.wasm
	rm -rf $(WASM_OBJDIR)

# ---- manual ----------------------------------------------------------------
# Master source: manual/manual.md (guide) + manual/reference.md (generated
# from the binary by tools/gen_cmdref.sh).  Produces the single-file
# markdown and the PDF.  Requires pandoc + texlive.
manual: $(BIN)
	./tools/gen_cmdref.sh
	printf '\\newcommand{\\teaversion}{%s}\n' "$(VER)" > manual/version.tex
	cat manual/manual.md manual/reference.md > tea-manual.md
	pandoc manual/manual.md manual/reference.md -o tea-manual.pdf \
	  --pdf-engine=pdflatex -H manual/preamble.tex -B manual/titlepage.tex \
	  --toc --toc-depth=2 -V colorlinks=true
	@echo "built tea-manual.md and tea-manual.pdf (version $(VER))"

# run both wasm gates: the regression suite through tea.js, then the
# page-script smoke (index.html's inline JS in a stubbed browser) —
# Bug 41 shipped through a green 77/77 gate precisely because the page
# script had no coverage; it now does
wasm-test:
	cd web && node run_wasm_tests.cjs && node run_page_smoke.cjs

# stamp the VERSION into the browser splash (marker-based, idempotent)
sync-web-version:
	sed -E -i 's|(<!--TEAVER-->)[^<]*(<!--/TEAVER-->)|\1$(VER)\2|' web/index.html
	@echo "web splash stamped with $(VER)"

# release tarball: test-gated, named from VERSION, invariant tea/ top level
dist: test
	@# artifacts are excluded from the tarball rather than deleted, so
	@# dist no longer destroys the build tree (the old rm forced a full
	@# recompile after every release)
	tar czf /tmp/tea-v$(VER).tar.gz --exclude='.git' --exclude='tea/vendor' --exclude='*.o' \
	  --exclude='*.d' --exclude='*.o' --exclude='tea/tea' \
	  --exclude='tea/tea-debug' --exclude='tea/web/obj' \
	  --exclude='tea/src/tea_version.h' --exclude='tea/*.tar.gz' -C .. tea
	mv /tmp/tea-v$(VER).tar.gz .
	@echo "dist: tea-v$(VER).tar.gz"

# PDF renditions of the companion documents (same styling as the manual).
docs-pdf:
	for f in README COMPATIBILITY KNOWN_BUGS WASM_NOTES; do \
	  pandoc $$f.md -o $$f.pdf --pdf-engine=xelatex \
	    -V mainfont="DejaVu Serif" -V monofont="DejaVu Sans Mono" \
	    -H manual/docheader.tex --toc -V colorlinks=true || exit 1; \
	done
	pandoc data/SOURCES.md -o data/SOURCES.pdf --pdf-engine=xelatex \
	  -V mainfont="DejaVu Serif" -V monofont="DejaVu Sans Mono" \
	  -H manual/docheader.tex -V colorlinks=true
	@echo "built README.pdf COMPATIBILITY.pdf KNOWN_BUGS.pdf WASM_NOTES.pdf data/SOURCES.pdf"

# The Stata-user quickstart ("hook" document): 5-page pitch, real output.
quickstart:
	pandoc STATA-QUICKSTART.md -o STATA-QUICKSTART.pdf --pdf-engine=xelatex \
	  -V mainfont="DejaVu Serif" -V monofont="DejaVu Sans Mono" -V fontsize=11pt \
	  -H manual/qsheader.tex -V colorlinks=true
	@echo "built STATA-QUICKSTART.pdf"
