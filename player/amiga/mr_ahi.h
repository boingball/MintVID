#ifndef MR_AHI_H
#define MR_AHI_H

/* Non-zero when ahi.device (V4 or later) opens, so the GUIs can offer its
 * units as audio outputs. */
int mr_ahi_available(void);

#endif /* MR_AHI_H */
