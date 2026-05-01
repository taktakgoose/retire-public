.DEFAULT_GOAL := all

CC      = gcc

ifeq ($(OS),Windows_NT)
HOST_OS := windows
EXEEXT  := .exe
else
HOST_OS := unix
EXEEXT  :=
endif

# -ftls-model=global-dynamic : threadprivate years[] is too large for the
#   default local-exec TLS model on MinGW/Windows (R_X86_64_TPOFF32 overflow).
# -D_FORTIFY_SOURCE=0        : GCC 12+ auto-enables FORTIFY_SOURCE at -O2+,
#   replacing printf with __printf_chk which MinGW does not ship.
# -D__USE_MINGW_ANSI_STDIO=1 : route stdio through MinGW's own implementation
#   so stdout resolves correctly and printf formatting matches C99.
# EXTRA_CFLAGS is used by the dev/plan/hq profile targets below to inject
# -D overrides (N_TRIALS, RETURN_PATHS_PER_STRATEGY, etc.) without editing
# params.h.  The guarded constants in params.h use #ifndef so these -D flags
# win at compile time.
EXTRA_CFLAGS ?=
CFLAGS  = -O3 -march=native -fopenmp -Wall -Wextra -std=c11 \
          -ftls-model=global-dynamic \
          -D_FORTIFY_SOURCE=0 \
          -D__USE_MINGW_ANSI_STDIO=1 \
          $(EXTRA_CFLAGS)
LDFLAGS = -lm -fopenmp

TARGET  = retire$(EXEEXT)
SRCS    = retire.c tax.c tables.c output.c
OBJS    = $(SRCS:.c=.o)

ifeq ($(HOST_OS),windows)
# MinGW gcc on Windows/MSYS2 uses GetTempPath() which reads TMP/TEMP (not TMPDIR).
# Without these, it falls back to C:\Windows\ which is write-protected.
# PowerShell resolves LocalApplicationData via .NET regardless of domain prefix
# in whoami, and Replace converts backslashes to forward slashes for gcc/MSYS2.
_WINTMP := $(shell powershell -NoProfile -Command \
    "([Environment]::GetFolderPath('LocalApplicationData')).Replace('\','/')" \
    | tr -d '\r')/Temp
export TMP    := $(_WINTMP)
export TEMP   := $(_WINTMP)
export TMPDIR := $(_WINTMP)
endif

# Most-recent RRSP seed file (used by the 'refine' target).
LATEST_SEED := $(shell ls -t saved_rrsp_withdrawals_*.csv 2>/dev/null | head -1)

# Set AUTO_OPEN=0 to suppress automatically opening the xlsx/docx after a run.
#   make hq AUTO_OPEN=0
AUTO_OPEN ?= 1

# Opens the most-recently modified xlsx and docx files using the OS default
# application.  Uses os.startfile() on Windows, open on macOS, and xdg-open
# on Linux.  Silently skipped when AUTO_OPEN=0 or no files exist.
define open-outputs
	@if [ "$(AUTO_OPEN)" = "1" ]; then \
	    python3 -c " \
import glob, os, platform, shutil, subprocess; \
xl = sorted(glob.glob('retirement_*.xlsx'),        key=os.path.getmtime, reverse=True); \
dc = sorted(glob.glob('retirement_plan_*.docx'),   key=os.path.getmtime, reverse=True); \
files = xl[:1] + dc[:1]; \
system = platform.system(); \
opener = 'open' if system == 'Darwin' else 'xdg-open'; \
[os.startfile(f) for f in files] if system == 'Windows' else [subprocess.Popen([opener, f]) for f in files if shutil.which(opener)]; \
"; \
	fi
endef

.PHONY: all portable debug run xlsx docx save refine cca-sweep sor survivor-ralph survivor-sarah retire-ralph retire-sarah clean dev plan hq hq-sor hq-bucket hq-bucket-sor profile-run

# Age at which the named spouse dies in the survivor-* stress tests.
# Override on the command line: make survivor-ralph DEATH_AGE=75
DEATH_AGE ?= 80

# Age at which the named spouse retires in the retire-* targets.
# Override on the command line: make retire-ralph RETIRE_AGE=55
RETIRE_AGE ?= 50

