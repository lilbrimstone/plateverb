# Makefile for LilBrimstone PlateVerb LV2
# Supports: S2400 (default) and Windows/Reaper (ARCH=win)

PLUGIN    := plateverb
BUNDLE    := $(PLUGIN).lv2
SRC_DIR   := src
SRCS      := $(SRC_DIR)/plateverb.c
# Convert .c to .o
OBJS      := $(SRCS:.c=.o)

# Defaults
ARCH      ?= aarch64
S2400_PATH ?= /mnt/d/dspcard/plugins/lv2

# Detect System settings
ifeq ($(ARCH),aarch64)
	# --- S2400 Build (Default) ---
	CC        := aarch64-linux-gnu-gcc
	TARGET    := $(PLUGIN).so
	LDFLAGS   += -shared -Wl,-Bsymbolic
	LDLIBS    += -lm
else ifeq ($(ARCH),win)
	# --- Windows Build (for Reaper testing) ---
	CC        := gcc
	TARGET    := $(PLUGIN).dll
	CFLAGS    += -static-libgcc
	LDFLAGS   += -shared -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic -static-libgcc
	LDLIBS    += -lm
endif

# Common Flags
CFLAGS  += -std=c11 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Wpedantic -Wno-unused-parameter

.PHONY: all bundle clean install_s2400

all: bundle

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

bundle: $(TARGET) manifest.ttl plateverb.ttl
	rm -rf $(BUNDLE)
	mkdir -p $(BUNDLE)
	cp -f $(TARGET) $(BUNDLE)/
	cp -f manifest.ttl plateverb.ttl $(BUNDLE)/
	@echo "Bundled -> $(BUNDLE)/"

install_s2400: bundle
	@echo "🔌 Connecting S2400 (Mounting Drive D)..."
	@sudo mkdir -p /mnt/d
	@sudo mount -t drvfs D: /mnt/d 2>/dev/null || true
	@echo "📦 Deploying to S2400..."
	@if [ ! -d "$(S2400_PATH)" ]; then \
		echo "❌ Error: Path $(S2400_PATH) still not found after mounting."; \
		echo "   - Is the S2400 USB mode active?"; \
		echo "   - Is it assigned to Drive Letter D: in Windows?"; \
		exit 1; \
	fi
	rm -rf $(S2400_PATH)/$(BUNDLE)
	cp -r $(BUNDLE) $(S2400_PATH)/
	@echo "✅ Installed to $(S2400_PATH)/$(BUNDLE)"
	@echo "⚠️  REMINDER: Power Cycle S2400 to clear LV2 cache!"

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf $(BUNDLE)