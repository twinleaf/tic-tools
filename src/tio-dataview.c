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
  uint32_t sample_number = dsp->start_sample;
  int segment = -1;
  if (stream_id > 0) {
    sample_number = dsp->sample.sample_start_0 |
      (dsp->sample.sample_start_1 << 8) | (dsp->sample.sample_start_2 << 16);
    segment = dsp->sample.segment_id;
  }
  printf("%s/stream%d sample %u (segment %d), %zd bytes:", route, stream_id,
         sample_number, segment, data_len);
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

struct tl_varlen_helper {
  const char *ptr;
  const char *end;
};

void tl_varlen_init(struct tl_varlen_helper *vh,
                    struct tl_metadata_container *meta)
{
  vh->ptr = (char*)&meta->payload[meta->payload[0]];
  vh->end = (char*)&meta->payload[meta->hdr.payload_size - sizeof(meta->mhdr)];
}

const char *tl_varlen_str(struct tl_varlen_helper *vh)
{
  return vh->ptr;
}

int tl_varlen_advance(struct tl_varlen_helper *vh, size_t size)
{
  size_t max = vh->end - vh->ptr;
  if (size > max)
    size = max;
  vh->ptr += size;
  return (int) size;
}

void print_metadata(struct tl_metadata_container *meta, const char *route)
{
  const char *type = "UNKNOWN";
  switch (meta->mhdr.type) {
  case TL_METADATA_DEVICE: type = "device"; break;
  case TL_METADATA_STREAM: type = "stream"; break;
  case TL_METADATA_CURRENT_SEGMENT: type = "current segment"; break;
  case TL_METADATA_COLUMN: type = "column"; break;
  }
  size_t size = meta->hdr.payload_size - sizeof(tl_metadata_header);
  printf("%s/metadata %s (%zu bytes):%s%s%s\n", route, type, size,
         (meta->mhdr.flags & TL_METADATA_PERIODIC) ? " PERIODIC" : "",
         (meta->mhdr.flags & TL_METADATA_UPDATE) ? " UPDATE" : "",
         (meta->mhdr.flags & TL_METADATA_LAST) ? " LAST" : "");

  struct tl_varlen_helper vh;
  const char *val;
  tl_varlen_init(&vh, meta);

  if (meta->mhdr.type == TL_METADATA_DEVICE) {
    struct tl_metadata_device *dev =
      (struct tl_metadata_device*) &meta->payload;
    val = tl_varlen_str(&vh);
    printf("  name: %.*s\n",
           tl_varlen_advance(&vh, dev->name_varlen), val);
    printf("  streams: %d\n", dev->n_streams);
    printf("  session id: %d\n", dev->session_id);
    val = tl_varlen_str(&vh);
    printf("  serial: %.*s\n",
           tl_varlen_advance(&vh, dev->serial_varlen), val);
    val = tl_varlen_str(&vh);
    printf("  firmware: %.*s\n",
           tl_varlen_advance(&vh, dev->firmware_varlen), val);
  } else if (meta->mhdr.type == TL_METADATA_STREAM) {
    struct tl_metadata_stream *stream =
      (struct tl_metadata_stream*) &meta->payload;
    printf("  stream id: %d\n", stream->stream_id);
    val = tl_varlen_str(&vh);
    printf("  name: %.*s\n",
           tl_varlen_advance(&vh, stream->name_varlen), val);
    printf("  columns: %d\n", stream->n_columns);
    printf("  segments: %d\n", stream->n_segments);
    printf("  sample size: %d\n", stream->sample_size);
    printf("  buffered samples: %d\n", stream->buf_samples);
  } else if (meta->mhdr.type == TL_METADATA_CURRENT_SEGMENT) {
    struct tl_metadata_segment *seg =
      (struct tl_metadata_segment*) &meta->payload;
    printf("  stream id: %d\n", seg->stream_id);
    printf("  current segment id: %d\n", seg->segment_id);
    if (seg->flags & TL_METADATA_SEGMENT_FLAG_INVALID) {
      printf("  flags: invalid\n");
    } else {
      printf("  time reference:\n");
      const char *epoch_str = "INVALID";
      switch (seg->time_ref_epoch) {
      case TL_METADATA_EPOCH_ZERO: epoch_str = "ZERO"; break;
      case TL_METADATA_EPOCH_SYSTIME: epoch_str = "SYSTIME"; break;
      case TL_METADATA_EPOCH_UNIX: epoch_str = "UNIX"; break;
      }
      printf("    epoch: %s\n", epoch_str);
      val = tl_varlen_str(&vh);
      printf("    serial: %.*s\n",
           tl_varlen_advance(&vh, seg->time_ref_serial_varlen), val);
      printf("    session id: %d\n", seg->time_ref_session_id);
      printf("  start time: %u\n", seg->start_time);
      printf("  sampling rate: %u sps\n", seg->sampling_rate);
      printf("  decimation: %u\n", seg->decimation);
      if (seg->filter_type != TL_METADATA_FILTER_NONE) {
        printf("  filter order: %d\n", seg->filter_type);
        printf("  filter cutoff: %f Hz\n", seg->filter_cutoff);
      }
    }
  } else if (meta->mhdr.type == TL_METADATA_COLUMN) {
    struct tl_metadata_column *col =
      (struct tl_metadata_column*) &meta->payload;
    printf("  stream id: %d\n", col->stream_id);
    printf("  column index:  %d\n", col->index);
    val = tl_varlen_str(&vh);
    printf("  name: %.*s\n", tl_varlen_advance(&vh, col->name_varlen), val);
    val = tl_varlen_str(&vh);
    printf("  units: %.*s\n", tl_varlen_advance(&vh, col->units_varlen), val);
    val = tl_varlen_str(&vh);
    printf("  description: %.*s\n",
           tl_varlen_advance(&vh, col->description_varlen), val);
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
  char session_id[9] = "[empty]";
  if (pkt->hdr.payload_size == 4)
    snprintf(session_id, sizeof(session_id), "%08X", *(uint32_t*)pkt->payload);
  printf("%s/heartbeat: %s\n", route, session_id);
}

int main(int argc, char *argv[])
{
  const char *root_url = "tcp://localhost";
  const char *sensor_path = "";
  int list = 0;
  int updates_only = 0;
  int initial_refresh = 0;
  int exclude_legacy_stream = 0;

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
      exclude_legacy_stream = 1;
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
              "  -u                 Show only metadata updates.\n"
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
      if (!updates_only && (!exclude_legacy_stream || (id != 0)))
        print_data((tl_data_stream_packet*) &pkt, id, route_str);
    } else if (pkt.hdr.type == TL_PTYPE_METADATA) {
      struct tl_metadata_container *mc = (struct tl_metadata_container*) &pkt;
      print_metadata(mc, route_str);
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
    } else if (!updates_only && (pkt.hdr.type == TL_PTYPE_HEARTBEAT)) {
      print_heartbeat(&pkt, route_str);
    }
  }

  return 0;
}