# Portable / redistributable build for Windows: statically links libgomp,
# libgcc, and libwinpthread so the resulting binary has no MinGW DLL
# dependencies and can be copied to any 64-bit Windows machine.  Uses
# -march=x86-64 (baseline x86-64 without AVX/AVX2) so the binary runs on older
# hardware too.
ifeq ($(HOST_OS),windows)
portable: CFLAGS_PORTABLE = -O3 -march=x86-64 -fopenmp -Wall -Wextra -std=c11 \
                             -ftls-model=global-dynamic \
                             -D_FORTIFY_SOURCE=0 \
                             -D__USE_MINGW_ANSI_STDIO=1
portable: LDFLAGS_PORTABLE = -Wl,-Bstatic -lgomp -lpthread -Wl,-Bdynamic \
                              -static-libgcc -lm
portable:
	$(CC) $(CFLAGS_PORTABLE) -c -o retire.o   retire.c
	$(CC) $(CFLAGS_PORTABLE) -c -o tax.o      tax.c
	$(CC) $(CFLAGS_PORTABLE) -c -o tables.o   tables.c
	$(CC) $(CFLAGS_PORTABLE) -c -o output.o   output.c
	$(CC) $(CFLAGS_PORTABLE) -o $(TARGET) retire.o tax.o tables.o output.o $(LDFLAGS_PORTABLE)
	@echo "Built portable $(TARGET) — no MinGW DLLs required."
else
portable:
	@echo "The portable target is Windows-specific; building the standard release binary on this host instead."
	$(MAKE) all
endif

all: $(TARGET)

# Optimised release build (default)
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Debug build: no optimisation, full symbols
debug: CFLAGS = -g -O0 -fopenmp -Wall -Wextra -std=c11
debug: $(TARGET)

# Generic compile rule
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Explicit header dependencies (keeps incremental builds correct)
retire.o:  retire.c  params.h types.h tax.h tables.h output.h
tax.o:     tax.c     tax.h params.h
tables.o:  tables.c  tables.h
output.o:  output.c  output.h types.h tax.h params.h

# Build, simulate, then convert CSV → Excel + Word action plan and save RRSP seed file.
# All post-processing steps are skipped if the simulation produced no output.
run: $(TARGET)
	OMP_WAIT_POLICY=passive ./$(TARGET)
	@if [ -f retirement.csv ]; then \
	    python3 retirement_to_xlsx.py retirement.csv; \
	    python3 retirement_to_docx.py retirement.csv; \
	    python3 save_rrsp_withdrawals.py retirement.csv; \
	else \
	    echo "Skipping post-processing — retirement.csv was not produced."; \
	fi
	$(call open-outputs)

# ---- Profile targets ------------------------------------------------------
# Three preset iteration-count recipes that balance fidelity vs. runtime.
# Each target does a clean rebuild with -D overrides so the .o files pick up
# the new constants, then runs the simulation + post-processing pipeline.
#
#   make dev   — fast smoke test: 2k trials, 4 opt / 16 eval paths, fsy 2034-2036
#   make plan  — default planning run: 10k trials, 16/64 paths, fsy 2032-2038
#   make hq    — high-quality headline: 20k trials, 32/128 paths, fsy 2030-2040
#
# (None of the profiles run --cca by default; if you want the CCA ON vs OFF
#  comparison, use the dedicated `make cca-sweep` target.)
#
# Override individual knobs with EXTRA_CFLAGS, e.g.
#   make plan EXTRA_CFLAGS='-DN_TRIALS=50000'
# (the profile flags come first, so any EXTRA_CFLAGS on the command line wins
#  because later -D definitions override earlier ones in GCC.)

dev:  PROFILE_FLAGS = -DN_TRIALS=2000  -DRETURN_PATHS_PER_STRATEGY=4  -DEVALUATION_PATHS_PER_STRATEGY=16  -DMOONBROOK_SALE_YEAR_MIN=2034 -DMOONBROOK_SALE_YEAR_MAX=2036
dev:  PROFILE_ARGS  =
dev:  PROFILE_NAME  = dev
dev:  profile-run

plan: PROFILE_FLAGS = -DN_TRIALS=10000 -DRETURN_PATHS_PER_STRATEGY=16 -DEVALUATION_PATHS_PER_STRATEGY=64  -DMOONBROOK_SALE_YEAR_MIN=2032 -DMOONBROOK_SALE_YEAR_MAX=2038
plan: PROFILE_ARGS  =
plan: PROFILE_NAME  = plan
plan: profile-run

hq:   PROFILE_FLAGS = -DN_TRIALS=20000 -DRETURN_PATHS_PER_STRATEGY=32 -DEVALUATION_PATHS_PER_STRATEGY=128 -DMOONBROOK_SALE_YEAR_MIN=2032 -DMOONBROOK_SALE_YEAR_MAX=2038
hq:   PROFILE_ARGS  =
hq:   PROFILE_NAME  = hq
hq:   profile-run

