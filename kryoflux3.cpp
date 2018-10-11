
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"

struct flux_bits {
    unsigned int        bits;
    unsigned int        left;

    void clear(void) {
        bits = 0;
        left = 0;
    }
    void add(unsigned int len) {
        if (len != 0) {
            left += len;
            bits |= (1 << (left - 1));
        }
    }
    unsigned int avail(void) const {
        return left;
    }
    unsigned int peek(unsigned int bc) const {
        if (bc != 0) return bits & ((1u << bc) - 1u);
        return 0;
    }
    unsigned int get(unsigned int bc) {
        if (left >= bc) {
            unsigned int r = peek(bc);
            bits >>= bc;
            left -= bc;
            return r;
        }

        return 0;
    }
};

/* shortest distance seen in histogram of an MFM floppy represending 1010101010 */
static unsigned int shortest = 49;
/* distance between each bit (between 101010 and 1001001001001) */
static unsigned int dist = 23;

void bits_refill(flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    unsigned int mn;

    {
        int x = (int)shortest - (int)dist;
        if (x < 0) x = 0;
        mn = (unsigned int)x;
    }

    while (fb.avail() <= 24) {
        if (!kryoflux_read(ev,fp))
            break;

        if (ev.message == MSG_FLUX) {
            unsigned int len;

            if (ev.flux_interval >= mn) {
                len = (((ev.flux_interval - mn) + (dist / 2u)) / dist) + 1;
            }
            else {
                len = 1;
            }

            if (len > 8u) len = 8u;
            fb.add(len);
        }
    }
}

int main(int argc,char **argv) {

    if (argc < 4) {
        fprintf(stderr,"%s <raw> <shortest> <dist>\n",argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1],"rb");
    if (fp == NULL) return 1;

    /* TODO: autodetection is possible (for MFM at least).
     *       look at the histogram code in kryoflux2.cpp */

    shortest = atoi(argv[2]);
    if (shortest < 10 || shortest > 500) return 1;

    dist = atoi(argv[3]);
    if (dist < 3 || dist > 400) return 1;

    struct flux_bits fb;
    struct kryoflux_event ev;

    do {
        bits_refill(fb,ev,fp);

        unsigned int c = fb.avail();
        for (unsigned int i=0;i < c;i++) printf("%u",fb.get(1));

        bits_refill(fb,ev,fp);
    } while (fb.avail() > 0);

    fclose(fp);
    return 0;
}

