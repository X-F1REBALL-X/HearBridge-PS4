# HearBridge PS4 launcher tile (fpkg). Built from the top-level Makefile:
#   make launcher       ->  build/launcher/IV0000-HRBG40001_00-HEARBRIDGEPS4000.pkg
# Needs OO_PS4_TOOLCHAIN (OpenOrbis PS4 toolchain) in the environment. The system
# modules that go into the package (libc.prx, libSceFios2.prx, right.sprx) are
# copied from that toolchain at build time; they are not part of this repository.

TITLE       ?= HearBridge
VERSION     := 1.2.0
# param.sfo wants "xx.yy" (two digits each side) for APP_VER; VERSION (master
# version) of a full app is 01.00.
APP_VER     := 01.20
TITLE_ID    ?= HRBG40001
CONTENT_ID  ?= IV0000-HRBG40001_00-HEARBRIDGEPS4000
CATEGORY    ?= gd

ifeq ($(OO_PS4_TOOLCHAIN),)
$(error Set OO_PS4_TOOLCHAIN to the OpenOrbis PS4 toolchain, see the Build section of README.md)
endif
TC     := $(OO_PS4_TOOLCHAIN)
BIN    := $(TC)/bin/linux
# PkgTool.Core (.NET Core 3) needs ICU off and OpenSSL 1.1. If your system has
# only a newer OpenSSL, point LIBSSL11 at a directory with libssl.so.1.1.
LIBSSL11 ?=
PKGT   := DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 $(if $(LIBSSL11),LD_LIBRARY_PATH=$(LIBSSL11)) $(BIN)/PkgTool.Core
# System modules shipped with the toolchain (also used by its samples).
SYS_LIBC   ?= $(TC)/src/modules/libc.prx
SYS_FIOS2  ?= $(TC)/src/modules/libSceFios2.prx
SYS_RIGHT  ?= $(TC)/samples/dialogs/sce_sys/about/right.sprx
OUT    ?= ../build/launcher
STAGE  := $(OUT)/stage
DAEMON_ELF := ../build/hearbridge_ps4_payload.elf
TAP_PRX    := ../build/hearbridge_game_tap.prx
DAEMON_PRX := ../build/hearbridge_daemon.prx
BOOT_PRX   := ../build/hearbridge_boot.prx
USBD_SPRX  := ../assets/libSceUsbd.sprx
DAEMON_VERSION := $(shell sed -n 's/^\#define PROBE_VERSION "\(.*\)"/\1/p' ../src/probe.c)
BUILD_ID ?= $(shell sh ../scripts/build_id.sh)

CFLAGS  := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wextra \
           -isysroot $(TC) -isystem $(TC)/include \
           -DLAUNCHER_VERSION='"$(VERSION)"' -DDAEMON_VERSION='"$(DAEMON_VERSION)"' -DDAEMON_BUILD='"$(BUILD_ID)"'
LIBS    := -lc -lkernel -lSceNet -lSceUserService -lSceSystemService -lSceSysmodule \
           -lSceCommonDialog -lSceMsgDialog -lSceWebBrowserDialog -lScePad
LDFLAGS := -m elf_x86_64 -pie --script $(TC)/link.x --eh-frame-hdr -L$(TC)/lib $(LIBS) $(TC)/lib/crt1.o

STAGE_FILES := eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic1.png \
               sce_sys/about/right.sprx sce_module/libc.prx sce_module/libSceFios2.prx \
               hearbridge_ps4_payload.elf hearbridge_game_tap.prx hearbridge_daemon.prx \
               hearbridge_boot.prx libSceUsbd.sprx

.PHONY: all
all: $(OUT)/$(CONTENT_ID).pkg

# Rebuilt when the daemon id changes.
$(OUT)/build_id: FORCE
	@mkdir -p $(OUT)
	@echo '$(BUILD_ID)' | cmp -s - $@ || echo '$(BUILD_ID)' > $@
FORCE:

$(OUT)/main.o: main.c install.h addr.h Makefile ../src/probe.c ../src/daemon_main.h $(OUT)/build_id
	@mkdir -p $(OUT)
	clang $(CFLAGS) -c -o $@ $<

$(OUT)/install.o: install.c install.h Makefile
	@mkdir -p $(OUT)
	clang $(CFLAGS) -c -o $@ $<

