/* See LICENSE file for copyright and license details.
 *
 * Recursive descent JSON parser into an arena-allocated tree. Strict
 * enough to reject garbage, lax where providers are sloppy: numbers and
 * numeric strings are interchangeable through json_int()/json_str().
 */
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "json.h"

#define MAXDEPTH 64

typedef struct {
	const char *p, *err;
	Arena *a;
	int depth;
} Parser;

static JVal *value(Parser *ps);

static void
ws(Parser *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
		ps->p++;
}

static long
hex4(const char *s)
{
	long v = 0;
	int i, c;

	for (i = 0; i < 4; i++) {
		c = s[i];
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= c - '0';
		else if (c >= 'a' && c <= 'f')
			v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			v |= c - 'A' + 10;
		else
			return -1;
	}
	return v;
}

static char *
utf8enc(char *o, long c)
{
	if (c < 0x80) {
		*o++ = c;
	} else if (c < 0x800) {
		*o++ = 0xc0 | c >> 6;
		*o++ = 0x80 | (c & 0x3f);
	} else if (c < 0x10000) {
		*o++ = 0xe0 | c >> 12;
		*o++ = 0x80 | (c >> 6 & 0x3f);
		*o++ = 0x80 | (c & 0x3f);
	} else {
		*o++ = 0xf0 | c >> 18;
		*o++ = 0x80 | (c >> 12 & 0x3f);
		*o++ = 0x80 | (c >> 6 & 0x3f);
		*o++ = 0x80 | (c & 0x3f);
	}
	return o;
}

/* decoded output never exceeds the escaped input, so size by that */
static char *
string(Parser *ps)
{
	const char *s = ps->p + 1, *e;
	char *out, *o;
	long c, lo;

	for (e = s; *e != '"'; e++) {
		if (!*e || (*e == '\\' && !*++e)) {
			ps->err = "unterminated string";
			return NULL;
		}
	}
	out = o = arena_alloc(ps->a, e - s + 1);
	while (s < e) {
		if (*s != '\\') {
			*o++ = *s++;
			continue;
		}
		s++;
		switch (*s++) {
		case '"':  *o++ = '"'; break;
		case '\\': *o++ = '\\'; break;
		case '/':  *o++ = '/'; break;
		case 'b':  *o++ = '\b'; break;
		case 'f':  *o++ = '\f'; break;
		case 'n':  *o++ = '\n'; break;
		case 'r':  *o++ = '\r'; break;
		case 't':  *o++ = '\t'; break;
		case 'u':
			if (e - s < 4 || (c = hex4(s)) < 0) {
				ps->err = "bad \\u escape";
				return NULL;
			}
			s += 4;
			if (c >= 0xd800 && c < 0xdc00 && e - s >= 6 && s[0] == '\\' && s[1] == 'u' &&
			    (lo = hex4(s + 2)) >= 0xdc00 && lo < 0xe000) {
				c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
				s += 6;
			} else if (c >= 0xd800 && c < 0xe000) {
				c = 0xfffd;
			}
			o = utf8enc(o, c);
			break;
		default:
			ps->err = "bad escape";
			return NULL;
		}
	}
	*o = '\0';
	ps->p = e + 1;
	return out;
}

/* elements of an array or members of an object, after the opening bracket */
static int
items(Parser *ps, JVal *v, char close)
{
	JVal *k, **tail = &v->child;
	char *key = NULL;

	ws(ps);
	if (*ps->p == close) {
		ps->p++;
		return 1;
	}
	for (;;) {
		ws(ps);
		if (v->type == JObj) {
			if (*ps->p != '"' || !(key = string(ps))) {
				ps->err = ps->err ? ps->err : "expected key";
				return 0;
			}
			ws(ps);
			if (*ps->p++ != ':') {
				ps->err = "expected ':'";
				return 0;
			}
		}
		if (!(k = value(ps)))
			return 0;
		k->key = key;
		*tail = k;
		tail = &k->next;
		ws(ps);
		if (*ps->p == ',') {
			ps->p++;
		} else if (*ps->p == close) {
			ps->p++;
			return 1;
		} else {
			ps->err = close == ']' ? "expected ',' or ']'" : "expected ',' or '}'";
			return 0;
		}
	}
}

static int
literal(Parser *ps, JVal *v, int type, const char *word)
{
	size_t n = strlen(word);

	if (strncmp(ps->p, word, n)) {
		ps->err = "unexpected character";
		return 0;
	}
	v->type = type;
	v->s = (char *)word;
	ps->p += n;
	return 1;
}

static JVal *
value(Parser *ps)
{
	JVal *v;
	char *end;
	int ok;

	ws(ps);
	if (++ps->depth > MAXDEPTH) {
		ps->err = "nested too deep";
		return NULL;
	}
	v = arena_alloc(ps->a, sizeof *v);
	switch (*ps->p) {
	case '{':
		v->type = JObj;
		ps->p++;
		ok = items(ps, v, '}');
		break;
	case '[':
		v->type = JArr;
		ps->p++;
		ok = items(ps, v, ']');
		break;
	case '"':
		v->type = JStr;
		ok = (v->s = string(ps)) != NULL;
		break;
	case 't': ok = literal(ps, v, JBool, "true"); break;
	case 'f': ok = literal(ps, v, JBool, "false"); break;
	case 'n': ok = literal(ps, v, JNull, "null"); break;
	default:
		ok = 0;
		if (*ps->p != '-' && (*ps->p < '0' || *ps->p > '9')) {
			ps->err = "unexpected character";
			break;
		}
		strtod(ps->p, &end);
		v->type = JNum;
		v->s = arena_alloc(ps->a, end - ps->p + 1);
		memcpy(v->s, ps->p, end - ps->p);
		ps->p = end;
		ok = 1;
	}
	ps->depth--;
	return ok ? v : NULL;
}

JVal *
json_parse(Arena *a, const char *s, const char **err)
{
	Parser ps = { s, NULL, a, 0 };
	JVal *v;

	if ((v = value(&ps))) {
		ws(&ps);
		if (*ps.p) {
			ps.err = "trailing garbage";
			v = NULL;
		}
	}
	*err = ps.err;
	return v;
}

JVal *
json_get(const JVal *obj, const char *key)
{
	JVal *c;

	if (!obj || obj->type != JObj)
		return NULL;
	for (c = obj->child; c; c = c->next)
		if (!strcmp(c->key, key))
			return c;
	return NULL;
}

/* string or number text; "" for anything else, including missing */
const char *
json_str(const JVal *v)
{
	return v && (v->type == JStr || v->type == JNum) ? v->s : "";
}

long long
json_int(const JVal *v, long long def)
{
	char *end;
	long long n;

	if (!v || (v->type != JNum && v->type != JStr))
		return def;
	n = strtoll(v->s, &end, 10);
	return end == v->s ? def : n;
}
