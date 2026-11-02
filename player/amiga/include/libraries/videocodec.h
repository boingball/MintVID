/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * libraries/videocodec.h - videocodec.library: video and image codecs for
 * AmigaOS programs, independent of the hardware that does the work.
 *
 * A session is a decoder (packets in, pictures out), an encoder (pictures in,
 * packets out) or a converter (pictures in, pictures out: scale, crop, change
 * the pixel format).  It never blocks: the library Signal()s your task when
 * something is ready, and VC_Poll() hands it over.  Nothing is copied: you
 * fill the library's buffers, or give it your own memory to write into.
 *
 *
 * Example 1: play an H.264 file, 16-bit RGB pictures
 *
 *   LONG err;
 *   struct VCSession *s = VC_OpenSessionTags(
 *       VCA_Codec, VCC_H264, VCA_Kind, VCK_Decoder,
 *       VCA_OutputFormat, VCF_RGB16,        // leave out for the native I420
 *       VCA_SigMask, 1UL << sig,            // Signal() this task
 *       VCA_ErrorPtr, &err, TAG_DONE);
 *
 *   for (;;)
 *   {
 *       struct VCItem it;
 *       struct VCPacket *pk;
 *       while (VC_Poll(s, &it))
 *           switch (it.Type)
 *           {
 *           case VCIT_PacketReturned:       // an empty input packet: fill it, send it
 *               pk = it.u.Packet;
 *               pk->Length = Read(file, pk->Data, pk->Capacity);
 *               pk->Flags = pk->Length < pk->Capacity ? VCPF_EndOfStream : 0;   // the end goes with the last data
 *               VC_SendPacket(s, pk);
 *               break;
 *           case VCIT_Frame:                // a picture: show it, give it back
 *               show(it.u.Frame);
 *               VC_ReleaseFrame(s, it.u.Frame);
 *               break;
 *           case VCIT_Event:
 *               if (it.u.Event.Event == VCEV_StreamInfo)   // the size is known:
 *                   VC_SetAttrsTags(s, VCA_OutputWidth, 640,  // scale it, if you like
 *                                   VCA_OutputHeight, 360, TAG_DONE);
 *               break;
 *           }
 *       Wait((1UL << sig) | SIGBREAKF_CTRL_C);
 *   }
 *   VC_CloseSession(s);
 *
 * Example 2: the pictures straight into a screen bitmap, no copy at all
 *
 *   The screen's bitmap is made three pictures tall: three 1920x1080 "parts"
 *   stacked one above the other in VRAM.  Each part is imported as one output
 *   frame.  One part is on display while the hardware draws into the other
 *   two; showing a finished part is a scroll of the viewport, not a copy.
 *
 *   struct VCFrame *part[3];                  // the three output frames
 *   ULONG shown;                              // the part on display
 *
 *   scr = p96OpenScreenTags(P96SA_DisplayID, id, P96SA_Width, 1920, P96SA_Height, 3 * 1080,
 *                           P96SA_Depth, 16, P96SA_Quiet, TRUE, TAG_DONE);
 *   struct RenderInfo ri;                     // where the bitmap is, in VRAM
 *   LONG lock = p96LockBitMap(scr->RastPort.BitMap, (UBYTE *)&ri, sizeof(ri));
 *   p96UnlockBitMap(scr->RastPort.BitMap, lock);   // a shown bitmap stays put
 *
 *   s = VC_OpenSessionTags(VCA_Codec, VCC_H264, VCA_Kind, VCK_Decoder,
 *                          VCA_OutputFormat, VCF_RGB16,      // the screen's RGBFB_R5G6B5PC
 *                          VCA_OutputWidth, 1920, VCA_OutputHeight, 1080,
 *                          VCA_OutputPitch, ri.BytesPerRow,
 *                          VCA_KeepAspect, TRUE,             // any video size, centred
 *                          VCA_SigMask, 1UL << sig, TAG_DONE);
 *   for (i = 0; i < 3; i++)
 *   {
 *       part[i] = VC_ImportFrame(s, VCS_Output, (UBYTE *)ri.Memory + i * 1080 * ri.BytesPerRow);
 *       part[i]->UserData = (APTR)i;          // comes back with every picture
 *       if (i != 0)
 *           VC_ReleaseFrame(s, part[i]);      // parts 1 and 2 to be filled; part 0 is shown
 *   }
 *   shown = 0;
 *
 *   // the poll loop of example 1, with this for a picture:
 *   case VCIT_Frame:
 *       k = (ULONG)it.u.Frame->UserData;
 *       scr->ViewPort.RasInfo->RyOffset = k * 1080;   // scroll to part k
 *       ScrollVPort(&scr->ViewPort);
 *       VC_ReleaseFrame(s, part[shown]);              // the part just hidden: fill it next
 *       shown = k;
 *       break;
 *
 *   The hardware writes each picture into VRAM by DMA and the 68k only
 *   scrolls.  Close the session before the screen.  (VPUPlay holds a hidden
 *   part back for one refresh before releasing it, against tearing.)
 *
 * Example 3: a JPEG into a 320x240 RGB picture, one call
 *
 *   err = VC_DecodeImageTags(VCA_Data, jpeg, VCA_Length, jpeg_bytes,
 *                            VCA_Dest, pixels,          // 320 * 240 * 3 bytes
 *                            VCA_OutputFormat, VCF_RGB24,
 *                            VCA_OutputWidth, 320, VCA_OutputHeight, 240, TAG_DONE);
 *
 * Example 4: encode pictures to H.264
 *
 *   s = VC_OpenSessionTags(VCA_Codec, VCC_H264, VCA_Kind, VCK_Encoder,
 *                          VCA_Width, 1280, VCA_Height, 720, VCA_InputFormat, VCF_I420,
 *                          VCA_FrameRateNum, 30, VCA_FrameRateDen, 1,
 *                          VCA_Bitrate, 4000000, VCA_SigMask, 1UL << sig, TAG_DONE);
 *   // the poll loop of example 1, with these two cases:
 *   case VCIT_FrameReturned:   draw into it.u.Frame, then VC_SendFrame(s, it.u.Frame);
 *   case VCIT_Packet:          Write() Data/Length, then VC_ReleasePacket(s, it.u.Packet);
 *
 * An H.264 encoder takes every format of its VCCodecInfo.InputFormats, not only
 * I420, and scales with VCA_OutputWidth/Height: the VideoCore backend puts the
 * ISP in front of the encoder inside the VPU.  So a screen goes in as it is:
 *
 *   s = VC_OpenSessionTags(VCA_Codec, VCC_H264, VCA_Kind, VCK_Encoder,
 *                          VCA_Width, 1920, VCA_Height, 1080, VCA_InputFormat, VCF_BGRA32,
 *                          VCA_InputCount, 1, VCA_OutputWidth, 1280, VCA_OutputHeight, 720,
 *                          VCA_SigMask, 1UL << sig, TAG_DONE);
 *   frame = VC_ImportFrame(s, VCS_Input, bitmap);   // before the first send
 *
 * Example 5: a camera's pictures shown on an RGB screen
 *
 * A converter takes pictures and gives pictures, in another format and size.
 * It reads the formats of its VCCodecInfo.InputFormats, a camera's YUYV and
 * NV12 among them:
 *
 *   s = VC_OpenSessionTags(VCA_Codec, VCC_Raw, VCA_Kind, VCK_Converter,
 *                          VCA_Width, 640, VCA_Height, 480, VCA_InputFormat, VCF_YUYV,
 *                          VCA_OutputFormat, VCF_BGRA32,
 *                          VCA_OutputWidth, 320, VCA_OutputHeight, 240,
 *                          VCA_SigMask, 1UL << sig, TAG_DONE);
 *   frame = VC_ImportFrame(s, VCS_Input, buffer);   // each camera buffer, before the first send
 *   // VC_SendFrame() each picture as it comes; VCIT_FrameReturned gives the
 *   // buffer back for the camera, VCIT_Frame the picture to draw
 *
 * NV12 goes as a camera sends it, the UV rows right after the picture's own.
 * The VideoCore backend reads YUV as full-range BT.601 (VCCS_JPEG), so a
 * camera's video-range picture (16 to 235) shows its blacks a little grey.
 *
 *
 * The rules
 *
 * - Every buffer is a VCPacket or VCFrame object of one session.  Each side
 *   has a fixed set: the library's own (counted and sized by the buffer tags)
 *   or your memory, imported with VC_ImportPacket()/VC_ImportFrame() before
 *   the first send.
 * - Input: you get an empty object as a VCIT_PacketReturned or
 *   VCIT_FrameReturned item, fill it, send it; it is the library's until it
 *   comes back the same way once used.  Output: you get a full object as a VCIT_Frame or VCIT_Packet
 *   item and release it to be filled again.  An object you imported starts
 *   in your hands.
 * - VCEV_StreamInfo comes before the first picture of a new size.  With the
 *   library's own frames a size change mid-stream just works.  Through the
 *   hardware scaler (an output format or size given) the frames keep their
 *   size and the scaler follows the change, with no event: with imported
 *   frames, give an output size.
 * - A VC_SetAttrsTags() that changes the output geometry replaces that side's
 *   objects: the old pointers are gone, import again if they were yours.
 * - Objects and info structures start with Size and may grow at their end:
 *   read only up to Size (VC_HAS_FIELD()).  Events come by value, nothing to
 *   release.
 * - Calls return 0 or a negative VCERR_*.  VC_OpenSession*() returns NULL and
 *   the image calls return the error; VCA_ErrorPtr gets it in both cases.
 *   VC_Poll() returns 1 when it filled the item, 0 when nothing is left.
 * - No callbacks: library code runs in your task only inside your calls.
 * - A session goes to the backend whose VC_QueryCodec() row for its codec
 *   and kind has the highest Priority (hardware above software; per codec,
 *   so one backend may be hardware for one codec and software for another),
 *   unless VCA_Backend names one.
 */
