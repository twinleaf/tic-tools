# Copyright: 2016-2018 Twinleaf LLC
# Author: gilberto@tersatech.com
# License: Proprietary

LIBTIO ?= ./libtio

CCFLAGS = -g -Wall -Wextra -I$(LIBTIO)/include/ -std=gnu11
CXXFLAGS = -g -Wall -Wextra -I$(LIBTIO)/include/ -std=gnu++11
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

obj/proxy.o: src/proxy.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/udp_proxy.o: src/udp_proxy.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/rpc_req.o: src/rpc_req.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/sensor_tree.o: src/sensor_tree.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/data_stream_dump.o: src/data_stream_dump.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

bin/proxy: obj/proxy.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/udp_proxy: obj/udp_proxy.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/rpc_req: obj/rpc_req.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/sensor_tree: obj/sensor_tree.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/data_stream_dump: obj/data_stream_dump.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

all: bin/proxy \
     bin/udp_proxy \
     bin/rpc_req \
     bin/sensor_tree \
     bin/data_stream_dump

clean:
	@$(MAKE) -C $(LIBTIO) clean
	@rm -rf obj bin
