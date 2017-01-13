// Copyright: 2017 Twinleaf LLC
// Author: gilberto@tersatech.com
// License: Proprietary

// Simple program to start capturing synchronized data from VM4 boards
// connected to a TIM R2 board.

#include <tio/io.h>
#include <tio/rpc.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define USE_GPS 1

int main(int argc, char *argv[])
{
  if (argc < 4)
    return 1;

  uint32_t period = atoi(argv[1]);
  const char *tim_url = argv[2];

  int fd = tlopen(tim_url, O_CLOEXEC, NULL);

  if (fd < 0)
    return 1;

  tl_rpc_reply_packet rep;
  uint8_t val;

#if USE_GPS
  val = 1;
  printf("Locking to GPS timebase...\n");
#else
  val = 0;
  printf("Using  local timebase...\n");
#endif
  if (tl_simple_rpc(fd, "lock_to_gps", 0, &val, 1, &rep,
                    NULL, 0, NULL) != 0)
    return 1;

#if USE_GPS
  // Synchronize time to GPS
  printf("Syncing TIM's time to GPS...\n");
  val = 1;
  if (tl_simple_rpc(fd, "set_time_to_gps", 0, &val, 1, &rep,
                    NULL, 0, NULL) != 0)
    return 1;

  usleep(1500000); // make sure there is at least one pulse to set the clock

  printf("Waiting for GPS lock...\n");
  for (uint8_t lock = 0;;) {
    if (tl_simple_rpc_fixed_size(fd, "gps_valid", 0, NULL, 0,
                                 &lock, sizeof(lock), NULL, 0, NULL) != 0)
      return 1;
    if (lock)
      break;
    else
      usleep(1000000);
  }
#endif

  // Set system times for individual VM4 boards
  for (int i = 3; i < argc; i++) {
    uint8_t routing[TL_PACKET_MAX_ROUTING_SIZE];
    int routing_len = tl_parse_routing(routing, argv[i]);
    if (routing_len < 0) {
      return 1;
    }

    uint64_t tim_time;
    if (tl_simple_rpc_fixed_size(fd, "dev.time", 0, NULL, 0,
                                 &tim_time, sizeof(tim_time),
                                 NULL, 0, NULL) != 0)
      return 1;

    if (tl_simple_rpc(fd, "dev.time", 0, &tim_time, sizeof(tim_time), &rep,
                      routing, routing_len, NULL) != 0)
      return 1;

    printf("Set VM4:%s time to %lu\n", argv[i], tim_time);

    if (tl_simple_rpc(fd, "period", 0, &period, sizeof(period), &rep,
                      routing, routing_len, NULL) != 0)
      return 1;

    val = 1;
    if (tl_simple_rpc(fd, "ext", 0, &val, 1, &rep,
                      routing, routing_len, NULL) != 0)
      return 1;

    if (tl_simple_rpc(fd, "start", 0, NULL, 0, &rep,
                      routing, routing_len, NULL) != 0)
      return 1;

    printf("Started VM4:%s in external mode\n", argv[i]);
  }

  if (tl_simple_rpc(fd,
#if USE_GPS
                    "start_clk_pulse",
#else
                    "start_clk",
#endif
                    0, NULL, 0, &rep, NULL, 0, NULL) != 0)
    return 1;

  return 0;
}
