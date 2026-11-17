#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

export TARGET := relift-hwtest
export TOPDIR := $(CURDIR)

GAME_ICON :=
NITRO_FILES :=

GAME_TITLE := relift hwtest
GAME_SUBTITLE1 := CPU timing measurements
GAME_SUBTITLE2 := writes relift-hwtest.csv

include $(DEVKITARM)/ds_rules

# Two builds of the same program:
#   relift-hwtest-ntr.nds  plain DS (NTR) cartridge header (0x200 bytes, unit code 0), so DSi and
#                          3DS systems start it in DS mode; refuses to measure in DSi mode or at 133 MHz
#   relift-hwtest-dsi.nds  DSi-enhanced header, measures whatever mode it starts in
.PHONY: checkarm7 arm9-ntr arm9-dsi clean

all: $(TARGET)-ntr.nds $(TARGET)-dsi.nds

# the kernels and case tables (../gen.py), shared with the clang build (../build.py)
gen/cases.h: ../gen.py
	mkdir -p gen
	python3 -I ../gen.py gen
	cp gen/k9.s arm9/source/k9.s
	cp gen/k7.s arm7/source/k7.s

checkarm7: gen/cases.h
	$(MAKE) -C arm7

arm9-ntr: gen/cases.h
	$(MAKE) -C arm9 VARIANT=ntr

arm9-dsi: gen/cases.h
	$(MAKE) -C arm9 VARIANT=dsi

$(TARGET)-ntr.nds: checkarm7 arm9-ntr
	ndstool -c $@ -h 0x200 -7 arm7/$(TARGET).elf -9 arm9/$(TARGET)-ntr.elf \
	-b $(GAME_ICON) "$(GAME_TITLE);$(GAME_SUBTITLE1);$(GAME_SUBTITLE2)" $(_ADDFILES)

$(TARGET)-dsi.nds: checkarm7 arm9-dsi
	ndstool -c $@ -7 arm7/$(TARGET).elf -9 arm9/$(TARGET)-dsi.elf \
	-b $(GAME_ICON) "$(GAME_TITLE);$(GAME_SUBTITLE1);$(GAME_SUBTITLE2)" $(_ADDFILES)

clean:
	$(MAKE) -C arm9 clean
	$(MAKE) -C arm7 clean
	rm -rf $(TARGET)-ntr.nds $(TARGET)-dsi.nds gen arm9/source/k9.s arm7/source/k7.s
