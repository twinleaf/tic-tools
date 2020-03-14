// Copyright: 2019 Twinleaf LLC
// Author: gilberto@tersatech.com
// License: Proprietary

// Program to upgrade a sensor's firwmare.

#include <tio/rpc.h>
#include <tio/io.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sysexits.h>

int usage(FILE *out, const char *program, const char *error)
{
  if (error)
    fprintf(out, "%s\n", error);
  fprintf(out, "Usage: %s [-r root URL] [-s sensor path] firmware_file\n",
          program);
  fprintf(out, "  -r   Specify the root of the sensor tree to which issue "
               "       the RPC request. Defaults to tcp://localhost.\n");
  fprintf(out, "  -s   Specify the sensor path relative to the root (/).\n");
  return EX_USAGE;
}

int main(int argc, char *argv[])
{
  const char *root_url = "tcp://localhost";
  const char *sensor_path = "/";

  for (int opt = -1; (opt = getopt(argc, argv, "r:s:i")) != -1; ) {
    if (opt == 'r') {
      root_url = optarg;
    } else if (opt == 's') {
      sensor_path = optarg;
    } else {
      return usage(stderr, argv[0], "Invalid command line option");
    }
  }

  int nargs = argc - optind;
  if (nargs != 1)
    return usage(stderr, argv[0], "Invalid parameters");

  int firmware_fd = open(argv[optind], O_RDONLY);
  if (firmware_fd < 0) {
    fprintf(stderr, "Failed to open %s: %s\n", argv[optind], strerror(errno));
    return 1;
  }
  size_t total_size = lseek(firmware_fd, 0, SEEK_END);

  int fd = tlopen(root_url, 0, NULL);
  if (fd < 0) {
    fprintf(stderr, "Failed to open %s: %s\n", root_url, strerror(errno));
    return 1;
  }

  uint8_t routing[TL_PACKET_MAX_ROUTING_SIZE];
  int routing_len = tl_parse_routing(routing, sensor_path);
  if (routing_len < 0) {
    fprintf(stderr, "Failed parse routing '%s'\n", sensor_path);
    return 1;
  }

  size_t offset = 0;
  tl_rpc_reply_packet rep;
  int ret;

  // Stop device
  ret = tl_simple_rpc(fd, "dev.stop", 0, NULL, 0, &rep,
                      routing, routing_len, NULL);
  if (ret < 0) {
    fprintf(stderr, "Error stopping device; RPC failed: %s\n", strerror(errno));
  }

  for (;;) {
    uint8_t buf[288];
    ssize_t size = pread(firmware_fd, buf, sizeof(buf), offset);
    if (size == 0) {
      // EOF
      close(firmware_fd);
      break;
    }
    if (size < 0) {
      fprintf(stderr, "Failed to read firmware: %s\n", strerror(errno));
      return 1;
    }
    // Got 'size' bytes packet. Write to sensor
  retry_rpc:
    ret = tl_simple_rpc(fd, "dev.firmware.upload", 0, buf, size, &rep,
                        routing, routing_len, NULL);
    if (ret < 0)
      fprintf(stderr, "RPC failed: %s\n", strerror(errno));
    if (ret > 0) {
      if (ret == TL_RPC_ERROR_TIMEOUT)
        goto retry_rpc;
      fprintf(stderr, "RPC failed: %s\n", tl_rpc_strerror(ret));
    }
    if (ret != 0)
      return 1;

    offset += size;
    printf("Uploaded %.1f%%\r", 100.0*offset/total_size);
    fflush(stdout);
  }

  printf("\n");

  ret = tl_simple_rpc(fd, "dev.firmware.upgrade", 0, NULL, 0, &rep,
                      routing, routing_len, NULL);
  if (ret < 0)
    fprintf(stderr, "RPC failed: %s\n", strerror(errno));
  if (ret > 0)
    fprintf(stderr, "RPC failed: %s\n", tl_rpc_strerror(ret));
  if (ret != 0)
    return 1;

  printf("Upgrade initiated correctly.\n");

  tlclose(fd);

  return 0;
}
