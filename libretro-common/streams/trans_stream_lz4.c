/* Simple LZ4 adapter for the trans_stream interface.
 * Compression is one-shot per call; flush flag is ignored since LZ4
 * does not require streaming state for independent blocks. */

#include <stdlib.h>
#include <string.h>

#include <lz4.h>
#include <streams/trans_stream.h>

struct lz4_trans_stream
{
   const uint8_t *in;
   uint32_t in_size;
   uint8_t *out;
   uint32_t out_size;
};

static void *lz4_stream_new(void)
{
   struct lz4_trans_stream *s = (struct lz4_trans_stream*)calloc(1, sizeof(*s));
   return s;
}

static void lz4_stream_free(void *data)
{
   free(data);
}

static bool lz4_define(void *data, const char *prop, uint32_t val)
{
   (void)data;
   (void)prop;
   (void)val;
   /* No tunable properties for now. */
   return true;
}

static void lz4_set_in(void *data, const uint8_t *in, uint32_t in_size)
{
   struct lz4_trans_stream *s = (struct lz4_trans_stream*)data;
   if (!s)
      return;
   s->in = in;
   s->in_size = in_size;
}

static void lz4_set_out(void *data, uint8_t *out, uint32_t out_size)
{
   struct lz4_trans_stream *s = (struct lz4_trans_stream*)data;
   if (!s)
      return;
   s->out = out;
   s->out_size = out_size;
}

static bool lz4_deflate_trans(
      void *data, bool flush, uint32_t *read, uint32_t *written,
      enum trans_stream_error *err)
{
   struct lz4_trans_stream *s = (struct lz4_trans_stream*)data;
   int ret;
   (void)flush;

   if (!s || !s->in || !s->out || !read || !written)
      return false;

   ret = LZ4_compress_default(
         (const char*)s->in,
         (char*)s->out,
         (int)s->in_size,
         (int)s->out_size);
   if (ret <= 0)
   {
      if (err)
         *err = TRANS_STREAM_ERROR_OTHER;
      return false;
   }

   *read = s->in_size;
   *written = (uint32_t)ret;
   if (err)
      *err = TRANS_STREAM_ERROR_NONE;
   return true;
}

static bool lz4_inflate_trans(
      void *data, bool flush, uint32_t *read, uint32_t *written,
      enum trans_stream_error *err)
{
   struct lz4_trans_stream *s = (struct lz4_trans_stream*)data;
   int ret;
   (void)flush;

   if (!s || !s->in || !s->out || !read || !written)
      return false;

   ret = LZ4_decompress_safe(
         (const char*)s->in,
         (char*)s->out,
         (int)s->in_size,
         (int)s->out_size);
   if (ret < 0)
   {
      if (err)
         *err = TRANS_STREAM_ERROR_OTHER;
      return false;
   }

   *read = s->in_size;
   *written = (uint32_t)ret;
   if (err)
      *err = TRANS_STREAM_ERROR_NONE;
   return true;
}

const struct trans_stream_backend lz4_deflate_backend = {
   "lz4_deflate", NULL,
   lz4_stream_new,
   lz4_stream_free,
   lz4_define,
   lz4_set_in,
   lz4_set_out,
   lz4_deflate_trans
};

const struct trans_stream_backend lz4_inflate_backend = {
   "lz4_inflate", NULL,
   lz4_stream_new,
   lz4_stream_free,
   lz4_define,
   lz4_set_in,
   lz4_set_out,
   lz4_inflate_trans
};
