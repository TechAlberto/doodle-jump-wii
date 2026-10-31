#---------------------------------------------------------------------------------
# Doodle Jump for Wii - native reimplementation
#
#   make                 build the app
#   make AUTOPLAY=1      scripted player (used for automated Dolphin runs)
#   make DEBUG_LOG=1     log gameplay state to the Dolphin log
#   make PERF=1          draw the frame-time overlay
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

TARGET      := doodle-wii
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := source

CFLAGS      = -g -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(MACHDEP) $(INCLUDE)
CFLAGS      += -DHW_RVL

ifdef AUTOPLAY
CFLAGS      += -DAUTOPLAY
endif
ifdef DEBUG_LOG
CFLAGS      += -DDEBUG_LOG
endif
ifdef PERF
CFLAGS      += -DPERF_OVERLAY
endif
ifdef CALIBRATE
CFLAGS      += -DCALIBRATE
endif
ifdef TELEMETRY
CFLAGS      += -DTELEMETRY
endif
ifdef NOCLEAR
CFLAGS      += -DTEST_NOCLEAR
endif

CXXFLAGS    = $(CFLAGS)
LDFLAGS     = -g $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS        := -lwiiuse -lbte -lasnd -logc -lm
LIBDIRS     := $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES    := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES_BIN    := $(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(sFILES:.s=.o) $(SFILES:.S=.o)
export OFILES        := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD) \
                  -I$(LIBOGC_INC)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                   -L$(LIBOGC_LIB)

.PHONY: $(BUILD) clean run

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

run:
	wiiload $(OUTPUT).dol

else

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES_SOURCES) : $(OFILES_BIN)
$(HFILES) : ;

%.bin.o : %.bin
	@echo $(notdir $<)
	@bin2s -a 32 -H $(DEPSDIR)/$(subst .,_,$(notdir $<)).h $< | $(AS) -o $@

%.bin : ;

-include $(DEPSDIR)/*.d

endif
