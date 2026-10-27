/* See LICENSE file for copyright and license details. */
/* the programmes of the wanted channels, sorted by start within a channel */
typedef struct {
	const char **keys;    /* wanted ids, lower case; open addressing */
	int *vals;
	size_t mask;
	int nch;
	int *first, *cnt;     /* per channel: slice of progs */
	Prog *progs;
} Xml;

void xml_parse(Job *j);
const Prog *xml_chan(const Xml *x, const char *id, int *n);
