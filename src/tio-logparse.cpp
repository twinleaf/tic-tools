// Copyright: 2020-2021 Twinleaf LLC
// Author: gilberto@tersatech.com
// License: MIT

// Tool to generate tables from captured data
//
// The general idea is that there is a map from routing address to
// a tio_node object, which contains the parsed metadata, and in particular
// a tio_stream for each stream.
// In parallel, there is a map from timebase ID to a tio_row_merger. Each
// row merger generates a table in its own file (since in general we can't
// merge data from separate timebases).
// A row merger has a sequence of pointers tio_streams in the order they will
// appear in the output file, and each tio_stream points back to the
// row merger processing its data.
//
// Packets are read in and metadata is processed, samples are stored in a
// queue, and the rest is discarded, until the queue accumulated
// INITAL_QUEUE samples. At this point, the metadata received so far is
// used to generate the columns that will be in the output tables, and
// samples are processed, starting from the queued samples and then reading
// them from the input file.
//
// Each row merger keeps track of the earliest time of any sample of the
// contained tio_streams, and the processed samples are kept in the streams,
// until a sample is processed for a stream in the merger whose time
// exceeds the earliest time by DELTA_T. When that happens, all the stream
// samples with time within EPSILON of the earliest time are merged in
// a row and outputted to the file. Once all the data is read, the remaining
// samples in the streams are merged and written out.
//
// Note: samples are held in an ordered map in the streams, so this program
// can reorder out of order samples with a window of DELTA_T.

#include "tio/data.h"
#include "tio/io.h"

#include <string.h>
#include <cmath>
#include <inttypes.h>

#include <string>
#include <sstream>
#include <map>
#include <vector>
#include <deque>
#include <functional>

std::string bin2string(uint8_t **pdataptr, uint8_t tio_type);

struct column {
  column(const std::string &name, const std::string &desc,
         uint8_t tio_type, uint32_t period):
    name(name), desc(desc), period(period) {
    parser = [tio_type] (uint8_t **pdataptr) {
      return bin2string(pdataptr, tio_type);
    };
  }
  std::string name;
  std::string desc;
  using dataparser = std::function<std::string(uint8_t**)>;
  dataparser parser;
  uint32_t period;
};

struct tio_row_merger;

struct tio_stream {
  tio_stream(): is_good(false) {}

  tl_stream_info info;
  bool is_good;
  tio_row_merger *merger;
  std::vector<tl_stream_component_info> components;
  uint64_t sps;
  double sample_time;
  double start_time;
  std::vector<column> columns;
  std::map<double, std::vector<std::string>> samples;
};

struct tio_row_merger {
  tio_row_merger(): first_time(NAN), fp(nullptr) {}
  double first_time;
  std::vector<tio_stream*> streams;
  FILE *fp;

  void write_next_row();
};

std::map<std::string,tio_row_merger> mergers;

struct tio_source {
  tio_source(const tl_source_info &si, const std::string &name):
    info(si), full_name(name) {
  }
  tl_source_info info;
  std::string full_name;
};

struct tio_node {
  tio_node(const std::string &path): path(path) {}
  std::string path;
  std::map<uint16_t,tl_timebase_info> timebases;
  std::map<uint16_t,tio_source> sources;
  std::map<uint8_t,tio_stream> streams;
};

struct node_route {
  node_route(size_t n, const uint8_t *route): n_hops(n) {
    memcpy(this->route, route, n);
  }
  node_route(tl_packet_header *hdr):
    node_route(tl_packet_routing_size(hdr),
               tl_packet_routing_data(hdr)) {
  }
  size_t n_hops;
  uint8_t route[TL_PACKET_MAX_ROUTING_SIZE];
  bool operator< (const node_route &nr) const {
    if (n_hops >= nr.n_hops) {
      return (memcmp(route, nr.route, nr.n_hops) < 0);
    } else {
      return (memcmp(route, nr.route, n_hops) <= 0);
    }
  }
};

std::map<node_route,tio_node> nodes;

tio_node *get_node(tl_packet *pkt)
{
  node_route nr(&pkt->hdr);
  auto it = nodes.find(nr);
  if (it == nodes.end()) {
    char fmt[TL_ROUTING_FMT_BUF_SIZE];
    tl_format_routing(tl_packet_routing_data(&pkt->hdr),
                      tl_packet_routing_size(&pkt->hdr),
                      fmt, sizeof(fmt), 1);
    std::string routing_path(fmt);
    if (routing_path != "/")
      routing_path += "/";
    it = nodes.emplace(std::make_pair(nr, tio_node(routing_path))).first;
  }

  return &it->second;
}

