/*
 * WAVstream output driver. This file is part of Shairport.
 * Copyright (c) James Laird 2013, PieVo 2018
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "audio.h"
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <stdio.h>

#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/socket.h>
#include <netinet/tcp.h>

#define AUDIO_HTTP_PORT 5668

static pthread_t tcp_thread;
static int client_socket = -1;
static int client_connected = 0;
static int server_socket = 0;
static int stop_accept_thread = 0;
int warned = 0;
int wavstream_port = AUDIO_HTTP_PORT;

int tcp_open(int port, int *sock)
{
  int sockfd;
  struct sockaddr_in serv_addr;
  int flag = 1, ret = -1;

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    debug(1, "error opening socket");
    goto error;
  }

  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  serv_addr.sin_port = htons(port);

  if (bind(sockfd, (struct sockaddr *) &serv_addr,
     sizeof(serv_addr)) < 0) {
     debug(1, "error binding");
     goto error;
  }

  ret = setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(flag));
  if (ret < 0) {
   debug(1, "failed to set socket option TCP_NODELAY\n");
   goto error;
  }

  listen(sockfd, 3);
  ret = 0;
  *sock = sockfd;

error:
  return ret;
}

int tcp_write(int socket, const char *buf, unsigned int size)
{
  int ret;
  int count = size;
  char *ptr = (char*)buf;

  do {
    ret = send(socket, buf, count, MSG_NOSIGNAL);
    if (ret == -1)
      if (errno != EINTR)
        return ret;
    ptr += ret;
    count -= ret;
  } while (count > 0);
  return count;
}

int tcp_open_socket(void)
{
  int ret, srv_socket;
  // Open our server side socket, retry for ever
  do {
    ret = tcp_open(wavstream_port, &srv_socket);
    if (ret < 0)    {
      debug(1, "failed to open socket, retrying in 5 seconds\n");
      sleep(5);
    }
  } while (ret < 0);
  return srv_socket;
}


static void start(__attribute__((unused)) int sample_rate,
                  __attribute__((unused)) int sample_format) {
  debug(1, "wavstream start");
}

static void play(short buf[], int samples) {
  // if the file is not open, try to open it.
  char errorstring[1024];
  
  if (!client_connected) {
    if (!warned) {
      debug(1, "no client connected, not playing");
      warned = 1;
    }
    return;
  }

  // if it's got a reader, write to it.
  int rc = non_blocking_write(client_socket, buf, samples * 4);
  if ((rc < 0) && (warned == 0)) {
    strerror_r(errno, (char *)errorstring, 1024);
    warn("Error %d writing to the client socket: %s", errno, errorstring);
    warned = 0;
    client_connected = 0;
  }
}

static void stop(void) {
  debug(1, "wavstream stop");
}

int send_wav_header(void)
{
  int ret;

  // Send WAV header
  typedef struct wave_header
  {
    unsigned char  id[4];          // should always contain "RIFF"
    unsigned int   totallength;    // total file length minus 8
    unsigned char  wavefmt[8];     // should be "WAVEfmt "
    unsigned int   format;         // 16 for PCM format
    unsigned short pcm;            // 1 for PCM format
    unsigned short channels;       // channels
    unsigned int   frequency;      // sampling frequency
    unsigned int   bytes_per_second;
    unsigned short bytes_by_capture;
    unsigned short bits_per_sample;
    unsigned char  data[4];        // should always contain "data"
    unsigned int   bytes_in_data;
  }__attribute__((__packed__)) wavfile_t;

  wavfile_t wavhdr = {
    "RIFF",
    ~0,
    "WAVEfmt ",
    16,
    1,
    2,
    44100,
    176400,
    4,
    16,
    "data",
    ~0,
  };

  ret = tcp_write(client_socket, (char*)&wavhdr, sizeof(wavhdr));
  if (ret != 0) {
    debug(1, "Failed to write WAVE header to socket");
  }
  return ret;
}

int send_http_ok(void) {
  char http_msg[256];
  int offs = 0;

  // Send HTTP reply
  offs  = sprintf(http_msg, "HTTP/1.1 200 OK\r\n");
  offs += sprintf(http_msg + offs, "Cache-Control: no-cache\r\n");
  offs += sprintf(http_msg + offs, "Content-Type: audio/x-wav\r\n");
  offs += sprintf(http_msg + offs, "Accept-Ranges: none\r\n");
  offs += sprintf(http_msg + offs, "Content-length: 4294967296\r\n");
  offs += sprintf(http_msg + offs, "\r\n");
  
  return tcp_write(client_socket, http_msg, strlen(http_msg));
}

void *accept_thread(void *dummy)
{
  struct sockaddr_in cli_addr;
  socklen_t clilen = sizeof(cli_addr);

  server_socket = tcp_open_socket();
  debug(1, "http_audio thread started, listening on port %d", wavstream_port);

  while(!stop_accept_thread) {
    client_socket = accept(server_socket,(struct sockaddr *)&cli_addr, &clilen);
    if (client_socket < 0) {
      debug(1, "tcp accept returned %d", client_socket);
      break;
    }
    debug(1, "new http audio client connected, client socket %d", client_socket);

    // Send OK and WAV header
    if (send_http_ok() == 0 && send_wav_header() == 0) {
      // Toggle flag so "play function" can directly write to client_socket
      client_connected = 1;
    }

#if 0
    // Test
    while(!stop_accept_thread)
    { 
      char buf[1764] = {0};
      if (tcp_write(client_socket, (char*)&buf, sizeof(buf)) != 0)
      {  
        debug(1, "write failed, closing socket");
	client_connected = 0;
        break;
      }
      usleep(10*1000);
    }
#endif
  };
  debug(1, "tcp accept thread ended");
  return NULL;
}

static int init(int argc, char **argv) {
  debug(1, "wavstream init");

  // set up default values first
  config.audio_backend_buffer_desired_length = 1.0;
  config.audio_backend_latency_offset = 0;

  // do the "general" audio  options. Note, these options are in the "general" stanza!
  parse_general_audio_options();
  if (config.cfg != NULL) {
    int port;
    if (config_lookup_int(config.cfg, "wavstream.port", &port)) {
      wavstream_port = port;
      debug(1, "http_audio port changed to %d", wavstream_port);
    }
    else {
      debug(1, "no conf found..");
    }
  }

  // Start accept thread
  int ret = pthread_create(&tcp_thread, NULL, &accept_thread, NULL);

  return ret;
}

static void deinit(void) {
  void* ret = NULL;

  debug(1, "wavstream deinit");

  // Stop the accept thread
  stop_accept_thread = 1;
  shutdown(server_socket, SHUT_RDWR);
  shutdown(client_socket, SHUT_RDWR);
  pthread_join(tcp_thread, &ret);
}

static void help(void) { printf("    wavstream takes 1 argument: the port of the server to listen on.\n"); }

audio_output audio_wavstream = {.name = "wavstream",
                           .help = &help,
                           .init = &init,
                           .deinit = &deinit,
                           .start = &start,
                           .stop = &stop,
                           .flush = NULL,
                           .delay = NULL,
                           .play = &play,
                           .volume = NULL,
                           .parameters = NULL,
                           .mute = NULL};
