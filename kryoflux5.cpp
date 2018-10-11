
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"

static int flux_bits_pbit = 0;

int flux_bits_mfm_decode_bit(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    unsigned int raw,bit;

    if (fb.avail() < 2) {
        if (!kryoflux_bits_refill(fb,ev,fp))
            return -1;
    }

    raw = fb.peek(2);
    if (raw == 1/*01*/) {
        bit = 1;
    }
    else if (raw == 3/*11*/) {
        return -1;
    }
    else {
        /* 10 if previous bit 0
         * 00 if previous bit 1 */
        raw ^= (1u ^ flux_bits_pbit) << 1u;
        if (raw != 0) return -1;
        bit = 0;
    }

    fb.get(2);
    flux_bits_pbit = bit;
    return bit;
}

int flux_bits_mfm_decode(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    unsigned int byte = 0,i;
    int c;

    if (fb.avail() < 16) {
        if (!kryoflux_bits_refill(fb,ev,fp))
            return -1;
    }

    if (fb.peek(MFM_A1_SYNC_LENGTH) == MFM_A1_SYNC) {
        fb.get(MFM_A1_SYNC_LENGTH);
        flux_bits_pbit = 1; /* last bit of 0xA1 is 1 */
        return -MFM_A1_SYNC_BYTE;
    }

    for (i=0;i < 8;i++) {
        c = flux_bits_mfm_decode_bit(fb,ev,fp);
        if (c < 0 || c >= 2) return -1;
        byte |= (unsigned int)c << (7u - i);
    }

    return byte;
}

int main(int argc,char **argv) {
    struct flux_bits fb;
    struct kryoflux_event ev;
    int c;

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
                printf("Sync at about %lu\n",ev.offset);

                c = flux_bits_mfm_decode(fb,ev,fp);
                if (c != -MFM_A1_SYNC_BYTE) fprintf(stderr,"Unexpected decode from MFM_A1_SYNC 0x%x %d\n",c,c);

                c = flux_bits_mfm_decode(fb,ev,fp);
                if (c != -MFM_A1_SYNC_BYTE) fprintf(stderr,"Next byte not MFM_A1_SYNC 0x%x %d\n",c,c);

                c = flux_bits_mfm_decode(fb,ev,fp);
                if (c != -MFM_A1_SYNC_BYTE) fprintf(stderr,"Next byte not MFM_A1_SYNC 0x%x %d\n",c,c);

                c = flux_bits_mfm_decode(fb,ev,fp);
                if (c == 0xFA || c == 0xFB || c == 0xFE) {
                    printf("Sync type: 0x%02x\n",c);

                    if (c == 0xFE) {
                        unsigned char tmp[30];
                        unsigned int tmpsz=0;

                        for (unsigned int i=0;i < (6u/*header*/+16u/*bytes of 0x4e*/);i++) {
                            c = flux_bits_mfm_decode(fb,ev,fp);
                            if (c < 0) break;
                            tmp[i] = (unsigned char)c;
                            tmpsz = i;
                        }

                        printf(" Header: ");
                        for (unsigned int i=0;i < tmpsz;i++) printf("0x%02x ",tmp[i]);
                        printf("\n");

                        if (tmpsz >= 6) {
                            printf("  Track=%u side=%u sector=%u size=%u crc=0x%02x%02x\n",
                                tmp[0],
                                tmp[1],
                                tmp[2],
                                128 << tmp[3],
                                tmp[4],tmp[5]);
                        }
                    }
                }
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

