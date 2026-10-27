/* See LICENSE file for copyright and license details. */
enum { ImgFree, ImgLoading, ImgReady, ImgFailed };

typedef struct {
	unsigned long long key;
	unsigned int tex;
	int w, h, state;
	unsigned long used;
} Img;

Img *img_get(const char *url, int maxw);
void img_result(Job *j);
void img_tick(void);