# In-process daemon (firmware where BinLoader receives but does not run ELFs).
DAEMON_SRC := ../src/probe.c ../src/sdp_server.c ../src/sbc_enc.c ../src/linkkeys.c \
	../src/pcm_udp.c ../src/devices.c ../src/webui.c ../src/avrcp.c ../src/eq.c \
	../src/btchip.c ../src/realtek_fw.c ../src/hci_cmd_sync.c ../src/host_bdaddr.c \
	../src/adapter_pref.c ../src/backup.c ../src/hfp.c
DAEMON_CFLAGS := $(CFLAGS) -I../src -DHEARBRIDGE_PAYLOAD=1 -DHEARBRIDGE_PLUGIN=1 -DHEARBRIDGE_EBOOT=1 -DHB_BUILD='"$(BUILD_ID)"'
$(OUT)/daemon_%.o: ../src/%.c
	@mkdir -p $(OUT)
	clang $(DAEMON_CFLAGS) -c -o $@ $<
DAEMON_OBJS := $(patsubst ../src/%.c,$(OUT)/daemon_%.o,$(DAEMON_SRC))

$(OUT)/launcher.elf: $(OUT)/main.o $(OUT)/install.o $(DAEMON_OBJS)
	ld.lld $^ -o $@ $(LDFLAGS)

$(STAGE)/eboot.bin: $(OUT)/launcher.elf
	@mkdir -p $(STAGE)
	$(BIN)/create-fself -in=$< -out=$(OUT)/launcher.oelf --eboot $@ --paid 0x3800000000000011

$(STAGE)/sce_sys/param.sfo: Makefile
	@mkdir -p $(STAGE)/sce_sys
	rm -f $@
	$(PKGT) sfo_new $@
	$(PKGT) sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(PKGT) sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(APP_VER)'
	$(PKGT) sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(PKGT) sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value '$(CATEGORY)'
	$(PKGT) sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(PKGT) sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(PKGT) sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(PKGT) sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(PKGT) sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(PKGT) sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '01.00'

$(STAGE)/hearbridge_ps4_payload.elf: $(DAEMON_ELF)
	@mkdir -p $(STAGE)
	cp $< $@

$(STAGE)/hearbridge_game_tap.prx: $(TAP_PRX)
	@mkdir -p $(STAGE)
	cp $< $@

$(STAGE)/hearbridge_daemon.prx: $(DAEMON_PRX)
	@mkdir -p $(STAGE)
	cp $< $@

$(STAGE)/hearbridge_boot.prx: $(BOOT_PRX)
	@mkdir -p $(STAGE)
	cp $< $@

$(STAGE)/libSceUsbd.sprx: $(USBD_SPRX)
	@mkdir -p $(STAGE)
	cp $< $@



stage-static:
	@for f in $(SYS_LIBC) $(SYS_FIOS2) $(SYS_RIGHT); do \
	    test -f $$f || { echo "missing $$f (is OO_PS4_TOOLCHAIN right?)"; exit 1; }; done
	@mkdir -p $(STAGE)/sce_sys/about $(STAGE)/sce_module
	cp sce_sys/icon0.png sce_sys/pic1.png $(STAGE)/sce_sys/
	cp $(SYS_RIGHT) $(STAGE)/sce_sys/about/right.sprx
	cp $(SYS_LIBC) $(STAGE)/sce_module/libc.prx
	cp $(SYS_FIOS2) $(STAGE)/sce_module/libSceFios2.prx

$(OUT)/$(CONTENT_ID).pkg: $(STAGE)/eboot.bin $(STAGE)/sce_sys/param.sfo $(STAGE)/hearbridge_ps4_payload.elf \
                          $(STAGE)/hearbridge_game_tap.prx $(STAGE)/hearbridge_daemon.prx \
                          $(STAGE)/hearbridge_boot.prx $(STAGE)/libSceUsbd.sprx \
                          sce_sys/icon0.png sce_sys/pic1.png
	$(MAKE) stage-static
	cd $(STAGE) && rm -f pkg.gp4 && $(BIN)/create-gp4 -out pkg.gp4 --content-id=$(CONTENT_ID) --files "$(STAGE_FILES)"
	cd $(STAGE) && $(PKGT) pkg_build pkg.gp4 ..
	@ls -l $@
