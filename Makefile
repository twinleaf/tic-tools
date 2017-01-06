# Copyright: 2016 Twinleaf LLC
# Author: gilberto@tersatech.com
# License: Proprietary

LIBTIO = ../libtio

CCFLAGS = -g -Wall -Wextra -I$(LIBTIO)/include/ -std=gnu11
CXXFLAGS = -g -Wall -Wextra -I$(LIBTIO)/include/ -std=gnu++11
LDFLAGS = -L$(LIBTIO)/lib/ -ltio

.DEFAULT_GOAL = all
.SECONDARY:

LIB_HEADERS = $(wildcard $(LIBTIO)/include/tio/*.h)
LIB_FILE = $(LIBTIO)/lib/libtio.a

obj bin:
	@mkdir -p $@

obj/proxy.o: src/proxy.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/rpc_req.o: src/rpc_req.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/dstream_dump.o: src/dstream_dump.c $(LIB_HEADERS) | obj
	@$(CC) $(CCFLAGS) -c $< -o $@

obj/dstream_record.o: src/dstream_record.cpp $(LIB_HEADERS) | obj
	@$(CXX) $(CXXFLAGS) -c $< -o $@

obj/vm4.o: src/vm4.cpp $(LIB_HEADERS) | obj
	@$(CXX) $(CXXFLAGS) -c $< -o $@

bin/proxy: obj/proxy.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/rpc_req: obj/rpc_req.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/dstream_dump: obj/dstream_dump.o $(LIB_FILE) | bin
	@$(CC) -o $@ $< $(LDFLAGS)

bin/dstream_record: obj/dstream_record.o $(LIB_FILE) | bin
	@$(CXX) -o $@ $< $(LDFLAGS)

bin/vm4: obj/vm4.o $(LIB_FILE) | bin
	@$(CXX) -o $@ $< $(LDFLAGS)


all: bin/proxy bin/rpc_req bin/dstream_dump bin/dstream_record bin/vm4

clean:
	@rm -rf obj bin
