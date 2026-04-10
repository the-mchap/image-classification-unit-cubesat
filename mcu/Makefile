# --- Configuration ---
SHELL      := /bin/bash
export WINEPREFIX := $(HOME)/.ccs_compiler
export WINEDEBUG  := -all
WINE       := wine
PICC_DIR   := C:/Program Files (x86)/PICC
CC_EXE     := "$(PICC_DIR)/ccsc.exe"
PWD_WIN    := $(shell winepath -w . 2>/dev/null)

# --- Documentation Config ---
GIT_VERSION := $(shell git describe --always --dirty)
DOCS_DIR    := docs

# --- Project Files ---
TARGET     := main.hex
LINKER_SRC := project.c
OPTIONS    := ccsc_options.txt
BUILD_DIR  := build
BUILD_LOG  := $(BUILD_DIR)/build.log
DEPS       := $(shell find . -type f \( -name "*.c" -o -name "*.h" \) \
                ! -path "*/__history/*" ! -path "*/multiple_compilation_units/*")

# Compilation units (each compiled separately with +EXPORT)
UNITS := \
	src/logger.c                  \
	src/driver/uart.c             \
	src/driver/flash.c            \
	src/driver/setup.c            \
	src/protocol/conversions.c    \
	src/protocol/crc.c            \
	src/flash/image_transfer.c    \
	src/flash/quick_sorter.c      \
	src/flash/metadata.c          \
	src/flash/eraser.c            \
	src/flash/index_manager.c     \
	src/flash/space_finder.c      \
	src/app/task_helper.c         \
	src/app/obc_tasks.c           \
	src/app/rpi_tasks.c           \
	src/app/dbg_tasks.c           \
	src/app/parser.c              \
	main.c

# Include paths (Windows-style, semicolon-separated).
# Project root (PWD_WIN) listed first so #include "globals.h" resolves.
INCLUDE_PATHS := $(PWD_WIN);$(PWD_WIN)\local_devices;$(PWD_WIN)\local_drivers;$(PWD_WIN)\include\drivers;$(PWD_WIN)\include\flash;$(PWD_WIN)\include\protocol;$(PWD_WIN)\include\app;$(PWD_WIN)\include

# --- Rules ---

all: $(TARGET)

.PHONY: setup_links
setup_links:
	@ln -sf "$(HOME)/.ccs_compiler/drive_c/Program Files (x86)/PICC/Devices" local_devices
	@ln -sf "$(HOME)/.ccs_compiler/drive_c/Program Files (x86)/PICC/Drivers" local_drivers

$(TARGET): $(DEPS) setup_links
	@echo "Building PIC18 Project (Multiple Compilation Units)..."
	@mkdir -p $(BUILD_DIR)
	@echo "+FH" > $(OPTIONS)
	@echo "+STDOUT" >> $(OPTIONS)
	@echo "+Y9" >> $(OPTIONS)
	@echo "+EA" >> $(OPTIONS)
	@echo "+ES" >> $(OPTIONS)
	@echo "+EX" >> $(OPTIONS)
	@echo "+EXT=HEX" >> $(OPTIONS)
	@sed -i 's/$$/\r/' $(OPTIONS)
	@echo "=== Build metadata ===" > $(BUILD_LOG)
	@echo "Date: $$(date -Iseconds)" >> $(BUILD_LOG)
	@echo "Project dir: $$(pwd)" >> $(BUILD_LOG)
	@echo "Compiler: $(PICC_DIR)/ccsc.exe" >> $(BUILD_LOG)
	@echo "Units: $(words $(UNITS)) compilation units" >> $(BUILD_LOG)
	@echo "Include path: $(INCLUDE_PATHS)" >> $(BUILD_LOG)
	@echo "" >> $(BUILD_LOG)
	@echo "=== Compilation output ===" >> $(BUILD_LOG)
	@for src in $(UNITS); do \
		echo "  CC  $$src"; \
		$(WINE) $(CC_EXE) I="$(INCLUDE_PATHS)" +EXPORT \
			@$$(winepath -w $(OPTIONS)) \
			"$$(winepath -w $$src)" >> $(BUILD_LOG) 2>&1; \
		status=$$?; \
		if [ $$status -ne 0 ]; then \
			echo ""; \
			echo "COMPILE FAILED: $$src (exit $$status)"; \
			echo "COMPILE FAILED: $$src (exit $$status)" >> $(BUILD_LOG); \
			cat $(BUILD_LOG); \
			exit $$status; \
		fi; \
	done
	@find src -name "*.o" -exec cp {} $(BUILD_DIR)/ \;
	@find src -name "*.osym" -exec cp {} $(BUILD_DIR)/ \;
	@cp main.o main.osym $(BUILD_DIR)/ 2>/dev/null || true
	@cp $(LINKER_SRC) $(BUILD_DIR)/
	@cp $(OPTIONS) $(BUILD_DIR)/
	@echo "" >> $(BUILD_LOG)
	@echo "=== Linker output ===" >> $(BUILD_LOG)
	@echo "  LINK $(LINKER_SRC) -> $(TARGET)"
	@cd $(BUILD_DIR) && $(WINE) $(CC_EXE) I="$(INCLUDE_PATHS)" \
		@$$(winepath -w $(OPTIONS)) \
		"$$(winepath -w $(LINKER_SRC))" >> build.log 2>&1; \
	status=$$?; \
	echo "" >> build.log; \
	echo "Linker exit code: $$status" >> build.log; \
	cat build.log; \
	if [ $$status -eq 0 ] && [ -f project.hex ]; then \
		cp project.hex ../$(TARGET); \
	fi; \
	exit $$status
	@echo "Build Finished."

clean:
	@echo "Cleaning up the mess..."
	rm -rf $(BUILD_DIR)
	rm -f *.hex *.err *.esym *.xsym *.lst *.cod *.sym *.cof *.pjt *.ccspjt
	rm -f main.o main.osym
	rm -f $(OPTIONS)
	find src -type f \( -name "*.o" -o -name "*.err" -o -name "*.osym" \) -delete
	rm -rf $(DOCS_DIR)/**
	@echo "Done."

errors:
	@echo "=== Root error files ==="
	@for f in main.err; do [ -f "$$f" ] && echo "--- $$f ---" && cat "$$f"; done 2>/dev/null || true
	@echo ""
	@echo "=== Per-unit error files ==="
	@find src -name "*.err" -exec sh -c 'echo "--- {} ---" && cat "{}"' \; 2>/dev/null || true
	@echo ""
	@echo "=== Linker error files ==="
	@for f in $(BUILD_DIR)/*.err; do [ -f "$$f" ] && echo "--- $$f ---" && cat "$$f"; done 2>/dev/null || true

.PHONY: all clean errors
