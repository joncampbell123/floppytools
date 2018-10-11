
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"

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

int main(int argc,char **argv) {
    struct flux_bits fb;
    struct kryoflux_event ev;

    if (argc < 2) {
        fprintf(stderr,"%s <raw>\n",argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1],"rb");
    if (fp == NULL) return 1;

    if (!autodetect_flux_bits_mfm(fb,ev,fp)) {
        fprintf(stderr,"Autodetect failure\n");
        return 1;
    }

    /* TODO: autodetection is possible (for MFM at least).
     *       look at the histogram code in kryoflux2.cpp */

    /* from my tests:
     *
     * distance of 10101010101010 = 49
     * change in distance to encode 1001001001001001 = 23.
     *
     * shortest is therefore 49 - 23 = 26 which is the distance to encode 111111111111 */

    fb.clear();

    do {
        kryoflux_bits_refill(fb,ev,fp);

        unsigned int c = fb.avail();
        for (unsigned int i=0;i < c;i++) printf("%u",fb.get(1));

        kryoflux_bits_refill(fb,ev,fp);
    } while (fb.avail() > 0);

    fclose(fp);
    return 0;
}

