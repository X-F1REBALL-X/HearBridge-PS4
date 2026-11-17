# Build hearbridge_game_tap.prx (GoldHEN game plugin) plus the experimental
# shell_audio_tap.prx / shell_tap_test.prx (OpenOrbis + GoldHEN SDK).
# OO_PS4_TOOLCHAIN and GOLDHEN_SDK come from the environment (see README, Build).
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(and $(OO_PS4_TOOLCHAIN),$(GOLDHEN_SDK)),)
$(error Set OO_PS4_TOOLCHAIN and GOLDHEN_SDK, see the Build section of README.md)
endif
endif

OUT    := ../build
INTDIR := build

CC  := clang
LD  := ld.lld

DEFS := -D_BSD_SOURCE=1 -D__BSD_VISIBLE=1 -D__PS4__=1 -DOO=1 -D__OPENORBIS__=1 -D__FINAL__=1
CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wextra -Wno-missing-field-initializers \
	$(DEFS) -isysroot $(OO_PS4_TOOLCHAIN) -isystem $(OO_PS4_TOOLCHAIN)/include \
	-I$(GOLDHEN_SDK)/include
LIBS_AUDIO := -lGoldHEN_Hook -lkernel -lc -lSceAudioOut -lSceNet -lSceSysmodule -lSceLibcInternal
# Same import set as GoldHEN plugin_template; AudioOut / Net are resolved at runtime.
# Game plugins use the exact flags of GoldHEN_Plugins_Repository's plugin
# Makefiles (no -O, i.e. -O0; -D__FINAL__=1 -D__USE_PRINTF__), plus -Wextra.
GAME_CFLAGS := -D__FINAL__=1 --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables \
	-D__FINAL__=1 -D__USE_PRINTF__ -Wall -Wextra -Wno-missing-field-initializers \
	-isysroot $(OO_PS4_TOOLCHAIN) -isystem $(OO_PS4_TOOLCHAIN)/include -I$(GOLDHEN_SDK)/include -I.
LIBS_GAME  := -lSceLibcInternal -lGoldHEN_Hook -lkernel -lSceSysmodule
LIBS_TEST  := -lGoldHEN_Hook -lkernel -lc -lSceLibcInternal
LDFLAGS_BASE := -m elf_x86_64 -pie -e _init --script $(OO_PS4_TOOLCHAIN)/link.x --eh-frame-hdr \
	-L$(OO_PS4_TOOLCHAIN)/lib -L$(GOLDHEN_SDK)

.PHONY: all clean sdk audio test game game-min game-steps daemon boot

all: sdk game game-min game-steps audio test

sdk:
	@test -f $(GOLDHEN_SDK)/libGoldHEN_Hook.a || $(MAKE) -C $(GOLDHEN_SDK) PRINTF=1
	@test -f $(GOLDHEN_SDK)/build/crtprx.o || $(MAKE) -C $(GOLDHEN_SDK) PRINTF=1

game:  sdk $(OUT)/hearbridge_game_tap.prx
game-min: sdk $(OUT)/hearbridge_game_min.prx
daemon: sdk $(OUT)/hearbridge_daemon.prx
boot: sdk $(OUT)/hearbridge_boot.prx
game-steps: sdk $(OUT)/hearbridge_step1.prx $(OUT)/hearbridge_step2.prx
audio: $(OUT)/shell_audio_tap.prx
test:  $(OUT)/shell_tap_test.prx

