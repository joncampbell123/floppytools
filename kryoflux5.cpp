
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

        /*                                                      *            */
        /*                                            1 0 1 0 0 0 0 1   (A1) */
        /* look for A1 sync (100010010001). Look for '0100010010001001' */
        /*                                            ................  16 bits */
        /*                                            4-->4-->8-->9-->  */
        /*                                            3210321032103210  */
        while (fb.avail() >= MFM_A1_SYNC_LENGTH) {
            if (fb.peek(MFM_A1_SYNC_LENGTH) == MFM_A1_SYNC) {
                fb.get(MFM_A1_SYNC_LENGTH);
                printf("Sync\n");
            }
            else {
                fb.get(1);
            }
        }

        if (!kryoflux_bits_refill(fb,ev,fp))
            break;
    } while (fb.avail() > 0);

    fclose(fp);
    return 0;
}

