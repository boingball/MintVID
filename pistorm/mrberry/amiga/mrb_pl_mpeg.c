/* pl_mpeg's implementation, for the MP2 decoder in audio/mr_audio_decode.c
 * (--paula). mrplay gets it from core/mr_mpeg1.c along with the MPEG-1
 * video path, which mrberry does not need. Built without MR_M68K_ASM: the
 * portable C, no hand kernels. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "../core/pl_mpeg.h"