#ifndef LIBRARIES_VIDEOCODEC_H
#define LIBRARIES_VIDEOCODEC_H

#include <exec/types.h>
#include <exec/tasks.h>
#include <utility/tagitem.h>

#define VIDEOCODEC_NAME "videocodec.library"

/* Backends: LIBS:videocodec/<name>.codec (writing one: libraries/videocodec_backend.h) */
#define VIDEOCODEC_BACKEND_DRAWER "videocodec"
#define VIDEOCODEC_BACKEND_SUFFIX ".codec"

typedef long long VCTime; /* a timestamp in microseconds */
#define VC_TIME_UNKNOWN ((VCTime)1 << 63)

/* ---- Errors: negative ------------------------------------------------------------- */

#define VCERR_NOMEM -1       /* out of Amiga memory */
#define VCERR_DEVMEM -2      /* out of the hardware's own memory (Pi: raise gpu_mem in config.txt) */
#define VCERR_NOBACKEND -3   /* no backend does this codec and kind */
#define VCERR_BADPARAM -4    /* a tag or argument was rejected */
#define VCERR_BADSTATE -5    /* out of order: another session's object, sent twice, set already fixed */
#define VCERR_UNSUPPORTED -6 /* the hardware cannot do it (a pixel format, a progressive JPEG) */
#define VCERR_TIMEOUT -7     /* an image call's VCA_Timeout ran out */
#define VCERR_STREAM -8      /* the bitstream was refused (an H.264 picture larger than the decoder takes, say) */
#define VCERR_TRANSPORT -9   /* the link to the hardware failed */

