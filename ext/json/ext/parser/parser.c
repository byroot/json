#include "ruby.h"
#include "../fbuffer/fbuffer.h"

static VALUE mJSON, eNestingError, Encoding_UTF_8;
static VALUE CNaN, CInfinity, CMinusInfinity;

static ID i_json_creatable_p, i_json_create, i_create_id,
          i_chr, i_deep_const_get, i_match, i_aset, i_aref,
          i_leftshift, i_new, i_try_convert, i_uminus, i_encode;

static VALUE sym_max_nesting, sym_allow_nan, sym_allow_trailing_comma, sym_symbolize_names, sym_freeze,
             sym_create_additions, sym_create_id, sym_object_class, sym_array_class,
             sym_decimal_class, sym_match_string;

static int binary_encindex;
static int utf8_encindex;

#ifdef HAVE_RB_CATEGORY_WARN
# define json_deprecated(message) rb_category_warn(RB_WARN_CATEGORY_DEPRECATED, message)
#else
# define json_deprecated(message) rb_warn(message)
#endif

static const char deprecated_create_additions_warning[] =
    "JSON.load implicit support for `create_additions: true` is deprecated "
    "and will be removed in 3.0, use JSON.unsafe_load or explicitly "
    "pass `create_additions: true`";

#ifndef HAVE_RB_HASH_BULK_INSERT
// For TruffleRuby
void
rb_hash_bulk_insert(long count, const VALUE *pairs, VALUE hash)
{
    long index = 0;
    while (index < count) {
        VALUE name = pairs[index++];
        VALUE value = pairs[index++];
        rb_hash_aset(hash, name, value);
    }
    RB_GC_GUARD(hash);
}
#endif

#ifndef HAVE_RB_HASH_NEW_CAPA
#define rb_hash_new_capa(n) rb_hash_new()
#endif


/* name cache */

#include <string.h>
#include <ctype.h>

// Object names are likely to be repeated, and are frozen.
// As such we can re-use them if we keep a cache of the ones we've seen so far,
// and save much more expensive lookups into the global fstring table.
// This cache implementation is deliberately simple, as we're optimizing for compactness,
// to be able to fit safely on the stack.
// As such, binary search into a sorted array gives a good tradeoff between compactness and
// performance.
#define JSON_RVALUE_CACHE_CAPA 63
typedef struct rvalue_cache_struct {
    int length;
    VALUE entries[JSON_RVALUE_CACHE_CAPA];
} rvalue_cache;

static rb_encoding *enc_utf8;

#define JSON_RVALUE_CACHE_MAX_ENTRY_LENGTH 55

static inline VALUE build_interned_string(const char *str, const long length)
{
# ifdef HAVE_RB_ENC_INTERNED_STR
    return rb_enc_interned_str(str, length, enc_utf8);
# else
    VALUE rstring = rb_utf8_str_new(str, length);
    return rb_funcall(rb_str_freeze(rstring), i_uminus, 0);
# endif
}

static inline VALUE build_symbol(const char *str, const long length)
{
    return rb_str_intern(build_interned_string(str, length));
}

static void rvalue_cache_insert_at(rvalue_cache *cache, int index, VALUE rstring)
{
    MEMMOVE(&cache->entries[index + 1], &cache->entries[index], VALUE, cache->length - index);
    cache->length++;
    cache->entries[index] = rstring;
}

static inline int rstring_cache_cmp(const char *str, const long length, VALUE rstring)
{
    long rstring_length = RSTRING_LEN(rstring);
    if (length == rstring_length) {
        return memcmp(str, RSTRING_PTR(rstring), length);
    } else {
        return (int)(length - rstring_length);
    }
}

