/* See LICENSE file for copyright and license details. */
enum { JNull, JBool, JNum, JStr, JArr, JObj };

typedef struct JVal JVal;
struct JVal {
	int type;
	char *s;              /* string value, or the literal of a number/bool */
	char *key;            /* member name inside an object */
	JVal *child, *next;
};

JVal *json_parse(Arena *a, const char *s, const char **err);
JVal *json_get(const JVal *obj, const char *key);
const char *json_str(const JVal *v);
long long json_int(const JVal *v, long long def);
