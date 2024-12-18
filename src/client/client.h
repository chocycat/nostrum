#pragma once

#include "../common/buffer.h"

struct Client {
  int fd;
  struct CircularBuffer *recv_buffer;
  struct CircularBuffer *send_buffer;
  uint32_t client_id;
};

/// Client Handling

// Connect and register self to the socket
struct Client *client_connect(const char *socket_addr);

// Disconnect and remove self from the socket
void client_destroy(struct Client *client);
