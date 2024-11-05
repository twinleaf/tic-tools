// Copyright: 2016-2021 Twinleaf LLC
// License: MIT

#include <tio/io.h>
#include <tio/packet.h>
#include <tio/log.h>
#include <tio/rpc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <poll.h>
#include <sysexits.h>

#ifndef WEBSOCKETS
#define WEBSOCKETS 0
#endif

#if WEBSOCKETS
#include <openssl/sha.h>
#include <openssl/evp.h>
#endif

#define MAX_CLIENTS_DEFAULT 64
#define MAX_RPCS_DEFAULT 64

#if defined (__linux__)
// For some reason, at least on some linux systems there is no declaration
// of ppoll, even defining _GNU_SOURCE. Provide it here, since having it
// twice should not hurt.
int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo_p,
          const sigset_t *sigmask);
#elif defined(__APPLE__)
// Apple does not provide ppoll. For this specific use, it is ok to
// have the signal masking race condition, so we do a straightforward
// implementation (with race). Since we timeout every second, the
// worst case scenario is that there is a one second lag between
// receiving SIGINT and exiting the main loop
static int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo_p,
                 const sigset_t *sigmask)
{
  unsigned msec = tmo_p->tv_sec * 1000 + (tmo_p->tv_nsec + 999999)/1000000;
  sigset_t restore;
  if (sigprocmask(SIG_SETMASK, sigmask, &restore) != 0)
    return -1;
  int ret = poll(fds, nfds, msec);
  int errno_ret = errno;
  if (sigprocmask(SIG_SETMASK, &restore, NULL) != 0)
    return -1;
  errno = errno_ret;
  return ret;
}
#endif

#define CLIENT_MODE_SHARED  0
#define CLIENT_MODE_FORWARD 1

int client_mode = CLIENT_MODE_SHARED;

#define SENSOR_MODE_DIRECT  0
#define SENSOR_MODE_HUB     1

int sensor_mode = SENSOR_MODE_DIRECT;

volatile sig_atomic_t keep_running = 1;

void terminate_loop_on_signal(int sig)
{
  (void) sig;
  keep_running = 0;
}

#define QUOTE(str) #str
#define EXPAND_AND_QUOTE(str) QUOTE(str)
const char *service_port = EXPAND_AND_QUOTE(TL_TCP_DEFAULT_PORT);

const char *hub_name = "TIO PROXY";
char hub_id[128] = "";

size_t n_sensors = 0;
size_t n_listen = 0;
size_t n_descriptors = 0;
size_t max_descriptors = 0;

const char **sensor_url = NULL;
int sensor_reconnect_timeout = 60;
struct timespec last_reconnect_attempt;

struct pollfd *poll_array = NULL;
uint32_t *descriptor_flags = NULL;
#define WEBSOCKET_PORT         1 // server flag: websocket port
#define WEBSOCKET_HANDSHAKE    1 // client flag: handshake hasn't happened yet

#if WEBSOCKETS
const char *websock_port = EXPAND_AND_QUOTE(TL_WS_DEFAULT_PORT);
#endif

int disconnected_clients_flag = 0;

struct rpc_remap {
  struct rpc_remap *next, *prev;
  struct rpc_remap *to_next, *to_prev;
  time_t send_time;
  int client_desc; // index of client in poll_array
  uint16_t id;
  uint16_t orig_id;
  int routing_size; // keep routing to send out timeout messages
  uint8_t routing[TL_PACKET_MAX_ROUTING_SIZE];
};
typedef struct rpc_remap rpc_remap;

size_t max_rpcs_in_flight = MAX_RPCS_DEFAULT;
rpc_remap *remap_array;
rpc_remap *client_list;
rpc_remap orphan_list;
rpc_remap timeout_list; // circular list of timeouts

int verbose = 0;
const char *timefmt = "%F %T";
int timestamp_us = 0;

int usage(FILE *out, const char *program, const char *error)
{
  if (error)
    fprintf(out, "%s\n", error);
  fprintf(out, "Usage: %s [-p port] [-f] [-c max_clients] [-r max_rpc] [-v] "
          "[-h [-i hub_id]] [-t timefmt] sensor_url [sensor_url ...]\n",
          program);
  fprintf(out, "  -p port   TCP listen port. default 7855\n");
  fprintf(out, "  -w port   WebSocket listen port. default 7853\n");
  fprintf(out, "  -f        client forward mode\n");
  fprintf(out, "  -c max    max simultaneous clients in shared mode, "
          "default %d\n", MAX_CLIENTS_DEFAULT);
  fprintf(out, "  -r max    max number of RPCs in flight in shared mode, "
          "default %d\n", MAX_RPCS_DEFAULT);
  fprintf(out, "  -h        hub sensor mode\n");
  fprintf(out, "  -i id     id of the hub\n");
  fprintf(out, "  -v        verbose logging\n");
  fprintf(out, "  -4        force IPv4 server only\n");
  fprintf(out, "  -t fmt    timestamp format (default \"%%F %%T\", "
          "see man strftime)\n");
  fprintf(out, "  -u        append microseconds to timestamp\n");
  fprintf(out, "  -T sec    seconds to auto-reconnect a sensor before "
          "exiting (default 60)\n");
  return EX_USAGE;
}