# High-quality run with sequence-of-returns stress test.
# Same trial counts as `hq` but passes --sor so investment assets receive
# −10 % then −5 % in the two years after the first retirement (return-neutral:
# the deficit is spread back across the remaining years as a +0.58 %/yr boost).
hq-sor: PROFILE_FLAGS = -DN_TRIALS=40000 -DRETURN_PATHS_PER_STRATEGY=32 -DEVALUATION_PATHS_PER_STRATEGY=128 -DMOONBROOK_SALE_YEAR_MIN=2030 -DMOONBROOK_SALE_YEAR_MAX=2040
hq-sor: PROFILE_ARGS  = --sor
hq-sor: PROFILE_NAME  = hq-sor
hq-sor: profile-run

# High-quality run with RRIF cash-bucket strategy enabled.
# Same trial counts as `hq` but passes --bucket so each spouse's RRIF is
# split into a conservative cash bucket (drawn first for mandatory minimums)
# and an equity portion (normal volatility), insulating mandatory draws from
# sequence-of-returns shocks.
hq-bucket: PROFILE_FLAGS = -DN_TRIALS=20000 -DRETURN_PATHS_PER_STRATEGY=32 -DEVALUATION_PATHS_PER_STRATEGY=128 -DMOONBROOK_SALE_YEAR_MIN=2032 -DMOONBROOK_SALE_YEAR_MAX=2038
hq-bucket: PROFILE_ARGS  = --bucket
hq-bucket: PROFILE_NAME  = hq-bucket
hq-bucket: profile-run

# High-quality run with both RRIF cash-bucket and sequence-of-returns stress.
# Combines --bucket and --sor: mandatory RRIF draws are insulated by the cash
# bucket while the portfolio also absorbs the −10%/−5% SOR shock in the two
# years following the first retirement.  Uses 40k trials (same as hq-sor) to
# keep the success-rate estimate stable under the tighter constraints.
hq-bucket-sor: PROFILE_FLAGS = -DN_TRIALS=40000 -DRETURN_PATHS_PER_STRATEGY=32 -DEVALUATION_PATHS_PER_STRATEGY=128 -DMOONBROOK_SALE_YEAR_MIN=2030 -DMOONBROOK_SALE_YEAR_MAX=2040
hq-bucket-sor: PROFILE_ARGS  = --bucket --sor
hq-bucket-sor: PROFILE_NAME  = hq-bucket-sor
hq-bucket-sor: profile-run

# Shared action for the profile targets.  Uses recursive make so EXTRA_CFLAGS
# propagates into the compile rule with the right -D defines.  A clean is done
# first because .o files from a previous profile bake in the old constants.
profile-run:
	@echo "=== Profile: $(PROFILE_NAME) ==="
	@echo "=== Flags:   $(PROFILE_FLAGS) $(EXTRA_CFLAGS) ==="
	$(MAKE) clean
	$(MAKE) $(TARGET) EXTRA_CFLAGS='$(PROFILE_FLAGS) $(EXTRA_CFLAGS)'
	OMP_WAIT_POLICY=passive ./$(TARGET) $(PROFILE_ARGS)
	@if [ -f retirement.csv ]; then \
	    python3 retirement_to_xlsx.py retirement.csv; \
	    python3 retirement_to_docx.py retirement.csv; \
	    python3 save_rrsp_withdrawals.py retirement.csv; \
	else \
	    echo "Skipping post-processing — retirement.csv was not produced."; \
	fi
	$(call open-outputs)

# Convert an already-existing retirement.csv to Excel (skip re-running the simulation)
xlsx:
	python3 retirement_to_xlsx.py retirement.csv

# Generate the Word action plan from an already-existing retirement.csv
docx:
	python3 retirement_to_docx.py retirement.csv

# Run both CCA-disabled and CCA-enabled passes and print a comparison table.
# The output file reflects whichever strategy produced the highest net worth.
cca-sweep: $(TARGET)
	OMP_WAIT_POLICY=passive ./$(TARGET) --cca
	@if [ -f retirement.csv ]; then \
	    python3 retirement_to_xlsx.py retirement.csv; \
	    python3 retirement_to_docx.py retirement.csv; \
	    python3 save_rrsp_withdrawals.py retirement.csv; \
	else \
	    echo "Skipping post-processing — retirement.csv was not produced."; \
	fi
	$(call open-outputs)

