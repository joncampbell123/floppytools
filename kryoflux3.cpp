
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

    unsigned int        shortest;
    unsigned int        dist;

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

void bits_refill(flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
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

int main(int argc,char **argv) {
    struct flux_bits fb;
    struct kryoflux_event ev;

    if (argc < 4) {
        fprintf(stderr,"%s <raw> <shortest> <dist>\n",argv[0]);
        fprintf(stderr," shortest = distance of one bit cell.\n");
        fprintf(stderr,"            use the histogram function in kryoflux2.cpp to determine this.\n");
        fprintf(stderr,"            for an MFM encoded disk, the 1010101010 distance will be the shortest,\n");
        fprintf(stderr,"            therefore take that and subtract distance and apply here.\n");
        fprintf(stderr," dist =     distance between 10101010101 and 1001001001001.\n");
        fprintf(stderr,"\n");
        fprintf(stderr,"In my tests with a 1.44MB MFM, shortest = 26 and dist = 23.\n");
        return 1;
    }

    FILE *fp = fopen(argv[1],"rb");
    if (fp == NULL) return 1;

    /* TODO: autodetection is possible (for MFM at least).
     *       look at the histogram code in kryoflux2.cpp */

    /* from my tests:
     *
     * distance of 10101010101010 = 49
     * change in distance to encode 1001001001001001 = 23.
     *
     * shortest is therefore 49 - 23 = 26 which is the distance to encode 111111111111 */

    fb.clear();

    fb.shortest = atoi(argv[2]);
    if (fb.shortest < 10 || fb.shortest > 500) return 1;

    fb.dist = atoi(argv[3]);
    if (fb.dist < 3 || fb.dist > 400) return 1;

    do {
        bits_refill(fb,ev,fp);

        unsigned int c = fb.avail();
        for (unsigned int i=0;i < c;i++) printf("%u",fb.get(1));

        bits_refill(fb,ev,fp);
    } while (fb.avail() > 0);

    fclose(fp);
    return 0;
}

