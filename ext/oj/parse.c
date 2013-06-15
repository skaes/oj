/* parse.c
 * Copyright (c) 2013, Peter Ohler
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  - Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 *  - Neither the name of Peter Ohler nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include "oj.h"
#include "parse.h"
#include "buf.h"
#include "val_stack.h"

// Workaround in case INFINITY is not defined in math.h or if the OS is CentOS
#define OJ_INFINITY (1.0/0.0)

#ifdef RUBINIUS_RUBY
#define NUM_MAX 0x07FFFFFF
#else
#define NUM_MAX (FIXNUM_MAX >> 8)
#endif
#define EXP_MAX	1023
#define I64_MAX	0x7FFFFFFFFFFFFFFFLL
#define DEC_MAX	14

static void
next_non_white(ParseInfo pi) {
    for (; 1; pi->cur++) {
	switch(*pi->cur) {
	case ' ':
	case '\t':
	case '\f':
	case '\n':
	case '\r':
	    break;
	default:
	    return;
	}
    }
}

static void
skip_comment(ParseInfo pi) {
    if ('*' == *pi->cur) {
	pi->cur++;
	for (; '\0' != *pi->cur; pi->cur++) {
	    if ('*' == *pi->cur && '/' == *(pi->cur + 1)) {
		pi->cur += 2;
		return;
	    } else if ('\0' == *pi->cur) {
		oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "comment not terminated");
		return;
	    }
	}
    } else if ('/' == *pi->cur) {
	for (; 1; pi->cur++) {
	    switch (*pi->cur) {
	    case '\n':
	    case '\r':
	    case '\f':
	    case '\0':
		return;
	    default:
		break;
	    }
	}
    } else {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "invalid comment format");
    }
}

static void
add_value(ParseInfo pi, VALUE rval) {
    Val	parent = stack_peek(&pi->stack);

    if (0 == parent) { // simple add
	pi->add_value(pi, rval);
    } else {
	switch (parent->next) {
	case NEXT_ARRAY_NEW:
	case NEXT_ARRAY_ELEMENT:
	    pi->array_append_value(pi, rval);
	    parent->next = NEXT_ARRAY_COMMA;
	    break;
	case NEXT_HASH_VALUE:
	    pi->hash_set_value(pi, parent->key, parent->klen, rval);
	    if (0 != parent->key && (parent->key < pi->json || pi->cur < parent->key)) {
		xfree((char*)parent->key);
		parent->key = 0;
	    }
	    parent->next = NEXT_HASH_COMMA;
	    break;
	case NEXT_HASH_NEW:
	case NEXT_HASH_KEY:
	case NEXT_HASH_COMMA:
	case NEXT_NONE:
	case NEXT_ARRAY_COMMA:
	case NEXT_HASH_COLON:
	default:
	    oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s", oj_stack_next_string(parent->next));
	    break;
	}
    }
}

static void
read_null(ParseInfo pi) {
    if ('u' == *pi->cur++ && 'l' == *pi->cur++ && 'l' == *pi->cur++) {
	add_value(pi, Qnil);
    } else {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "expected null");
    }
}

static void
read_true(ParseInfo pi) {
    if ('r' == *pi->cur++ && 'u' == *pi->cur++ && 'e' == *pi->cur++) {
	add_value(pi, Qtrue);
    } else {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "expected true");
    }
}

static void
read_false(ParseInfo pi) {
    if ('a' == *pi->cur++ && 'l' == *pi->cur++ && 's' == *pi->cur++ && 'e' == *pi->cur++) {
	add_value(pi, Qfalse);
    } else {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "expected false");
    }
}

static uint32_t
read_hex(ParseInfo pi, const char *h) {
    uint32_t	b = 0;
    int		i;

    for (i = 0; i < 4; i++, h++) {
	b = b << 4;
	if ('0' <= *h && *h <= '9') {
	    b += *h - '0';
	} else if ('A' <= *h && *h <= 'F') {
	    b += *h - 'A' + 10;
	} else if ('a' <= *h && *h <= 'f') {
	    b += *h - 'a' + 10;
	} else {
	    oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "invalid hex character");
	    return 0;
	}
    }
    return b;
}

static void
unicode_to_chars(ParseInfo pi, Buf buf, uint32_t code) {
    if (0x0000007F >= code) {
	buf_append(buf, (char)code);
    } else if (0x000007FF >= code) {
	buf_append(buf, 0xC0 | (code >> 6));
	buf_append(buf, 0x80 | (0x3F & code));
    } else if (0x0000FFFF >= code) {
	buf_append(buf, 0xE0 | (code >> 12));
	buf_append(buf, 0x80 | ((code >> 6) & 0x3F));
	buf_append(buf, 0x80 | (0x3F & code));
    } else if (0x001FFFFF >= code) {
	buf_append(buf, 0xF0 | (code >> 18));
	buf_append(buf, 0x80 | ((code >> 12) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 6) & 0x3F));
	buf_append(buf, 0x80 | (0x3F & code));
    } else if (0x03FFFFFF >= code) {
	buf_append(buf, 0xF8 | (code >> 24));
	buf_append(buf, 0x80 | ((code >> 18) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 12) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 6) & 0x3F));
	buf_append(buf, 0x80 | (0x3F & code));
    } else if (0x7FFFFFFF >= code) {
	buf_append(buf, 0xFC | (code >> 30));
	buf_append(buf, 0x80 | ((code >> 24) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 18) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 12) & 0x3F));
	buf_append(buf, 0x80 | ((code >> 6) & 0x3F));
	buf_append(buf, 0x80 | (0x3F & code));
    } else {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "invalid Unicode character");
    }
}

// entered at /
static void
read_escaped_str(ParseInfo pi, const char *start) {
    struct _Buf	buf;
    const char	*s;
    int		cnt = pi->cur - start;
    uint32_t	code;
    Val		parent = stack_peek(&pi->stack);

    buf_init(&buf);
    if (0 < cnt) {
	buf_append_string(&buf, start, cnt);
    }
    for (s = pi->cur; '"' != *s; s++) {
	if ('\0' == *s) {
	    oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "quoted string not terminated");
	    buf_cleanup(&buf);
	    return;
	} else if ('\\' == *s) {
	    s++;
	    switch (*s) {
	    case 'n':	buf_append(&buf, '\n');	break;
	    case 'r':	buf_append(&buf, '\r');	break;
	    case 't':	buf_append(&buf, '\t');	break;
	    case 'f':	buf_append(&buf, '\f');	break;
	    case 'b':	buf_append(&buf, '\b');	break;
	    case '"':	buf_append(&buf, '"');	break;
	    case '/':	buf_append(&buf, '/');	break;
	    case '\\':	buf_append(&buf, '\\');	break;
	    case 'u':
		s++;
		if (0 == (code = read_hex(pi, s)) && err_has(&pi->err)) {
		    buf_cleanup(&buf);
		    return;
		}
		s += 3;
		if (0x0000D800 <= code && code <= 0x0000DFFF) {
		    uint32_t	c1 = (code - 0x0000D800) & 0x000003FF;
		    uint32_t	c2;

		    s++;
		    if ('\\' != *s || 'u' != *(s + 1)) {
			pi->cur = s;
			oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "invalid escaped character");
			buf_cleanup(&buf);
			return;
		    }
		    s += 2;
		    if (0 == (c2 = read_hex(pi, s)) && err_has(&pi->err)) {
			buf_cleanup(&buf);
			return;
		    }
		    s += 3;
		    c2 = (c2 - 0x0000DC00) & 0x000003FF;
		    code = ((c1 << 10) | c2) + 0x00010000;
		}
		unicode_to_chars(pi, &buf, code);
		if (err_has(&pi->err)) {
		    buf_cleanup(&buf);
		    return;
		}
		break;
	    default:
		pi->cur = s;
		oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "invalid escaped character");
		buf_cleanup(&buf);
		return;
	    }
	} else {
	    buf_append(&buf, *s);
	}
    }
    if (0 == parent) {
	pi->add_cstr(pi, buf.head, buf_len(&buf), start);
    } else {
	switch (parent->next) {
	case NEXT_ARRAY_NEW:
	case NEXT_ARRAY_ELEMENT:
	    pi->array_append_cstr(pi, buf.head, buf_len(&buf), start);
	    parent->next = NEXT_ARRAY_COMMA;
	    break;
	case NEXT_HASH_NEW:
	case NEXT_HASH_KEY:
	    // key will not be between pi->json and pi->cur.
	    parent->key = strdup(buf.head);
	    parent->klen = buf_len(&buf);
	    parent->next = NEXT_HASH_COLON;
	    break;
	case NEXT_HASH_VALUE:
	    pi->hash_set_cstr(pi, parent->key, parent->klen, buf.head, buf_len(&buf), start);
	    if (0 != parent->key && (parent->key < pi->json || pi->cur < parent->key)) {
		xfree((char*)parent->key);
		parent->key = 0;
	    }
	    parent->next = NEXT_HASH_COMMA;
	    break;
	case NEXT_HASH_COMMA:
	case NEXT_NONE:
	case NEXT_ARRAY_COMMA:
	case NEXT_HASH_COLON:
	default:
	    oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s, not a string", oj_stack_next_string(parent->next));
	    break;
	}
    }
    pi->cur = s + 1;
    buf_cleanup(&buf);
}

static void
read_str(ParseInfo pi) {
    const char	*str = pi->cur;
    Val		parent = stack_peek(&pi->stack);

    for (; '"' != *pi->cur; pi->cur++) {
	if ('\0' == *pi->cur) {
	    oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "quoted string not terminated");
	    return;
	} else if ('\\' == *pi->cur) {
	    read_escaped_str(pi, str);
	    return;
	}
    }
    if (0 == parent) { // simple add
	pi->add_cstr(pi, str, pi->cur - str, str);
    } else {
	switch (parent->next) {
	case NEXT_ARRAY_NEW:
	case NEXT_ARRAY_ELEMENT:
	    pi->array_append_cstr(pi, str, pi->cur - str, str);
	    parent->next = NEXT_ARRAY_COMMA;
	    break;
	case NEXT_HASH_NEW:
	case NEXT_HASH_KEY:
	    parent->key = str;
	    parent->klen = pi->cur - str;
	    parent->next = NEXT_HASH_COLON;
	    break;
	case NEXT_HASH_VALUE:
	    pi->hash_set_cstr(pi, parent->key, parent->klen, str, pi->cur - str, str);
	    if (0 != parent->key && (parent->key < pi->json || pi->cur < parent->key)) {
		xfree((char*)parent->key);
		parent->key = 0;
	    }
	    parent->next = NEXT_HASH_COMMA;
	    break;
	case NEXT_HASH_COMMA:
	case NEXT_NONE:
	case NEXT_ARRAY_COMMA:
	case NEXT_HASH_COLON:
	default:
	    oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s, not a string", oj_stack_next_string(parent->next));
	    break;
	}
    }
    pi->cur++; // move past "
}

static void
read_num(ParseInfo pi) {
    struct _NumInfo	ni;
    Val			parent = stack_peek(&pi->stack);
    int			zero_cnt = 0;

    ni.str = pi->cur;
    ni.i = 0;
    ni.num = 0;
    ni.div = 1;
    ni.len = 0;
    ni.exp = 0;
    ni.dec_cnt = 0;
    ni.big = 0;
    ni.infinity = 0;
    ni.neg = 0;

    if ('-' == *pi->cur) {
	pi->cur++;
	ni.neg = 1;
    } else if ('+' == *pi->cur) {
	pi->cur++;
    }
    if ('I' == *pi->cur) {
	if (0 != strncmp("Infinity", pi->cur, 8)) {
	    oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "not a number or other value");
	    return;
	}
	pi->cur += 8;
	ni.infinity = 1;
	return;
    }
    for (; '0' <= *pi->cur && *pi->cur <= '9'; pi->cur++) {
	ni.dec_cnt++;
	if (ni.big) {
	    ni.big++;
	} else {
	    int	d = (*pi->cur - '0');

	    if (0 == d) {
		zero_cnt++;
	    } else {
		zero_cnt = 0;
	    }
	    ni.i = ni.i * 10 + d;
	    if (I64_MAX <= ni.i || DEC_MAX < ni.dec_cnt - zero_cnt) {
		ni.big = 1;
	    }
	}
    }
    if ('.' == *pi->cur) {
	pi->cur++;
	for (; '0' <= *pi->cur && *pi->cur <= '9'; pi->cur++) {
	    int	d = (*pi->cur - '0');

	    if (0 == d) {
		zero_cnt++;
	    } else {
		zero_cnt = 0;
	    }
	    ni.dec_cnt++;
	    ni.num = ni.num * 10 + d;
	    ni.div *= 10;
	    if (I64_MAX <= ni.div || DEC_MAX < ni.dec_cnt - zero_cnt) {
		ni.big = 1;
	    }
	}
    }
    if ('e' == *pi->cur || 'E' == *pi->cur) {
	int	eneg = 0;

	pi->cur++;
	if ('-' == *pi->cur) {
	    pi->cur++;
	    eneg = 1;
	} else if ('+' == *pi->cur) {
	    pi->cur++;
	}
	for (; '0' <= *pi->cur && *pi->cur <= '9'; pi->cur++) {
	    ni.exp = ni.exp * 10 + (*pi->cur - '0');
	    if (EXP_MAX <= ni.exp) {
		ni.big = 1;
	    }
	}
	if (eneg) {
	    ni.exp = -ni.exp;
	}
    }
    ni.dec_cnt -= zero_cnt;
    ni.len = pi->cur - ni.str;
    if (Yes == pi->options.bigdec_load) {
	ni.big = 1;
    }
    if (0 == parent) {
	pi->add_num(pi, &ni);
    } else {
	switch (parent->next) {
	case NEXT_ARRAY_NEW:
	case NEXT_ARRAY_ELEMENT:
	    pi->array_append_num(pi, &ni);
	    parent->next = NEXT_ARRAY_COMMA;
	    break;
	case NEXT_HASH_VALUE:
	    pi->hash_set_num(pi, parent->key, parent->klen, &ni);
	    if (0 != parent->key && (parent->key < pi->json || pi->cur < parent->key)) {
		xfree((char*)parent->key);
		parent->key = 0;
	    }
	    parent->next = NEXT_HASH_COMMA;
	    break;
	default:
	    oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s", oj_stack_next_string(parent->next));
	    break;
	}
    }
}

static void
array_start(ParseInfo pi) {
    VALUE	v = Qnil;

    v = pi->start_array(pi);
    stack_push(&pi->stack, v, NEXT_ARRAY_NEW);
}

static void
array_end(ParseInfo pi) {
    Val	array = stack_pop(&pi->stack);

    if (0 == array) {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected array close");
    } else if (NEXT_ARRAY_COMMA != array->next && NEXT_ARRAY_NEW != array->next) {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s, not an array close", oj_stack_next_string(array->next));
    } else {
	pi->end_array(pi);
	add_value(pi, array->val);
    }
}

static void
hash_start(ParseInfo pi) {
    VALUE	v = Qnil;

    v = pi->start_hash(pi);
    stack_push(&pi->stack, v, NEXT_HASH_NEW);
}

static void
hash_end(ParseInfo pi) {
    Val	hash = stack_peek(&pi->stack);

    // leave hash on stack until just before 
    if (0 == hash) {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected hash close");
    } else if (NEXT_HASH_COMMA != hash->next && NEXT_HASH_NEW != hash->next) {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "expected %s, not a hash close", oj_stack_next_string(hash->next));
    } else {
	pi->end_hash(pi);
	stack_pop(&pi->stack);
	add_value(pi, hash->val);
    }
}

static void
comma(ParseInfo pi) {
    Val	parent = stack_peek(&pi->stack);

    if (0 == parent) {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected comma");
    } else if (NEXT_ARRAY_COMMA == parent->next) {
	parent->next = NEXT_ARRAY_ELEMENT;
    } else if (NEXT_HASH_COMMA == parent->next) {
	parent->next = NEXT_HASH_KEY;
    } else {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected comma");
    }
}

static void
colon(ParseInfo pi) {
    Val	parent = stack_peek(&pi->stack);

    if (0 != parent && NEXT_HASH_COLON == parent->next) {
	parent->next = NEXT_HASH_VALUE;
    } else {
	oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected colon");
    }
}

void
oj_parse2(ParseInfo pi) {
    pi->cur = pi->json;
    err_init(&pi->err);
    stack_init(&pi->stack);
    while (1) {
	next_non_white(pi);
	switch (*pi->cur++) {
	case '{':
	    hash_start(pi);
	    break;
	case '}':
	    hash_end(pi);
	    break;
	case ':':
	    colon(pi);
	    break;
	case '[':
	    array_start(pi);
	    break;
	case ']':
	    array_end(pi);
	    break;
	case ',':
	    comma(pi);
	    break;
	case '"':
	    read_str(pi);
	    break;
	case '+':
	case '-':
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	case 'I':
	    pi->cur--;
	    read_num(pi);
	    break;
	case 't':
	    read_true(pi);
	    break;
	case 'f':
	    read_false(pi);
	    break;
	case 'n':
	    read_null(pi);
	    break;
	case '/':
	    skip_comment(pi);
	    break;
	case '\0':
	    pi->cur--;
	    return;
	default:
	    oj_set_error_at(pi, oj_parse_error_class, __FILE__, __LINE__, "unexpected character");
	    return;
	}
	if (err_has(&pi->err)) {
	    return;
	}
    }
}

VALUE
oj_num_as_value(NumInfo ni) {
    VALUE	rnum = Qnil;

    if (ni->infinity) {
	if (ni->neg) {
	    rnum = rb_float_new(-OJ_INFINITY);
	} else {
	    rnum = rb_float_new(OJ_INFINITY);
	}
    } else if (1 == ni->div && 0 == ni->exp) { // fixnum
	if (ni->big) {
	    if (256 > ni->len) {
		char	buf[256];

		memcpy(buf, ni->str, ni->len);
		buf[ni->len] = '\0';
		rnum = rb_cstr_to_inum(buf, 10, 0);
	    } else {
		char	*buf = ALLOC_N(char, ni->len + 1);

		memcpy(buf, ni->str, ni->len);
		buf[ni->len] = '\0';
		rnum = rb_cstr_to_inum(buf, 10, 0);
		xfree(buf);
	    }
	} else {
	    if (ni->neg) {
		rnum = LONG2NUM(-ni->i);
	    } else {
		rnum = LONG2NUM(ni->i);
	    }
	}
    } else { // decimal
	if (ni->big) {
	    rnum = rb_funcall(oj_bigdecimal_class, oj_new_id, 1, rb_str_new(ni->str, ni->len));
	} else {
	    double	d = (double)ni->i + (double)ni->num / (double)ni->div;

	    if (ni->neg) {
		d = -d;
	    }
	    if (0 != ni->exp) {
		d *= pow(10.0, ni->exp);
	    }
	    rnum = rb_float_new(d);
	}
    }
    return rnum;
}

void
oj_set_error_at(ParseInfo pi, VALUE err_clas, const char* file, int line, const char *format, ...) {
    va_list	ap;
    char	msg[128];

    va_start(ap, format);
    vsnprintf(msg, sizeof(msg) - 1, format, ap);
    va_end(ap);
    pi->err.clas = err_clas;
    _oj_err_set_with_location(&pi->err, err_clas, msg, pi->json, pi->cur - 1, file, line);
}

static VALUE
protect_parse(VALUE pip) {
    oj_parse2((ParseInfo)pip);

    return Qnil;
}

VALUE
oj_pi_parse(int argc, VALUE *argv, ParseInfo pi, char *json) {
    char	*buf = 0;
    VALUE	input;
    VALUE	result = Qnil;
    int		line = 0;
    int		free_json = 0;

    if (argc < 1) {
	rb_raise(rb_eArgError, "Wrong number of arguments to parse.");
    }
    input = argv[0];
    pi->options = oj_default_options;
    if (2 == argc) {
	oj_parse_options(argv[1], &pi->options);
    }
    pi->cbc = (void*)0;
    if (0 != json) {
	pi->json = json;
	free_json = 1;
    } else if (rb_type(input) == T_STRING) {
	pi->json = StringValuePtr(input);
    } else {
	VALUE	clas = rb_obj_class(input);
	VALUE	s;

	if (oj_stringio_class == clas) {
	    s = rb_funcall2(input, oj_string_id, 0, 0);
	    pi->json = StringValuePtr(s);
#ifndef JRUBY_RUBY
#if !IS_WINDOWS
	    // JRuby gets confused with what is the real fileno.
	} else if (rb_respond_to(input, oj_fileno_id) && Qnil != (s = rb_funcall(input, oj_fileno_id, 0))) {
	    int		fd = FIX2INT(s);
	    ssize_t	cnt;
	    size_t	len = lseek(fd, 0, SEEK_END);

	    lseek(fd, 0, SEEK_SET);
	    if (pi->options.max_stack < len) {
		buf = ALLOC_N(char, len + 1);
		pi->json = buf;
	    } else {
		pi->json = ALLOCA_N(char, len + 1);
	    }
	    if (0 >= (cnt = read(fd, (char*)pi->json, len)) || cnt != (ssize_t)len) {
		if (0 != buf) {
		    xfree(buf);
		}
		rb_raise(rb_eIOError, "failed to read from IO Object.");
	    }
	    ((char*)pi->json)[len] = '\0';
	    /* skip UTF-8 BOM if present */
	    if (0xEF == (uint8_t)*pi->json && 0xBB == (uint8_t)pi->json[1] && 0xBF == (uint8_t)pi->json[2]) {
		pi->json += 3;
	    }
#endif
#endif
	} else if (rb_respond_to(input, oj_read_id)) {
	    s = rb_funcall2(input, oj_read_id, 0, 0);
	    pi->json = StringValuePtr(s);
	} else {
	    rb_raise(rb_eArgError, "strict_parse() expected a String or IO Object.");
	}
    }
    if (Yes == pi->options.circular) {
	pi->circ_array = oj_circ_array_new();
    } else {
	pi->circ_array = 0;
    }
    rb_protect(protect_parse, (VALUE)pi, &line);
    if (0 != pi->circ_array) {
	oj_circ_array_free(pi->circ_array);
    }
    if (0 != buf) {
	xfree(buf);
    } else if (free_json) {
	xfree(json);
    }
    result = stack_head_val(&pi->stack);
    stack_cleanup(&pi->stack);
    if (0 != line) {
	rb_jump_tag(line);
    }
    if (err_has(&pi->err)) {
	oj_err_raise(&pi->err);
    }
    return result;
}
