// Copyright: 2016-2017 Twinleaf LLC
// Author: gilberto@tersatech.com
// License: Proprietary

#include <tio/data.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <sysexits.h>

int usage(FILE *out, const char *program, const char *error)
{
  if (error)
    fprintf(out, "%s\n", error);
  fprintf(out, "Usage: %s [-r] [-n] [-s] recorded_dstream_file\n", program);
  fprintf(out, "  -r   Use timestamps relative to the beginning of the\n"
               "       stream (default uses absolute timestamps).\n");
  fprintf(out, "  -n   Print null samples at the beginning of the stream.\n");
  fprintf(out, "  -i   Don't print mid-stream null samples.\n");
  fprintf(out, "  -s   Print sample number in the first column.\n");
  return EX_USAGE;
}

int main(int argc, char *argv[])
{
  int relative_time = 0;
  int print_initial_nan = 0;
  int print_midstream_nan = 1;
  int print_sample_no = 0;

  for (int opt = -1; (opt = getopt(argc, argv, "rnis")) != -1; ) {
    if (opt == 'r') {
      relative_time = 1;
    } else if (opt == 'n') {
      print_initial_nan = 1;
    } else if (opt == 'i') {
      print_midstream_nan = 0;
    } else if (opt == 's') {
      print_sample_no = 1;
    } else {
      return usage(stderr, argv[0], "Invalid command line option");
    }
  }

  if ((argc - optind) != 1)
    return usage(stderr, argv[0], "Must pass one input file.");

  FILE *fp = fopen(argv[optind], "r");
  if (!fp) {
    fprintf(stderr, "Failed to open %s\n", argv[optind]);
    return 1;
  }

  // read stream description
  tl_packet_header hdr;
  tl_data_stream_desc_header desc;
  fread(&hdr, sizeof(hdr), 1, fp);
  fread(&desc, sizeof(desc), 1, fp);
  {
    size_t name_len = hdr.payload_size - sizeof(desc);
    char name[name_len+1];
    fread(&name, name_len, 1, fp);
    name[name_len] = '\0';
    double period = 1.0e-6 * desc.period_numerator / desc.period_denominator;
    double rate = 1/period;
    fprintf(stderr, "Dumping stream '%s', %d channels, %zd bytes "
            "at %lf Hz (%lf us)\n",
            name, desc.channels, tl_data_type_size(desc.type),
            rate, period * 1e6);
  }

  size_t sample_size = desc.channels * tl_data_type_size(desc.type);

  uint64_t next_sample = 0;
  while (fread(&hdr, sizeof(hdr), 1, fp) == 1) {
    tl_data_stream_packet data;
    fread(&data.start_sample, hdr.payload_size, 1, fp);

    uint32_t delta = data.start_sample - (uint32_t)next_sample;
    uint64_t start = next_sample + delta;

    if ((next_sample == 0) && !print_initial_nan)
      next_sample = start;

    size_t n_samples = (hdr.payload_size - sizeof(uint32_t)) / sample_size;
    for (size_t end = start + n_samples; next_sample < end; next_sample++) {

      if ((next_sample < start) && !print_midstream_nan)
        continue;

      if (print_sample_no)
        printf("%"PRIu64" ", next_sample);

      double tstamp = next_sample * 1e-6 *
        desc.period_numerator / desc.period_denominator;
      if (!relative_time)
        tstamp += desc.start_timestamp * 1e-9;
      printf("%f", tstamp);

      for (size_t i = 0; i < desc.channels; i++) {
        if (next_sample < start) {
          printf(" nan");
        } else {
          void *n = &data.data[sample_size * (next_sample-start) +
                               i * tl_data_type_size(desc.type)];
          if (desc.type == TL_DATA_TYPE_INT32) {
            printf(" %"PRId32, *(int32_t*)n);
          } else if (desc.type == TL_DATA_TYPE_INT16) {
            printf(" %"PRId16, *(int16_t*)n);
          } else if (desc.type == TL_DATA_TYPE_FLOAT32) {
            printf(" %f", *(float*)n);
          } else {
            fprintf(stderr, "unsupported format (TODO)\n");
            return 1;
          }
        }
      }
      printf("\n");
    }
  }

  fclose(fp);

  return 0;
}