/* ---- What a session is ------------------------------------------------------------ */

/* Kinds (VCA_Kind) */
#define VCK_Decoder 1   /* packets in, frames out */
#define VCK_Encoder 2   /* frames in, packets out */
#define VCK_Converter 3 /* frames in, frames out */

/* Sides (VC_ImportPacket(), VC_ImportFrame()) */
#define VCS_Input 1
#define VCS_Output 2

/* Codecs (VCA_Codec) */
#define VCC_None 0
#define VCC_H264 1  /* Annex-B byte stream; SPS/PPS in the stream or in VCA_Extradata */
#define VCC_MJPEG 2 /* one baseline JPEG per picture */
#define VCC_JPEG 3  /* a single JPEG picture */
#define VCC_H265 4  /* reserved */
#define VCC_Raw 5   /* uncompressed: the converter's codec */
#define VCC_PNG 6   /* a PNG picture */
#define VCC_GIF 7   /* a GIF picture: its first image */
#define VCC_BMP 8   /* a Windows BMP picture */
#define VCC_TGA 9   /* a Truevision TGA picture */
#define VCC_PPM 10  /* a binary PPM or PGM picture (P6, P5) */

/*
 * Pixel formats (VCA_InputFormat, VCA_OutputFormat, VCFrame.Format).  The
 * name is the byte order in memory; the Picasso96 RGBFTYPE is in brackets.
 * 4:2:0 formats keep chroma at half the width and half the rows.
 */
