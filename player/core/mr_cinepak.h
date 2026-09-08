/* MintVID - Cinepak decoder-specific output controls. */
#ifndef MR_CINEPAK_H
#define MR_CINEPAK_H

#include "mr_codec.h"

/* Switch a newly-opened Cinepak decoder from RGB24 to byte-per-pixel output
 * using the fixed ordered-dither palette for the requested native depth.
 * Must be called before the first compressed frame is decoded. */
int mr_cinepak_set_indexed_output(mr_decoder *dec, int depth);

#endif /* MR_CINEPAK_H */
