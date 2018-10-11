
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

void kryoflux_bits_refill(flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    while (fb.avail() <= 24) {
        if (!kryoflux_read(ev,fp))
            break;

        if (ev.message == MSG_FLUX) {
            unsigned int len;

            if (ev.flux_interval >= fb.shortest) {
                len = (((ev.flux_interval - fb.shortest) + (fb.dist / 2u)) / fb.dist) + 1;
            }
            else {
                len = 1;
            }

            if (len > 8u) len = 8u;
            fb.add(len);
        }
    }
}

void flux_bits::clear(void) {
    bits = 0;
    left = 0;
}

void flux_bits::add(unsigned int len) {
    if (len != 0) {
        left += len;
        bits |= (1 << (left - 1));
    }
}

unsigned int flux_bits::avail(void) const {
    return left;
}

unsigned int flux_bits::peek(unsigned int bc) const {
    if (bc != 0) return bits & ((1u << bc) - 1u);
    return 0;
}

unsigned int flux_bits::get(unsigned int bc) {
    if (left >= bc) {
        unsigned int r = peek(bc);
        bits >>= bc;
        left -= bc;
        return r;
    }

    return 0;
}

