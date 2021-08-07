# Copyright: 2016-2021 Twinleaf LLC
# Author: gilberto@tersatech.com
# License: MIT

LIBTIO ?= ./libtio

CCFLAGS = -g -Wall -Wextra -I$(LIBTIO)/include/ -std=gnu11
CXXFLAGS = -g -Wall -Wextra -I$(LIBTIO)/include/ -std=gnu++17
LDFLAGS = -L$(LIBTIO)/lib/ -ltio

.DEFAULT_GOAL = all
.SECONDARY:

LIB_HEADERS = $(wildcard $(LIBTIO)/include/tio/*.h)
LIB_FILE = $(LIBTIO)/lib/libtio.a

LIB_DEPS = $(wildcard $(LIBTIO)/include/tio/*.h) \
           $(wildcard $(LIBTIO)/src/*.c) $(wildcard $(LIBTIO)/src/*.h)

$(LIB_FILE): $(LIB_DEPS)
	@$(MAKE) -C $(LIBTIO)

obj bin:
	@mkdir -p $@

obj/tio-proxy.o: src/tio-proxy.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/tio-udp-proxy.o: src/tio-udp-proxy.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/tio-rpc.o: src/tio-rpc.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/tio-firmware-upgrade.o: src/tio-firmware-upgrade.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/tio-sensor-tree.o: src/tio-sensor-tree.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/tio-dataview.o: src/tio-dataview.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/tio-logparse.o: src/tio-logparse.cpp $(LIB_HEADERS) | obj
	@$(CXX) $(CXXFLAGS) -c $< -o $@

bin/tio-proxy: obj/tio-proxy.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

obj/tio-record.o: src/tio-record.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

bin/tio-udp-proxy: obj/tio-udp-proxy.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/tio-rpc: obj/tio-rpc.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/tio-firmware-upgrade: obj/tio-firmware-upgrade.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/tio-sensor-tree: obj/tio-sensor-tree.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/tio-dataview: obj/tio-dataview.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/tio-logparse: obj/tio-logparse.o $(LIB_FILE) | bin
	@$(CXX) -o $@ $< $(LDFLAGS)

bin/tio-record: obj/tio-record.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/tio-autoproxy: src/tio-autoproxy | bin
	@install $< $@

all: bin/tio-proxy \
     bin/tio-autoproxy \
     bin/tio-rpc \
     bin/tio-firmware-upgrade \
     bin/tio-udp-proxy \
     bin/tio-sensor-tree \
     bin/tio-dataview \
     bin/tio-logparse \
     bin/tio-record

clean:
	@$(MAKE) -C $(LIBTIO) clean
	@rm -rf obj bin

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
install: all
	@mkdir -p $(DESTDIR)$(BINDIR)
	@cp -p bin/tio-proxy $(DESTDIR)$(BINDIR)/
	@cp -p bin/tio-autoproxy $(DESTDIR)$(BINDIR)/
	@cp -p bin/tio-rpc $(DESTDIR)$(BINDIR)/
	@cp -p bin/tio-firmware-upgrade $(DESTDIR)$(BINDIR)/
	@cp -p bin/tio-record $(DESTDIR)$(BINDIR)/
	@cp -p bin/tio-logparse $(DESTDIR)$(BINDIR)/
	@cp -p bin/tio-dataview $(DESTDIR)$(BINDIR)/
#@cp -p bin/tio-udp-proxy $(DESTDIR)$(BINDIR)/
#@cp -p bin/tio-sensor-tree $(DESTDIR)$(BINDIR)/

