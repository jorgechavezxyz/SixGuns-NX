#---------------------------------------------------------------------------------
# Six Guns NX loader — builds sixguns.nro
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := Six Guns
APP_AUTHOR  := so-loader port
APP_VERSION := 0.1.0

TARGET   := sixguns
BUILD    := build
SOURCES  := source
DATA     :=
INCLUDES := source
ICON     := icon.jpg

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS   := -g -Wall -O2 -ffunction-sections -fno-omit-frame-pointer $(ARCH) $(DEFINES) \
            -Wno-unused-function -Wno-unused-variable
CFLAGS   += $(INCLUDE) -D__SWITCH__ -I$(PORTLIBS)/include/freetype2
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS     := -lSDL2 -lEGL -lglapi -ldrm_nouveau -lfreetype -lharfbuzz -lfreetype -lpng16 -lbz2 -lcurl -lz -lstdc++ -lnx -lm

LIBDIRS  := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

# Mesa (libEGL.a) is C++, so link with the C++ driver to pull in libstdc++.
export LD := $(CXX)
export OFILES_SRC := $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_SRC)
export INCLUDE    := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                     $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS   := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifneq (,$(wildcard $(TOPDIR)/$(ICON)))
	export APP_ICON := $(TOPDIR)/$(ICON)
else
	export APP_ICON := $(LIBNX)/default_icon.jpg
endif
export NROFLAGS += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

# updater.c compares the build time with release dates: rebuild it every time.
BUILD_EPOCH := $(shell date +%s)
updater.o: CFLAGS += -DBUILD_EPOCH=$(BUILD_EPOCH)LL
updater.o: FORCE
.PHONY: FORCE
FORCE:
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)
endif
