# slim2diretta - Makefile build system
# Fully replaces CMake-based build workflow.
#
# Usage:
#   make
#   make -j$(nproc)
#   make ARCH_NAME=x64-linux-15v3
#   make DIRETTA_SDK_PATH=/path/to/DirettaHostSDK_148
#   make ENABLE_MP3=0 ENABLE_OGG=0 ENABLE_AAC=0

CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

SRCDIR := src
DIRETTADIR := diretta
OBJDIR := obj
BINDIR := bin
TARGET := $(BINDIR)/slim2diretta

INSTALL_BIN ?= /usr/local/bin
SYSTEMD_SERVICE ?= /etc/systemd/system/slim2diretta@.service
CONFIG_FILE ?= /etc/default/slim2diretta

UNAME_M := $(shell uname -m)

NON_BUILD_GOALS := clean info uninstall
ifeq ($(strip $(MAKECMDGOALS)),)
NEED_BUILD_DEPS := 1
else ifeq ($(filter-out $(NON_BUILD_GOALS),$(MAKECMDGOALS)),)
NEED_BUILD_DEPS := 0
else
NEED_BUILD_DEPS := 1
endif

# ============================================
# Architecture detection
# ============================================

ifeq ($(UNAME_M),x86_64)
BASE_ARCH := x64
ARCH_DESC_BASE := x86_64 (Intel/AMD 64-bit)
else ifeq ($(UNAME_M),aarch64)
BASE_ARCH := aarch64
ARCH_DESC_BASE := ARM64 (aarch64)
else ifeq ($(UNAME_M),arm64)
BASE_ARCH := aarch64
ARCH_DESC_BASE := ARM64 (arm64)
else ifeq ($(UNAME_M),armv7l)
BASE_ARCH := arm
ARCH_DESC_BASE := ARM 32-bit (armv7l)
else ifeq ($(UNAME_M),riscv64)
BASE_ARCH := riscv64
ARCH_DESC_BASE := RISC-V 64-bit
else
BASE_ARCH := unknown
ARCH_DESC_BASE := Unknown ($(UNAME_M))
endif

ifeq ($(BASE_ARCH),x64)
HAS_AVX2 := $(shell grep -q avx2 /proc/cpuinfo 2>/dev/null && echo 1 || echo 0)
HAS_AVX512 := $(shell grep -q avx512 /proc/cpuinfo 2>/dev/null && echo 1 || echo 0)
IS_ZEN4 := $(shell lscpu 2>/dev/null | grep -qi 'AMD.*Zen 4' && echo 1 || echo 0)

ifeq ($(IS_ZEN4),1)
DEFAULT_VARIANT := x64-linux-15zen4
CPU_DESC := AMD Zen 4 detected
else ifeq ($(HAS_AVX512),1)
DEFAULT_VARIANT := x64-linux-15v4
CPU_DESC := AVX512 detected (x86-64-v4)
else ifeq ($(HAS_AVX2),1)
DEFAULT_VARIANT := x64-linux-15v3
CPU_DESC := AVX2 detected (x86-64-v3)
else
DEFAULT_VARIANT := x64-linux-15v2
CPU_DESC := Basic x64 (x86-64-v2)
endif

else ifeq ($(BASE_ARCH),aarch64)
PAGE_SIZE := $(shell getconf PAGESIZE 2>/dev/null || echo 4096)
DT_MODEL := $(shell if [ -r /proc/device-tree/model ]; then tr -d '\0' < /proc/device-tree/model; elif [ -r /sys/firmware/devicetree/base/model ]; then tr -d '\0' < /sys/firmware/devicetree/base/model; fi)

