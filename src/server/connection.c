#include "connection.h"
#include "buffer.h"
#include "src/common/protocol.h"
#include "src/server/buffer.h"
#include <cmath>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INITIAL_CONNECTIONS 8 // This should be promptly resized during runtime
#define MAX_CONNECTIONS                                                        \
  1024 // Realistically, no one is gonna run 1024 GUI clients

static uint32_t next_id = 1;

struct ConnectionManager *cman_new(void) {
  struct ConnectionManager *cman = malloc(sizeof(*cman));
  if (!cman)
    return NULL;

  cman->connections = malloc(sizeof(struct Connection *) * INITIAL_CONNECTIONS);
  if (!cman->connections) {
    free(cman);
    return NULL;
  }

  cman->count = 0;
  cman->capacity = INITIAL_CONNECTIONS;

  return cman;
}

void cman_destroy(struct ConnectionManager *cman) {
  if (!cman)
    return;

  for (size_t i = 0; i < cman->count; i++) {
    conn_close(cman, cman->connections[i]);
  }

  free(cman->connections);
  free(cman);
}

static bool grow_cman(struct ConnectionManager *cman) {
  if (cman->capacity >= MAX_CONNECTIONS)
    return false;

  size_t ncapacity = cman->capacity * 2;
  if (ncapacity > MAX_CONNECTIONS)
    ncapacity = MAX_CONNECTIONS;

  struct Connection **narr =
      realloc(cman->connections, sizeof(struct Connection *) * ncapacity);
  if (!narr)
    return false;

  cman->connections = narr;
  cman->capacity = ncapacity;
  return true;
}

bool conn_append(struct ConnectionManager *cman, struct Connection *conn) {
  if (!cman || !conn)
    return false;

  // check if we're at capacity
  if (cman->count >= cman->capacity) {
    if (!grow_cman(cman))
      return false;
  }

  if (cman->count >= MAX_CONNECTIONS)
    return false;

  conn->id = next_id++;

  // wraparound
  if (next_id == 0)
    next_id = 1;

  cman->connections[cman->count++] = conn;
  conn->state = CONN_STATE_OPEN;
}

struct Connection *conn_new(int fd) {
  struct Connection *conn = malloc(sizeof(*conn));
  if (!conn)
    return NULL;

  conn->fd = fd;
  conn->type = CLIENT_TYPE_UNKNOWN; // yet to register
  conn->state = CONN_STATE_WAITING;
  conn->pid = -1; // yet to set one
  conn->id = next_id++;

  // create buffers
  conn->recv_buffer = buffer_new();
  conn->send_buffer = buffer_new();

  if (!conn->recv_buffer || !conn->send_buffer) {
    buffer_destroy(conn->recv_buffer);
    buffer_destroy(conn->send_buffer);
    free(conn);
    return NULL;
  }

  return conn;
}

void conn_close(struct ConnectionManager *cman, struct Connection *conn) {
  if (!cman || !conn)
    return;

  // remove from cman
  for (size_t i = 0; i < cman->count; i++) {
    if (cman->connections[i] == conn) {
      cman->count--;
      if (i < cman->count) {
        cman->connections[i] = cman->connections[cman->count];
      }
    }
  }

  close(conn->fd);
  buffer_destroy(conn->recv_buffer);
  buffer_destroy(conn->send_buffer);
  free(conn);
}

struct Message *conn_read(struct Connection *conn) {
  // temporary reserve buffer
  uint8_t tbuf[8192];

  while (1) {
    ssize_t n = read(conn->fd, tbuf, sizeof(tbuf));

    if (n < 0) {
      // there's no more data
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return NULL;

      // error
      return NULL;
    }

    // connection is closed
    if (n == 0) {
      conn->state = CONN_STATE_CLOSED;
      return NULL;
    }

    // buffer is full
    // usually means invalid or unsupported data
    if (buffer_read(conn->recv_buffer, tbuf, n) < 0)
      return NULL;

    // check if the message is complete
    if (conn->recv_buffer->used < sizeof(struct MessageHeader))
      return NULL; // no(t yet)

    struct MessageHeader header;
    memcpy(&header, conn->recv_buffer->data + conn->recv_buffer->read_pos,
           sizeof(header));

    // get message size
    size_t size = sizeof(header) + header.length;

    if (conn->recv_buffer->used < size)
      break; // not enough data

    // read complete message
    struct Message *msg = malloc(sizeof(struct Message));
    if (!msg)
      return NULL;

    // read header
    buffer_read(conn->recv_buffer, &msg->header, sizeof(struct MessageHeader));

    if (header.length > 0) {
      // allocate enough resources for the entire message
      msg->payload = malloc(header.length);
      if (!msg->payload) {
        free(msg);
        return NULL;
      }

      buffer_read(conn->recv_buffer, msg->payload, header.length);
    } else {
      msg->payload = NULL;
    }

    return msg;
  }
}

void conn_write(struct Connection *conn) {
  while (conn->send_buffer->used > 0) {
    size_t space = conn->send_buffer->capacity - conn->send_buffer->read_pos;
    size_t left = conn->send_buffer->used;

    // normalize
    if (left > space)
      left = space;

    // write to fd
    ssize_t n = write(
        conn->fd, conn->send_buffer->data + conn->send_buffer->read_pos, left);

    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;

      // error
      return;
    }

    conn->send_buffer->read_pos += n;
    conn->send_buffer->used -= n;

    if (conn->send_buffer->read_pos >= conn->send_buffer->capacity)
      conn->send_buffer->read_pos = 0;
  }
}