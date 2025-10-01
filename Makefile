# Makefile for LilBrimstone PlateVerb LV2
# Default: cross-compile for aarch64 and build the .lv2 bundle

.RECIPEPREFIX := >

PLUGIN    := plateverb
BUNDLE    := $(PLUGIN).lv2
TARGET    := $(PLUGIN).so
SRC_DIR   := src
SRCS      := $(SRC_DIR)/plateverb.c
OBJS      := $(SRCS:.c=.o)

# Default to aarch64 cross
ARCH      ?= aarch64
CC_native  = gcc
CC_aarch64 = aarch64-linux-gnu-gcc
CC        := $(CC_$(ARCH))

# Default pkg-config (cross by default)
ifeq ($(ARCH),aarch64)
  PKG_CONFIG ?= pkg-config-aarch64-linux-gnu
else
  PKG_CONFIG ?= pkg-config
endif

LV2_CFLAGS := $(shell $(PKG_CONFIG) --cflags lv2 2>/dev/null)
LV2_LIBS   := $(shell $(PKG_CONFIG) --libs   lv2 2>/dev/null)

CFLAGS  ?= -std=c11 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Wpedantic -Wconversion -Wno-unused-parameter
CFLAGS  += $(LV2_CFLAGS)
LDFLAGS ?= -shared -Wl,-Bsymbolic
LDLIBS  ?= $(LV2_LIBS) -lm

# Default target builds the bundle
.PHONY: all bundle clean zip print-config
all: bundle

$(TARGET): $(OBJS)
> $(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
> @mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c -o $@ $<

bundle: $(TARGET) manifest.ttl plateverb.ttl
> rm -rf $(BUNDLE)
> mkdir -p $(BUNDLE)
> cp -f $(TARGET) $(BUNDLE)/
> cp -f manifest.ttl plateverb.ttl $(BUNDLE)/
> @echo "Bundled -> $(BUNDLE)/ { $(TARGET), manifest.ttl, plugin.ttl }"

zip: bundle
> rm -f $(BUNDLE).zip
> zip -r9 $(BUNDLE).zip $(BUNDLE)

clean:
> rm -f $(OBJS) $(TARGET)
> rm -rf $(BUNDLE) $(BUNDLE).zip

print-config:
> @echo ARCH=$(ARCH)
> @echo CC=$(CC)
> @echo PKG_CONFIG=$(PKG_CONFIG)
> @echo LV2_CFLAGS="$(LV2_CFLAGS)"
> @echo LV2_LIBS="$(LV2_LIBS)"