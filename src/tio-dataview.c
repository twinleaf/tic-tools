// Copyright: 2016-2021 Twinleaf LLC
// Author: gilberto@tersatech.com
// License: MIT

#include <tio/io.h>
#include <tio/data.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int single_line = 1;

void print_data(tl_data_stream_packet *dsp, int stream_id, const char *route)
{
  size_t data_len = dsp->hdr.payload_size - sizeof(uint32_t);
  printf("%s/stream%d sample %u, %zd bytes:", route, stream_id,
         dsp->start_sample, data_len);
  if (single_line) {
    for (size_t i = 0; i < data_len; i++)
      printf(" %02X", dsp->data[i]);
  } else {
    for (size_t i = 0; i < data_len; i+= 16) {
      printf("\n   ");
      for (size_t j = 0; (j < 16) && ((i+j) < data_len); j++) {
        if (j == 8)
          printf(" ");
        printf(" %02X", dsp->data[i+j]);
      }
    }
  }
  printf("\n");
  if (data_len && ((data_len % 4) == 0)) {
    // heuristic: print out as a set of floats
    printf("   ");
    for (size_t i = 0; i < data_len/4; i++) {
      if (!single_line && (i > 0) && ((i%3) == 0))
        printf("   ");
      printf(" %15g", *(float*)(dsp->data+i*4));
      if (!single_line && (i%3) == 2)
        printf("\n");
    }
    if (single_line || ((data_len % (3*4)) != 0))
      printf("\n");
  }
}

void print_timebase(tl_timebase_info *tbi, const char *route)
{
  const char *source = "INVALID";
  const char *epoch = "INVALID";
  switch (tbi->source) {
   case TL_TIMEBASE_SRC_LOCAL:
    source = "LOCAL";
    break;
   case TL_TIMEBASE_SRC_GLOBAL:
    source = "GLOBAL";
    break;
  }
  switch (tbi->epoch) {
   case TL_TIMEBASE_EPOCH_START:
    epoch = "START";
    break;
   case TL_TIMEBASE_EPOCH_SYSTIME:
    epoch = "SYSTIME";
    break;
   case TL_TIMEBASE_EPOCH_UNIX:
    epoch = "UNIX";
    break;
   case TL_TIMEBASE_EPOCH_GPS:
    epoch = "GPS";
    break;
  }
  printf("%s/timebase%d: %s %s start %f tick %f us %s%s\n",
         route, tbi->id, source, epoch, tbi->start_time * 1e-9,
         ((double)tbi->period_num_us)/tbi->period_denom_us,
         (tbi->flags & TL_TIMEBASE_VALID) ? "VALID" : "INVALID",
         (tbi->flags & TL_TIMEBASE_DELETED) ? " DELETED" : "");

  printf("    param ");
  for (int i = 0; i < 16; i++)
    printf("%02X", tbi->source_id[i]);
  printf(" stability %f ppm\n", tbi->stability * 1e6);
}

void print_source(tl_source_info *psi, const char *name, const char *route)
{
  const char *type = "unknown";
  switch (psi->type) {
   case TL_DATA_TYPE_UINT8:
    type = "uint8";
    break;
   case TL_DATA_TYPE_UINT16:
    type = "uint16";
    break;
   case TL_DATA_TYPE_UINT32:
    type = "uint32";
    break;
   case TL_DATA_TYPE_INT8:
    type = "int8";
    break;
   case TL_DATA_TYPE_INT16:
    type = "int16";
    break;
   case TL_DATA_TYPE_INT32:
    type = "int32";
    break;
   case TL_DATA_TYPE_FLOAT32:
    type = "float32";
    break;
   case TL_DATA_TYPE_FLOAT64:
    type = "float64";
    break;
  }
  printf ("%s/source%d \"%s\"%s: ", route, psi->id, name,
          (psi->flags & TL_SOURCE_DELETED) ? " (DELETED)" : "");
  printf("timebase %d period %d offset %d  %dx(%s)\n", psi->timebase_id,
         psi->period, psi->offset, psi->channels, type);
}

void print_stream(tl_stream_info *dsi, tl_stream_component_info *dci,
                   const char *route)
{
  printf ("%s/stream%d: timebase %d period %d offset %d sample %lu\n",
          route, dsi->id, dsi->timebase_id, dsi->period, dsi->offset,
          (unsigned long)dsi->sample_number);

  if (dsi->flags & TL_STREAM_DELETED) {
    printf("    DELETED\n");
    dsi->total_components = 0;
  } else if (!(dsi->flags & TL_STREAM_ACTIVE)) {
    printf("    INACTIVE\n");
    dsi->total_components = 0;
  } else if (dsi->flags & TL_STREAM_ONLY_INFO) {
    printf("    INFO-UPDATE (%d components)\n", dsi->total_components);
    dsi->total_components = 0;
  }

  for (uint16_t i = 0; i < dsi->total_components; i++) {
    printf("    %d: source %d%s period %d offset %d\n", i,
           dci[i].source_id, (dci[i].flags & TL_STREAM_COMPONENT_RESAMPLED) ?
           " RESAMPLED" : "", dci[i].period, dci[i].offset);
  }
}

