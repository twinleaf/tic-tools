// Copyright: 2018 Twinleaf LLC
// Author: kornack@twinleaf.com
// License: MIT

// Send sensor data over muticast UDP; test with
// iperf -s -u -B 226.94.1.1 -i 1
// Connect using
// mudp://226.94.1.1:5001

#include <tio/io.h>
#include <tio/rpc.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define TIO_UDP_PORT 5001
#define TIO_UDP_GROUP "226.94.1.1"

void proxy_udp(int sensor_fd)
{
  int sock, status;
  char buffer[65536];
  struct sockaddr_in saddr;
  struct in_addr iaddr;

  // set content of struct saddr and imreq to zero
  memset(&saddr, 0, sizeof(struct sockaddr_in));
  memset(&iaddr, 0, sizeof(struct in_addr));

  // open a UDP socket
  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if ( sock < 0 )
    perror("Error creating socket"), exit(0);

  // Bind socket
  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = inet_addr(TIO_UDP_GROUP);
  saddr.sin_port = htons(TIO_UDP_PORT); // Use the first free port

  // put some data in buffer
  strcpy(buffer, "Hello world\n");

  // receive packet from socket
  status = sendto(sock, buffer, strlen(buffer), 0, (struct sockaddr *)&saddr, sizeof(saddr));


  for (;;) {
    
    // Read from sensor
    tl_packet packet;
    errno = 0;
    int ret = tlrecv(sensor_fd, &packet, sizeof(packet));
    if (ret < 0) {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
        break;
      if (errno == 0)
        perror("disconnected");
        exit(1);
      return;
    }

    // Send to UDP multicast
    if (sendto(sock, &packet, tl_packet_total_size(&packet.hdr), 0, (struct sockaddr*)&saddr, sizeof(saddr))<0) {
      perror("sendto");
      exit(1);
    }
  }

}

int main(int argc, char *argv[])
{
  const char *root_url = "tcp://localhost";

  if (argc > 2)
    return 1;
  if (argc == 2)
    root_url = argv[1];

  int sensor_fd = tlopen(root_url, O_CLOEXEC, NULL);

  if (sensor_fd < 0) {
    printf("Error opening port\r\n");
    return 1;
  }

  proxy_udp(sensor_fd);

  return 0;
}