# Run sequence-of-returns stress test (−10 % then −5 % on investment assets
# in the two years following the first spouse's retirement year).
sor: $(TARGET)
	OMP_WAIT_POLICY=passive ./$(TARGET) --sor
	@if [ -f retirement.csv ]; then \
	    python3 retirement_to_xlsx.py retirement.csv; \
	    python3 retirement_to_docx.py retirement.csv; \
	    python3 save_rrsp_withdrawals.py retirement.csv; \
	else \
	    echo "Skipping post-processing — retirement.csv was not produced."; \
	fi
	$(call open-outputs)

# Surviving-spouse stress tests.
# Ralph dies at DEATH_AGE (default 80); assets roll to Sarah, expenses → 70%.
survivor-ralph: $(TARGET)
	OMP_WAIT_POLICY=passive ./$(TARGET) --survivor ralph $(DEATH_AGE)
	@if [ -f retirement.csv ]; then \
	    python3 retirement_to_xlsx.py retirement.csv; \
	    python3 retirement_to_docx.py retirement.csv; \
	    python3 save_rrsp_withdrawals.py retirement.csv; \
	else \
	    echo "Skipping post-processing — retirement.csv was not produced."; \
	fi
	$(call open-outputs)

# Sarah dies at DEATH_AGE (default 80); assets roll to Ralph, expenses → 70%.
survivor-sarah: $(TARGET)
	OMP_WAIT_POLICY=passive ./$(TARGET) --survivor sarah $(DEATH_AGE)
	@if [ -f retirement.csv ]; then \
	    python3 retirement_to_xlsx.py retirement.csv; \
	    python3 retirement_to_docx.py retirement.csv; \
	    python3 save_rrsp_withdrawals.py retirement.csv; \
	else \
	    echo "Skipping post-processing — retirement.csv was not produced."; \
	fi
	$(call open-outputs)

# Retirement-age override targets.
# Ralph retires at RETIRE_AGE (default 50); Sarah stays at her default.
retire-ralph: $(TARGET)
	OMP_WAIT_POLICY=passive ./$(TARGET) --retire ralph $(RETIRE_AGE)
	@if [ -f retirement.csv ]; then \
	    python3 retirement_to_xlsx.py retirement.csv; \
	    python3 retirement_to_docx.py retirement.csv; \
	    python3 save_rrsp_withdrawals.py retirement.csv; \
	else \
	    echo "Skipping post-processing — retirement.csv was not produced."; \
	fi
	$(call open-outputs)

# Sarah retires at RETIRE_AGE (default 50); Ralph stays at his default.
retire-sarah: $(TARGET)
	OMP_WAIT_POLICY=passive ./$(TARGET) --retire sarah $(RETIRE_AGE)
	@if [ -f retirement.csv ]; then \
	    python3 retirement_to_xlsx.py retirement.csv; \
	    python3 retirement_to_docx.py retirement.csv; \
	    python3 save_rrsp_withdrawals.py retirement.csv; \
	else \
	    echo "Skipping post-processing — retirement.csv was not produced."; \
	fi
	$(call open-outputs)

# Save RRSP withdrawals from an already-existing retirement.csv (skip re-running)
save:
	python3 save_rrsp_withdrawals.py retirement.csv

# Run in seeded refinement mode using the most-recent saved_rrsp_withdrawals_*.csv.
# Produces a new retirement.csv / xlsx / docx / seed file if a better net worth is found.
refine: $(TARGET)
	@if [ -z "$(LATEST_SEED)" ]; then \
	    echo "No seed file found — run 'make run' first to generate one."; \
	    exit 1; \
	fi
	@echo "Refining from seed: $(LATEST_SEED)"
	OMP_WAIT_POLICY=passive ./$(TARGET) --rrsp $(LATEST_SEED)
	@if [ -f retirement.csv ]; then \
	    python3 retirement_to_xlsx.py retirement.csv; \
	    python3 retirement_to_docx.py retirement.csv; \
	    python3 save_rrsp_withdrawals.py retirement.csv; \
	else \
	    echo "Skipping post-processing — retirement.csv was not produced."; \
	fi

clean:
	rm -f $(TARGET) $(OBJS) retirement.csv retirement_*.xlsx retirement_plan_*.docx saved_rrsp_withdrawals_*.csv
n:
	rm -f $(TARGET) $(OBJS) retirement.csv retirement_*.xlsx retirement_plan_*.docx saved_rrsp_withdrawals_*.csv