void print_heartbeat(tl_packet *pkt, const char *route)
{
  printf("%s/heartbeat: %.*s%s\n", route, pkt->hdr.payload_size, pkt->payload,
         pkt->hdr.payload_size ? "" : "[empty]");
}

int main(int argc, char *argv[])
{
  const char *root_url = "tcp://localhost";
  const char *sensor_path = "";
  int list = 0;
  int updates_only = 0;
  int initial_refresh = 0;
  int exclude_default_stream = 0;

  for (int opt = -1; (opt = getopt(argc, argv, "r:s:cluxi")) != -1; ) {
    if (opt == 'r') {
      root_url = optarg;
    } else if (opt == 's') {
      sensor_path = optarg;
    } else if (opt == 'c') {
      single_line = 0;
    } else if (opt == 'l') {
      list = 1;
    } else if (opt == 'u') {
      updates_only = 1;
    } else if (opt == 'x') {
      exclude_default_stream = 1;
    } else if (opt == 'i') {
      initial_refresh = 1;
    } else {
      fprintf(stderr, "Usage: %s [-r root_url] [-s sensor_path] "
              "[-c] [-l] [-u] [-i] [-x]\n", argv[0]);
      fprintf(stderr,
              "  -r root_url        Root URL, defaults to tcp://localhost.\n"
              "  -s sensor_path     Sensor path relative to the root\n"
              "  -c                 Canonical data hexdump formatting.\n"
              "  -l                 List data sources and exit.\n"
              "  -u                 Show only metadata updates, not data.\n"
              "  -i                 Trigger initial send of metadata.\n"
              "  -x                 Skip printing data for stream 0.\n"
        );
      return 1;
    }
  }

  char sensor_url[256];
  snprintf(sensor_url, sizeof(sensor_url), "%s%s%s", root_url,
           strlen(sensor_path) ? "/" : "", sensor_path);
  int fd = tlopen(sensor_url, 0, NULL);
  if (fd < 0) {
    fprintf(stderr, "Failed to open %s: %s\n", sensor_url, strerror(errno));
    return 1;
  }

  if (list) {
    tl_rpc_reply_packet rep;
    if (tl_simple_rpc(fd, "data.source.list", 0, NULL, 0, &rep,
                      NULL, 0, NULL) != 0) {
      return 1;
    }
    uint16_t n = *(uint16_t*) rep.payload;
    for (uint16_t i = 0; i < n; i++) {
      if (tl_simple_rpc(fd, "data.source.list", 0, &i, sizeof(i), &rep,
                      NULL, 0, NULL) != 0)
        return 1;
      rep.payload[tl_rpc_reply_payload_size(&rep)] = '\0';
      tl_source_info *psi = (tl_source_info*) rep.payload;
      print_source(psi, (const char*) (psi+1), "");
    }
    return 0;
  }

  if (initial_refresh) {
    tl_rpc_request_packet req;
    tl_rpc_request_by_name(&req, 0, "data.send_all", NULL, 0);

    if (tlsend(fd, &req) != 0)
      return 1;
  }

  for (;;) {
    tl_packet pkt;
    if (tlrecv(fd, &pkt, sizeof(pkt)) != 0)
      return 1;
    char route_str[128];
    tl_format_routing(tl_packet_routing_data(&pkt.hdr),
                      tl_packet_routing_size(&pkt.hdr),
                      route_str, sizeof(route_str), 0);
    int id = tl_packet_stream_id(&pkt.hdr);
    if (id >= 0) {
      if (!updates_only && (!exclude_default_stream || (id != 0)))
        print_data((tl_data_stream_packet*) &pkt, id, route_str);
    } else if (pkt.hdr.type == TL_PTYPE_TIMEBASE) {
      tl_timebase_update_packet *tbu = (tl_timebase_update_packet*) &pkt;
      print_timebase(&tbu->info, route_str);
    } else if (pkt.hdr.type == TL_PTYPE_SOURCE) {
      // Null terminate the name string
      pkt.payload[pkt.hdr.payload_size] = '\0';
      tl_source_update_packet *psu = (tl_source_update_packet*) &pkt;
      print_source(&psu->info, psu->name, route_str);
    } else if (pkt.hdr.type == TL_PTYPE_STREAM) {
      tl_stream_update_packet *dsu = (tl_stream_update_packet*) &pkt;
      print_stream(&dsu->info, dsu->component, route_str);
    } else if (pkt.hdr.type == TL_PTYPE_HEARTBEAT) {
      print_heartbeat(&pkt, route_str);
    }
  }

  return 0;
}
