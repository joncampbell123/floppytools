
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"

bool kryoflux_read(struct kryoflux_event &ev,FILE *fp) {
    int c;

    ev.msg.clear();
    ev.oob_code = 0;
    ev.message = MSG_FLUX;
    ev.flux_interval = 0;
    ev.offset = ftell(fp);

    do {
        if ((c = fgetc(fp)) < 0)
            return false;

        if (c <= 0x07) {
            ev.flux_interval += ((unsigned char)c << 8u);

            if ((c = fgetc(fp)) < 0)
                return false;
            ev.flux_interval +=  (unsigned char)c;

            return true;
        }
        else if (c == 0x08) {
            // ignore
        }
        else if (c == 0x09) {
            if ((c = fgetc(fp)) < 0)
                return false;

            // ignore
        }
        else if (c == 0x0A) {
            if ((c = fgetc(fp)) < 0)
                return false;
            if ((c = fgetc(fp)) < 0)
                return false;

            // ignore
        }
        else if (c == 0x0B) {
            // overflow16
            ev.flux_interval += 0x10000u;
        }
        else if (c == 0x0C) {
            // value16
            if ((c = fgetc(fp)) < 0)
                return false;
            ev.flux_interval +=  (unsigned char)c;

            if ((c = fgetc(fp)) < 0)
                return false;
            ev.flux_interval += ((unsigned char)c << 8u);

            return true;
        }
        else if (c == 0x0D) {
            unsigned long oob_len;

            // OOB
            if ((c = fgetc(fp)) < 0)
                return false;
            unsigned char type = (unsigned char)c;

            if ((c = fgetc(fp)) < 0)
                return false;
            oob_len  = (unsigned char)c;

            if ((c = fgetc(fp)) < 0)
                return false;
            oob_len += (unsigned char)c << 8u;

            ev.msg.clear();
            ev.msg.resize(oob_len);
            for (unsigned long i=0;i < oob_len;i++) {
                if ((c = fgetc(fp)) < 0)
                    return false;

                ev.msg[i] = (unsigned char)c;
            }

            ev.oob_code = type;

            return true;
        }
        else {
            ev.flux_interval +=  (unsigned char)c;

            return true;
        }
    } while (1);

    return false;
}