int error(const char *fmt, ...)
{
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  fprintf(stderr, "%s", msg);
  if (errno)
    fprintf(stderr, ": %s\n", strerror(errno));
  else
    fprintf(stderr, "\n");
  return EXIT_FAILURE;
}

// Log a message to terminal, prefixed with a timestamp
void logmsg(const char *fmt, ...)
{
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  struct tm tm;
  localtime_r(&now.tv_sec, &tm);
  char timebuf[128];
  if (strftime(timebuf, sizeof(timebuf), timefmt, &tm) == 0)
    timebuf[0] = '\0';
  if (timestamp_us)
    printf("%s.%06d  ", timebuf, (int)(now.tv_nsec/1000));
  else
    printf("%s  ", timebuf);
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
}

// As above, but will only display if given verbose flag
#define logmsgverbose(...) { if (verbose) logmsg(__VA_ARGS__); }

void io_log(int fd, const char *message)
{
  // Send messages from the IO layer to log
  logmsgverbose("IO fd #%d message: %s", fd, message);
}

// State dump for debugging
void dump_state(void)
{
  printf("** BEGIN STATE DUMP **\n");
  printf("Remap array:\n");
  for (size_t i = 0; i <= max_rpcs_in_flight; i++) {
    rpc_remap *remap = &remap_array[i];
    printf("%zd(%p): <%p:%p> <%p:%p> %ld %d %d %d\n",
           i, remap, remap->prev, remap->next, remap->to_prev, remap->to_next,
           remap->send_time, remap->client_desc, remap->id, remap->orig_id);
  }
  printf("Client lists:\n");
  for (size_t i = 0; i < max_descriptors; i++) {
    rpc_remap *remap = &client_list[i];
    printf("%zd(%p): <%p:%p> <%p:%p> %ld %d %d %d\n",
           i, remap, remap->prev, remap->next, remap->to_prev, remap->to_next,
           remap->send_time, remap->client_desc, remap->id, remap->orig_id);
  }
  // TODO: add more stuff if needed
  printf("** END STATE DUMP **\n");
}

void init_remap_struct(rpc_remap *remap, rpc_remap *prev, rpc_remap *next)
{
  remap->prev = prev;
  remap->next = next;
  remap->to_prev = NULL;
  remap->to_next = NULL;
  remap->send_time = 0;
  remap->client_desc = -1;
  remap->id = 0xFFFF;
  remap->orig_id = 0xFFFF;
  remap->routing_size = 0;
}

void init_rpc_remap()
{
  remap_array = calloc(max_rpcs_in_flight + 1, sizeof(rpc_remap));
  if (!remap_array)
    exit(error("No memory for rpc translation lists"));
  for (size_t i = 0; i < (max_rpcs_in_flight + 1); i++) {
    init_remap_struct(&remap_array[i], i ?  &remap_array[i-1] : NULL,
                      (i < max_rpcs_in_flight) ?  &remap_array[i+1] : NULL);
    if (i > 0)
      remap_array[i].id = i-1;
  }

  client_list = calloc(max_descriptors, sizeof(rpc_remap));
  if (!client_list)
    exit(error("No memory for rpc translation lists"));
  for (size_t i = 0; i < max_descriptors; i++)
    init_remap_struct(&client_list[i], NULL, NULL);

  init_remap_struct(&orphan_list, NULL, NULL);
  init_remap_struct(&timeout_list, NULL, NULL);
  timeout_list.to_next = &timeout_list;
  timeout_list.to_prev = &timeout_list;
}

void insert_after(rpc_remap *list_element, rpc_remap *to_insert)
{
  to_insert->prev = list_element;
  to_insert->next = list_element->next;
  list_element->next = to_insert;
  if (to_insert->next)
    to_insert->next->prev = to_insert;
}

// also removes from timeout list
rpc_remap *remove_next(rpc_remap *list_element, int remove_timeout)
{
  if (!list_element || !list_element->next)
    return NULL;

  rpc_remap *ret = list_element->next;
  list_element->next = ret->next;
  if (list_element->next)
    list_element->next->prev = list_element;
  ret->prev = ret->next = NULL;

  if (remove_timeout && ret->to_prev) {
    ret->to_prev->to_next = ret->to_next;
    ret->to_next->to_prev = ret->to_prev;
    ret->to_next = ret->to_prev = NULL;
  }
  return ret;
}

void append_timeout(rpc_remap *remap, time_t send_time)
{
  if (remap->to_prev) {
    // should never happen.
    logmsg("Critical error: remapping already in timeout list");
    exit(1);
  }
  remap->to_prev = timeout_list.to_prev;
  remap->to_next = &timeout_list;
  remap->to_prev->to_next = remap;
  remap->to_next->to_prev = remap;
  remap->send_time = send_time;
}

