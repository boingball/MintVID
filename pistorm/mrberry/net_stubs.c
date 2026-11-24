#include "core/mr_source.h"
#include "core/mr_http.h"
#include "core/mr_hls.h"
#include "core/mr_youtube.h"
#include <string.h>
int mr_source_is_hls(const char *url){(void)url;return 0;}
mr_source *mr_hls_source_open_ex(const char *url,const struct mr_http_options *o){(void)url;(void)o;return 0;}
mr_source *mr_http_source_open_ex(const char *url,const mr_http_options *o){(void)url;(void)o;return 0;}
int mr_youtube_is_url(const char *url){(void)url;return 0;}
int mr_youtube_http_options_init(struct mr_http_options *out,const struct mr_http_options *base){if(!out)return 0;if(base)*out=*base;else memset(out,0,sizeof *out);return 1;}
int mr_youtube_resolve_media(const char *url,const struct mr_http_options *o,char *out,size_t sz,mr_youtube_media_kind *kind){(void)url;(void)o;(void)out;(void)sz;(void)kind;return 0;}
int mr_youtube_media_http_options_init(struct mr_http_options *out,const struct mr_http_options *base){if(!out)return 0;if(base)*out=*base;else memset(out,0,sizeof *out);return 1;}