ifeq ($(PAGE_SIZE),16384)
DEFAULT_VARIANT := aarch64-linux-15k16
CPU_DESC := ARM64 with 16KB pages detected via getconf PAGESIZE
else ifneq (,$(findstring Raspberry Pi 5,$(DT_MODEL)))
DEFAULT_VARIANT := aarch64-linux-15k16
CPU_DESC := $(DT_MODEL) detected (using k16 variant)
else ifneq (,$(findstring Raspberry Pi Compute Module 5,$(DT_MODEL)))
DEFAULT_VARIANT := aarch64-linux-15k16
CPU_DESC := $(DT_MODEL) detected (using k16 variant)
else
DEFAULT_VARIANT := aarch64-linux-15
ifeq ($(DT_MODEL),)
CPU_DESC := ARM64 with 4KB pages (standard variant)
else
CPU_DESC := ARM64 ($(DT_MODEL)) with 4KB pages (standard variant)
endif
endif

else ifeq ($(BASE_ARCH),riscv64)
DEFAULT_VARIANT := riscv64-linux-15
CPU_DESC := RISC-V 64-bit
else
DEFAULT_VARIANT := unknown
CPU_DESC := Unknown architecture
endif

ifdef ARCH_NAME
FULL_VARIANT := $(ARCH_NAME)
else
FULL_VARIANT := $(DEFAULT_VARIANT)
endif

# ============================================
# SDK detection (search order aligned to DirettaRendererUPnP-X)
# ============================================

ifdef DIRETTA_SDK_PATH
SDK_PATH := $(DIRETTA_SDK_PATH)
SDK_SOURCE := DIRETTA_SDK_PATH
else
SDK_SEARCH_PATHS := \
	../DirettaHostSDK_148 \
	./DirettaHostSDK_148 \
	$(HOME)/DirettaHostSDK_148 \
	/opt/DirettaHostSDK_148 \
	../DirettaHostSDK_147 \
	./DirettaHostSDK_147 \
	$(HOME)/DirettaHostSDK_147 \
	/opt/DirettaHostSDK_147
SDK_PATH := $(firstword $(foreach path,$(SDK_SEARCH_PATHS),$(wildcard $(path))))
SDK_SOURCE := auto-detected
endif

ifeq ($(SDK_PATH),)
ifeq ($(NEED_BUILD_DEPS),1)
$(error Diretta SDK not found. Set DIRETTA_SDK_PATH or install DirettaHostSDK_148/147 in known locations)
endif
endif

DIRETTA_LIB_NAME := libDirettaHost_$(FULL_VARIANT).a
ACQUA_LIB_NAME := libACQUA_$(FULL_VARIANT).a

SDK_LIB_DIRETTA := $(SDK_PATH)/lib/$(DIRETTA_LIB_NAME)
SDK_LIB_ACQUA := $(SDK_PATH)/lib/$(ACQUA_LIB_NAME)

ifeq (,$(wildcard $(SDK_LIB_DIRETTA)))
ifeq ($(NEED_BUILD_DEPS),1)
$(error Required library not found: $(SDK_LIB_DIRETTA). Override with ARCH_NAME=<variant> if needed)
endif
endif

ifneq (,$(wildcard $(SDK_LIB_ACQUA)))
HAVE_ACQUA := 1
EXTRA_SDK_LIBS := $(SDK_LIB_ACQUA)
else
HAVE_ACQUA := 0
EXTRA_SDK_LIBS :=
endif

# ============================================
# Dependency probing
# ============================================

HAVE_PKG_CONFIG := $(shell command -v pkg-config >/dev/null 2>&1 && echo 1 || echo 0)

ifeq ($(HAVE_PKG_CONFIG),1)
FLAC_CFLAGS := $(shell pkg-config --cflags flac 2>/dev/null)
FLAC_LIBS := $(shell pkg-config --libs flac 2>/dev/null)
endif

ifneq ($(strip $(FLAC_LIBS)),)
HAVE_FLAC := 1
else
ifneq (,$(wildcard /usr/include/FLAC/stream_decoder.h /usr/local/include/FLAC/stream_decoder.h))
HAVE_FLAC := 1
FLAC_CFLAGS :=
FLAC_LIBS := -lFLAC
else
HAVE_FLAC := 0
endif
endif

