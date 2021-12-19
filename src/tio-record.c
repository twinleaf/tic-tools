// Copyright: 2021 Twinleaf LLC
// Author: gilberto@tersatech.com
// License: MIT

#include <tio/io.h>
#include <tio/data.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

void usage(const char *name)
{
  fprintf(stderr, "Usage: %s [-r root_sensor_url] [output_file]\n", name);
  exit(1);
}

int main(int argc, char *argv[])
{
  const char *root_url = "tcp://localhost";
  const char *output_file = NULL;

  for (int opt = -1; (opt = getopt(argc, argv, "r:")) != -1; ) {
    if (opt == 'r') {
      root_url = optarg;
    } else {
      usage(argv[0]);
    }
  }

  if (optind == (argc - 1)) {
    output_file = argv[optind];
  } else if (optind != argc) {
    usage(argv[0]);
  }

  int fd = tlopen(root_url, 0, NULL);
  if (fd < 0) {
    fprintf(stderr, "Failed to open %s: %s\n", root_url, strerror(errno));
    return 1;
  }

  int output_fd = STDOUT_FILENO;
  if (output_file) {
    output_fd = open(output_file, O_WRONLY|O_CREAT|O_TRUNC, 0755);
    if (output_fd < 0) {
      fprintf(stderr, "Failed to open %s: %s\n", output_file, strerror(errno));
      return 1;
    }
  }

  for (;;) {
    tl_packet pkt;
    if (tlrecv(fd, &pkt, sizeof(pkt)) != 0)
      return 1;
    int id = tl_packet_stream_id(&pkt.hdr);
    if (id < 0) {
      if ((pkt.hdr.type != TL_PTYPE_TIMEBASE) &&
          (pkt.hdr.type != TL_PTYPE_SOURCE) &&
          (pkt.hdr.type != TL_PTYPE_STREAM))
        continue;
    }
    ssize_t ret = write(output_fd, &pkt, tl_packet_total_size(&pkt.hdr));
    if (ret < (ssize_t)tl_packet_total_size(&pkt.hdr)) {
      fprintf(stderr, "Short write to output file, terminating: %s\n",
              strerror(errno));
      break;
    }
  }

  close(output_fd);

  return 0;
}
