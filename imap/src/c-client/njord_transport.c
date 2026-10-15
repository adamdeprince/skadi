/* Copyright 2026 Adam DePrince. Apache-2.0; see LICENSE.
 * Included by mail.c: an optional NETDRIVER layered above TCP or implicit TLS.
 */
#include "njord_codec.h"

typedef struct njord_stream {
  NETSTREAM underlying;
  ZSTD_CCtx *encoder;
  ZSTD_DCtx *decoder;
  NJORD_BYTES incoming, plain, outgoing;
  int failed;
} NJORDSTREAM;

static long njord_failure(NJORDSTREAM *s, const char *message)
{
  if(!s->failed){
    s->failed = 1;
    mm_log((char *)message, ERROR);
  }
  return NIL;
}

static int njord_fill(NJORDSTREAM *s)
{
  unsigned char header[NJORD_FRAME_HEADER + 1]; /* wire field + getbuffer NUL */
  size_t size;
  const char *error;
  while(!s->failed && s->plain.position == s->plain.size){
    if(!(*s->underlying.dtb->getbuffer)(s->underlying.stream,
                                      NJORD_FRAME_HEADER, (char *)header))
      return (int)njord_failure(s, "Njord connection ended");
    size = njord_frame_size(header);
    if(!size || size == SIZE_MAX)
      return (int)njord_failure(s, "Invalid Njord frame length");
    if(!njord_reserve(&s->incoming, size + 1))
      return (int)njord_failure(s, "Could not allocate Njord input frame");
    if(!(*s->underlying.dtb->getbuffer)(s->underlying.stream, (unsigned long)size,
                                      (char *)s->incoming.data))
      return (int)njord_failure(s, "Truncated Njord input frame");
    error = njord_decode(s->decoder, s->incoming.data, size, &s->plain);
    if(error) return (int)njord_failure(s, error);
  }
  return !s->failed;
}

static long njord_getbuffer(void *value, unsigned long count, char *buffer)
{
  NJORDSTREAM *s = (NJORDSTREAM *)value;
  size_t n;
  while(count){
    if(!njord_fill(s)) return NIL;
    n = s->plain.size - s->plain.position;
    if(n > count) n = count;
    memcpy(buffer, s->plain.data + s->plain.position, n);
    s->plain.position += n;
    buffer += n;
    count -= n;
  }
  *buffer = '\0';
  return s->failed ? NIL : LONGT;
}

static char *njord_getline(void *value)
{
  NJORDSTREAM *s = (NJORDSTREAM *)value;
  NJORD_BYTES line = {0};
  const unsigned char *start, *end;
  size_t n, capacity;
  while(njord_fill(s)){
    start = s->plain.data + s->plain.position;
    end = (const unsigned char *)memchr(start, '\n', s->plain.size - s->plain.position);
    n = end ? (size_t)(end - start) + 1 : s->plain.size - s->plain.position;
    if(n >= SIZE_MAX - line.size){
      njord_failure(s, "Njord response line size overflow"); break;
    }
    capacity = line.size + n + 1;
    if(capacity > line.capacity && line.capacity <= SIZE_MAX / 2 && capacity < line.capacity * 2)
      capacity = line.capacity * 2;
    if(!njord_reserve(&line, capacity)){
      njord_failure(s, "Could not allocate Njord response line"); break;
    }
    memcpy(line.data + line.size, start, n);
    line.size += n;
    s->plain.position += n;
    if(end){
      --line.size;
      if(line.size && line.data[line.size - 1] == '\r') --line.size;
      line.data[line.size] = 0;
      return (char *)line.data;
    }
  }
  free(line.data);
  return NIL;
}

static char *njord_getsize(void *value, unsigned long count)
{
  char *buffer;
  if(count >= SIZE_MAX || !(buffer = (char *)malloc((size_t)count + 1))){
    njord_failure((NJORDSTREAM *)value, "Could not allocate Njord response");
    return NIL;
  }
  if(njord_getbuffer(value, count, buffer)) return buffer;
  free(buffer);
  return NIL;
}

