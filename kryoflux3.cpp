
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"

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
        kryoflux_bits_refill(fb,ev,fp);

        unsigned int c = fb.avail();
        for (unsigned int i=0;i < c;i++) printf("%u",fb.get(1));

        kryoflux_bits_refill(fb,ev,fp);
    } while (fb.avail() > 0);

    fclose(fp);
    return 0;
}