#define VCF_Unknown 0
#define VCF_I420 1   /* planar YUV 4:2:0: Y, U, V planes; a decoder's native output */
#define VCF_YV12 2   /* planar YUV 4:2:0: Y, V, U planes */
#define VCF_NV12 3   /* Y plane, then one plane of UV pairs */
#define VCF_RGB16 4  /* R5G6B5, little-endian 16-bit words [RGBFB_R5G6B5PC] */
#define VCF_RGB24 5  /* R G B [RGBFB_R8G8B8] */
#define VCF_BGR24 6  /* B G R [RGBFB_B8G8R8] */
#define VCF_BGRA32 7 /* B G R A [RGBFB_B8G8R8A8] */
#define VCF_RGBA32 8 /* R G B A [RGBFB_R8G8B8A8] */
#define VCF_YUYV 9   /* packed 4:2:2: Y U Y V [RGBFB_Y4U2V2] */
#define VCF_UYVY 10  /* packed 4:2:2: U Y V Y */

/* A format's bit in VCCodecInfo.OutputFormats and InputFormats, as RGBFF_ is to RGBFB_ */
#define VCF_MASK(f) (1UL << (f))

/* H.264 profiles (VCA_Profile) */
#define VCP_Default 0
#define VCP_H264_Baseline 1
#define VCP_H264_ConstrainedBaseline 2
#define VCP_H264_Main 3
#define VCP_H264_High 4

/* What YUV pictures hold (VCA_ColorSpace) */
#define VCCS_Unknown 0
#define VCCS_BT601 1 /* limited range, 16 to 235 */
#define VCCS_BT709 2 /* limited range, 16 to 235 */
#define VCCS_JPEG 3  /* BT.601, full range: what the hardware converter makes of RGB, on the VideoCore */

/* Rate control (VCA_RateControl) */
#define VCRC_Default 0
#define VCRC_VBR 1 /* variable bit rate: VCA_Bitrate is the average */
#define VCRC_CBR 2 /* constant bit rate */

/* ---- What VC_Poll() returns --------------------------------------------------------- */

/* Items (VCItem.Type) */
#define VCIT_None 0
#define VCIT_Frame 1          /* a full output picture: use it, then VC_ReleaseFrame() */
#define VCIT_Packet 2         /* a full output packet: use it, then VC_ReleasePacket() */
#define VCIT_FrameReturned 3  /* an empty input picture: fill it, then VC_SendFrame() */
#define VCIT_PacketReturned 4 /* an empty input packet: fill it, then VC_SendPacket() */
#define VCIT_Event 5          /* VCItem.u.Event */

/* Events (VCEvent.Event) */
#define VCEV_StreamInfo 1  /* the stream's size and rate, learned or changed: VCEvent.Stream */
#define VCEV_EndOfStream 2 /* the end mark went through; nothing more until new input */
#define VCEV_Error 3       /* the session failed: VCEvent.Status; only VC_CloseSession() is left */

/* Packet flags (VCPacket.Flags) */
#define VCPF_KeyFrame (1UL << 0)    /* a key frame starts here */
#define VCPF_Config (1UL << 1)      /* holds codec configuration (SPS/PPS); an encoder's first packet holds the first picture too */
#define VCPF_FrameStart (1UL << 2)  /* the first bytes of a picture (VCA_Framed input) */
#define VCPF_FrameEnd (1UL << 3)    /* the last bytes of a picture: an encoder's big ones come in several packets of one Pts */
#define VCPF_EndOfStream (1UL << 4) /* input: the end, flush (JPEG: on the last picture's packet); output: the end went through */
#define VCPF_Corrupt (1UL << 5)     /* input: these bytes are known damaged (a lost chunk); the decoder conceals */

/* Frame flags (VCFrame.Flags, VCStreamInfo.Flags) */
#define VCFF_KeyFrame (1UL << 0)
#define VCFF_EndOfStream (1UL << 1)    /* input: the last picture, flush after it */
#define VCFF_NoPicture (1UL << 2)      /* input, with VCFF_EndOfStream: the mark alone, the frame's pixels not read */
#define VCFF_Interlaced (1UL << 16)
#define VCFF_TopFieldFirst (1UL << 17)

/* ---- Structures ------------------------------------------------------------------------ */

/*
 * TRUE when a structure whose Size is @size holds @field of @type: what a
 * reader checks before it reads a field another version may lack.
 *   if (VC_HAS_FIELD(info.Size, struct VCInfo, MemFree)) ...
 */