static long njord_sout(void *value, char *data, unsigned long count)
{
  NJORDSTREAM *s = (NJORDSTREAM *)value;
  size_t n;
  const char *error;
  if(s->failed) return NIL;
  while(count){
    /* Codec-recommended streaming chunks; no message or batch size limit. */
    n = ZSTD_CStreamInSize();
    if(n > count) n = count;
    error = njord_encode(s->encoder, data, n, &s->outgoing);
    if(error) return njord_failure(s, error);
    if(!(*s->underlying.dtb->sout)(s->underlying.stream,
                                  (char *)s->outgoing.data,
                                  (unsigned long)s->outgoing.size))
      return njord_failure(s, "Could not send Njord frame");
    data += n;
    count -= n;
  }
  return LONGT;
}

static long njord_soutr(void *s, char *text) { return njord_sout(s, text, strlen(text)); }
static char *njord_host(void *v) { NJORDSTREAM *s = (NJORDSTREAM *)v; return net_host(&s->underlying); }
static char *njord_remotehost(void *v) { NJORDSTREAM *s = (NJORDSTREAM *)v; return net_remotehost(&s->underlying); }
static char *njord_localhost(void *v) { NJORDSTREAM *s = (NJORDSTREAM *)v; return net_localhost(&s->underlying); }
static unsigned long njord_port(void *v) { NJORDSTREAM *s = (NJORDSTREAM *)v; return net_port(&s->underlying); }
static void njord_close(void *value)
{
  NJORDSTREAM *s = (NJORDSTREAM *)value;
  if(s->underlying.stream) (*s->underlying.dtb->close)(s->underlying.stream);
  ZSTD_freeCCtx(s->encoder);
  ZSTD_freeDCtx(s->decoder);
  free(s->incoming.data); free(s->plain.data); free(s->outgoing.data);
  free(s);
}

static NETDRIVER njord_driver = {
  NIL, NIL, njord_getline, njord_getbuffer, njord_soutr, njord_sout,
  njord_close, njord_host, njord_remotehost, njord_port, njord_localhost,
  njord_getsize
};

static NETSTREAM *njord_wrap_protocol(NETSTREAM *underlying, const char *protocol)
{
  NJORDSTREAM *s;
  char preamble[NJORD_PREAMBLE_SIZE + 1]; /* protocol preamble + getbuffer NUL */
  if(!underlying) return NIL;
  s = (NJORDSTREAM *)calloc(1, sizeof(*s));
  if(!s){ net_close(underlying); mm_log("Could not allocate Njord stream", ERROR); return NIL; }
  s->underlying = *underlying;
  s->encoder = ZSTD_createCCtx();
  s->decoder = ZSTD_createDCtx();
  if(!s->encoder || !s->decoder ||
     !net_sout(underlying, (char *)protocol, NJORD_PREAMBLE_SIZE) ||
     !net_getbuffer(underlying, NJORD_PREAMBLE_SIZE, preamble) ||
     memcmp(preamble, protocol, NJORD_PREAMBLE_SIZE)){
    njord_close(s);
    fs_give((void **)&underlying);
    mm_log("Njord v1 compressed connection setup failed", ERROR);
    return NIL;
  }
  underlying->stream = s;
  underlying->dtb = &njord_driver;
  return underlying;
}

static NETSTREAM *njord_wrap(NETSTREAM *underlying)
{ return njord_wrap_protocol(underlying,NJORD_PREAMBLE); }

NETSTREAM *net_njord_image(NETMBX *mb)
{
  unsigned long flags = mb->novalidate ? NET_NOVALIDATECERT : 0;
  if(!mb->njordflag || (mb->sslflag && !mailssldriver)) return NIL;
  return njord_wrap_protocol(net_open_work(mb->sslflag ? mailssldriver : &tcpdriver,
                             mb->host,NIL,1143,mb->port,flags),NJORD_IMAGE_PREAMBLE);
}
