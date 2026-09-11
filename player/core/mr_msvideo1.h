#ifndef MR_MSVIDEO1_H
#define MR_MSVIDEO1_H
#include "mr_codec.h"
extern const mr_codec mr_codec_msvideo1;

/* Switch a newly-opened MS Video 1 decoder from RGB24 to byte-per-pixel
 * ordered-dither output for a native 4-, 5- or 8-plane display. */
int mr_msvideo1_set_indexed_output(mr_decoder *dec, int depth);
#endif
