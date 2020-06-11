// Copyright: 2020 Twinleaf LLC
// Author: gilberto@tersatech.com
// License: MIT

#include "tio/data.h"
#include "tio/io.h"

#include <string.h>
#include <cmath>
#include <inttypes.h>

#include <string>
#include <sstream>
#include <map>
#include <vector>
#include <functional>

// Work In Progress

struct tio_stream {
  tl_stream_info info;
  std::vector<tl_stream_component_info> components;
  std::vector<tl_data_stream_packet> data;
};

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
  char fmtbuf[64];
  strcpy(fmtbuf, "NaN");
  if (tio_type == TL_DATA_TYPE_FLOAT32) {
    float val;
    memcpy(&val, dataptr, sizeof(val));
    sprintf(fmtbuf, "%f", val);
  } else if (tio_type == TL_DATA_TYPE_FLOAT64) {
    double val;
    memcpy(&val, dataptr, sizeof(val));
    sprintf(fmtbuf, "%f", val);
  } else {
    uint8_t buf[sizeof(uint64_t)];
    size_t size = tio_type >> 4;
    if (size > sizeof(buf))
      throw "Unexpected size";
    memcpy(buf, dataptr, size);
    memset(buf+size, 0, sizeof(buf)-size);
    if (tio_type & 1) {
      // unsigned
      uint64_t val;
      memcpy(&val, buf, sizeof(val));
      sprintf(fmtbuf, "%" PRIu64, val);
    } else {
      // signed
      int64_t val;
      memcpy(&val, buf, sizeof(val));
      // sign extend
      val = (val << ((sizeof(buf)-size)*8)) >> ((sizeof(buf)-size)*8);
      sprintf(fmtbuf, "%" PRId64, val);
    }
  }
  return std::string(fmtbuf);
};

int main(int argc, char *argv[])
{
  if (argc != 2)
    return 1;

  int fd = -1;
  {
    std::string url = "file://";
    url += argv[1];
    fd = tlopen(url.c_str(), 0, NULL);
  }

  for (;;) {
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
        tio_node *tn = get_node(&pkt);
        const tl_data_stream_packet *dsp =
          reinterpret_cast<tl_data_stream_packet*>(&pkt);
        tn->streams[id].data.push_back(*dsp);
      }
    }
  }

  // map: column->(map:time->data)
  using dataparser = std::function<std::string(uint8_t**)>;
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
    std::map<double, std::string> data;
    dataparser parser;
    uint32_t period;
  };
  using tbmap = std::map<std::string, std::vector<column>>;
  tbmap tables;
  char fmtbuf[64];

  for (auto const& [route, tn]: nodes) {
    (void) route;
    std::string addr_prefix = tn.path;

    for (auto sit: tn.streams) {
      auto stream_id = sit.first;
      auto &stream = sit.second;

      // determine column map
      auto tbit = tn.timebases.find(stream.info.timebase_id);
      if (tbit == tn.timebases.end()) {
        printf("Cannot find metadata for Timebase %d, ignoring stream %s%d\n",
               stream.info.timebase_id, addr_prefix.c_str(), stream_id);
        continue;
      }
      uint64_t sps =
        1000000ull * tbit->second.period_denom_us / tbit->second.period_num_us;
      sps /= stream.info.period;
      double frac = 1.0/sps;
      double start_time = tbit->second.start_time * 1e-9;

      std::string tbid;
      if (tbit->second.source == TL_TIMEBASE_SRC_GLOBAL) {
        if (tbit->second.epoch == TL_TIMEBASE_EPOCH_UNIX)
          tbid = "unix";
        else if (tbit->second.epoch == TL_TIMEBASE_EPOCH_UNIX)
          tbid = "invalid"; // TODO
      } else {
        for (size_t i = 0; i < sizeof(tbit->second.source_id); i++) {
          sprintf(fmtbuf, "%02X", tbit->second.source_id[i]);
          tbid += fmtbuf;
        }
      }

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
        // TODO: validate that number of string components/units match
        if (si.channels == 1) {
          colvec.push_back(column(addr_prefix + name, desc + ", " + units[0],
                                  si.type, comp.period));
        } else {
          for (size_t i = 0; i < si.channels; i++) {
            std::string units_str = units[0];
            if (units.size() > 1)
              units_str = units[i];
            colvec.push_back(column(addr_prefix + name + "." + channel_names[i],
                                    desc + ", " + units_str,
                                    si.type, comp.period));
          }
        }
      }
      if (skip)
        continue;

      std::vector<column> &columns = tables[tbid];
      size_t start_offset = columns.size();
      columns.insert(columns.end(), colvec.begin(), colvec.end());

      for (tl_data_stream_packet &dsp: stream.data) {
        // Determine sample number (64 bit wraparound)
        // Some times we can see the streaminfo a bit out of sync with the
        // packet, so find the closest 
        uint64_t sample =
          ((stream.info.sample_number >> 32)<<32) | dsp.start_sample;
        uint64_t sample1 =
          (((stream.info.sample_number >> 32)+1)<<32) | dsp.start_sample;
        int64_t d1 = sample - stream.info.sample_number;
        int64_t d2 = sample1 - stream.info.sample_number;
        if (std::abs(d1) > std::abs(d2))
          sample = sample1;

        stream.info.sample_number = sample;
        // Calculate time
        uint64_t secs = sample / sps;
        double t = (sample - secs*sps)*frac + secs + start_time;

        uint8_t *dataptr = dsp.data;
        for (size_t i = start_offset; i < columns.size(); i++) {
          if ((sample % columns[i].period) != 0)
            continue;
          columns[i].data[t] = columns[i].parser(&dataptr);
        }
      }
    }
  }

  for (auto & [tbid, columns]: tables) {
    std::string filename = std::string(argv[1]) + "." + tbid + ".tsv";
    FILE *fp = fopen(filename.c_str(), "w");

    if (!fp)
      exit(1);

    // Construct array of iterators for all data maps
    std::vector<std::map<double, std::string>::iterator> its;
    std::vector<std::string> header_names, header_descs;
    header_names.push_back("t");
    header_descs.push_back("Time, s");
    for (auto & col: columns) {
      header_names.push_back(col.name);
      header_descs.push_back(col.desc);
      its.push_back(col.data.begin());
    }

    fprintf(fp, "%s\n", tabjoin(header_names).c_str());
    fprintf(fp, "%s\n", tabjoin(header_descs).c_str());

    for (;;) {
      double min_time = INFINITY;
      for (size_t i = 0; i < its.size(); i++) {
        if (its[i] == columns[i].data.end())
          continue;
        if (its[i]->first < min_time)
          min_time = its[i]->first;
      }
      if (!std::isfinite(min_time))
        break;
      std::vector<std::string> row;
      sprintf(fmtbuf, "%.6f", min_time);
      row.push_back(fmtbuf);
      min_time += 1e-5; // 10us sample time epsilon
      for (size_t i = 0; i < its.size(); i++) {
        if ((its[i] != columns[i].data.end()) && (its[i]->first < min_time)) {
          row.push_back(its[i]->second);
          ++(its[i]);
        } else {
          row.push_back("");
        }
      }
      fprintf(fp, "%s\n", tabjoin(row).c_str());
    }
  }

  return 0;
}