rpc_remap *get_timedout(time_t t)
{
  if (timeout_list.to_next == &timeout_list)
    return NULL;

  rpc_remap *ret = timeout_list.to_next;
  if ((ret->send_time + 5) >= t)
    return NULL;

  ret->to_prev->to_next = ret->to_next;
  ret->to_next->to_prev = ret->to_prev;
  ret->to_next = ret->to_prev = NULL;

  return ret;
}

void disconnect_client(size_t ps)
{
  // close the descriptor
  tlclose(poll_array[ps].fd);
  logmsgverbose("Disconnected client #%d", poll_array[ps].fd);
  poll_array[ps].fd = -1;
  // invalidate all of the client's RPCs in shared mode
  if (client_mode == CLIENT_MODE_SHARED) {
    for (rpc_remap *rpc = NULL; (rpc = remove_next(&client_list[ps], 0));) {
      rpc->client_desc = -1;
      insert_after(&orphan_list, rpc);
    }
  }
  // and flag that a client was disconnected, to compactify the poll array
  disconnected_clients_flag = 1;
}

int set_nonblock_cloexec(int fd)
{
  if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
    return -1;
  int flags = fcntl(fd, F_GETFL);
  if (flags == -1)
    return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// sends a packet to an entity, setting up writeable notifications if needed.
// return -1 on error that would require closing the connection, 0 on success,
// 1 if the packet was not sent because we were out of buffer space.
int send_packet(size_t ps, tl_packet *packet)
{
  int ret = tlsend(poll_array[ps].fd, packet);
  if (ret == 0)
    return 0;

  if ((errno == EOVERFLOW) || (errno == ENOTEMPTY))
    poll_array[ps]. events |= POLLOUT;
  if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == ENOTEMPTY))
    return 1;
  if (errno == EOVERFLOW)
    return 0;
  return -1;
}

#define SUCCESS          0
#define ERROR_LOCAL     -1
#define ERROR_CRITICAL  -2

// process a packet sent by client 'ps' to the proxy in hub mode
int hub_packet(size_t ps, tl_packet *packet)
{
  if (packet->hdr.type == TL_PTYPE_RPC_REQ) {
    tl_rpc_request_packet *req = (tl_rpc_request_packet*) packet;
    size_t method_size = tl_rpc_request_method_size(req);
#define METHOD(x)                                                       \
    ((strlen(x) == method_size) && (memcmp(x, req->payload, method_size) == 0))
    if (METHOD("dev.desc")) {
      tl_rpc_reply_packet *rep = tl_rpc_make_reply(req);
      size_t len = strlen(hub_name);
      if (len > TL_RPC_REPLY_MAX_PAYLOAD_SIZE)
        len = TL_RPC_REPLY_MAX_PAYLOAD_SIZE;
      memcpy(rep->payload, hub_name, len);
      rep->hdr.payload_size += len;
    } else if (METHOD("dev.proc.id")) {
      tl_rpc_reply_packet *rep = tl_rpc_make_reply(req);
      size_t len = strlen(hub_id);
      if (len > TL_RPC_REPLY_MAX_PAYLOAD_SIZE)
        len = TL_RPC_REPLY_MAX_PAYLOAD_SIZE;
      memcpy(rep->payload, hub_id, len);
      rep->hdr.payload_size += len;
    } else if (METHOD("dev.ports")) {
      tl_rpc_reply_packet *rep = tl_rpc_make_reply(req);
      *(uint32_t*)&rep->payload[0] = n_sensors;
      rep->hdr.payload_size += sizeof(uint32_t);
    } else {
#undef METHOD
      tl_rpc_make_error(req, TL_RPC_ERROR_NOTFOUND);
    }
    if (send_packet(ps, packet) < 0)
      return ERROR_LOCAL;
  } else {
    logmsg("Ignoring packet of type %u sent to hub by client#%d",
           packet->hdr.type, poll_array[ps].fd);
  }
  return SUCCESS;
}