#define VC_HAS_FIELD(size, type, field) \
	((ULONG)(size) >= (ULONG)&((type *)0)->field + (ULONG)sizeof(((type *)0)->field))

struct VCRect
{
	LONG X, Y, Width, Height;
};

/* A fraction: 30000/1001 is 29.97 fps, 30/1 is 30 fps; a pixel aspect of 1/1 is square pixels. */
struct VCRational
{
	LONG Num, Den;
};

/*
 * A picture in one buffer.  Plane n starts at Data + Offset[n] and has
 * Pitch[n] bytes per row.  Width x Height is the buffer (Width = the first
 * plane's pitch in pixels); Crop is the visible picture inside it.
 */
struct VCFrame
{
	ULONG Size;         /* of this structure */
	APTR Data;          /* the buffer */
	ULONG Format;       /* VCF_* */
	ULONG Width;        /* pixels per row of the buffer */
	ULONG Height;       /* rows of the buffer */
	struct VCRect Crop; /* the visible picture */
	ULONG Planes;       /* 1 to 4 */
	ULONG Offset[4];    /* of each plane, from Data */
	ULONG Pitch[4];     /* bytes per row of each plane */
	VCTime Pts;         /* presentation time, or VC_TIME_UNKNOWN */
	ULONG Flags;        /* VCFF_* */
	APTR UserData;      /* yours; the library never touches it */
};

/* Compressed bytes.  Before a send, set Length, Pts and Flags. */
struct VCPacket
{
	ULONG Size;     /* of this structure */
	APTR Data;      /* the buffer */
	ULONG Capacity; /* its size in bytes */
	ULONG Length;   /* bytes used */
	VCTime Pts;     /* presentation time, or VC_TIME_UNKNOWN */
	ULONG Flags;    /* VCPF_* */
	APTR UserData;  /* yours; the library never touches it */
};

/* A decoded stream's shape, with VCEV_StreamInfo */
struct VCStreamInfo
{
	ULONG Width, Height;         /* the buffer the decoder writes */
	struct VCRect Crop;          /* the visible picture inside it */
	struct VCRational FrameRate; /* Num 0 when the stream does not say */
	struct VCRational Par;       /* pixel aspect ratio */
	ULONG ColorSpace;            /* 0 unknown, else the codec's own number */
	ULONG Flags;                 /* VCFF_Interlaced, VCFF_TopFieldFirst */
	ULONG Format;                /* VCF_* of the frames that follow */
};

struct VCEvent
{
	ULONG Event;                /* VCEV_* */
	LONG Status;                /* VCERR_* with VCEV_Error */
	struct VCStreamInfo Stream; /* with VCEV_StreamInfo */
};

/* What VC_Poll() fills */
struct VCItem
{
	ULONG Type; /* VCIT_* */
	union
	{
		struct VCFrame *Frame;   /* VCIT_Frame, VCIT_FrameReturned */
		struct VCPacket *Packet; /* VCIT_Packet, VCIT_PacketReturned */
		struct VCEvent Event;    /* VCIT_Event */
	} u;
};

/* One thing a backend can do: call VC_QueryCodec() with index 0, 1, 2 and so on until it fails */
struct VCCodecInfo
{
	ULONG Size;  /* set it to sizeof(struct VCCodecInfo) before the call */
	ULONG Codec; /* VCC_* */
	ULONG Kind;  /* VCK_* */
	/* the largest 16:9 picture; a decoder may take other shapes of the same area */
	ULONG MaxWidth, MaxHeight;
	ULONG MaxLevel;    /* H.264 level times ten (42 = 4.2): the highest it keeps up with at the level's rates; 0 unknown */
	ULONG OutputFormats; /* VCF_MASK() of each VCF_* a decoder or converter writes; 0 for an encoder */
	UBYTE Backend[32]; /* the backend's name, for VCA_Backend */
	LONG Priority;     /* 50 hardware, 0 software (VCB_PRIORITY_*): the row of the highest gets the session */
	ULONG Flags;       /* VCCF_*: what the row serves */
	ULONG InputFormats; /* VCF_MASK() of each VCF_* an encoder or converter reads, others refused; 0 for a decoder */
};

