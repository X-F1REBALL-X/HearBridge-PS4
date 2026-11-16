# HearBridge PS4
#
#   make payload         daemon ELF (GoldHEN BinLoader)
#   make helpers         capture_diag, pcm_sweep, pcm_stop
#   make game-tap        hearbridge_game_tap.prx (GoldHEN game plugin)
#   make game-min        hearbridge_game_min.prx (load test, log only)
#   make plugin          game tap + shell_audio_tap.prx + shell_tap_test.prx
#   make inject          full audio injector (embeds shell_audio_tap.prx)
#   make inject-test     minimal load-test injector (embeds shell_tap_test.prx)
#   make all-payload     daemon + helpers + inject + inject-test
#   make launcher        HearBridge launcher tile .pkg (bundles the daemon ELF); the default
#   make test            host tests (class filter, resampler, tap call patterns, sample formats, output boost)
#
# The toolchains are found through the environment (see README, Build):
#   OO_PS4_TOOLCHAIN  OpenOrbis PS4 toolchain
#   PS4_PAYLOAD_SDK   PS4 payload SDK
#   GOLDHEN_SDK       GoldHEN plugins SDK

# Host tests and clean need no SDK; everything else does.
ifneq ($(filter-out test clean,$(or $(MAKECMDGOALS),launcher)),)
ifeq ($(and $(OO_PS4_TOOLCHAIN),$(PS4_PAYLOAD_SDK),$(GOLDHEN_SDK)),)
$(error Set OO_PS4_TOOLCHAIN, PS4_PAYLOAD_SDK and GOLDHEN_SDK, see the Build section of README.md)
endif
endif
export OO_PS4_TOOLCHAIN PS4_PAYLOAD_SDK GOLDHEN_SDK

# Identifies the daemon that this package carries: the launcher replaces a
# running daemon of the same version when its id differs.
BUILD_ID := $(shell sh scripts/build_id.sh)
export BUILD_ID

OUT   := build
NAME  := hearbridge_ps4

DAEMON_SRC := src/probe.c src/sdp_server.c src/sbc_enc.c src/linkkeys.c src/pcm_udp.c src/devices.c src/webui.c src/avrcp.c src/eq.c src/btchip.c src/realtek_fw.c src/hci_cmd_sync.c src/host_bdaddr.c
DAEMON_HDR := src/sdp_server.h src/sbc_enc.h src/linkkeys.h src/pcm_udp.h src/devices.h src/ui.h src/webui_html.h src/webui_assets.h src/avrcp.h src/log.h src/cmdq.h src/hciframe.h src/cod.h src/resample.h src/boost.h src/eq.h src/gprof.h src/hci.h src/hci_cmd.h src/btchip.h src/realtek_fw.h src/host_bdaddr.h src/util.h src/rtl_fw_blobs.h

PAYLOAD_CFLAGS := -O2 -Wall -Wextra -DHEARBRIDGE_PAYLOAD=1 -DHB_BUILD='"$(BUILD_ID)"' -I src
ORBIS_CC := $(PS4_PAYLOAD_SDK)/bin/orbis-clang

.PHONY: test launcher payload helpers plugin game-tap game-min inject inject-test all-payload clean

.DEFAULT_GOAL := launcher

payload: $(OUT)/$(NAME)_payload.elf

launcher: $(OUT)/$(NAME)_payload.elf game-tap
	$(MAKE) -C launcher

src/webui_html.h: src/web/index.html scripts/embed_html.py
	python3 scripts/embed_html.py src/web/index.html $@

src/webui_assets.h: src/web/icon-192.png src/web/icon-512.png src/web/apple-touch-icon.png scripts/embed_assets.py
	python3 scripts/embed_assets.py $@ webui_icon192=src/web/icon-192.png webui_icon512=src/web/icon-512.png webui_icon180=src/web/apple-touch-icon.png

$(OUT)/$(NAME)_payload.elf: $(DAEMON_SRC) $(DAEMON_HDR) | $(OUT)
	$(ORBIS_CC) $(PAYLOAD_CFLAGS) -o $@ $(DAEMON_SRC)

helpers: $(OUT)/capture_diag_payload.elf $(OUT)/pcm_sweep_payload.elf $(OUT)/pcm_stop_payload.elf

$(OUT)/capture_diag_payload.elf: src/capture_diag.c | $(OUT)
	$(ORBIS_CC) $(PAYLOAD_CFLAGS) -o $@ src/capture_diag.c