// process data incoming from sensor 'ps'
int sensor_data(size_t ps, tl_packet *packet)
{
  size_t client_start = n_sensors + n_listen;
  size_t client_end = n_descriptors;

  if (((packet->hdr.type == TL_PTYPE_RPC_REP) ||
       (packet->hdr.type == TL_PTYPE_RPC_ERROR)) &&
      (client_mode == CLIENT_MODE_SHARED)) {
    // Remap RPC to original one. either packet type is fine to access the id
    tl_rpc_reply_packet *rep = (tl_rpc_reply_packet*) packet;
    if (rep->rep.req_id >= max_rpcs_in_flight) {
      // don't want to crash if there is a misbehaving sensor
      logmsg("Unexpected returned rpc id, cannot remap");
      return SUCCESS;
    }
    rpc_remap *remap = remove_next(remap_array[rep->rep.req_id + 1].prev, 1);
    if (!remap) {
      logmsg("Cannot find remapping information for rpc %u", rep->rep.req_id);
      return ERROR_CRITICAL;
    }
    if (remap->client_desc >= 0) {
      // the client that placed the RPC is still connected
      rep->rep.req_id = remap->orig_id;
      client_start = remap->client_desc;
      client_end = client_start + 1;
    }
    insert_after(&remap_array[0], remap);
  }

  // If in hub mode, add back routing
  if (sensor_mode == SENSOR_MODE_HUB) {
    // in hub mode, we must add the last hop
    size_t routing_size = tl_packet_routing_size(&packet->hdr);
    if (routing_size >= TL_PACKET_MAX_ROUTING_SIZE) {
      // too deep in the sensor tree. don't break because of this
      logmsg("Warning: dropped sensor packet. Full routing in hub mode");
      return SUCCESS;
    }

    uint8_t *routing = tl_packet_routing_data(packet);
    routing[routing_size++] = ps;
    tl_packet_set_routing_size(&packet->hdr, routing_size);
  }

  if (packet->hdr.type == TL_PTYPE_LOG) {
    tl_log_packet *logp = (tl_log_packet*) packet;
    char path[TL_ROUTING_FMT_BUF_SIZE];
    if (tl_format_routing(tl_packet_routing_data(&packet->hdr),
                          tl_packet_routing_size(&packet->hdr),
                          path, sizeof(path), 1) != 0)
      strcpy(path, "<INVALID PATH>");
    size_t len = tl_log_packet_message_size(logp);
    const char *type = "UNKNOWN";
    switch(logp->log.level) {
     case TL_LOG_CRITICAL: type = "CRITICAL"; break;
     case TL_LOG_ERROR: type = "ERROR"; break;
     case TL_LOG_WARNING: type = "WARNING"; break;
     case TL_LOG_INFO: type = "INFO"; break;
     case TL_LOG_DEBUG: type = "DEBUG"; break;
    }
    logmsg("%s %s: %.*s", path, type, len, logp->message);
  }

  if (packet->hdr.type == TL_PTYPE_TEXT) {
    logmsg("TEXT (%zd bytes): %.*s", packet->hdr.payload_size,
	   packet->hdr.payload_size, (const char*)packet->payload);
    // automatically send out heartbeat to switch to binary mode
    tl_packet_header heartbeat = { TL_PTYPE_HEARTBEAT, 0, 0 };
    send_packet(ps, (struct tl_packet*) &heartbeat);
  }

  for (size_t i = client_start; i < client_end; i++) {
    if (poll_array[i].fd < 0) continue;
    errno = 0;
    if (send_packet(i, packet) < 0) {
      if ((errno != EPIPE) && (errno != ECONNRESET))
        logmsg("Failed to send sensor packet to client #%d [%s]",
               poll_array[i].fd, strerror(errno));
      disconnect_client(i);
    }
  }

  return SUCCESS;
}

// Closes a sensor and sets things up for automatic reconnection
void close_sensor(int sensor)
{
  tlclose(poll_array[sensor].fd);
  poll_array[sensor].fd = -1;
  if (sensor_reconnect_timeout > 0) {
    clock_gettime(CLOCK_REALTIME, &last_reconnect_attempt);
    last_reconnect_attempt.tv_sec += sensor_reconnect_timeout;
  }
}

// Process packets from clients
int client_data(size_t ps, tl_packet *packet)
{
  if ((sensor_mode == SENSOR_MODE_HUB) &&
      (tl_packet_routing_size(&packet->hdr) == 0)) {
    // This packet is for the proxy. handle and reply
    return hub_packet(ps, packet);
  }

  if ((client_mode == CLIENT_MODE_SHARED) &&
      (packet->hdr.type == TL_PTYPE_RPC_REQ)) {
    // translate RPC request IDs to avoid conflicts, and set up to collect
    // the remapping if the call times out.
    tl_rpc_request_packet *req = (tl_rpc_request_packet*) packet;
    rpc_remap *remap = remove_next(&remap_array[0], 0);
    if (!remap) {
      logmsg("Could not remap rpc %u from client #%d, out of buffers",
             req->req.id, poll_array[ps].fd);
      // courtesy reply, send an error to the caller
      uint8_t routing_size, routing[TL_PACKET_MAX_ROUTING_SIZE];
      routing_size = tl_packet_routing_size(&req->hdr);
      memcpy(routing, tl_packet_routing_data(&req->hdr), routing_size);
      tl_rpc_make_error(req, TL_RPC_ERROR_BUSY);
      memcpy(tl_packet_routing_data(&req->hdr), routing, routing_size);
      tl_packet_set_routing_size(&req->hdr, routing_size);
      if (send_packet(ps, packet) < 0) {
        logmsg("Failed to send back error of too many rpcs in flight");
        return ERROR_LOCAL;
      } else {
        return SUCCESS; // of sorts :)
      }
    }

    logmsgverbose("Remapping client #%d rpc %u to %u",
           poll_array[ps].fd, req->req.id, remap->id);
    remap->orig_id = req->req.id;
    req->req.id = remap->id;
    remap->client_desc = ps;
    remap->routing_size = tl_packet_routing_size(&req->hdr);
    memcpy(remap->routing, tl_packet_routing_data(&req->hdr),
           remap->routing_size);
    insert_after(&client_list[ps], remap);
    append_timeout(remap, time(NULL));
  }

  // Forward packet to the right sensor. In direct mode, there is only
  // one of them at offset zero. In hub mode, need to get address from
  // routing.
  size_t dest = 0;
  if (sensor_mode == SENSOR_MODE_HUB) {
    size_t routing_size = tl_packet_routing_size(&packet->hdr);
    dest = tl_packet_routing_data(&packet->hdr)[--routing_size];
    tl_packet_set_routing_size(&packet->hdr, routing_size);
  }

  if (dest >= n_sensors) {
    // client is trying to reach an invalid sensor, just ignore packet
    // just like if the sensor was "valid" but not plugged in. RPC remap
    // will timeout if any
    logmsg("Client #%d attempted to access invalid sensor %zd",
           poll_array[dest].fd, dest);
    return SUCCESS;
  }

  int ret = 1;
  if (poll_array[dest].fd >= 0) {
    ret = send_packet(dest, packet);
    if (ret < 0) {
      logmsg("Error writing to sensor %zd: %s", dest, strerror(errno));
      if (sensor_reconnect_timeout == 0)
        return ERROR_CRITICAL;
      close_sensor(dest);
    }
  }
  if (ret != 0) {
    logmsg("Packet dropped from client #%d to sensor %zd",
           poll_array[dest].fd, dest);
  }

  return SUCCESS;
}

