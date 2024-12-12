#ifndef _FBUFFER_H_
#define _FBUFFER_H_

#include "ruby.h"
#include "ruby/encoding.h"

/* shims */
/* This is the fallback definition from Ruby 3.4 */

#ifndef RBIMPL_STDBOOL_H
#if defined(__cplusplus)
# if defined(HAVE_STDBOOL_H) && (__cplusplus >= 201103L)
#  include <cstdbool>
# endif
#elif defined(HAVE_STDBOOL_H)
# include <stdbool.h>
#elif !defined(HAVE__BOOL)
typedef unsigned char _Bool;
# define bool  _Bool
# define true  ((_Bool)+1)
# define false ((_Bool)+0)
# define __bool_true_false_are_defined
#endif
#endif

#ifndef RB_UNLIKELY
#define RB_UNLIKELY(expr) expr
#endif

#ifndef RB_LIKELY
#define RB_LIKELY(expr) expr
#endif

#ifndef MAYBE_UNUSED
# define MAYBE_UNUSED(x) x
#endif

typedef struct FBufferStruct {
    unsigned long initial_length;
    VALUE buf;
    VALUE io;
} FBuffer;

#define FBUFFER_IO_BUFFER_SIZE (16384 - 1)
#define FBUFFER_INITIAL_LENGTH_DEFAULT 1024

#define FBUFFER_PTR(fb) (RSTRING_PTR((fb)->buf))
#define FBUFFER_CAPA(fb) ((fb)->capa)

static void fbuffer_free(FBuffer *fb);
#ifndef JSON_GENERATOR
static void fbuffer_clear(FBuffer *fb);
#endif
static void fbuffer_append(FBuffer *fb, const char *newstr, unsigned long len);
#ifdef JSON_GENERATOR
static void fbuffer_append_long(FBuffer *fb, long number);
#endif
static inline void fbuffer_append_char(FBuffer *fb, char newchr);
#ifdef JSON_GENERATOR
static VALUE fbuffer_finalize(FBuffer *fb);
#endif

static void fbuffer_init(FBuffer *fb, unsigned long initial_length)
{
    fb->initial_length = (initial_length > 0) ? initial_length : FBUFFER_INITIAL_LENGTH_DEFAULT;
    fb->buf = 0;
}

static void fbuffer_free(FBuffer *fb)
{
    // noop
}

static void fbuffer_clear(FBuffer *fb)
{
    if (fb->buf) {
        rb_str_set_len(fb->buf, 0);
    }
}

#ifdef JSON_GENERATOR
static void fbuffer_flush(FBuffer *fb)
{
    if (fb->buf) {
        VALUE buf = fb->buf;
        fb->buf = 0;
        rb_enc_associate_index(buf, rb_utf8_encindex());
        rb_io_write(fb->io, buf);
    }
    fbuffer_clear(fb);
}
#endif

static inline void fbuffer_prepare(FBuffer *fb)
{
    if (RB_UNLIKELY(!fb->buf)) {
        fb->buf = rb_str_buf_new(fb->initial_length);
    }
}


static void fbuffer_append(FBuffer *fb, const char *newstr, unsigned long len)
{
    if (len > 0) {
        fbuffer_prepare(fb);
        rb_str_cat(fb->buf, newstr, len);
    }
}

#ifdef JSON_GENERATOR
static void fbuffer_append_str(FBuffer *fb, VALUE str)
{
    fbuffer_prepare(fb);
    rb_str_append(fb->buf, str);
}
#endif

static inline void fbuffer_append_char(FBuffer *fb, char newchr)
{
    fbuffer_prepare(fb);
    rb_str_cat(fb->buf, &newchr, 1);
}

#ifdef JSON_GENERATOR
static long fltoa(long number, char *buf)
{
    static const char digits[] = "0123456789";
    long sign = number;
    char* tmp = buf;

    if (sign < 0) number = -number;
    do *tmp-- = digits[number % 10]; while (number /= 10);
    if (sign < 0) *tmp-- = '-';
    return buf - tmp;
}

#define LONG_BUFFER_SIZE 20
static void fbuffer_append_long(FBuffer *fb, long number)
{
    char buf[LONG_BUFFER_SIZE];
    char *buffer_end = buf + LONG_BUFFER_SIZE;
    long len = fltoa(number, buffer_end - 1);
    fbuffer_append(fb, buffer_end - len, len);
}

static VALUE fbuffer_finalize(FBuffer *fb)
{
    if (fb->io) {
        fbuffer_flush(fb);
        fbuffer_free(fb);
        rb_io_flush(fb->io);
        return fb->io;
    } else {
        VALUE result = fb->buf;
        fb->buf = 0;
        if (RB_UNLIKELY(!result)) {
            result = rb_utf8_str_new("", 0);
        } else {
            rb_enc_associate_index(result, rb_utf8_encindex());
        }
        fbuffer_free(fb);
        return result;
    }
}
#endif
#endif