ifeq ($(HAVE_FLAC),0)
ifeq ($(NEED_BUILD_DEPS),1)
$(error libFLAC not found. Install libflac development headers/libraries)
endif
endif

ENABLE_MP3 ?= auto
ENABLE_OGG ?= auto
ENABLE_AAC ?= auto

HAVE_MP3 := 0
HAVE_OGG := 0
HAVE_AAC := 0

ifneq ($(ENABLE_MP3),0)
ifeq ($(HAVE_PKG_CONFIG),1)
MPG123_CFLAGS := $(shell pkg-config --cflags libmpg123 2>/dev/null)
MPG123_LIBS := $(shell pkg-config --libs libmpg123 2>/dev/null)
endif
ifneq ($(strip $(MPG123_LIBS)),)
HAVE_MP3 := 1
else ifneq (,$(wildcard /usr/include/mpg123.h /usr/local/include/mpg123.h))
HAVE_MP3 := 1
MPG123_CFLAGS :=
MPG123_LIBS := -lmpg123
endif
ifeq ($(ENABLE_MP3),1)
ifeq ($(HAVE_MP3),0)
ifeq ($(NEED_BUILD_DEPS),1)
$(error MP3 requested (ENABLE_MP3=1), but libmpg123 was not found)
endif
endif
endif
endif

ifneq ($(ENABLE_OGG),0)
ifeq ($(HAVE_PKG_CONFIG),1)
VORBIS_CFLAGS := $(shell pkg-config --cflags vorbisfile 2>/dev/null)
VORBIS_LIBS := $(shell pkg-config --libs vorbisfile 2>/dev/null)
endif
ifneq ($(strip $(VORBIS_LIBS)),)
HAVE_OGG := 1
else ifneq (,$(wildcard /usr/include/vorbis/vorbisfile.h /usr/local/include/vorbis/vorbisfile.h))
HAVE_OGG := 1
VORBIS_CFLAGS :=
VORBIS_LIBS := -lvorbisfile -lvorbis -logg
endif
ifeq ($(ENABLE_OGG),1)
ifeq ($(HAVE_OGG),0)
ifeq ($(NEED_BUILD_DEPS),1)
$(error Ogg requested (ENABLE_OGG=1), but libvorbisfile was not found)
endif
endif
endif
endif

ifneq ($(ENABLE_AAC),0)
ifeq ($(HAVE_PKG_CONFIG),1)
FDKAAC_CFLAGS := $(shell pkg-config --cflags fdk-aac 2>/dev/null)
FDKAAC_LIBS := $(shell pkg-config --libs fdk-aac 2>/dev/null)
endif
ifneq ($(strip $(FDKAAC_LIBS)),)
HAVE_AAC := 1
else ifneq (,$(wildcard /usr/include/fdk-aac/aacdecoder_lib.h /usr/local/include/fdk-aac/aacdecoder_lib.h))
HAVE_AAC := 1
FDKAAC_CFLAGS :=
FDKAAC_LIBS := -lfdk-aac
endif
ifeq ($(ENABLE_AAC),1)
ifeq ($(HAVE_AAC),0)
ifeq ($(NEED_BUILD_DEPS),1)
$(error AAC requested (ENABLE_AAC=1), but fdk-aac was not found)
endif
endif
endif
endif

# ============================================
# Sources
# ============================================

CORE_SOURCES := \
	src/main.cpp \
	src/SlimprotoClient.cpp \
	src/HttpStreamClient.cpp \
	src/Decoder.cpp \
	src/FlacDecoder.cpp \
	src/PcmDecoder.cpp \
	src/DsdProcessor.cpp \
	src/DsdStreamReader.cpp \
	diretta/DirettaSync.cpp \
	diretta/globals.cpp

SOURCES := $(CORE_SOURCES)