$(INTDIR)/shell_audio_tap.o: shell_audio_tap.c | $(INTDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(INTDIR)/hearbridge_game_tap.o: hearbridge_game_tap.c hb_plugin_common.h tap_rate.h tap_convert.h tap_diag.h | $(INTDIR)
	$(CC) $(GAME_CFLAGS) -c -o $@ $<

$(INTDIR)/hearbridge_game_min.o: hearbridge_game_min.c hb_plugin_common.h | $(INTDIR)
	$(CC) $(GAME_CFLAGS) -DSTEP=3 -c -o $@ $<

$(INTDIR)/hearbridge_step%.o: hearbridge_game_min.c hb_plugin_common.h | $(INTDIR)
	$(CC) $(GAME_CFLAGS) -DSTEP=$* -c -o $@ $<

$(OUT)/hearbridge_step%.elf: $(INTDIR)/hearbridge_step%.o $(GOLDHEN_SDK)/build/crtprx.o $(GOLDHEN_SDK)/libGoldHEN_Hook.a | $(OUT)
	$(LD) $(GOLDHEN_SDK)/build/crtprx.o $< -o $@ $(LDFLAGS_BASE) $(LIBS_GAME)

$(OUT)/hearbridge_step%.prx: $(OUT)/hearbridge_step%.elf
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself -in=$< -out=$(OUT)/hearbridge_step$*.oelf \
		--lib=$@ --paid 0x3800000000000011

$(INTDIR)/shell_tap_test.o: shell_tap_test.c | $(INTDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/shell_audio_tap.elf: $(INTDIR)/shell_audio_tap.o $(GOLDHEN_SDK)/build/crtprx.o $(GOLDHEN_SDK)/libGoldHEN_Hook.a | $(OUT)
	$(LD) $(GOLDHEN_SDK)/build/crtprx.o $(INTDIR)/shell_audio_tap.o -o $@ $(LDFLAGS_BASE) $(LIBS_AUDIO)

$(OUT)/hearbridge_game_tap.elf: $(INTDIR)/hearbridge_game_tap.o $(GOLDHEN_SDK)/build/crtprx.o $(GOLDHEN_SDK)/libGoldHEN_Hook.a | $(OUT)
	$(LD) $(GOLDHEN_SDK)/build/crtprx.o $(INTDIR)/hearbridge_game_tap.o -o $@ $(LDFLAGS_BASE) $(LIBS_GAME)

$(OUT)/hearbridge_game_min.elf: $(INTDIR)/hearbridge_game_min.o $(GOLDHEN_SDK)/build/crtprx.o $(GOLDHEN_SDK)/libGoldHEN_Hook.a | $(OUT)
	$(LD) $(GOLDHEN_SDK)/build/crtprx.o $(INTDIR)/hearbridge_game_min.o -o $@ $(LDFLAGS_BASE) $(LIBS_GAME)

$(OUT)/shell_tap_test.elf: $(INTDIR)/shell_tap_test.o $(GOLDHEN_SDK)/build/crtprx.o $(GOLDHEN_SDK)/libGoldHEN_Hook.a | $(OUT)
	$(LD) $(GOLDHEN_SDK)/build/crtprx.o $(INTDIR)/shell_tap_test.o -o $@ $(LDFLAGS_BASE) $(LIBS_TEST)

$(OUT)/shell_audio_tap.prx: $(OUT)/shell_audio_tap.elf
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself -in=$< -out=$(OUT)/shell_audio_tap.oelf \
		--lib=$@ --paid 0x3800000000000011

$(OUT)/hearbridge_game_tap.prx: $(OUT)/hearbridge_game_tap.elf
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself -in=$< -out=$(OUT)/hearbridge_game_tap.oelf \
		--lib=$@ --paid 0x3800000000000011

$(OUT)/hearbridge_game_min.prx: $(OUT)/hearbridge_game_min.elf
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself -in=$< -out=$(OUT)/hearbridge_game_min.oelf \
		--lib=$@ --paid 0x3800000000000011

$(OUT)/shell_tap_test.prx: $(OUT)/shell_tap_test.elf
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself -in=$< -out=$(OUT)/shell_tap_test.oelf \
		--lib=$@ --paid 0x3800000000000011

$(INTDIR):
	mkdir -p $@

$(OUT):
	mkdir -p $@

# HearBridge daemon as a GoldHEN plugin (firmware 14.02 BinLoader workaround).
DAEMON_SRC := ../src/probe.c ../src/sdp_server.c ../src/sbc_enc.c ../src/linkkeys.c \
	../src/pcm_udp.c ../src/devices.c ../src/webui.c ../src/avrcp.c ../src/eq.c \
	../src/btchip.c ../src/realtek_fw.c ../src/hci_cmd_sync.c ../src/host_bdaddr.c \
	../src/adapter_pref.c ../src/backup.c ../src/hfp.c
DAEMON_CFLAGS := $(GAME_CFLAGS) -O2 -I../src -DHEARBRIDGE_PAYLOAD=1 -DHEARBRIDGE_PLUGIN=1 \
	-DHB_BUILD='"plugin"'
LIBS_DAEMON := -lSceLibcInternal -lGoldHEN_Hook -lkernel -lSceSysmodule -lSceNet -lc

$(INTDIR)/hearbridge_daemon_wrap.o: hearbridge_daemon.c hb_plugin_common.h ../src/daemon_main.h | $(INTDIR)
	$(CC) $(DAEMON_CFLAGS) -c -o $@ hearbridge_daemon.c

$(INTDIR)/daemon_%.o: ../src/%.c | $(INTDIR)
	$(CC) $(DAEMON_CFLAGS) -c -o $@ $<

DAEMON_OBJS := $(INTDIR)/hearbridge_daemon_wrap.o \
	$(patsubst ../src/%.c,$(INTDIR)/daemon_%.o,$(DAEMON_SRC))

$(OUT)/hearbridge_daemon.elf: $(DAEMON_OBJS) $(GOLDHEN_SDK)/build/crtprx.o $(GOLDHEN_SDK)/libGoldHEN_Hook.a | $(OUT)
	$(LD) $(GOLDHEN_SDK)/build/crtprx.o $(DAEMON_OBJS) -o $@ $(LDFLAGS_BASE) $(LIBS_DAEMON)

$(OUT)/hearbridge_daemon.prx: $(OUT)/hearbridge_daemon.elf
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself -in=$< -out=$(OUT)/hearbridge_daemon.oelf \
		--lib=$@ --paid 0x3800000000000011


$(INTDIR)/hearbridge_boot.o: hearbridge_boot.c hb_plugin_common.h | $(INTDIR)
	$(CC) $(GAME_CFLAGS) -c -o $@ $<

$(OUT)/hearbridge_boot.elf: $(INTDIR)/hearbridge_boot.o $(GOLDHEN_SDK)/build/crtprx.o $(GOLDHEN_SDK)/libGoldHEN_Hook.a | $(OUT)
	$(LD) $(GOLDHEN_SDK)/build/crtprx.o $(INTDIR)/hearbridge_boot.o -o $@ $(LDFLAGS_BASE) $(LIBS_GAME) -lSceNet

$(OUT)/hearbridge_boot.prx: $(OUT)/hearbridge_boot.elf
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself -in=$< -out=$(OUT)/hearbridge_boot.oelf \
		--lib=$@ --paid 0x3800000000000011

clean:
	rm -rf $(INTDIR)
	rm -f $(OUT)/hearbridge_step*.elf $(OUT)/hearbridge_step*.oelf $(OUT)/hearbridge_step*.prx
	rm -f $(OUT)/hearbridge_game_min.elf $(OUT)/hearbridge_game_min.oelf $(OUT)/hearbridge_game_min.prx
	rm -f $(OUT)/hearbridge_boot.elf $(OUT)/hearbridge_boot.oelf $(OUT)/hearbridge_boot.prx
	rm -f $(OUT)/hearbridge_daemon.elf $(OUT)/hearbridge_daemon.oelf $(OUT)/hearbridge_daemon.prx
	rm -f $(INTDIR)/hearbridge_daemon_wrap.o $(INTDIR)/daemon_*.o
	rm -f $(OUT)/hearbridge_game_tap.elf $(OUT)/hearbridge_game_tap.oelf $(OUT)/hearbridge_game_tap.prx
	rm -f $(OUT)/shell_audio_tap.elf $(OUT)/shell_audio_tap.oelf $(OUT)/shell_audio_tap.prx
	rm -f $(OUT)/shell_tap_test.elf $(OUT)/shell_tap_test.oelf $(OUT)/shell_tap_test.prx