$(OUT)/pcm_sweep_payload.elf: src/pcm_sweep.c | $(OUT)
	$(ORBIS_CC) $(PAYLOAD_CFLAGS) -o $@ src/pcm_sweep.c

$(OUT)/pcm_stop_payload.elf: src/pcm_stop.c | $(OUT)
	$(ORBIS_CC) $(PAYLOAD_CFLAGS) -o $@ src/pcm_stop.c

plugin:
	$(MAKE) -C plugin

game-tap:
	$(MAKE) -C plugin game

game-min:
	$(MAKE) -C plugin game-min

# Embed PRX bytes then build the one-shot SceShellUI injectors.
inject: $(OUT)/inject_shell_audio_payload.elf
inject-test: $(OUT)/inject_test_payload.elf

$(OUT)/shell_audio_tap.prx:
	$(MAKE) -C plugin audio

$(OUT)/shell_tap_test.prx:
	$(MAKE) -C plugin test

src/shell_audio_tap_prx.h: $(OUT)/shell_audio_tap.prx
	python3 scripts/embed_prx.py $< $@ shell_audio_tap_prx

src/shell_tap_test_prx.h: $(OUT)/shell_tap_test.prx
	python3 scripts/embed_prx.py $< $@ shell_tap_test_prx

$(OUT)/inject_shell_audio_payload.elf: src/inject_shell_audio.c src/inject_common.h src/shell_audio_tap_prx.h | $(OUT)
	$(ORBIS_CC) $(PAYLOAD_CFLAGS) -o $@ src/inject_shell_audio.c

$(OUT)/inject_test_payload.elf: src/inject_test.c src/inject_common.h src/shell_tap_test_prx.h | $(OUT)
	$(ORBIS_CC) $(PAYLOAD_CFLAGS) -o $@ src/inject_test.c

all-payload: payload helpers plugin inject inject-test

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)
	$(MAKE) -C plugin clean
	rm -f src/shell_audio_tap_prx.h src/shell_tap_test_prx.h

# Embed Realtek USB firmware blobs from assets/firmware into src/rtl_fw_blobs.h.
.PHONY: rtl-fw
rtl-fw:
	python3 scripts/gen_rtl_fw.py src/rtl_fw_blobs.h

src/rtl_fw_blobs.h: assets/firmware/rtl8761bu_fw.bin assets/firmware/rtl8761bu_config.bin scripts/gen_rtl_fw.py
	python3 scripts/gen_rtl_fw.py src/rtl_fw_blobs.h

# Host tests of the pure parts (no SDK needed).
test: $(OUT)/test_host $(OUT)/test_webui $(OUT)/test_realtek_fw
	$(OUT)/test_host
	$(OUT)/test_webui
	$(OUT)/test_realtek_fw $(OUT)/rtl_fw_out

$(OUT)/test_host: tests/test_host.c src/cod.h src/resample.h src/pager.h src/boost.h src/eq.h src/eq.c src/gprof.h launcher/addr.h | $(OUT)
	gcc -O2 -Wall -Wextra -o $@ tests/test_host.c src/eq.c -lm

# The web UI state with the system calls stubbed (webui.c is included as it is;
# sockaddr_in has no sin_len on Linux, so it is mapped to a harmless field).
$(OUT)/test_webui: tests/test_webui.c src/webui.c src/webui_html.h src/webui_assets.h src/ui.h src/eq.c src/eq.h src/gprof.h src/boost.h src/devices.c src/linkkeys.c | $(OUT)
	gcc -O2 -Wall -Wextra -Wno-format-truncation -Dsin_len=sin_port -DSETTINGS_PATH='"/tmp/hbt/settings.txt"' -DHB_GAMES_PATH='"/tmp/hbt/games.txt"' \
	    -DTAP_OFF_PATH='"/tmp/hbt/tap_off"' -DSKIP_PATH='"/tmp/hbt/skip.txt"' -I src \
	    -o $@ tests/test_webui.c src/eq.c src/devices.c src/linkkeys.c -lm

$(OUT)/test_realtek_fw: tests/test_realtek_fw.c tests/log_stub.c src/realtek_fw.c src/hci_cmd_sync.c src/btchip.c src/host_bdaddr.c src/realtek_fw.h src/hci.h src/hci_cmd.h src/btchip.h src/host_bdaddr.h src/util.h src/rtl_fw_blobs.h | $(OUT)
	gcc -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -I src \
	    -o $@ tests/test_realtek_fw.c tests/log_stub.c src/realtek_fw.c src/hci_cmd_sync.c src/btchip.c src/host_bdaddr.c
