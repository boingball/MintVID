/* MintVID - ahi.device presence check for the GUIs' Audio output lists. */
#include "mr_ahi.h"

#include <exec/types.h>
#include <exec/io.h>
#include <devices/ahi.h>
#include <proto/exec.h>

int mr_ahi_available(void)
{
    struct MsgPort *port = CreateMsgPort();
    struct AHIRequest *io = port ? (struct AHIRequest *)CreateIORequest(
                                       port, sizeof(struct AHIRequest))
                                 : NULL;
    int ok = 0;
    if (io) {
        io->ahir_Version = 4;
        /* AHI_NO_UNIT opens the device without claiming any unit's
         * hardware, so the probe never disturbs something already
         * playing. */
        if (OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT,
                       (struct IORequest *)io, 0) == 0) {
            ok = 1;
            CloseDevice((struct IORequest *)io);
        }
        DeleteIORequest((struct IORequest *)io);
    }
    if (port) DeleteMsgPort(port);
    return ok;
}
