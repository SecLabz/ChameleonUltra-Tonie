-include .local.env

SDK ?= deps/chameleon/firmware/nrf52_sdk
TINYUSB ?= deps/tinyusb
LWIP ?= deps/lwip
ARM_PREFIX ?= arm-none-eabi-
HOST_CC ?= cc
FIRMWARE_VERSION ?= 0.1.0-dev
export FIRMWARE_VERSION

ifeq ($(shell uname -s),Darwin)
HOST_GC = -Wl,-dead_strip
else
HOST_GC = -Wl,--gc-sections
endif

DATA = src/common/bytes.c src/common/crc32.c src/core/result.c \
       src/core/tag_image.c src/core/tag_library.c
COMMON = $(DATA) src/core/app.c src/net/api.c
PROTOCOL = $(wildcard src/tag/profiles/nfcv/*.c) \
           src/tag/protocol/nfcv/frame.c \
           src/cards/slix/tag.c src/tag/nfcv_session.c
INCLUDES = -Isrc -Isrc/config -Ibuild
BASEFLAGS = -std=c11 -Wall -Wextra -Os -ffunction-sections -fdata-sections
SANITIZERS = -g -fsanitize=address,undefined
DHCP_TEST_SRC = src/net/dhcp.c tests/dhcp.c \
                $(addprefix $(LWIP)/src/core/,pbuf.c mem.c memp.c def.c stats.c ipv4/ip4_addr.c)

ARMFLAGS = $(BASEFLAGS) $(EXTRA_CFLAGS) -mcpu=cortex-m4 -mthumb \
           -mfloat-abi=hard -mfpu=fpv4-sp-d16 -DNRF52840_XXAA -DNRF52_SERIES \
           -D__HEAP_SIZE=4096 -D__STACK_SIZE=8192 -fno-strict-aliasing
ARM_INC = $(INCLUDES) \
          $(addprefix -I$(SDK)/modules/nrfx/,mdk hal drivers/src drivers/include templates drivers) \
          -I$(SDK)/modules/nrfx -I$(SDK)/components/toolchain/cmsis/include \
          -I$(TINYUSB)/lib/networking -I$(TINYUSB)/src -I$(LWIP)/src/include
USB = $(addprefix $(TINYUSB)/src/,tusb.c common/tusb_fifo.c device/usbd.c \
      device/usbd_control.c portable/nordic/nrf5x/dcd_nrf5x.c class/net/ecm_rndis_device.c)
START = $(SDK)/modules/nrfx/mdk/gcc_startup_nrf52840.S \
        $(SDK)/modules/nrfx/mdk/system_nrf52840.c
LWIP_SRC = $(wildcard $(LWIP)/src/core/*.c) $(wildcard $(LWIP)/src/core/ipv4/*.c) \
           $(LWIP)/src/netif/ethernet.c
APP_SRC = $(COMMON) $(PROTOCOL) $(START) $(USB) $(LWIP_SRC) \
          $(SDK)/modules/nrfx/drivers/src/nrfx_nfct.c \
          $(SDK)/modules/nrfx/drivers/src/nrfx_timer.c \
          src/net/http.c src/net/network.c src/net/dhcp.c src/net/rndis.c \
          src/platform/ultra/nfcv_gpio.c src/tag/nfcv_emulation.c src/platform/ultra/platform.c \
          src/platform/ultra/usb.c src/platform/ultra/clock.c src/platform/ultra/flash.c \
          src/platform/ultra/battery.c src/platform/ultra/controls.c src/platform/ultra/power.c src/main.c

.PHONY: all version web host app test test-core test-phy test-controls test-app test-dhcp test-usb test-web test-http-parser test-http test-tools \
        test-release check-deps check-sdk check-tinyusb check-lwip clean

all: app

build:
	mkdir -p build

version: | build
	python3 tools/build_version.py

web: version
	python3 tools/build_web.py

host: web
	$(HOST_CC) $(BASEFLAGS) $(INCLUDES) -DPLATFORM_HOST \
		$(COMMON) src/net/http.c src/host/main.c -o build/tonie-host

# Local tests use simulated RF/flash and never connect to USB hardware.
test: test-core test-phy test-controls test-app test-dhcp test-usb test-web test-http-parser test-http test-tools

test-core: | build
	$(HOST_CC) $(BASEFLAGS) $(INCLUDES) $(SANITIZERS) \
		$(DATA) $(PROTOCOL) tests/core.c -o build/test-core
	UBSAN_OPTIONS=halt_on_error=1 build/test-core

test-phy: | build
	$(HOST_CC) $(BASEFLAGS) $(INCLUDES) $(SANITIZERS) tests/nfcv_ppm.c -o build/test-nfcv-ppm
	UBSAN_OPTIONS=halt_on_error=1 build/test-nfcv-ppm

test-controls: | build
	$(HOST_CC) $(BASEFLAGS) $(INCLUDES) $(SANITIZERS) tests/buttons.c -o build/test-buttons
	UBSAN_OPTIONS=halt_on_error=1 build/test-buttons

test-app: | build
	$(HOST_CC) $(BASEFLAGS) $(INCLUDES) $(SANITIZERS) \
		$(DATA) src/core/app.c tests/app.c -o build/test-app
	UBSAN_OPTIONS=halt_on_error=1 build/test-app

test-dhcp: check-lwip | build
	$(HOST_CC) $(BASEFLAGS) -DLWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS \
		-Isrc -Isrc/config -I$(LWIP)/src/include $(SANITIZERS) \
		$(DHCP_TEST_SRC) $(HOST_GC) -o build/test-dhcp
	UBSAN_OPTIONS=halt_on_error=1 build/test-dhcp

test-usb: check-tinyusb check-lwip | build
	$(HOST_CC) $(BASEFLAGS) $(INCLUDES) $(SANITIZERS) -DPLATFORM_HOST \
		-I$(TINYUSB)/src -I$(TINYUSB)/lib/networking -I$(LWIP)/src/include \
		src/common/bytes.c src/platform/ultra/usb.c src/net/rndis.c tests/usb.c -o build/test-usb
	UBSAN_OPTIONS=halt_on_error=1 build/test-usb

test-tools:
	python3 tests/dfu.py
	python3 tests/version.py

test-release:
	python3 tests/release.py

test-web: web
	node tests/image-layout.mjs
	node tests/formats.mjs
	node tests/browser-state.mjs

# This target binds a localhost socket; restricted sandboxes may require port permission.
test-http: host test-web
	python3 tests/test_http.py

test-http-parser: web
	$(HOST_CC) $(BASEFLAGS) $(INCLUDES) $(SANITIZERS) \
		src/net/http.c tests/http.c -o build/test-http-parser
	UBSAN_OPTIONS=halt_on_error=1 build/test-http-parser

check-deps: check-sdk check-tinyusb check-lwip

check-sdk:
	@test -f '$(SDK)/modules/nrfx/mdk/nrf.h' || \
		{ echo 'Run git submodule update --init, or set SDK to a compatible Chameleon SDK in .local.env.'; exit 1; }

check-tinyusb:
	@test -f '$(TINYUSB)/src/tusb.h' || \
		{ echo 'Run git submodule update --init, or set TINYUSB in .local.env.'; exit 1; }

check-lwip:
	@test -f '$(LWIP)/src/include/lwip/init.h' || \
		{ echo 'Run git submodule update --init, or set LWIP in .local.env.'; exit 1; }

app: check-deps web
	python3 tools/build_arm.py '$(ARM_PREFIX)' '$(ARMFLAGS)' '$(ARM_INC)' '$(APP_SRC)' \
		'$(SDK)' '$(TINYUSB)' '$(LWIP)'

# Recovery packages and user backups belong in ignored private/, outside this directory.
clean:
	rm -rf build
