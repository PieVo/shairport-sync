/*
 * HTTP output driver. This file is part of Shairport.
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

// TODO: Make configurable via configfile
#define AUDIO_HTTP_PORT 5600
#define TCP_MAX_LEN 256

static pthread_t tcp_thread;

static int client_socket = -1;
static int client_connected = 0;
static int server_socket = 0;
static int stop_accept_thread = 0;
char *pipename = NULL;
int warned = 0;

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

  listen(sockfd, 0);
  ret = 0;
  *sock = sockfd;

error:
  return ret;
}

int tcp_read(int socket, char *buffer, size_t count)
{
  int ret = -1;

  do {
    ret = read(socket, buffer, count);
    if (ret == 0) {
      /* client disconnected */
      ret = -EPIPE;
      goto error;
    } else
    
    if (ret == -1) {
      if (errno == EINTR) {
        /* interrupted system call, try again */
        ret = 0;
        } else {
          goto error;
        }
    } else if (ret > 0) {
      count -= ret;
      buffer += ret;
    }
  } while (count > 0);

  ret = 0;
error:
  return ret;
}

int tcp_write(int socket, const char *buf, unsigned int size)
{
  int ret;
  int count;
  char *ptr;

  ptr = (char*)buf;
  count = size;

  //debug(1, "TCP write: size=%d, msg=%s", count, buf);
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
  /* Open our server side socket, retry for ever */
  do {
    ret = tcp_open(AUDIO_HTTP_PORT, &srv_socket);
    if (ret < 0)    {
      debug(1, "failed to open socket, sleeping for 5 seconds\n");
      sleep(5);
    }
  } while (ret < 0);
  return srv_socket;
}


static void start(__attribute__((unused)) int sample_rate,
                  __attribute__((unused)) int sample_format) {
#if 0
  // this will leave fd as -1 if a reader hasn't been attached
  fd = open(pipename, O_WRONLY | O_NONBLOCK);
  if ((fd < -1) && (warned == 0)) {
    warn("Error %d opening the pipe named \"%s\".", errno, pipename);
    warned = 1;
  }
  // Check if a client is connected
  if (client_socket <= 0)
  {
    debug(1, "no client connected");
    return;
  }
#endif
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
    warned = 1;
    client_connected = 0;
  }
}

static void stop(void) {
  // Don't close the pipe just because a play session has stopped.
  //  if (fd > 0)
  //    close(fd);
}

void send_wav_header(void)
{
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
    2048, /* (2048*1024*1024) - 9, */
    "WAVEfmt ",
    16,
    1,
    2,
    44100,
    176400,
    4,
    16,
    "data",
    2048, /*(2048*1024*1024) - 1 */
  };

  tcp_write(client_socket, (char*)&wavhdr, sizeof(wavhdr));
}

void *accept_thread(void *dummy)
{
  struct sockaddr_in cli_addr;
  socklen_t clilen = sizeof(cli_addr);
  //int client_socket = 0;
  char http_msg[256];

  server_socket = tcp_open_socket();
  debug(1, "tcp accept thread started");
  while(!stop_accept_thread) {
    client_socket = accept(server_socket,(struct sockaddr *)&cli_addr, &clilen);
    if (client_socket < 0) {
      debug(1, "tcp accept returned %d", client_socket);
      break;
    }
    debug(1, "new http audio client connected, client socket %d", client_socket);
    // Dump what client has to say

    // Send HTTP reply
    snprintf(http_msg, sizeof(http_msg), "HTTP/1.1 200 OK\n");
    tcp_write(client_socket, http_msg, strlen(http_msg));
    snprintf(http_msg, sizeof(http_msg), "Content-length: 100000000\n");
    tcp_write(client_socket, http_msg, strlen(http_msg));
    snprintf(http_msg, sizeof(http_msg), "Content-Type: audio/x-wav\n\n");
    tcp_write(client_socket, http_msg, strlen(http_msg));
    // Send WAV header
    send_wav_header();
    // Toggle flag so "play function" can directly write to client_socket
    client_connected = 1;

#if 0
    // Test
    while(!stop_accept_thread)
    { 
      char buf[256] = {0};
      if (tcp_write(client_socket, (char*)&buf, sizeof(buf)) != 0)
      {  
        debug(1, "write failed, closing socket");
	client_connected = 0;
        break;
      }
    }
#endif
  };
  debug(1, "tcp accept thread ended");
  return NULL;
}

static int init(int argc, char **argv) {
  debug(1, "http init");
  //  const char *str;
  //  int value;
  //  double dvalue;

  // set up default values first

  config.audio_backend_buffer_desired_length = 1.0;
  config.audio_backend_latency_offset = 0;

  // do the "general" audio  options. Note, these options are in the "general" stanza!
  parse_general_audio_options();
#if 0
  if (config.cfg != NULL) {
    /* Get the Output Pipename. */
    const char *str;
    if (config_lookup_string(config.cfg, "pipe.name", &str)) {
      pipename = (char *)str;
    }

    if ((pipename) && (strcasecmp(pipename, "STDOUT") == 0))
      die("Can't use \"pipe\" backend for STDOUT. Use the \"stdout\" backend instead.");
  }

  if ((pipename == NULL) && (argc != 1))
    die("bad or missing argument(s) to pipe");

  if (argc == 1)
    pipename = strdup(argv[0]);

  // here, create the pipe
  if (mkfifo(pipename, 0644) && errno != EEXIST)
    die("Could not create output pipe \"%s\"", pipename);

  debug(1, "Pipename is \"%s\"", pipename);
#endif
  // Start accept thread
  int ret = pthread_create(&tcp_thread, NULL, &accept_thread, NULL);

  return ret;
}

static void deinit(void) {
  void* ret = NULL;

  debug(1, "running deinit for audio_http");
  if (client_socket)
    close(client_socket);

  // Stop the accept thread
  stop_accept_thread = 1;
  shutdown(server_socket, SHUT_RDWR);
  shutdown(client_socket, SHUT_RDWR);
  pthread_join(tcp_thread, &ret);
}

static void help(void) { printf("    http takes 1 argument: the port of the server to listen on.\n"); }

audio_output audio_http = {.name = "http",
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