/* VCCodecInfo.Flags */
#define VCCF_Session (1UL << 0) /* sessions (VC_OpenSession*()) */
#define VCCF_Image (1UL << 1)   /* the image calls (VC_DecodeImage(), VC_EncodeImage(), VC_ConvertImage()) */

/* One of a backend's own counters (VCInfo.Counters): the backend names it and documents what it counts */
struct VCCounter
{
	UBYTE Name[32]; /* lowercase letters, digits and _: "stray" */
	ULONG Value;
};

/*
 * A backend's state and counters: call VC_GetBackendInfo() with index 0, 1,
 * 2 and so on until it fails.  For the counters, point Counters at room for
 * MaxCounters of them; NumCounters says how many the backend has, so more
 * than were written means the room was short.
 */
struct VCInfo
{
	ULONG Size;       /* set it to sizeof(struct VCInfo) before the call */
	ULONG ABIVersion; /* of the backend interface */
	UBYTE Backend[32];
	ULONG Sessions;             /* open now */
	UBYTE Interface[32];        /* the hardware's own interface and its version while one is open ("MMAL 0.2"), else empty */
	struct VCCounter *Counters; /* in: room for MaxCounters, or NULL */
	ULONG MaxCounters;          /* in: its length */
	ULONG NumCounters;          /* out: how many the backend has, for diagnostics; at most MaxCounters were written */
	ULONG Codecs;               /* rows of this backend's codec table (VC_QueryCodec() merges them all) */
	ULONG MemTotal;             /* bytes of the hardware's own memory its codecs allocate from, 0 unknown */
	ULONG MemFree;              /* of that, free now */
};

/*
 * What a still picture's header says, to size the picture VC_DecodeImage()
 * writes: call VC_GetImageInfo().  Whether the hardware decodes it is the
 * decode's answer.
 */
struct VCImageInfo
{
	ULONG Size;          /* set it to sizeof(struct VCImageInfo) before the call */
	ULONG Codec;         /* in: VCC_* to read the bytes as (a TGA has no signature: VCC_TGA), VCC_None to tell; out: the codec */
	ULONG Width, Height; /* pixels */
	ULONG Transparency;  /* VCTR_* */
	ULONG Frames;        /* pictures in the file, counted as far as the bytes go (a GIF's); a decoder takes the first */
};

/* VCImageInfo.Transparency */
#define VCTR_None 0  /* opaque */
#define VCTR_Mask 1  /* each pixel opaque or fully transparent: a transparent colour, or palette entries */
#define VCTR_Alpha 2 /* a level per pixel: an alpha channel, or a level per palette entry */

struct VCSession; /* opaque */

/* ---- Tags ------------------------------------------------------------------------------ */

/*
 * A session reads only the tags of its kind and ignores the rest.  Defaults
 * in brackets; "backend's" means the backend picks a sensible value.
 */
#define VCA_Base (TAG_USER | 0x56430000UL)

/* Every session */
#define VCA_Codec (VCA_Base + 1)     /* VCC_*: required for a session; the image calls default it (VC_DecodeImage: from the bytes) */
#define VCA_Kind (VCA_Base + 2)      /* VCK_*, required */
#define VCA_SigMask (VCA_Base + 19)  /* signal bits to Signal() when something is ready [none: poll yourself] */
#define VCA_SigTask (VCA_Base + 18)  /* struct Task * to signal [the opener] */
#define VCA_ErrorPtr (VCA_Base + 32) /* LONG *: gets the VCERR_* when an open or an image call fails */
#define VCA_Backend (VCA_Base + 31)  /* STRPTR: this backend, by its VCCodecInfo.Backend name, with or without .codec [the one with the codec's highest-priority row] */

/* Input pictures: encoder, converter */
#define VCA_Width (VCA_Base + 3)       /* pixels; for a decoder, what imported native frames are checked against */
#define VCA_Height (VCA_Base + 4)      /* rows; likewise */
#define VCA_InputFormat (VCA_Base + 5) /* VCF_* [I420]: one of the session's VCCodecInfo.InputFormats; YUV is read as full-range BT.601 */
#define VCA_InputPitch (VCA_Base + 6)  /* bytes per row [backend's for the width] */
#define VCA_SourceRect (VCA_Base + 12) /* struct VCRect *: converter, the part to take [all] */

/*
 * Output pictures: decoder, converter.  Changeable with VC_SetAttrsTags().
 * Hardware limits apply; the VideoCore backend wants rows of whole groups of
 * 32 pixels (64 for I420 and YV12) and an even number of rows.  An H.264
 * encoder reads VCA_OutputWidth and VCA_OutputHeight at the open, the size
 * it encodes, and refuses the rest of these.
 */
