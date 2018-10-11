
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"

/*-----*/
/**
 * \file
 * Functions and types for CRC checks.
 *
 * Generated on Thu Oct 11 13:49:52 2018
 * by pycrc v0.9.1, https://pycrc.org
 * using the configuration:
 *  - Width         = 16
 *  - Poly          = 0x1021
 *  - XorIn         = 0x1d0f
 *  - ReflectIn     = False
 *  - XorOut        = 0x0000
 *  - ReflectOut    = False
 *  - Algorithm     = table-driven
 */
#include <stdlib.h>
#include <stdint.h>

typedef uint16_t crc_t;

/**
 * Static table used for the table_driven implementation.
 */
static const crc_t crc_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
};

static crc_t crc_update(crc_t crc, const void *data, size_t data_len)
{
    const unsigned char *d = (const unsigned char *)data;
    unsigned int tbl_idx;

    while (data_len--) {
        tbl_idx = ((crc >> 8) ^ *d) & 0xff;
        crc = (crc_table[tbl_idx] ^ (crc << 8)) & 0xffff;
        d++;
    }
    return crc & 0xffff;
}
/*-----*/

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
                int type_c = c;
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

                        {
                            unsigned char t[4];
                            crc_t crc;

                            t[0] = t[1] = t[2] = 0xA1;
                            t[3] = (unsigned char)type_c;
                            crc = crc_update(0xffff,t,4);
                            crc = crc_update(crc,tmp,4);

                            crc_t check;

                            check = (tmp[4] << 8u) + tmp[5];

                            if (check != crc)
                                fprintf(stderr,"Checksum failed (got 0x%04x expect 0x%04x)\n",crc,check);
                        }
                    }
                    else if ((c&0xFE) == 0xFA) {
                        unsigned char tmp[128];
                        unsigned int tmpsz=0;

                        memset(tmp,0,sizeof(tmp));
                        for (unsigned int i=0;i < 128u;i++) {
                            c = flux_bits_mfm_decode(fb,ev,fp);
                            if (c < 0) break;
                            tmp[i] = (unsigned char)c;
                            tmpsz = i;
                        }

                        printf(" Sector contents:\n");

                        for (unsigned int i=0;i < 128;i += 16) {
                            for (unsigned int c=0;c < 16;c++)
                                printf("%02x ",tmp[i+c]);

                            printf("  ");

                            for (unsigned int c=0;c < 16;c++) {
                                if (tmp[i+c] >= 32 && tmp[i+c] < 127)
                                    printf("%c",tmp[i+c]);
                                else
                                    printf(".");
                            }

                            printf("\n");
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