// Return 0 on success, -1 on error
int handle_tlio(size_t ps)
{
  errno = 0;
  if (poll_array[ps].revents & POLLERR)
    return ERROR_LOCAL;

  if (poll_array[ps].revents & POLLOUT) {
    // Sensor or client was backed up, and we buffered up a partial packet.
    // Now we can write again, so try to send it out.
    poll_array[ps].events &= ~POLLOUT;
    if (send_packet(poll_array[ps].fd, NULL) < 0)
      return ERROR_LOCAL;
  }

  if (poll_array[ps].revents & POLLIN) {
    // Sensor or client sent some data
    for (;;) {
      tl_packet packet;
      errno = 0;
      int ret = tlrecv(poll_array[ps].fd, &packet, sizeof(packet));
      if (ret < 0) {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
          break;
        if (errno == 0)
          logmsgverbose("Detected client #%d disconnect", poll_array[ps].fd);
        return ERROR_LOCAL;
      }

      ret = (ps < n_sensors) ?
        sensor_data(ps, &packet) : client_data(ps, &packet);
      if (ret != 0)
        return ret;
    }
  }

  return SUCCESS;
}

#if WEBSOCKETS
int handle_websock(size_t ps)
{
  errno = 0;
  if (poll_array[ps].revents & POLLERR)
    return ERROR_LOCAL;

  // HACK: Right now assume the request comes in a single packet, or fast
  // enough that we can read it all in one go.
  char buf[2048+1];
  int ret = read(poll_array[ps].fd, buf, sizeof(buf)-1);
  if (ret <= 0)
    return ERROR_LOCAL;

  buf[ret] = '\0';
  const char *key = NULL;
  int status = 0;
  const char *line = buf;

  for (;;) {
    char *end = strstr(line, "\r\n");
    if (!end) {
      status = -1;
      break;
    }
    *end = '\0';
    if ((end-line) == 0)
      break;
    if (strcmp(line, "Upgrade: websocket") == 0)
      status = 1;
    if (strncmp(line, "Sec-WebSocket-Key:", 18) == 0)
      key = line + 19;
    line = end + 2;
  }

  if ((status != 1) || !key) {
    close(poll_array[ps].fd);
    return ERROR_LOCAL;
  }

  EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
  unsigned char sha_hash[SHA_DIGEST_LENGTH];
  if (!mdctx) {
    close(poll_array[ps].fd);
    return ERROR_LOCAL;
  }

  int success =
    EVP_DigestInit_ex(mdctx, EVP_sha1(), NULL) &&
    EVP_DigestUpdate(mdctx, key, strlen(key)) &&
    EVP_DigestUpdate(mdctx, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36) &&
    EVP_DigestFinal_ex(mdctx, sha_hash, NULL);
  EVP_MD_CTX_free(mdctx);
  if (!success) {
    close(poll_array[ps].fd);
    return ERROR_LOCAL;
  }

  unsigned char hash64[(sizeof(sha_hash)+2)/3*4+1];
  EVP_EncodeBlock(hash64, sha_hash, sizeof(sha_hash));

  snprintf(buf, sizeof(buf),
           "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: %s\r\n\r\n",
           hash64);

  if (write(poll_array[ps].fd, buf, strlen(buf)) != (ssize_t) strlen(buf)) {
    close(poll_array[ps].fd);
    return ERROR_LOCAL;
  }

  tlfdopen(poll_array[ps].fd, "ws", NULL, &io_log);

  descriptor_flags[ps] &= ~WEBSOCKET_HANDSHAKE;

  return SUCCESS;
}
#endif