std::string tabjoin(std::vector<std::string> &vs)
{
  std::string str;
  auto it = vs.begin();
  while (it != vs.end()) {
    str += *it;
    ++it;
    if (it != vs.end())
      str += '\t';
  }
  return str;
}

std::string bin2string(uint8_t **pdataptr, uint8_t tio_type)
{
  uint8_t *dataptr = *pdataptr;
  size_t size = tl_data_type_size(tio_type);
  *pdataptr = dataptr + size;
  char fmtbuf[256];
  strcpy(fmtbuf, "NaN");
  if (tio_type == TL_DATA_TYPE_FLOAT32) {
    float val;
    memcpy(&val, dataptr, sizeof(val));
    snprintf(fmtbuf, sizeof(fmtbuf), "%f", val);
  } else if (tio_type == TL_DATA_TYPE_FLOAT64) {
    double val;
    memcpy(&val, dataptr, sizeof(val));
    snprintf(fmtbuf, sizeof(fmtbuf), "%f", val);
  } else {
    uint8_t buf[sizeof(uint64_t)];
    size_t size = tio_type >> 4;
    if (size > sizeof(buf))
      throw "Unexpected size";
    memcpy(buf, dataptr, size);
    memset(buf+size, 0, sizeof(buf)-size);
    if (tio_type & 1) {
      // signed
      int64_t val;
      memcpy(&val, buf, sizeof(val));
      // sign extend
      val = (val << ((sizeof(buf)-size)*8)) >> ((sizeof(buf)-size)*8);
      snprintf(fmtbuf, sizeof(fmtbuf), "%" PRId64, val);
    } else {
      // unsigned
      uint64_t val;
      memcpy(&val, buf, sizeof(val));
      snprintf(fmtbuf, sizeof(fmtbuf), "%" PRIu64, val);
    }
  }
  return std::string(fmtbuf);
};

#define INITIAL_QUEUE 200000
#define DELTA_T          5.0
#define EPSILON         1e-5

void tio_row_merger::write_next_row()
{
  std::vector<std::string> row;
  char fmtbuf[32];
  snprintf(fmtbuf, 32, "%.6f", first_time);
  row.push_back(fmtbuf);
  double threshold = first_time + EPSILON;
  first_time = NAN;

  for (tio_stream *stream_ptr: streams) {
    if (!stream_ptr->samples.empty() &&
        (stream_ptr->samples.begin()->first <= threshold)) {
      for (const std::string &str: stream_ptr->samples.begin()->second)
        row.push_back(str);
      stream_ptr->samples.erase(stream_ptr->samples.begin());
    } else {
      for (size_t i = 0; i < stream_ptr->columns.size(); i++)
        row.push_back("");
    }
    if (!stream_ptr->samples.empty()) {
      if (!std::isfinite(first_time) ||
          (stream_ptr->samples.begin()->first < first_time))
        first_time = stream_ptr->samples.begin()->first;
    }
  }
  fprintf(fp, "%s\n", tabjoin(row).c_str());
}

void usage(const char *bin)
{
  fprintf(stderr, "\n    Usage: %s <path to .tio file>\n\n", bin);
  fprintf(stderr,
          "  This program will generate one TSV file for each timebase\n"
          "  present in the original data. For 'abcd.tio' with a local\n"
          "  and an absolute timebase, it will create:\n"
          "      - abcd.unix.tsv (data with absolute time)\n"
          "      - abcd.1.tsv (data with local time)\n\n");
}

