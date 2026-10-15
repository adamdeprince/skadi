/* Copyright 2026 Adam DePrince. Apache-2.0; see LICENSE.
 * Njord v1: an 8-byte preamble followed by length-prefixed zstd frames.
 * Each frame uses level 22 and a checksum. Frames are independent so a small
 * command does not reserve a level-22 context sized for an unbounded message.
 * Messages span as many frames as needed; frame boundaries have no IMAP meaning.
 */
#ifndef SKADI_NJORD_CODEC_H
#define SKADI_NJORD_CODEC_H
#include <zstd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define NJORD_PREAMBLE "NJORD1\r\n"
#define NJORD_IMAGE_PREAMBLE "NJIMG1\r\n"
#define NJORD_PREAMBLE_SIZE (sizeof(NJORD_PREAMBLE) - 1)
#define NJORD_FRAME_HEADER 4 /* unsigned 32-bit network-order compressed length */
#define NJORD_COMPRESSION_LEVEL 22

typedef struct njord_bytes {
  unsigned char *data;
  size_t capacity, size, position;
} NJORD_BYTES;

static inline int njord_reserve(NJORD_BYTES *b, size_t capacity)
{
  void *p;
  if(capacity <= b->capacity) return 1;
  p = realloc(b->data, capacity);
  if(!p) return 0;
  b->data = (unsigned char *)p;
  b->capacity = capacity;
  return 1;
}

static inline uint32_t njord_frame_size(const unsigned char *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline const char *njord_encode(ZSTD_CCtx *ctx, const void *data,
                                      size_t size, NJORD_BYTES *wire)
{
  size_t result, bound = ZSTD_compressBound(size);
  if(ZSTD_isError(bound) || bound > SIZE_MAX - NJORD_FRAME_HEADER)
    return "zstd frame allocation overflow";
  if(!njord_reserve(wire, bound + NJORD_FRAME_HEADER))
    return "could not allocate zstd output";
  result = ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, NJORD_COMPRESSION_LEVEL);
  if(ZSTD_isError(result)) return ZSTD_getErrorName(result);
  result = ZSTD_CCtx_setParameter(ctx, ZSTD_c_checksumFlag, 1);
  if(ZSTD_isError(result)) return ZSTD_getErrorName(result);
  result = ZSTD_compress2(ctx, wire->data + NJORD_FRAME_HEADER, bound, data, size);
  if(ZSTD_isError(result)) return ZSTD_getErrorName(result);
  if(result > UINT32_MAX) return "compressed frame exceeds the v1 length field";
  wire->data[0] = (unsigned char)(result >> 24);
  wire->data[1] = (unsigned char)(result >> 16);
  wire->data[2] = (unsigned char)(result >> 8);
  wire->data[3] = (unsigned char)result;
  wire->size = result + NJORD_FRAME_HEADER;
  wire->position = 0;
  return NULL;
}

static inline const char *njord_decode(ZSTD_DCtx *ctx, const void *data,
                                      size_t size, NJORD_BYTES *plain)
{
  unsigned long long expected = ZSTD_getFrameContentSize(data, size);
  size_t result = ZSTD_findFrameCompressedSize(data, size);
  if(ZSTD_isError(result) || result != size)
    return "invalid Njord zstd frame boundary";
  if(expected == ZSTD_CONTENTSIZE_ERROR || expected == ZSTD_CONTENTSIZE_UNKNOWN)
    return "Njord zstd frame must declare its content size";
  if(expected >= SIZE_MAX) return "zstd content size exceeds addressable memory";
  /* One extra byte supports c-client's NUL-terminating getbuffer convention. */
  if(!njord_reserve(plain, (size_t)expected + 1))
    return "could not allocate decompressed frame";
  result = ZSTD_decompressDCtx(ctx, plain->data, (size_t)expected, data, size);
  if(ZSTD_isError(result)) return ZSTD_getErrorName(result);
  if(result != expected) return "zstd content size mismatch";
  plain->data[result] = 0;
  plain->size = result;
  plain->position = 0;
  return NULL;
}
#endif