static VALUE rstring_cache_fetch(rvalue_cache *cache, const char *str, const long length)
{
    if (RB_UNLIKELY(length > JSON_RVALUE_CACHE_MAX_ENTRY_LENGTH)) {
        // Common names aren't likely to be very long. So we just don't
        // cache names above an arbitrary threshold.
        return Qfalse;
    }

    if (RB_UNLIKELY(!isalpha(str[0]))) {
        // Simple heuristic, if the first character isn't a letter,
        // we're much less likely to see this string again.
        // We mostly want to cache strings that are likely to be repeated.
        return Qfalse;
    }

    int low = 0;
    int high = cache->length - 1;
    int mid = 0;
    int last_cmp = 0;

    while (low <= high) {
        mid = (high + low) >> 1;
        VALUE entry = cache->entries[mid];
        last_cmp = rstring_cache_cmp(str, length, entry);

        if (last_cmp == 0) {
            return entry;
        } else if (last_cmp > 0) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    if (RB_UNLIKELY(memchr(str, '\\', length))) {
        // We assume the overwhelming majority of names don't need to be escaped.
        // But if they do, we have to fallback to the slow path.
        return Qfalse;
    }

    VALUE rstring = build_interned_string(str, length);

    if (cache->length < JSON_RVALUE_CACHE_CAPA) {
        if (last_cmp > 0) {
            mid += 1;
        }

        rvalue_cache_insert_at(cache, mid, rstring);
    }
    return rstring;
}

static VALUE rsymbol_cache_fetch(rvalue_cache *cache, const char *str, const long length)
{
    if (RB_UNLIKELY(length > JSON_RVALUE_CACHE_MAX_ENTRY_LENGTH)) {
        // Common names aren't likely to be very long. So we just don't
        // cache names above an arbitrary threshold.
        return Qfalse;
    }

    if (RB_UNLIKELY(!isalpha(str[0]))) {
        // Simple heuristic, if the first character isn't a letter,
        // we're much less likely to see this string again.
        // We mostly want to cache strings that are likely to be repeated.
        return Qfalse;
    }

    int low = 0;
    int high = cache->length - 1;
    int mid = 0;
    int last_cmp = 0;

    while (low <= high) {
        mid = (high + low) >> 1;
        VALUE entry = cache->entries[mid];
        last_cmp = rstring_cache_cmp(str, length, rb_sym2str(entry));

        if (last_cmp == 0) {
            return entry;
        } else if (last_cmp > 0) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    if (RB_UNLIKELY(memchr(str, '\\', length))) {
        // We assume the overwhelming majority of names don't need to be escaped.
        // But if they do, we have to fallback to the slow path.
        return Qfalse;
    }

    VALUE rsymbol = build_symbol(str, length);

    if (cache->length < JSON_RVALUE_CACHE_CAPA) {
        if (last_cmp > 0) {
            mid += 1;
        }

        rvalue_cache_insert_at(cache, mid, rsymbol);
    }
    return rsymbol;
}

/* rvalue stack */

#define RVALUE_STACK_INITIAL_CAPA 128

enum rvalue_stack_type {
    RVALUE_STACK_HEAP_ALLOCATED = 0,
    RVALUE_STACK_STACK_ALLOCATED = 1,
};

typedef struct rvalue_stack_struct {
    enum rvalue_stack_type type;
    long capa;
    long head;
    VALUE *ptr;
} rvalue_stack;

static rvalue_stack *rvalue_stack_spill(rvalue_stack *old_stack, VALUE *handle, rvalue_stack **stack_ref);

static rvalue_stack *rvalue_stack_grow(rvalue_stack *stack, VALUE *handle, rvalue_stack **stack_ref)
{
    long required = stack->capa * 2;

    if (stack->type == RVALUE_STACK_STACK_ALLOCATED) {
        stack = rvalue_stack_spill(stack, handle, stack_ref);
    } else {
        REALLOC_N(stack->ptr, VALUE, required);
        stack->capa = required;
    }
    return stack;
}

static void rvalue_stack_push(rvalue_stack *stack, VALUE value, VALUE *handle, rvalue_stack **stack_ref)
{
    if (RB_UNLIKELY(stack->head >= stack->capa)) {
        stack = rvalue_stack_grow(stack, handle, stack_ref);
    }
    stack->ptr[stack->head] = value;
    stack->head++;
}

static inline VALUE *rvalue_stack_peek(rvalue_stack *stack, long count)
{
    return stack->ptr + (stack->head - count);
}

static inline void rvalue_stack_pop(rvalue_stack *stack, long count)
{
    stack->head -= count;
}

static void rvalue_stack_mark(void *ptr)
{
    rvalue_stack *stack = (rvalue_stack *)ptr;
    long index;
    for (index = 0; index < stack->head; index++) {
        rb_gc_mark(stack->ptr[index]);
    }
}

static void rvalue_stack_free(void *ptr)
{
    rvalue_stack *stack = (rvalue_stack *)ptr;
    if (stack) {
        ruby_xfree(stack->ptr);
        ruby_xfree(stack);
    }
}

static size_t rvalue_stack_memsize(const void *ptr)
{
    const rvalue_stack *stack = (const rvalue_stack *)ptr;
    return sizeof(rvalue_stack) + sizeof(VALUE) * stack->capa;
}

static const rb_data_type_t JSON_Parser_rvalue_stack_type = {
    "JSON::Ext::Parser/rvalue_stack",
    {
        .dmark = rvalue_stack_mark,
        .dfree = rvalue_stack_free,
        .dsize = rvalue_stack_memsize,
    },
    0, 0,
    RUBY_TYPED_FREE_IMMEDIATELY,
};

static rvalue_stack *rvalue_stack_spill(rvalue_stack *old_stack, VALUE *handle, rvalue_stack **stack_ref)
{
    rvalue_stack *stack;
    *handle = TypedData_Make_Struct(0, rvalue_stack, &JSON_Parser_rvalue_stack_type, stack);
    *stack_ref = stack;
    MEMCPY(stack, old_stack, rvalue_stack, 1);

    stack->capa = old_stack->capa << 1;
    stack->ptr = ALLOC_N(VALUE, stack->capa);
    stack->type = RVALUE_STACK_HEAP_ALLOCATED;
    MEMCPY(stack->ptr, old_stack->ptr, VALUE, old_stack->head);
    return stack;
}

static void rvalue_stack_eagerly_release(VALUE handle)
{
    rvalue_stack *stack;
    TypedData_Get_Struct(handle, rvalue_stack, &JSON_Parser_rvalue_stack_type, stack);
    RTYPEDDATA_DATA(handle) = NULL;
    rvalue_stack_free(stack);
}

/* unicode */

static const signed char digit_values[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1,
    -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1
};

static uint32_t unescape_unicode(const unsigned char *p)
{
    const uint32_t replacement_char = 0xFFFD;

    signed char b;
    uint32_t result = 0;
    b = digit_values[p[0]];
    if (b < 0) return replacement_char;
    result = (result << 4) | (unsigned char)b;
    b = digit_values[p[1]];
    if (b < 0) return replacement_char;
    result = (result << 4) | (unsigned char)b;
    b = digit_values[p[2]];
    if (b < 0) return replacement_char;
    result = (result << 4) | (unsigned char)b;
    b = digit_values[p[3]];
    if (b < 0) return replacement_char;
    result = (result << 4) | (unsigned char)b;
    return result;
}

static int convert_UTF32_to_UTF8(char *buf, uint32_t ch)
{
    int len = 1;
    if (ch <= 0x7F) {
        buf[0] = (char) ch;
    } else if (ch <= 0x07FF) {
        buf[0] = (char) ((ch >> 6) | 0xC0);
        buf[1] = (char) ((ch & 0x3F) | 0x80);
        len++;
    } else if (ch <= 0xFFFF) {
        buf[0] = (char) ((ch >> 12) | 0xE0);
        buf[1] = (char) (((ch >> 6) & 0x3F) | 0x80);
        buf[2] = (char) ((ch & 0x3F) | 0x80);
        len += 2;
    } else if (ch <= 0x1fffff) {
        buf[0] =(char) ((ch >> 18) | 0xF0);
        buf[1] =(char) (((ch >> 12) & 0x3F) | 0x80);
        buf[2] =(char) (((ch >> 6) & 0x3F) | 0x80);
        buf[3] =(char) ((ch & 0x3F) | 0x80);
        len += 3;
    } else {
        buf[0] = '?';
    }
    return len;
}

typedef struct JSON_ParserStruct {
    VALUE create_id;
    VALUE object_class;
    VALUE array_class;
    VALUE decimal_class;
    VALUE match_string;
    int max_nesting;
    bool allow_nan;
    bool allow_trailing_comma;
    bool parsing_name;
    bool symbolize_names;
    bool freeze;
    bool create_additions;
    bool deprecated_create_additions;
} JSON_Parser;

typedef struct JSON_ParserStateStruct {
    JSON_Parser *json;
    VALUE Vsource;
    VALUE stack_handle;
    char *source;
    const uint8_t *cursor;
    const uint8_t *end;
    long len;
    char *memo;
    FBuffer fbuffer;
    rvalue_stack *stack;
    rvalue_cache name_cache;
    int in_array;
} JSON_ParserState;

#define GET_PARSER                          \
    JSON_Parser *json;                      \
    TypedData_Get_Struct(self, JSON_Parser, &JSON_Parser_type, json)

static const rb_data_type_t JSON_Parser_type;

#ifndef HAVE_STRNLEN
static size_t strnlen(const char *s, size_t maxlen)
{
    char *p;
    return ((p = memchr(s, '\0', maxlen)) ? p - s : maxlen);
}
#endif

#define PARSE_ERROR_FRAGMENT_LEN 32
#ifdef RBIMPL_ATTR_NORETURN
RBIMPL_ATTR_NORETURN()
#endif
static void raise_parse_error(const char *format, const char *start)
{
    char buffer[PARSE_ERROR_FRAGMENT_LEN + 1];

    size_t len = strnlen(start, PARSE_ERROR_FRAGMENT_LEN);
    const char *ptr = start;

    if (len == PARSE_ERROR_FRAGMENT_LEN) {
        MEMCPY(buffer, start, char, PARSE_ERROR_FRAGMENT_LEN);
        buffer[PARSE_ERROR_FRAGMENT_LEN] = '\0';
        ptr = buffer;
    }

    rb_enc_raise(enc_utf8, rb_path2class("JSON::ParserError"), format, ptr);
}

#define MinusInfinity "-Infinity"

static inline void
json_eat_whitespace(JSON_ParserState *state) {
    while (state->cursor < state->end) {
        switch (*state->cursor) {
            case ' ':
            case '\t':
            case '\n':
            case '\r':
                state->cursor++;
                break;
            default:
                return;
        }
    }
}

static VALUE
json_parse_any(JSON_ParserState *state) {
    json_eat_whitespace(state);
    if (state->cursor >= state->end) {
        raise_parse_error("unexpected end of input", state->cursor);
    }

    switch (*state->cursor) {
        case 'n':
            if ((state->end - state->cursor >= 4) && (memcmp(state->cursor, "null", 4) == 0)) {
                state->cursor += 4;
                return Qnil;
            }

            raise_parse_error("unexpected character", state->cursor);
            break;
        case 't':
            if ((state->end - state->cursor >= 4) && (memcmp(state->cursor, "true", 4) == 0)) {
                state->cursor += 4;
                return Qtrue;
            }

            raise_parse_error("unexpected character", state->cursor);
            break;
        case 'f':
            if ((state->end - state->cursor >= 5) && (memcmp(state->cursor, "false", 5) == 0)) {
                state->cursor += 5;
                return Qfalse;
            }

            raise_parse_error("unexpected character", state->cursor);
            break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
            // /\A-?(0|[1-9]\d*)(\.\d+)?([Ee][-+]?\d+)?/
            const uint8_t *start = state->cursor;
            while ((state->cursor < state->end) && (*state->cursor >= '0') && (*state->cursor <= '9')) {
                state->cursor++;
            }

            if ((state->cursor < state->end) && (*state->cursor == '.')) {
                state->cursor++;
                while ((state->cursor < state->end) && (*state->cursor >= '0') && (*state->cursor <= '9')) {
                    state->cursor++;
                }
            }

            if ((state->cursor < state->end) && ((*state->cursor == 'e') || (*state->cursor == 'E'))) {
                state->cursor++;
                if ((state->cursor < state->end) && ((*state->cursor == '+') || (*state->cursor == '-'))) {
                    state->cursor++;
                }

                while ((state->cursor < state->end) && (*state->cursor >= '0') && (*state->cursor <= '9')) {
                    state->cursor++;
                }
            }

            // TODO: proper number parsing
            return rb_cstr_to_inum((const char *) start, (int) (state->cursor - start), 10);
        }
        case '"': {
            // %r{\A"[^"\\\t\n\x00]*(?:\\[bfnrtu\\/"][^"\\]*)*"}
            state->cursor++;
            const uint8_t *start = state->cursor;

            while (state->cursor < state->end) {
                if (*state->cursor == '"') {
                    VALUE string = rb_enc_str_new((const char *) start, state->cursor - start, rb_utf8_encoding());
                    state->cursor++;
                    return string;
                } else if (*state->cursor == '\\') {
                    // Parse escape sequence
                    state->cursor++;
                }

                state->cursor++;
            }

            raise_parse_error("unexpected end of input", state->cursor);
            break;
        }
        case '[': {
            VALUE array = rb_ary_new();
            state->cursor++;

            json_eat_whitespace(state);
            if ((state->cursor < state->end) && (*state->cursor == ']')) {
                state->cursor++;
                return array;
            }

            while (state->cursor < state->end) {
                VALUE element = json_parse_any(state);
                rb_ary_push(array, element);

                switch (*state->cursor) {
                    case ',':
                        state->cursor++;
                        break;
                    case ']':
                        state->cursor++;
                        return array;
                    default:
                        raise_parse_error("expected ',' or ']' after array value", state->cursor);
                }
            }

            raise_parse_error("unexpected end of input", state->cursor);
            break;
        }
        case '{': {
            state->cursor++;
            json_eat_whitespace(state);

            if ((state->cursor < state->end) && (*state->cursor == '}')) {
                state->cursor++;
                return rb_hash_new();
            }

            VALUE elements = rb_ary_new();
            while (state->cursor < state->end) {
                json_eat_whitespace(state);
                if (*state->cursor != '"') {
                    raise_parse_error("expected object key", state->cursor);
                }

                VALUE key = json_parse_any(state);
                json_eat_whitespace(state);

                if ((state->cursor >= state->end) || (*state->cursor != ':')) {
                    raise_parse_error("expected ':' after object key", state->cursor);
                }
                state->cursor++;

                VALUE value = json_parse_any(state);
                VALUE pair[2] = { key, value };
                rb_ary_cat(elements, pair, 2);

                json_eat_whitespace(state);
                switch (*state->cursor) {
                    case ',':
                        state->cursor++;
                        break;
                    case '}': {
                        state->cursor++;
                        VALUE value = rb_hash_new_capa(RARRAY_LEN(elements));
                        rb_hash_bulk_insert(RARRAY_LEN(elements), RARRAY_CONST_PTR(elements), value);
                        return value;
                    }
                    default:
                        raise_parse_error("expected ',' or '}' after object value", state->cursor);
                }
            }

            raise_parse_error("unexpected end of input", state->cursor);
            break;
        }
        default:
            raise_parse_error("unexpected character", state->cursor);
            break;
    }

    raise_parse_error("unexpected character", state->cursor);
}

/*
 * Document-class: JSON::Ext::Parser
 *
 * This is the JSON parser implemented as a C extension. It can be configured
 * to be used by setting
 *
 *  JSON.parser = JSON::Ext::Parser
 *
 * with the method parser= in JSON.
 *
 */

static VALUE convert_encoding(VALUE source)
{
  int encindex = RB_ENCODING_GET(source);

  if (RB_LIKELY(encindex == utf8_encindex)) {
    return source;
  }

 if (encindex == binary_encindex) {
    // For historical reason, we silently reinterpret binary strings as UTF-8
    return rb_enc_associate_index(rb_str_dup(source), utf8_encindex);
  }

  return rb_funcall(source, i_encode, 1, Encoding_UTF_8);
}

static int configure_parser_i(VALUE key, VALUE val, VALUE data)
{
    JSON_Parser *json = (JSON_Parser *)data;

         if (key == sym_max_nesting)          { json->max_nesting = RTEST(val) ? FIX2INT(val) : 0; }
    else if (key == sym_allow_nan)            { json->allow_nan = RTEST(val); }
    else if (key == sym_allow_trailing_comma) { json->allow_trailing_comma = RTEST(val); }
    else if (key == sym_symbolize_names)      { json->symbolize_names = RTEST(val); }
    else if (key == sym_freeze)               { json->freeze = RTEST(val); }
    else if (key == sym_create_id)            { json->create_id = RTEST(val) ? val : Qfalse; }
    else if (key == sym_object_class)         { json->object_class = RTEST(val) ? val : Qfalse; }
    else if (key == sym_array_class)          { json->array_class = RTEST(val) ? val : Qfalse; }
    else if (key == sym_decimal_class)        { json->decimal_class = RTEST(val) ? val : Qfalse; }
    else if (key == sym_match_string)         { json->match_string = RTEST(val) ? val : Qfalse; }
    else if (key == sym_create_additions)     {
        if (NIL_P(val)) {
            json->create_additions = true;
            json->deprecated_create_additions = true;
        } else {
            json->create_additions = RTEST(val);
            json->deprecated_create_additions = false;
        }
    }

    return ST_CONTINUE;
}

static void parser_init(JSON_Parser *json, VALUE opts)
{
    json->max_nesting = 100;

    if (!NIL_P(opts)) {
        Check_Type(opts, T_HASH);
        if (RHASH_SIZE(opts) > 0) {
            // We assume in most cases few keys are set so it's faster to go over
            // the provided keys than to check all possible keys.
            rb_hash_foreach(opts, configure_parser_i, (VALUE)json);

            if (json->symbolize_names && json->create_additions) {
                rb_raise(rb_eArgError,
                    "options :symbolize_names and :create_additions cannot be "
                    " used in conjunction");
            }

            if (json->create_additions && !json->create_id) {
                json->create_id = rb_funcall(mJSON, i_create_id, 0);
            }
        }

    }
}

/*
 * call-seq: new(opts => {})
 *
 * Creates a new JSON::Ext::ParserConfig instance.
 *
 * It will be configured by the _opts_ hash. _opts_ can have the following
 * keys:
 *
 * _opts_ can have the following keys:
 * * *max_nesting*: The maximum depth of nesting allowed in the parsed data
 *   structures. Disable depth checking with :max_nesting => false|nil|0, it
 *   defaults to 100.
 * * *allow_nan*: If set to true, allow NaN, Infinity and -Infinity in
 *   defiance of RFC 4627 to be parsed by the Parser. This option defaults to
 *   false.
 * * *symbolize_names*: If set to true, returns symbols for the names
 *   (keys) in a JSON object. Otherwise strings are returned, which is
 *   also the default. It's not possible to use this option in
 *   conjunction with the *create_additions* option.
 * * *create_additions*: If set to false, the Parser doesn't create
 *   additions even if a matching class and create_id was found. This option
 *   defaults to false.
 * * *object_class*: Defaults to Hash. If another type is provided, it will be used
 *   instead of Hash to represent JSON objects. The type must respond to
 *   +new+ without arguments, and return an object that respond to +[]=+.
 * * *array_class*: Defaults to Array If another type is provided, it will be used
 *   instead of Hash to represent JSON arrays. The type must respond to
 *   +new+ without arguments, and return an object that respond to +<<+.
 * * *decimal_class*: Specifies which class to use instead of the default
 *    (Float) when parsing decimal numbers. This class must accept a single
 *    string argument in its constructor.
 */
static VALUE cParserConfig_initialize(VALUE self, VALUE opts)
{
    GET_PARSER;

    parser_init(json, opts);
    return self;
}

static VALUE cParser_parse_safe(VALUE vstate)
{
    JSON_ParserState *state = (JSON_ParserState *)vstate;
    return json_parse_any(state);
}

static VALUE cParser_parse(JSON_Parser *json, VALUE Vsource)
{
    Vsource = convert_encoding(StringValue(Vsource));
    StringValue(Vsource);

    VALUE rvalue_stack_buffer[RVALUE_STACK_INITIAL_CAPA];
    rvalue_stack stack = {
        .type = RVALUE_STACK_STACK_ALLOCATED,
        .ptr = rvalue_stack_buffer,
        .capa = RVALUE_STACK_INITIAL_CAPA,
    };

    JSON_ParserState _state = {
        .json = json,
        .len = RSTRING_LEN(Vsource),
        .source = RSTRING_PTR(Vsource),
        .Vsource = Vsource,
        .stack = &stack,
    };
    JSON_ParserState *state = &_state;

    char stack_buffer[FBUFFER_STACK_SIZE];
    fbuffer_stack_init(&state->fbuffer, FBUFFER_INITIAL_LENGTH_DEFAULT, stack_buffer, FBUFFER_STACK_SIZE);

    int interupted;
    VALUE result = rb_protect(cParser_parse_safe, (VALUE)state, &interupted);

    fbuffer_free(&state->fbuffer);
    if (interupted) {
        rb_jump_tag(interupted);
    }

    return result;
}

/*
 * call-seq: parse(source)
 *
 *  Parses the current JSON text _source_ and returns the complete data
 *  structure as a result.
 *  It raises JSON::ParserError if fail to parse.
 */
static VALUE cParserConfig_parse(VALUE self, VALUE Vsource)
{
    GET_PARSER;
    return cParser_parse(json, Vsource);
}

static VALUE cParser_m_parse(VALUE klass, VALUE Vsource, VALUE opts)
{
    Vsource = convert_encoding(StringValue(Vsource));
    StringValue(Vsource);

    JSON_Parser _parser = {0};
    JSON_Parser *json = &_parser;
    parser_init(json, opts);

    return cParser_parse(json, Vsource);
}

static void JSON_mark(void *ptr)
{
    JSON_Parser *json = ptr;
    rb_gc_mark(json->create_id);
    rb_gc_mark(json->object_class);
    rb_gc_mark(json->array_class);
    rb_gc_mark(json->decimal_class);
    rb_gc_mark(json->match_string);
}

static void JSON_free(void *ptr)
{
    JSON_Parser *json = ptr;
    ruby_xfree(json);
}

static size_t JSON_memsize(const void *ptr)
{
    return sizeof(JSON_Parser);
}

static const rb_data_type_t JSON_Parser_type = {
    "JSON/Parser",
    {JSON_mark, JSON_free, JSON_memsize,},
    0, 0,
    RUBY_TYPED_FREE_IMMEDIATELY,
};

static VALUE cJSON_parser_s_allocate(VALUE klass)
{
    JSON_Parser *json;
    return TypedData_Make_Struct(klass, JSON_Parser, &JSON_Parser_type, json);
}

void Init_parser(void)
{
#ifdef HAVE_RB_EXT_RACTOR_SAFE
    rb_ext_ractor_safe(true);
#endif

#undef rb_intern
    rb_require("json/common");
    mJSON = rb_define_module("JSON");
    VALUE mExt = rb_define_module_under(mJSON, "Ext");
    VALUE cParserConfig = rb_define_class_under(mExt, "ParserConfig", rb_cObject);
    eNestingError = rb_path2class("JSON::NestingError");
    rb_gc_register_mark_object(eNestingError);
    rb_define_alloc_func(cParserConfig, cJSON_parser_s_allocate);
    rb_define_method(cParserConfig, "initialize", cParserConfig_initialize, 1);
    rb_define_method(cParserConfig, "parse", cParserConfig_parse, 1);

    VALUE cParser = rb_define_class_under(mExt, "Parser", rb_cObject);
    rb_define_singleton_method(cParser, "parse", cParser_m_parse, 2);

    CNaN = rb_const_get(mJSON, rb_intern("NaN"));
    rb_gc_register_mark_object(CNaN);

    CInfinity = rb_const_get(mJSON, rb_intern("Infinity"));
    rb_gc_register_mark_object(CInfinity);

    CMinusInfinity = rb_const_get(mJSON, rb_intern("MinusInfinity"));
    rb_gc_register_mark_object(CMinusInfinity);

    rb_global_variable(&Encoding_UTF_8);
    Encoding_UTF_8 = rb_const_get(rb_path2class("Encoding"), rb_intern("UTF_8"));

    sym_max_nesting = ID2SYM(rb_intern("max_nesting"));
    sym_allow_nan = ID2SYM(rb_intern("allow_nan"));
    sym_allow_trailing_comma = ID2SYM(rb_intern("allow_trailing_comma"));
    sym_symbolize_names = ID2SYM(rb_intern("symbolize_names"));
    sym_freeze = ID2SYM(rb_intern("freeze"));
    sym_create_additions = ID2SYM(rb_intern("create_additions"));
    sym_create_id = ID2SYM(rb_intern("create_id"));
    sym_object_class = ID2SYM(rb_intern("object_class"));
    sym_array_class = ID2SYM(rb_intern("array_class"));
    sym_decimal_class = ID2SYM(rb_intern("decimal_class"));
    sym_match_string = ID2SYM(rb_intern("match_string"));

    i_create_id = rb_intern("create_id");
    i_json_creatable_p = rb_intern("json_creatable?");
    i_json_create = rb_intern("json_create");
    i_chr = rb_intern("chr");
    i_match = rb_intern("match");
    i_deep_const_get = rb_intern("deep_const_get");
    i_aset = rb_intern("[]=");
    i_aref = rb_intern("[]");
    i_leftshift = rb_intern("<<");
    i_new = rb_intern("new");
    i_try_convert = rb_intern("try_convert");
    i_uminus = rb_intern("-@");
    i_encode = rb_intern("encode");

    binary_encindex = rb_ascii8bit_encindex();
    utf8_encindex = rb_utf8_encindex();
    enc_utf8 = rb_utf8_encoding();
}
