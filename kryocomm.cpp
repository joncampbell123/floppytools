
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

bool autodetect_flux_bits_mfm(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    fseek(fp,0,SEEK_SET);
    fb.clear();

    unsigned long hist[256];
    for (unsigned long i=0;i < 256;i++) hist[i] = 0;

    while (kryoflux_read(ev,fp)) {
        if (ev.message == MSG_FLUX) {
            unsigned long ofs = ev.flux_interval;
            if (ofs < 256ul) hist[ofs]++;
        }
    }

    fseek(fp,0,SEEK_SET);

    unsigned long min = ~0UL,max = 0;
    for (unsigned int i=0;i < 256;i++) {
        if (hist[i] != 0ul) {
            if (min > hist[i]) min = hist[i];
            if (max < hist[i]) max = hist[i];
        }
    }

    if (min == ~0UL)
        return false;

    if (max < (min * 4ul))
        return false;

    unsigned long cutoff = min + ((max - min) / 100ul);

    /* look for the first peak, which represents 1010101010101010101010 */
    unsigned int scan = 0;

    unsigned long peak_max[3];
    unsigned int peak_idx[3];

    fprintf(stderr,"autoscan: min=%lu max=%lu cutoff=%lu\n",min,max,cutoff);

    for (unsigned int peak=0;peak < 3;peak++) {
        peak_max[peak] = 0;
        peak_idx[peak] = 0;
        while (scan < 256 && hist[scan] < cutoff) scan++;
        while (scan < 256 && hist[scan] >= cutoff) {
            if (peak_max[peak] < hist[scan]) {
                peak_max[peak] = hist[scan];
                peak_idx[peak] = scan;
            }
            scan++;
        }
    }

    printf("autoscan: peaks at %u, %u, %u [%lu, %lu, %lu]\n",
        peak_idx[0],peak_idx[1],peak_idx[2],
        peak_max[0],peak_max[1],peak_max[2]);

    /* we require two peaks at least */
    if (peak_idx[0] == 0 || peak_idx[1] == 0)
        return false;

    fb.dist = peak_idx[1] - peak_idx[0];
    if (fb.dist < 8)
        return false;

    if (peak_idx[2] != 0) {
        unsigned long dist2 = peak_idx[2] - peak_idx[1];

        printf("autoscan: dist=%lu dist2=%lu\n",fb.dist,dist2);
        if (labs((signed long)dist2 - (signed long)fb.dist) < (fb.dist / 4ul)) {
            printf("autoscan: using peaks 1-2 to average dist (diff=%ld)\n",(signed long)dist2 - (signed long)fb.dist);
            fb.dist = (fb.dist + dist2) / 2ul;
        }
    }

    fb.shortest = peak_idx[0];
    if (fb.shortest <= fb.dist)
        return false;

    /* peak[0] represents 101010101010101010 so to get the shortest (1111111)
     * subtract dist from it */
    fb.shortest -= fb.dist;

    printf("autoscan: final params shortest=%lu dist=%lu\n",fb.shortest,fb.dist);
    return true;
}

