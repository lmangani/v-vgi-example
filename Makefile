# VGI easter worker (V) — requires: v, Apache Arrow (pkg-config), C++20
ROOT := $(abspath .)
UNAME_S := $(shell uname -s)
ARROW_PKG := $(shell pkg-config --cflags --libs arrow 2>/dev/null)
EASTER_WORKER := $(ROOT)/easter_worker
SMOKE_CLIENT := $(ROOT)/bin/vgi_smoke_client
ERROR_SMOKE_CLIENT := $(ROOT)/bin/vgi_error_smoke_client
TESTDATA := $(ROOT)/vgi_v/testdata
HAYBARN ?= $(HOME)/.haybarn/cli/latest/haybarn

ifeq ($(UNAME_S),Darwin)
  SHLIB := vgi_v/c/libvgi_ipc.dylib
  CLIENT_RPATH := -Wl,-rpath,@loader_path/../vgi_v/c
else
  SHLIB := vgi_v/c/libvgi_ipc.so
  CLIENT_RPATH := -Wl,-rpath,$$ORIGIN/../vgi_v/c
endif

.PHONY: all shim smoke_client error_smoke_client worker test test-all test-computus test-ipc test-haybarn test-v-worker clean gen-wire ensure-haybarn

all: worker test

test-all: test test-haybarn

shim: $(SHLIB)

$(SHLIB): vgi_v/c/vgi_ipc.cpp vgi_v/c/vgi_ipc.h
	c++ -std=c++20 -O2 -fPIC -shared vgi_v/c/vgi_ipc.cpp -o $@ $(ARROW_PKG)
ifeq ($(UNAME_S),Darwin)
	install_name_tool -id @rpath/libvgi_ipc.dylib $@ 2>/dev/null || true
endif

smoke_client: bin/vgi_smoke_client

bin/vgi_smoke_client: vgi_v/c/vgi_smoke_client.cpp $(SHLIB)
	@mkdir -p bin
	c++ -std=c++20 -O2 vgi_v/c/vgi_smoke_client.cpp -o $@ -Ivgi_v/c -Lvgi_v/c -lvgi_ipc $(ARROW_PKG) \
	  $(CLIENT_RPATH)

bin/vgi_error_smoke_client: vgi_v/c/vgi_error_smoke_client.cpp $(SHLIB)
	@mkdir -p bin
	c++ -std=c++20 -O2 vgi_v/c/vgi_error_smoke_client.cpp -o $@ -Ivgi_v/c -Lvgi_v/c -lvgi_ipc $(ARROW_PKG) \
	  $(CLIENT_RPATH)

error_smoke_client: bin/vgi_error_smoke_client

gen-wire:
	chmod +x scripts/gen_wire.sh
	./scripts/gen_wire.sh

worker: shim
	cd vgi_v && v -o $(EASTER_WORKER) .

test: test-computus test-ipc

test-computus:
	cd vgi_v && v test .

test-ipc: worker smoke_client error_smoke_client
	$(SMOKE_CLIENT) $(EASTER_WORKER) $(TESTDATA)
	$(ERROR_SMOKE_CLIENT) $(EASTER_WORKER) $(TESTDATA) $(TESTDATA)/catalog_catalog_attach_wire.bin

test-v-worker: test-ipc

ensure-haybarn:
	@chmod +x scripts/ensure_haybarn.sh scripts/test_haybarn.sh
	@./scripts/ensure_haybarn.sh >/dev/null

test-haybarn: worker
	@chmod +x scripts/test_haybarn.sh
	@./scripts/test_haybarn.sh

clean:
	rm -f $(EASTER_WORKER) $(SMOKE_CLIENT) $(ERROR_SMOKE_CLIENT)
	rm -f vgi_v/c/libvgi_ipc.dylib vgi_v/c/libvgi_ipc.so
	rmdir bin 2>/dev/null || true