ifeq ($(HAVE_MP3),1)
SOURCES += src/Mp3Decoder.cpp
CPPFLAGS += -DENABLE_MP3
endif

ifeq ($(HAVE_OGG),1)
SOURCES += src/OggDecoder.cpp
CPPFLAGS += -DENABLE_OGG
endif

ifeq ($(HAVE_AAC),1)
SOURCES += src/AacDecoder.cpp
CPPFLAGS += -DENABLE_AAC
endif

OBJECTS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(SOURCES))
DEPENDS := $(OBJECTS:.o=.d)

# ============================================
# Flags and libraries
# ============================================

CPPFLAGS += \
	-I$(SRCDIR) \
	-I$(DIRETTADIR) \
	-I$(SDK_PATH)/Host \
	$(FLAC_CFLAGS) \
	$(MPG123_CFLAGS) \
	$(VORBIS_CFLAGS) \
	$(FDKAAC_CFLAGS)

CXXFLAGS += -pthread
LDFLAGS += -pthread

LDLIBS += \
	$(SDK_LIB_DIRETTA) \
	$(EXTRA_SDK_LIBS) \
	$(FLAC_LIBS) \
	$(MPG123_LIBS) \
	$(VORBIS_LIBS) \
	$(FDKAAC_LIBS) \
	-ldl

# SIMD tuning (compatible with old CMake behavior)
ifeq ($(BASE_ARCH),x64)
ifdef TARGET_MARCH
MARCH_LEVEL := $(TARGET_MARCH)
else ifdef ARCH_NAME
ifneq (,$(findstring zen4,$(ARCH_NAME)))
MARCH_LEVEL := zen4
else ifneq (,$(findstring v4,$(ARCH_NAME)))
MARCH_LEVEL := v4
else ifneq (,$(findstring v3,$(ARCH_NAME)))
MARCH_LEVEL := v3
else ifneq (,$(findstring v2,$(ARCH_NAME)))
MARCH_LEVEL := v2
else
MARCH_LEVEL := native
endif
else
ifeq ($(IS_ZEN4),1)
MARCH_LEVEL := zen4
else ifeq ($(HAS_AVX512),1)
MARCH_LEVEL := v4
else ifeq ($(HAS_AVX2),1)
MARCH_LEVEL := v3
else
MARCH_LEVEL := v2
endif
endif

ifeq ($(MARCH_LEVEL),zen4)
CXXFLAGS += -march=znver4 -mtune=znver4
SIMD_DESC := AMD Zen 4 optimizations enabled
else ifeq ($(MARCH_LEVEL),v4)
CXXFLAGS += -march=x86-64-v4 -mavx512f -mavx512bw -mavx512vl -mavx512dq
SIMD_DESC := AVX-512 optimizations enabled
else ifeq ($(MARCH_LEVEL),v3)
CXXFLAGS += -march=x86-64-v3 -mavx2 -mfma
SIMD_DESC := AVX2 optimizations enabled
else ifeq ($(MARCH_LEVEL),v2)
CXXFLAGS += -march=x86-64-v2
SIMD_DESC := x86-64-v2 baseline
else
CXXFLAGS += -march=native
SIMD_DESC := Native CPU optimizations
endif
else ifeq ($(BASE_ARCH),aarch64)
ifneq ($(TARGET_MARCH),)
ifneq ($(TARGET_MARCH),native)
CXXFLAGS += -mcpu=$(TARGET_MARCH)
SIMD_DESC := ARM64 -mcpu=$(TARGET_MARCH)
else
CXXFLAGS += -mcpu=native
SIMD_DESC := ARM64 native optimizations enabled
endif
else
CXXFLAGS += -mcpu=native
SIMD_DESC := ARM64 native optimizations enabled
endif
endif

ifneq ($(NOLOG),)
CPPFLAGS += -DNOLOG
endif