// Client waiting to connect on server socket. Return error if there are
// errors with listening sockets, but not if there are errors with
// new clients.
int client_connection(size_t ps)
{
  for (;;) {
    struct sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    int client_fd = accept(poll_array[ps].fd, (struct sockaddr*)&sa, &len);
    if (client_fd < 0) {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
        return SUCCESS;
      else
        return ERROR_CRITICAL;
    }

    if (set_nonblock_cloexec(client_fd) != 0) {
      logmsg("Failed to set client socket flags");
      close(client_fd);
      continue;
    }

    char host[128];
    char port[128];
    int ret = getnameinfo((struct sockaddr*)&sa, len, host, sizeof(host),
                          port, sizeof(port), NI_NUMERICSERV);
    if (ret != 0) {
      logmsg("Failed to getnameinfo for new client (%d)", ret);
      close(client_fd);
      continue;
    }

    // Make sure we have enough space for this client
    if (n_descriptors >= max_descriptors) {
      logmsg("Accepting client (%s:%s) will exceed maximum number of clients",
             host, port);
      close(client_fd);
      continue;
    }

    int tlfd = client_fd;
    if (!(descriptor_flags[ps] & WEBSOCKET_PORT)) {
      tlfd = tlfdopen(client_fd, "tcp", NULL, &io_log);
      if (tlfd < 0) {
        logmsg("Failed to open new client (%s:%s) in libtio: %s",
               host, port, strerror(errno));
        close(client_fd);
        continue;
      }
    }

    poll_array[n_descriptors].fd = tlfd;
    poll_array[n_descriptors].events = POLLIN;
    descriptor_flags[n_descriptors] = 0;
    if (client_list)
      init_remap_struct(&client_list[n_descriptors], NULL, NULL);
    if (descriptor_flags[ps] & WEBSOCKET_PORT)
      descriptor_flags[n_descriptors] |= WEBSOCKET_HANDSHAKE;
    n_descriptors++;

    logmsgverbose("Accepted client #%d: %s:%s", tlfd, host, port);
  }
}

int setup_listening_sock(struct addrinfo *i)
{
  int sock = socket(i->ai_family, i->ai_socktype, i->ai_protocol);
  if (sock < 0)
    return error("Failed to open listening socket");
  int on = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
  bind(sock, i->ai_addr, i->ai_addrlen);
  listen(sock, 32);
  if (set_nonblock_cloexec(sock) != 0)
    return error("Failed to set listening socket flags");
  poll_array[n_descriptors].fd = sock;
  poll_array[n_descriptors].events = POLLIN;
  return 0;
}

