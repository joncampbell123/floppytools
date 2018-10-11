
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"

// default 1.44MB floppy
unsigned int            sectors = 18;
unsigned int            heads = 2;
unsigned int            tracks = 80;
unsigned int            sector_size = 512;

unsigned char           sector_buf[16384];

std::vector<bool>       captured;

// at call:
// fb.peek() == A1 sync
void process_sync(FILE *dsk_fp,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    unsigned char tmp[128];
    unsigned int count;
    crc16fd_t check;
    int c;

    // A1 A1 A1
    // FE
    // track number
    // side
    // sector number
    // sector size as power of 2
    // <TWO BYTE CRC>
    // 22 bytes of 4E
    // 12 bytes of 00
    //
    // A1 A1 A1
    // FA/FB
    // <SECTOR DATA>
    // <TWO BYTE CRC>
    //
    // (inter-sector gap filled with 4E)

    if ((c=flux_bits_mfm_decode(fb,ev,fp)) != -MFM_A1_SYNC_BYTE) return; // A1
    tmp[0] = (unsigned char)MFM_A1_SYNC_BYTE;

    if ((c=flux_bits_mfm_decode(fb,ev,fp)) != -MFM_A1_SYNC_BYTE) return; // A1
    tmp[1] = (unsigned char)MFM_A1_SYNC_BYTE;

    if ((c=flux_bits_mfm_decode(fb,ev,fp)) != -MFM_A1_SYNC_BYTE) return; // A1
    tmp[2] = (unsigned char)MFM_A1_SYNC_BYTE;

    // we are expecting FE
    if ((c=flux_bits_mfm_decode(fb,ev,fp)) != 0xFE) return; // FE
    tmp[3] = (unsigned char)c;

    int track = flux_bits_mfm_decode(fb,ev,fp);
    if (track < 0) return;
    tmp[4] = (unsigned char)track;

    int side = flux_bits_mfm_decode(fb,ev,fp);
    if (side < 0) return;
    tmp[5] = (unsigned char)side;

    int sector = flux_bits_mfm_decode(fb,ev,fp);
    if (sector < 0) return;
    tmp[6] = (unsigned char)sector;

    int ssize = flux_bits_mfm_decode(fb,ev,fp);
    if (ssize < 0) return;
    tmp[7] = (unsigned char)ssize;

    unsigned int crc;
    if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0) return;//CRC-hi
    crc  = (unsigned int)c << 8;

    if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0) return;//CRC-hi
    crc += (unsigned int)c;

    check = crc16fd_update(0xffff,tmp,8);
    if (check != crc) return;

    printf("Sector: track=%u side=%u sector=%u ssize=%u\n",track,side,sector,128 << ssize);

    if ((128 << ssize) != sector_size) {
        printf("Not what we're looking for, wrong sector size\n");
        return;
    }
    if (sector < 1 || sector > sectors ||
        side < 0 || side >= heads ||
        track < 0 || track >= tracks) {
        printf("Not what we're looking for, track/side/sector %d/%d/%d out of range\n",track,side,sector);
        return;
    }

    /* 4E 4E .... */
    {
        unsigned int errs = 0;

        for (count=0;count < 16;count++) {
            c = flux_bits_mfm_decode(fb,ev,fp);

            if (c == 0x4E) {
            }
            else if (c == 0x00) {
                break;
            }
            else if (c < 0) {
                printf("! Gap between A1 sync where 4E should exist ends abruptly\n");
                return;
            }
            else {
                if (++errs >= 8) {
                    printf("* Gap between A1 sync has too many non-4E bytes\n");
                    return;
                }
            }
        }

        if (count < 8)
            printf("* Too few 4E bytes\n");
    }

    /* look for A1 sync.
     * mem_decode() won't reliably find it */
    kryoflux_bits_refill(fb,ev,fp);
    while (fb.avail() >= MFM_A1_SYNC_LENGTH) {
        if (fb.peek(MFM_A1_SYNC_LENGTH) == MFM_A1_SYNC) {
            break;
        }
        else {
            fb.get(1);
        }

        if (!kryoflux_bits_refill(fb,ev,fp))
            return;
    }

    if (fb.avail() < MFM_A1_SYNC_LENGTH)
        return;

    // A1 A1 A1 FA/FB
    if ((c=flux_bits_mfm_decode(fb,ev,fp)) != -MFM_A1_SYNC_BYTE) return; // A1
    tmp[0] = (unsigned char)MFM_A1_SYNC_BYTE;

    if ((c=flux_bits_mfm_decode(fb,ev,fp)) != -MFM_A1_SYNC_BYTE) return; // A1
    tmp[1] = (unsigned char)MFM_A1_SYNC_BYTE;

    if ((c=flux_bits_mfm_decode(fb,ev,fp)) != -MFM_A1_SYNC_BYTE) return; // A1
    tmp[2] = (unsigned char)MFM_A1_SYNC_BYTE;

    // we are expecting FA/FB
    if (((c=flux_bits_mfm_decode(fb,ev,fp))&0xFE) != 0xFA) return; // FA/FB
    tmp[3] = (unsigned char)c;

    /* begin checksum */
    check = crc16fd_update(0xffff,tmp,4);

    // sector data follows
    for (unsigned int b=0;b < sector_size;b++) {
        if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0) {
            printf(" ! flux error in sector\n");
            return;
        }
        sector_buf[b] = (unsigned char)c;
    }
    check = crc16fd_update(check,sector_buf,sector_size);

    // checksum
    if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0) return;//CRC-hi
    crc  = (unsigned int)c << 8;

    if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0) return;//CRC-hi
    crc += (unsigned int)c;

    if (check != crc) {
        printf("Sector checkum failed\n");
        return;
    }

    printf(" * DATA OK\n");

    unsigned long sector_num;

    sector_num  = (unsigned long)track;
    sector_num *= (unsigned long)heads;
    sector_num += (unsigned long)side;
    sector_num *= (unsigned long)sectors;
    sector_num += (unsigned long)sector - 1;

    sector_num *= (unsigned long)sector_size;

    fseek(dsk_fp,sector_num,SEEK_SET);
    if (ftell(dsk_fp) != sector_num) return;
    fwrite(sector_buf,sector_size,1,dsk_fp);
}

int main(int argc,char **argv) {
    struct flux_bits fb;
    struct kryoflux_event ev;
    FILE *dsk_fp;
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

    dsk_fp = fopen("disk.img","wb");
    if (dsk_fp == NULL) return 1;

    fb.clear();

    captured.resize(heads * sectors * tracks);

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
                process_sync(dsk_fp,fb,ev,fp);
#if 0
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
                            crc16fd_t crc;

                            t[0] = t[1] = t[2] = 0xA1;
                            t[3] = (unsigned char)type_c;
                            crc = crc16fd_update(0xffff,t,4);
                            crc = crc16fd_update(crc,tmp,4);

                            crc16fd_t check;

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
#endif
            }
            else {
                fb.get(1);
            }
        }

        if (!kryoflux_bits_refill(fb,ev,fp))
            break;
    } while (fb.avail() > 0);

    fclose(dsk_fp);
    fclose(fp);
    return 0;
}