int main(int argc, char *argv[])
{
  if (argc != 2) {
    usage(argv[0]);
    return 1;
  }

  int fd = -1;
  {
    std::string url = "file://";
    url += argv[1];
    fd = tlopen(url.c_str(), 0, NULL);
    if (fd < 0) {
      fprintf(stderr, "Failed to open %s\n", argv[1]);
      return 1;
    }
  }

  // read in fixed number of data packets and store the raw metadata
  // found, and put the samples in queued_data.
  std::deque<tl_data_stream_packet> queued_data;

  while (queued_data.size() < INITIAL_QUEUE) {
    tl_packet pkt;
    if (tlrecv(fd, &pkt, sizeof(pkt)) != 0)
      break;

    if (pkt.hdr.type == TL_PTYPE_TIMEBASE) {
      tio_node *tn = get_node(&pkt);
      const tl_timebase_update_packet *tup =
        reinterpret_cast<tl_timebase_update_packet*>(&pkt);
      tn->timebases.emplace(std::make_pair(tup->info.id, tup->info));
    } else if (pkt.hdr.type == TL_PTYPE_SOURCE) {
      tio_node *tn = get_node(&pkt);
      const tl_source_update_packet *sup =
        reinterpret_cast<tl_source_update_packet*>(&pkt);
      size_t name_len = (pkt.hdr.payload_size -
                         sizeof(tl_source_info));
      tn->sources.emplace(
        std::make_pair(sup->info.id,
                       tio_source(sup->info,
                                  std::string(sup->name, name_len))));
    } else if (pkt.hdr.type == TL_PTYPE_STREAM) {
      const tl_stream_update_packet *sup =
        reinterpret_cast<tl_stream_update_packet*>(&pkt);
      if (!(sup->info.flags & TL_STREAM_ONLY_INFO)) {
        tio_node *tn = get_node(&pkt);
        tio_stream &stream = tn->streams[sup->info.id];
        if (stream.components.size() != sup->info.total_components) {
          stream.info = sup->info;
          for(size_t i = 0; i < sup->info.total_components; i++)
            stream.components.push_back(sup->component[i]);
        }
      }
    } else {
      int id = tl_packet_stream_id(&pkt.hdr);
      if (id >= 0) {
        // To avoid processing packets for a previous acquisition mistakenly
        // recorded, discard anything before a stream's metadata is received.
        tio_node *tn = get_node(&pkt);
        if (tn->streams.count(id)) {
          const tl_data_stream_packet *dsp =
            reinterpret_cast<tl_data_stream_packet*>(&pkt);
          queued_data.push_back(*dsp);
        }
      }
    }
  }

  // Process metadata to create structures about how to parse data
  for (auto nit = nodes.begin(); nit != nodes.end(); ++nit) {
    auto &tn = nit->second;
    std::string addr_prefix = tn.path;

    for (auto sit = tn.streams.begin(); sit != tn.streams.end(); ++sit) {
      auto stream_id = sit->first;
      auto &stream = sit->second;

      // determine parameters using to convert sample number to sample time
      auto tbit = tn.timebases.find(stream.info.timebase_id);
      if (tbit == tn.timebases.end()) {
        printf("Cannot find metadata for Timebase %d, ignoring stream %s%d\n",
               stream.info.timebase_id, addr_prefix.c_str(), stream_id);
        continue;
      }
      stream.sps =
        1000000ull * tbit->second.period_denom_us / tbit->second.period_num_us;
      stream.sps /= stream.info.period;
      stream.sample_time = 1.0/stream.sps;
      stream.start_time = tbit->second.start_time * 1e-9;

      // Generate string id for timebase
      std::string tbid;
      if (tbit->second.source == TL_TIMEBASE_SRC_GLOBAL) {
        if (tbit->second.epoch == TL_TIMEBASE_EPOCH_UNIX)
          tbid = "unix";
        else {
          printf("Global timebase implemented only for unix time, ignoring "
                 "stream %s%d\n", addr_prefix.c_str(), stream_id);
          continue;
        }
      } else {
        char fmtbuf[32];
        for (size_t i = 0; i < sizeof(tbit->second.source_id); i++) {
          snprintf(fmtbuf, sizeof(fmtbuf), "%02X", tbit->second.source_id[i]);
          tbid += fmtbuf;
        }
      }

      // Generate the column structures for this stream.
      std::vector<column> colvec;

      bool skip = false;
      for (const auto &comp: stream.components) {
        auto it = tn.sources.find(comp.source_id);
        if (it == tn.sources.end()) {
          printf("Cannot find metadata for Source %d, ignoring stream %s%d\n",
                 comp.source_id, addr_prefix.c_str(), stream_id);
          skip = true;
          break;
        }
        std::stringstream ss(it->second.full_name);
        std::string name, _channel_names, desc, _units;
        std::getline(ss, name, '\t');
        std::getline(ss, _channel_names, '\t');
        std::getline(ss, desc, '\t');
        std::getline(ss, _units, '\t');
        std::stringstream css(_channel_names);
        std::stringstream uss(_units);
        std::vector<std::string> channel_names, units;
        std::string tmp;
        while (std::getline(css, tmp, ','))
          channel_names.push_back(tmp);
        while (std::getline(uss, tmp, ','))
          units.push_back(tmp);

        const tl_source_info &si = it->second.info;
        if (si.channels == 1) {
          std::string units_str = "";
          if (!units.empty())
            units_str = units[0];
          colvec.push_back(column(addr_prefix + name, desc + ", " + units_str,
                                  si.type, comp.period));
        } else {
          for (size_t i = 0; i < si.channels; i++) {
            std::string units_str = "";
            if (units.size() > i)
              units_str = units[i];
            else if (!units.empty())
              units_str = units[0];
            colvec.push_back(column(addr_prefix + name + "." + channel_names[i],
                                    desc + ", " + units_str,
                                    si.type, comp.period));
          }
        }
      }
      if (skip)
        continue;

      tio_row_merger &merger = mergers[tbid];
      merger.streams.push_back(&stream);
      stream.merger = &merger;
      stream.columns = colvec;
      stream.is_good = true;
    }
  }

  // Generate all the tsv file names, open the files and write out headers.
  {
    std::string base_output_path = argv[1];
    size_t last_dot = base_output_path.find_last_of(".");
    if (last_dot != std::string::npos) {
      std::string ext =
        base_output_path.substr(last_dot, base_output_path.length()-last_dot);
      if (ext == ".tio")
        base_output_path.resize(last_dot+1);
    }

    size_t index = 0;
    for (auto it = mergers.begin(); it != mergers.end(); ++it) {
      const std::string &tbid = it->first;
      tio_row_merger &merger = it->second;

      std::string output_path = base_output_path;
      if (tbid == "unix") {
        output_path += tbid;
      } else {
        char fmtbuf[32];
        snprintf(fmtbuf, sizeof(fmtbuf), "%zd", ++index);
        output_path += fmtbuf;
      }
      output_path += ".tsv";

      merger.fp = fopen(output_path.c_str(), "w");

      if (!merger.fp) {
        printf("Failed to open output file: %s\n", output_path.c_str());
        exit(1);
      }

      // Generate header. 2 lines: address+name, description+units
      std::vector<std::string> names, descs;
      names.push_back("t");
      descs.push_back("Time, s");
      for (const tio_stream *stream_ptr: merger.streams) {
        for (const column &c: stream_ptr->columns) {
          names.push_back(c.name);
          descs.push_back(c.desc);
        }
      }
      // Write first header line
      fprintf(merger.fp, "%s\n", tabjoin(names).c_str());
      // Write second header line with descriptions and units
      // Some tools expect just one line to label the data, so this is disabled:
      // fprintf(merger.fp, "%s\n", tabjoin(descs).c_str());
    }
  }

  // Process all the samples
  for (;;) {
    // Get the next sample, either from queue or from the input file.
    tl_data_stream_packet dsp;
    tl_packet *pkt = (tl_packet*)&dsp;
    if (queued_data.empty()) {
      if (tlrecv(fd, &dsp, sizeof(dsp)) != 0)
        break;
      if (tl_packet_stream_id(&pkt->hdr) < 0)
        continue;
    } else {
      dsp = queued_data.front();
      queued_data.pop_front();
    }

    // Get the corresponding stream
    tio_node *tn = get_node((tl_packet*)&dsp);
    auto it = tn->streams.find(tl_packet_stream_id(&pkt->hdr));
    if (it == tn->streams.end()) {
      uint8_t stream_id = tl_packet_stream_id(&pkt->hdr);
      tn->streams[stream_id].is_good = false;
      printf("Cannot find metadata, ignoring stream %s%d\n",
             tn->path.c_str(), stream_id);
      continue;
    }

    tio_stream &stream = it->second;
    if (!stream.is_good)
      continue;

    // Identify the sample number and time

    // Determine full 64 bit sample number.
    uint64_t sample =
      ((stream.info.sample_number >> 32)<<32) | dsp.start_sample;
    uint64_t sample_next =
      (((stream.info.sample_number >> 32)+1)<<32) | dsp.start_sample;
    int64_t d1 = sample - stream.info.sample_number;
    int64_t d2 = sample_next - stream.info.sample_number;
    if (std::abs(d1) > std::abs(d2))
      sample = sample_next;

    // Update latest sample number in metadata.
    stream.info.sample_number = sample;

    // Calculate sample time
    uint64_t secs = sample / stream.sps;
    double t =
      (sample - secs * stream.sps) * stream.sample_time +
      secs + stream.start_time;

    // Format sample and push it to the stream's map.
    std::vector<std::string> &sample_data = stream.samples[t];
    if (!sample_data.empty()) {
      printf("Duplicate sample at time %.6f for stream %s%d, keeping latest\n",
             t, tn->path.c_str(), it->first);
      sample_data.clear();
    }
    uint8_t *dataptr = dsp.data;
    for (column &c: stream.columns) {
      if ((sample % c.period) != 0) {
        sample_data.push_back("");
      } else {
        sample_data.push_back(c.parser(&dataptr));
      }
    }

    // Update row merger's earliest sample time if needed
    if (!std::isfinite(stream.merger->first_time) ||
        (t < stream.merger->first_time))
      stream.merger->first_time = t;

    // Output the next row as long as the current sample time exceeds the
    // earliest sample time by at least DELTA_T.
    tio_row_merger &merger = *stream.merger;
    while (t > (merger.first_time + DELTA_T))
      merger.write_next_row();
  }

  // Finish writing out all the rows and close the files cleanly.
  for (auto kv: mergers) {
    tio_row_merger &merger = kv.second;
    while (std::isfinite(merger.first_time))
      merger.write_next_row();
    fclose(merger.fp);
  }

  return 0;
}
