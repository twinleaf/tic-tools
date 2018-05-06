// Copyright: 2017 Twinleaf LLC
// Author: gilberto@tersatech.com
// License: Proprietary

// program to traverse and print out a sensor tree given a root url
// NOTE: right now it requires running through a proxy that times out
// rpc requests after they go unanswered for a while

#include <tio/io.h>
#include <tio/rpc.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#define REQ_ID 123

void print_tree(int fd, uint8_t *routing, size_t routing_len)
{
  size_t nports = 0;
  {
    tl_rpc_reply_packet rep;
    char printbuf[1024];
    int ret = tl_simple_rpc(fd, "dev.desc", REQ_ID, NULL, 0, &rep,
                            routing, routing_len, NULL);
    if (ret < 0) {
      fprintf(stderr, "I/O error: %s\n", strerror(errno));
      exit(1);
    }

    memset(printbuf, ' ', routing_len * 2);
    char *start = printbuf + routing_len*2;
    if (routing_len == 0)
      start += sprintf(start, "R: ");
    else
      start += sprintf(start, "%u: ", routing[0]);

    if (ret == 0) {
      memcpy(start, rep.payload, tl_rpc_reply_payload_size(&rep));
      *(start + tl_rpc_reply_payload_size(&rep)) = 0;
    } else if (ret == TL_RPC_ERROR_TIMEOUT) {
      sprintf(start, "No device connected");
    } else {
      sprintf(start, "ERROR: %s", tl_rpc_strerror(ret));
    }

    puts(printbuf);

    if (ret == 0) {
      ret = tl_simple_rpc(fd, "dev.port.count", REQ_ID, NULL, 0, &rep,
                          routing, routing_len, NULL);
      if (tl_rpc_reply_payload_size(&rep) == 4)
        nports = *(uint32_t*)(&rep.payload[0]);
    }
  }

  if (nports && routing_len == TL_PACKET_MAX_ROUTING_SIZE)
    return;

  memmove(routing + 1, routing, routing_len);
  ++routing_len;

  for (size_t i = 0; i < nports; i++) {
    routing[0] = i;
    print_tree(fd, routing, routing_len);
  }

  --routing_len;
  memmove(routing, routing+1, routing_len);
}

int main(int argc, char *argv[])
{
  const char *root_url = "tcp://localhost";

  if (argc > 2)
    return 1;
  if (argc == 2)
    root_url = argv[1];

  int fd = tlopen(root_url, O_CLOEXEC, NULL);

  if (fd < 0)
    return 1;

  uint8_t routing_data[TL_PACKET_MAX_ROUTING_SIZE];
  size_t routing_len = 0;

  print_tree(fd, routing_data, routing_len);

  return 0;
}
