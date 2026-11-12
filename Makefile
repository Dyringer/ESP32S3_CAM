#------------------------------------------------------------------------------
# Author:       Daniel Dyringer
#
# Brief:        Automation of common tasks
#------------------------------------------------------------------------------

build_dir           ?=
cmake_flags         ?=
debug               ?= y
ninja               ?= y
parallel            ?= $(shell nproc)
target_chip         ?= esp32s3

# Board directory name under boards/
board               ?= esp32s3_n16r8_dk
verbose             ?= n

# Print ESP-IDF memory usage summary after building the firmware
size                ?= y

# Project name
project_name        = esp32s3_cam

#  all, style-format, style-check, test, size-analysis
target              ?=

#------------------------------------------------------------------------------

AT := @

mkfile_dir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# ESP-IDF environment (idempotent). The container has no USB devices, so this
# Makefile only builds; flash/monitor from the host using _deploy/*/bin.
SHELL   := /bin/bash

# Same as idf.py: ninja strips colour from piped tool output unless CLICOLOR_FORCE
# is set, rich (esp_idf_size) needs FORCE_COLOR. Only on a real terminal; NO_COLOR
# turns it off.
COLOR_ENV := { [ -t 1 ] && [ -z "$${NO_COLOR:-}" ] \
                && export CLICOLOR_FORCE=1 FORCE_COLOR=1; true; }

IDF_ENV := . "$${IDF_PATH:?IDF_PATH is not set}/export.sh" > /dev/null && $(COLOR_ENV)

ifeq ($(ninja),y)
CMAKE_CONF_FLAGS    += -GNinja
endif

ifeq ($(debug),y)
CMAKE_CONF_FLAGS    += -DCMAKE_BUILD_TYPE=Debug
else
CMAKE_CONF_FLAGS    += -DCMAKE_BUILD_TYPE=Release
endif

ifeq ($(build_dir),)
ifeq ($(debug),y)
build_dir = _build_debug
else
build_dir = _build_release
endif
endif

ifeq ($(target),)
ifeq ($(MAKECMDGOALS), test)
target = $(project_name)_test_unitTest
else ifeq ($(MAKECMDGOALS), style-check)
target = $(project_name)_styleCheck $(project_name)_test_styleCheck
else ifeq ($(MAKECMDGOALS), style-format)
target = $(project_name)_styleFormat $(project_name)_test_styleFormat
else ifeq ($(MAKECMDGOALS), size-analysis)
target = $(project_name)_sizeReport
else
target = all
endif
endif

CMAKE_BUILD_FLAGS   += --target $(target)

ifeq ($(verbose),y)
CMAKE_BUILD_FLAGS   += -v
endif

CMAKE_CONF_FLAGS    += -DIDF_TARGET=$(target_chip)
CMAKE_CONF_FLAGS    += -DBOARD_DIR=$(mkfile_dir)boards/$(board)
CMAKE_CONF_FLAGS    += $(cmake_flags) -DAPP_PROJECT_NAME_BASE=$(project_name)

#------------------------------------------------------------------------------

CMAKE               := cmake
CMAKE_CONFIGURE     := $(CMAKE) $(CMAKE_CONF_FLAGS) ..
CMAKE_BUILD         := $(CMAKE) --build . $(CMAKE_BUILD_FLAGS) -- -j$(parallel)
CMAKE_INSTALL       := $(CMAKE) --install . --prefix ../_deploy/$(build_dir)

SIZE_SUMMARY        :=
ifeq ($(size)$(target),yall)
SIZE_SUMMARY        := && $(CMAKE) --build . --target size
endif

#------------------------------------------------------------------------------

default: fast-build
.PHONY: default

#------------------------------------------------------------------------------

configure: | .mk-build-dir
	$(AT) $(IDF_ENV) && cd $(build_dir) && \
	        $(CMAKE_CONFIGURE)
	$(AT) echo "DONE"
.PHONY: configure

#------------------------------------------------------------------------------

fast-build: | .mk-build-dir
	$(AT) $(IDF_ENV) && cd $(build_dir) && \
	        [ -f CMakeCache.txt ] \
	            && echo "warning: fast build mode: omitting configuration phase" \
	            || $(CMAKE_CONFIGURE) && \
	        $(CMAKE_BUILD) $(SIZE_SUMMARY)
	$(AT) echo "DONE"
.PHONY: fast-build

#------------------------------------------------------------------------------

build: | .mk-build-dir
	$(AT) $(IDF_ENV) && cd $(build_dir) && \
	        $(CMAKE_CONFIGURE) && $(CMAKE_BUILD) $(SIZE_SUMMARY)
	$(AT) echo "DONE"
.PHONY: build

#------------------------------------------------------------------------------

rebuild: clean fast-build
.PHONY: rebuild

#------------------------------------------------------------------------------

install:
	$(AT) $(IDF_ENV) && cd $(build_dir) && \
	        $(CMAKE_INSTALL)
	$(AT) echo "DONE"
.PHONY: install

#------------------------------------------------------------------------------

clean-test:
	$(AT) $(RM) -rf $(build_dir)/test/unit/build
.PHONY: clean-test

test: fast-build
.PHONY: test

#------------------------------------------------------------------------------

clean-all: clean
.PHONY: clean-all

clean:
	$(RM) -rf $(build_dir)/*
.PHONY: clean

#------------------------------------------------------------------------------

style-check: fast-build
.PHONY: style-check

style-format: fast-build
.PHONY: style-format

#------------------------------------------------------------------------------

size-analysis: fast-build
.PHONY: size-analysis

#------------------------------------------------------------------------------

.mk-build-dir:
	$(AT) mkdir -p "$(build_dir)"
.PHONY: .mk-build-dir

#------------------------------------------------------------------------------

help:
	$(AT) echo "Top-level Makefile for automating CMake tasks."
	$(AT) echo ""
	$(AT) echo "Parameters:"
	$(AT) echo "  build_dir   (string:'')    - CMake build directory"
	$(AT) echo "  cmake_flags (string:'')    - additional CMake configuration flags"
	$(AT) echo "  debug       (bool:y)       - Debug or Release build"
	$(AT) echo "  ninja       (bool:y)       - use Ninja generator"
	$(AT) echo "  parallel    (number:nproc) - number of parallel jobs"
	$(AT) echo "  target      (string:all)   - CMake build target"
	$(AT) echo "  verbose     (bool:n)       - enable CMake verbose output"
	$(AT) echo "  size        (bool:y)       - print memory usage summary after build"
	$(AT) echo "  target_chip (string:esp32s3) - ESP-IDF target chip"
	$(AT) echo "  board       (string)       - directory name under boards/ (use a fresh build_dir when switching)"
	$(AT) echo ""
	$(AT) echo "Targets:"
	$(AT) echo "  configure           - CMake configuration only"
	$(AT) echo "  build               - configure + build"
	$(AT) echo "  fast-build          - build, skip configure if cache present"
	$(AT) echo "  install             - install to _deploy/"
	$(AT) echo "  clean               - remove build directory contents"
	$(AT) echo "  clean-all           - alias for clean"
	$(AT) echo "  rebuild             - clean + fast-build"
	$(AT) echo "  test                - run unit tests"
	$(AT) echo "  clean-test          - remove unit test build artifacts"
	$(AT) echo "  style-check         - check code formatting"
	$(AT) echo "  style-format        - apply code formatting"
	$(AT) echo "  size-analysis       - print flash/RAM usage"
.PHONY: help
