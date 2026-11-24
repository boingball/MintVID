#ifndef MR_AVC_ANNEXB_H
#define MR_AVC_ANNEXB_H
#include "mr_types.h"
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint8_t nal_length_size;
} mr_avc_annexb;
void mr_avc_annexb_init(mr_avc_annexb *a);
void mr_avc_annexb_close(mr_avc_annexb *a);
mr_status mr_avc_config_to_annexb(mr_avc_annexb *a, const uint8_t *cfg, uint32_t n, const uint8_t **out, uint32_t *out_n);
mr_status mr_avc_sample_to_annexb(mr_avc_annexb *a, const uint8_t *src, uint32_t n, const uint8_t **out, uint32_t *out_n);
mr_status mr_avc_annexb_extract_parameter_sets(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap, uint32_t *out_n);
int mr_avc_annexb_is_keyframe(const uint8_t *src, uint32_t n);
#endif