ifeq ($(HAVE_MP3),1)
MP3_STATUS := ENABLED (libmpg123)
else
MP3_STATUS := DISABLED (libmpg123 not found)
endif

ifeq ($(HAVE_OGG),1)
OGG_STATUS := ENABLED (libvorbisfile)
else
OGG_STATUS := DISABLED (libvorbisfile not found)
endif

ifeq ($(HAVE_AAC),1)
AAC_STATUS := ENABLED (fdk-aac)
else
AAC_STATUS := DISABLED (fdk-aac not found)
endif

ifeq ($(HAVE_FLAC),1)
FLAC_STATUS := ENABLED (required)
else
FLAC_STATUS := DISABLED (libFLAC not found)
endif

ifeq ($(HAVE_ACQUA),1)
ACQUA_STATUS := ENABLED
else
ACQUA_STATUS := DISABLED
endif

# ============================================
# Build rules
# ============================================

.PHONY: all clean info install uninstall

all: $(TARGET)
	@echo ""
	@echo "Build complete: $(TARGET)"

$(TARGET): $(OBJECTS) | $(BINDIR)
	@echo "Linking $@"
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BINDIR):
	@mkdir -p $(BINDIR)

clean:
	@rm -rf $(OBJDIR) $(BINDIR)
	@echo "Clean complete"

info:
	@echo ""
	@echo "═══════════════════════════════════════════════════════"
	@echo "  slim2diretta - Build Configuration"
	@echo "═══════════════════════════════════════════════════════"
	@echo "System:"
	@echo "  uname -m:       $(UNAME_M)"
	@echo "  Base Arch:      $(BASE_ARCH)"
	@echo "  Arch Desc:      $(ARCH_DESC_BASE)"
	@echo "CPU:"
	@echo "  Detection:      $(CPU_DESC)"
	@echo "Selected:"
	@echo "  Variant:        $(FULL_VARIANT)"
	@echo "  Library:        $(DIRETTA_LIB_NAME)"
	@echo "  SIMD:           $(SIMD_DESC)"
	@echo "SDK:"
	@echo "  Source:         $(SDK_SOURCE)"
	@echo "  Path:           $(SDK_PATH)"
	@echo "  Diretta Lib:    $(SDK_LIB_DIRETTA)"
	@echo "  ACQUA:          $(ACQUA_STATUS)"
	@echo "Codecs:"
	@echo "  FLAC:           $(FLAC_STATUS)"
	@echo "  MP3:            $(MP3_STATUS)"
	@echo "  Ogg Vorbis:     $(OGG_STATUS)"
	@echo "  AAC:            $(AAC_STATUS)"
	@echo "Build:"
	@echo "  Compiler:       $(CXX)"
	@echo "  Target:         $(TARGET)"
	@echo "═══════════════════════════════════════════════════════"
	@echo ""

install: $(TARGET)
	@echo "Installing binary to $(INSTALL_BIN)/slim2diretta"
	sudo cp $(TARGET) $(INSTALL_BIN)/slim2diretta
	sudo chmod +x $(INSTALL_BIN)/slim2diretta
	@echo "Installing systemd template to $(SYSTEMD_SERVICE)"
	sudo cp slim2diretta@.service $(SYSTEMD_SERVICE)
	@if [ ! -f $(CONFIG_FILE) ]; then \
		echo "Installing default config to $(CONFIG_FILE)"; \
		sudo cp slim2diretta.default $(CONFIG_FILE); \
	else \
		echo "Keeping existing config: $(CONFIG_FILE)"; \
	fi
	sudo systemctl daemon-reload
	@echo "Install complete"

uninstall:
	@echo "Removing installed files"
	sudo rm -f $(INSTALL_BIN)/slim2diretta
	sudo rm -f $(SYSTEMD_SERVICE)
	sudo rm -f $(CONFIG_FILE)
	sudo systemctl daemon-reload
	@echo "Uninstall complete"

-include $(DEPENDS)
