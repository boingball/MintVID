#ifndef IH264_PAD_WPRED_PORT_H
#define IH264_PAD_WPRED_PORT_H
/* Exact shortcuts for default-weight weighted prediction and cheaper frame
 * edge padding - see ih264_pad_wpred_port.c. Call after every other
 * primitive has been chosen: the weighted wrappers capture the current
 * pf_weighted_* / pf_default_weighted_pred_* pointers. */
#include "ih264d_structs.h"
void mr_h264_port_install_pad_wpred(dec_struct_t *codec);
#endif