int main(int argc, char *argv[])
{
  struct addrinfo ai;
  size_t max_clients = MAX_CLIENTS_DEFAULT;
  errno = 0;
  memset(&ai, 0, sizeof(ai));
  ai.ai_family = AF_UNSPEC;

  for (int opt = -1; (opt = getopt(argc, argv,
                                   "fhv4up:w:c:r:i:t:T:")) != -1; ) {
    if (opt == 'f') {
      client_mode = CLIENT_MODE_FORWARD;
    } else if (opt == 'h') {
      sensor_mode = SENSOR_MODE_HUB;
    } else if (opt == 'p') {
      service_port = optarg;
#if WEBSOCKETS
    } else if (opt == 'w') {
      websock_port = optarg;
#endif
    } else if (opt == 'c') {
      max_clients = strtoul(optarg, NULL, 0);
      if (max_clients == 0)
        return usage(stderr, argv[0], "Must allow at least one client");
    } else if (opt == 'r') {
      max_rpcs_in_flight = strtoul(optarg, NULL, 0);
      if (max_rpcs_in_flight > 0xFFFF)
        max_rpcs_in_flight = 0xFFFF;
    } else if (opt == 'i') {
      strncpy(hub_id, optarg, sizeof(hub_id) - 1);
      hub_id[sizeof(hub_id) - 1] = '\0';
    } else if (opt == 'v') {
      verbose = 1;
    } else if (opt == '4') {
      ai.ai_family = AF_INET;
    } else if (opt == 't') {
      timefmt = optarg;
    } else if (opt == 'u') {
      timestamp_us = 1;
    } else if (opt == 'T') {
      sensor_reconnect_timeout = atoi(optarg);
    } else {
      return usage(stderr, argv[0], "Invalid command line option");
    }
  }

  if (client_mode == CLIENT_MODE_FORWARD)
    max_clients = 1;

  n_sensors = argc - optind;
  sensor_url = (const char**) (argv + optind);

  if (n_sensors == 0)
    return usage(stderr, argv[0], "No sensors specified");

  if ((sensor_mode == SENSOR_MODE_DIRECT) && (n_sensors != 1))
    return usage(stderr, argv[0], "Only one sensor allowed in direct mode");

  if (n_sensors > 255)
    return usage(stderr, argv[0], "Exceeded protocol limit of 255 sensors");

  // assign a default ID to a hub
  if ((sensor_mode == SENSOR_MODE_HUB) && (hub_id[0] == '\0')) {
    char host[64];
    if (gethostname(host, sizeof(host) - 1) == -1) {
      if (errno == ENAMETOOLONG)
        host[sizeof(host) - 1] = '\0';
      else
        return error("Failed to get host name");
    }
    snprintf(hub_id, sizeof(hub_id), "%s.%d", host, getpid());
  }

  // Initialize the service sockets (there are usually two, for IPv4 and IPv6)
  ai.ai_flags = AI_ADDRCONFIG | AI_PASSIVE;
  ai.ai_socktype = SOCK_STREAM;
  ai.ai_protocol = IPPROTO_TCP;
  struct addrinfo *result;
  if (getaddrinfo(NULL, service_port, &ai, &result) != 0)
    return error("Failed to get listening address info");

  for (struct addrinfo *i = result; i; i = i->ai_next)
    n_listen++;

#if WEBSOCKETS
  struct addrinfo ai_ws = ai;
  struct addrinfo *result_ws;
  if (getaddrinfo(NULL, websock_port, &ai_ws, &result_ws) != 0)
    return error("Failed to get websocket listening address info");

  for (struct addrinfo *i = result_ws; i; i = i->ai_next)
    n_listen++;
#endif

  if (n_listen == 0)
    return error("No listening sockets configurations available");

  max_descriptors = n_sensors + n_listen + max_clients;
  poll_array = calloc(max_descriptors, sizeof(struct pollfd));
  if (!poll_array)
    return error("Failed to allocate poll array");
  descriptor_flags = calloc(max_descriptors, sizeof(*descriptor_flags));
  if (!descriptor_flags)
    return error("Failed to allocate descriptor flags");

  // Connect to all sensors
  for (n_descriptors = 0; n_descriptors < n_sensors; n_descriptors++) {
    const char *url = sensor_url[n_descriptors];
    poll_array[n_descriptors].fd = tlopen(url, O_NONBLOCK|O_CLOEXEC, &io_log);
    poll_array[n_descriptors].events = POLLIN;
    if (poll_array[n_descriptors].fd < 0)
      return error("Failed to open sensor '%s'", url);
  }

  // Set up listening sockets
  for (struct addrinfo *i = result; i; i = i->ai_next, n_descriptors++) {
    int ret = setup_listening_sock(i);
    if (ret) return ret;
  }
  freeaddrinfo(result);

#if WEBSOCKETS
  for (struct addrinfo *i = result_ws; i; i = i->ai_next, n_descriptors++) {
    int ret = setup_listening_sock(i);
    if (ret) return ret;
    descriptor_flags[n_descriptors] = WEBSOCKET_PORT;
  }
  freeaddrinfo(result_ws);
#endif


  if (client_mode == CLIENT_MODE_SHARED)
    init_rpc_remap();

  logmsg("Initialized. %zd sockets listening, %zd sensors, %zd max clients",
         n_listen, n_sensors, max_clients);

  // Set up signal handling. SIGINT is used to quit, and is only delivered
  // when waiting in ppoll
  sigset_t sigmask;
  sigemptyset(&sigmask);
  sigaddset(&sigmask, SIGINT);
  if (sigprocmask(SIG_BLOCK, &sigmask, NULL) == -1)
    return error("Failed to block SIGINT");

  {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = terminate_loop_on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1)
      return error("Failed to install SIGINT handler");
  }

  sigemptyset(&sigmask);

  // Main loop
  int ret = 0;
  while (keep_running) {
    if (disconnected_clients_flag) {
      // At leat one client was disconnected during the last iteration.
      // Re-compact poll_array.
      disconnected_clients_flag = 0;
      size_t old_n = n_descriptors;
      n_descriptors = n_sensors + n_listen;
      for (size_t i = n_descriptors; i < old_n; i++) {
        if (poll_array[i].fd >= 0) {
          if (i != n_descriptors) {
            poll_array[n_descriptors] = poll_array[i];
            descriptor_flags[n_descriptors] = descriptor_flags[i];
            if (client_list) {
              client_list[n_descriptors] = client_list[i];
              rpc_remap *r = client_list[n_descriptors].next;
              if (r) {
                r->prev = &client_list[n_descriptors];
                do {
                  r->client_desc = n_descriptors;
                  r = r->next;
                } while (r);
              }
            }
          }
          n_descriptors++;
        }
      }
    }

    // At most every 200 ms, send out a heartbeat to each sensor.
    // If a sensor was disconnected, attempt to reconnect.
    static struct timespec last_heartbeat = { .tv_sec = 0, .tv_nsec = 0 };
    struct timespec cur_time;
    clock_gettime(CLOCK_REALTIME, &cur_time);
    long nsec = cur_time.tv_nsec - last_heartbeat.tv_nsec;
    time_t sec = cur_time.tv_sec - last_heartbeat.tv_sec;
    if (nsec < 0) {
      nsec += 1000000000;
      sec -= 1;
    }
    if ((sec != 0) || (nsec > 200000000)) {
      tl_packet_header heartbeat = { TL_PTYPE_HEARTBEAT, 0, 0 };
      memcpy(&last_heartbeat, &cur_time, sizeof(struct timespec));
      for (size_t i = 0; i < n_sensors; i++) {
        if (poll_array[i].fd >= 0) {
          // Send a NOP packet to switch to binary mode.
          send_packet(i, (struct tl_packet*) &heartbeat);
        } else {
          // Attempt to reconnect, or exit if too much time has passed
          const char *url = sensor_url[i];
          poll_array[i].fd = tlopen(url, O_NONBLOCK|O_CLOEXEC, &io_log);
          if (poll_array[i].fd >= 0) {
            logmsg("Successfully reopened sensor at %s", url);
          } else if (sensor_reconnect_timeout > 0) {
            if ((cur_time.tv_sec > last_reconnect_attempt.tv_sec) ||
                ((cur_time.tv_sec == last_reconnect_attempt.tv_sec) &&
                 (cur_time.tv_nsec > last_reconnect_attempt.tv_nsec))) {
              keep_running = 0;
              ret = error("sensor reconnect timeout");
              break;
            }
          }
        }
      }
      if (keep_running == 0)
        continue;
    }

    struct timespec timeout = { .tv_sec = 0, .tv_nsec = 100000000 };
    int n_events = ppoll(poll_array, n_descriptors, &timeout, &sigmask);
    if (n_events < 0) {
      if (errno != EINTR) {
        keep_running = 0;
        ret = error("poll failed");
      }
      continue;
    }

    // See if there are remapped RPCs that have had no reply for a while,
    // and free up the spots for new RPCs.
    if (client_mode == CLIENT_MODE_SHARED) {
      for (rpc_remap *remap = NULL; (remap = get_timedout(time(NULL)));) {
        int client_fd = -1;
        if (remap->client_desc >= 0) {
          // The client is still connected. Send a timeout error back.
          client_fd = poll_array[remap->client_desc].fd;
          if (client_fd >= 0) {
            tl_rpc_request_packet req;
            req.req.id = remap->orig_id;
            tl_rpc_error_packet *err =
              tl_rpc_make_error(&req, TL_RPC_ERROR_TIMEOUT);
            memcpy(tl_packet_routing_data(&err->hdr), remap->routing,
                   remap->routing_size);
            tl_packet_set_routing_size(&err->hdr, remap->routing_size);
            if (send_packet(remap->client_desc, (tl_packet*)err) < 0) {
              logmsg("Failed to send synthetic RPC timeout error");
              disconnect_client(remap->client_desc);
            }
          }
        }
        logmsg("RPC remap timeout: client #%d RPC #%d", client_fd,
               remap->orig_id);
        insert_after(&remap_array[0], remove_next(remap->prev, 1));
      }
    }

    if (n_events < 1)
      continue;

    for (size_t ps = 0; (n_events > 0) && (ps < n_descriptors); ps++) {
      if (poll_array[ps].revents != 0)
        n_events--;
      else
        continue;

      if (ps < n_sensors) {
        // Event on sensor's descriptor
        while (poll_array[ps].fd >= 0) {
          if (handle_tlio(ps) == SUCCESS)
            break;
          if (errno == EPROTO) {
            // Error in the data. could be corrupted serial data,
            // keep running since there could be valid data after the error.
            logmsg("Error in sensor communication");
          } else if (sensor_reconnect_timeout == 0) {
            // Some other error, e.g. the serial port went down. Exit.
            logmsg("Fatal error in sensor communication [%s]",
                   strerror(errno));
            keep_running = 0;
            ret = 1;
            break;
          } else {
            close_sensor(ps);
            logmsg("Error in sensor %s communication [%s]",
                   sensor_url[ps], strerror(errno));
            break;
          }
        }
        if (!keep_running)
          break;
      } else if (ps < (n_sensors + n_listen)) {
        // Event on listening sockets
        if (client_connection(ps) != SUCCESS) {
          logmsg("Fatal error on listening sockets");
          keep_running = 0;
          ret = 1;
          break;
        }
      } else {
        // Client interaction. Skip if we closed the client handling an
        // event in this same poll iteration. If something goes wrong,
        // simply close the client unless it's a critical error
        if (poll_array[ps].fd >= 0) {
          int ret;
          if (descriptor_flags[ps] & WEBSOCKET_HANDSHAKE) {
#if WEBSOCKETS
            ret = handle_websock(ps);
#else
            ret = ERROR_LOCAL;
#endif
          } else {
            ret = handle_tlio(ps);
          }

          if (ret == ERROR_CRITICAL) {
            keep_running = 0;
            ret = 1;
            break;
          } else if (ret != SUCCESS) {
            disconnect_client(ps);
          }
        }
      }
    }
  }

  logmsgverbose("Attempting clean termination of I/O descriptors");

  // Give it about a second.
  for (int n = 0; n < 20; n++, usleep(50000)) {
    size_t left = 0;
    for (size_t i = 0; i < n_descriptors; i++) {
      if (poll_array[i].fd >= 0) {
        if ((i >= n_sensors) && (i < (n_sensors + n_listen))) {
          // this was a listening socket, just close it.
          close(poll_array[i].fd);
          poll_array[i].fd = -1;
        }
      } else {
        // this was a TLIO descriptor. try to flush any remaining data
        if ((tlsend(poll_array[i].fd, NULL) == 0) || (errno != EOVERFLOW)) {
          if (tlclose(poll_array[i].fd) != 0)
            close(poll_array[i].fd);
          poll_array[i].fd = -1;
        } else {
          left++;
        }
      }
    }
    if (left == 0) {
      logmsg("Exiting.");
      return ret;
    }
  }

  return error("Unable to close all descriptors. Exit is not clean.");
}