#define VCA_OutputFormat (VCA_Base + 7) /* VCF_* [decoder: I420 at the stream's size, no scaling] */
#define VCA_OutputWidth (VCA_Base + 8)  /* pixels [the source's] */
#define VCA_OutputHeight (VCA_Base + 9) /* rows [the source's] */
#define VCA_OutputPitch (VCA_Base + 10) /* bytes per row [width x bytes per pixel] */
#define VCA_DestRect (VCA_Base + 11)    /* struct VCRect *: where the picture goes in the frame [all of it] */
#define VCA_KeepAspect (VCA_Base + 13)  /* BOOL: fit the picture as shown (pixel aspect) in the frame, centred [FALSE: stretch] */

/*
 * Buffers the library allocates for you.  Each side of a session gets a set
 * of buffers: packets on the compressed side, frames on the picture side (a
 * decoder: packets in, frames out; an encoder: frames in, packets out; a
 * converter: frames on both).  These tags say how many, and for packets how
 * big.  A frame's size follows from the picture's geometry, so frames take no
 * size.  A side whose buffers you import with VC_ImportPacket() or
 * VC_ImportFrame() ignores its tags here.  More buffers smooth out bursts;
 * fewer save memory.
 */
#define VCA_InputCount (VCA_Base + 14)  /* number of input buffers [backend's] */
#define VCA_InputSize (VCA_Base + 15)   /* bytes per input packet: the most one Read() may put in [backend's] */
#define VCA_OutputCount (VCA_Base + 16) /* number of output buffers [backend's] */
#define VCA_OutputSize (VCA_Base + 17)  /* bytes per output packet; a picture that does not fit spans several [backend's] */

/* Decoder */
#define VCA_Framed (VCA_Base + 20)        /* BOOL: each packet holds whole pictures, marked VCPF_FrameStart/End [FALSE: any chunks] */
#define VCA_Extradata (VCA_Base + 29)     /* APTR: codec configuration (H.264 SPS/PPS, Annex-B) when not in the stream; read by the open only.  The open refuses a stream the hardware has no memory for (VCERR_DEVMEM); without it the first packet does */
#define VCA_ExtradataSize (VCA_Base + 30) /* its bytes */

/* Encoder (VCA_Bitrate and VCA_RequestKeyFrame also with VC_SetAttrsTags() while running) */
#define VCA_FrameRateNum (VCA_Base + 21)    /* frame rate of the input; a framed decoder assumes it too [unknown] */
#define VCA_FrameRateDen (VCA_Base + 22)    /* 30000/1001 = 29.97 fps */
#define VCA_Bitrate (VCA_Base + 23)         /* bits per second [backend's] */
#define VCA_RateControl (VCA_Base + 27)     /* VCRC_* [backend's] */
#define VCA_KeyInterval (VCA_Base + 26)     /* pictures from one key frame to the next [backend's] */
#define VCA_Profile (VCA_Base + 24)         /* VCP_* [backend's] */
#define VCA_Level (VCA_Base + 25)           /* H.264 level times ten: 31, 40, 41, 42 [backend's] */
#define VCA_Quality (VCA_Base + 28)         /* JPEG: 1 to 100 [backend's] */
#define VCA_RequestKeyFrame (VCA_Base + 33) /* BOOL, VC_SetAttrsTags() only: make the next picture a key frame */
#define VCA_ColorSpace (VCA_Base + 39)      /* VCCS_*: what the input pictures hold, written into the stream for its players [unknown: they guess] */

/* Image calls: VC_DecodeImageTags(), VC_EncodeImageTags(), VC_ConvertImageTags() */
#define VCA_Data (VCA_Base + 34)     /* the source: compressed bytes, or a picture */
#define VCA_Length (VCA_Base + 35)   /* bytes at VCA_Data, when compressed */
#define VCA_Dest (VCA_Base + 36)     /* the destination: a picture, or room for compressed bytes */
#define VCA_DestSize (VCA_Base + 37) /* bytes at VCA_Dest, when compressed */
#define VCA_Timeout (VCA_Base + 38)  /* milliseconds each pass may wait [the backend's: 5000 for videocore.codec] */

#endif /* LIBRARIES_VIDEOCODEC_H */
