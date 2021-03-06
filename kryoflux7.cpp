
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"

// default 1.44MB floppy
unsigned int            sectors = 18;
unsigned int            heads = 2;
unsigned int            tracks = 80;
unsigned int            sector_size = 512;

unsigned char           sector_buf[16384+16];

std::vector<bool>       captured;

// at call:
// fb.peek() == A1 sync
void process_sync(FILE *dsk_fp,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    unsigned char tmp[128];
    unsigned int count;
    mfm_crc16fd_t check;
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

    check = mfm_crc16fd_update(0xffff,tmp,8);
    if (check != crc) return;

    if ((128 << ssize) != sector_size) {
//        printf("Not what we're looking for, wrong sector size\n");
        return;
    }
    if (sector < 1 || sector > sectors ||
        side < 0 || side >= heads ||
        track < 0 || track >= tracks) {
//        printf("Not what we're looking for, track/side/sector %d/%d/%d out of range\n",track,side,sector);
        return;
    }

    unsigned long sector_num;

    sector_num  = (unsigned long)track;
    sector_num *= (unsigned long)heads;
    sector_num += (unsigned long)side;
    sector_num *= (unsigned long)sectors;
    sector_num += (unsigned long)sector - 1;

    assert(sector_num < captured.size());
    if (captured[sector_num]) {
//        printf("Already captured\n");
        return;
    }

    printf("Sector: track=%u side=%u sector=%u ssize=%u\n",track,side,sector,128 << ssize);

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
                printf("* Gap between A1 sync where 4E should exist ends abruptly\n");
                break;
            }
            else {
                if (++errs >= 8) {
                    printf("* Gap between A1 sync has too many non-4E bytes\n");
                    break;
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
    check = mfm_crc16fd_update(0xffff,tmp,4);

    // sector data follows
    for (unsigned int b=0;b < (sector_size+2);b++) {
        kryoflux_bits_refill(fb,ev,fp);

        unsigned long cstart = ev.offset;
        unsigned int cpeek = fb.peek(16);

        if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0) {
            printf(" ! flux error in sector (%d) at byte %u / %u (flux 0x%04x offset %lu)\n",c,b,sector_size,cpeek,cstart);
            return;
        }
        sector_buf[b] = (unsigned char)c;
    }
    check = mfm_crc16fd_update(check,sector_buf,sector_size);

    crc  = (unsigned int)sector_buf[sector_size+0u] << 8u;
    crc += (unsigned int)sector_buf[sector_size+1u];

    if (check != crc) {
        printf("Sector checkum failed\n");
        return;
    }

    printf(" * DATA OK\n");

    unsigned long sector_ofs;

    sector_ofs = sector_num * (unsigned long)sector_size;

    fseek(dsk_fp,sector_ofs,SEEK_SET);
    if (ftell(dsk_fp) != sector_ofs) return;

    fwrite(sector_buf,sector_size,1,dsk_fp);

    assert(sector_num < captured.size());
    captured[sector_num] = true;
}

int main(int argc,char **argv) {
    struct flux_bits fb;
    struct kryoflux_event ev;
    std::vector<std::string> cappaths;
    std::string path;
    FILE *dsk_fp;
    int c;

    if (argc < 2) {
        fprintf(stderr,"%s <raw capture directory> [dir...]\n",argv[0]);
        return 1;
    }

    for (int i=1;i < argc;i++) {
        char *a = argv[i];

        if (*a == '-') {
            do { a++; } while (*a == '-');

            if (!strcmp(a,"1.4mb")) {
                sectors = 18;
                heads = 2;
                tracks = 80;
                sector_size = 512;
            }
            else if (!strcmp(a,"720k")) {
                sectors = 9;
                heads = 2;
                tracks = 80;
                sector_size = 512;
            }
            else {
                fprintf(stderr,"Unknown switch %s\n",a);
                return 1;
            }
        }
        else {
            cappaths.push_back(argv[i]);
        }
    }

    dsk_fp = fopen("disk.img","wb");
    if (dsk_fp == NULL) return 1;

    captured.resize(heads * sectors * tracks);

    for (size_t capidx=0;capidx < cappaths.size();capidx++) {
        for (unsigned int track=0;track < tracks;track++) {
            for (unsigned int head=0;head < heads;head++) {
                {
                    char tmp[128];

                    sprintf(tmp,"track%02u.%u.raw",track,head);
                    path = cappaths[capidx] + "/" + tmp;
                }

                printf("%s...\n",path.c_str());

                FILE *fp = fopen(path.c_str(),"rb");
                if (fp == NULL) {
                    printf("Failed to open\n");
                    continue;
                }

                if (!autodetect_flux_bits_mfm(fb,ev,fp)) {
                    fprintf(stderr,"Autodetect failure\n");
                    fclose(fp);
                    continue;
                }

                flux_bits ofb = fb;

                int adj_span = 8;
                int dadj_span = 8;

                for (int adj_c = 0;adj_c <= (adj_span*2);adj_c++) {
                    for (int dadj_c = 0;dadj_c <= (dadj_span*2);dadj_c++) {
                        unsigned int capcount = 0;

                        int adj = adj_c;
                        int dadj = dadj_c;

                        if (adj >= (adj_span+1)) adj -= adj_span*2;
                        if (dadj >= (dadj_span+1)) dadj -= dadj_span*2;

                        if ((int)ofb.shortest+(int)adj <= 0)
                            continue;
                        fb.shortest = ofb.shortest + adj;

                        if ((int)ofb.dist+(int)dadj <= 0)
                            continue;
                        fb.dist = ofb.dist + dadj;

                        unsigned long snum = ((track * heads) + head) * sectors;

                        for (size_t i=0;i < sectors;i++)
                            capcount += captured[i+snum];

                        if (capcount >= sectors) {
                            printf("Track %u head %u already captured\n",track,head);
                            adj_c = 99;
                            dadj_c = 99;
                            break;
                        }
                        else {
                            printf("Track %u head %u capture progress: %u/%u adj=%d dadj=%d ",track,head,capcount,sectors,adj,dadj);
                            for (size_t i=0;i < sectors;i++) printf("%u",captured[i+snum]?1:0);
                            printf("\n");
                        }

                        fseek(fp,0,SEEK_SET);
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
                                    process_sync(dsk_fp,fb,ev,fp);
                                }
                                else {
                                    fb.get(1);
                                }
                            }

                            if (!kryoflux_bits_refill(fb,ev,fp))
                                break;
                        } while (fb.avail() > 0);
                    }
                }

                {
                    unsigned int capcount = 0;
                    {
                        unsigned long snum = ((track * heads) + head) * sectors;
                        for (size_t i=0;i < sectors;i++)
                            capcount += captured[i+snum];
                    }

                    if (capcount < sectors) {
                        printf("Track %u head %u not captured fully (%u < %u)\n",track,head,capcount,sectors);
                    }
                }

                fclose(fp);
            }
        }
    }

    {
        size_t count = 0;

        for (size_t i=0;i < captured.size();i++) {
            if (captured[i]) count++;
        }

        printf("Capture report: %zu captured / %zu total\n",count,captured.size());

        if (count < captured.size()) {
            printf("Missing sectors:\n");
            for (size_t i=0;i < captured.size();i++) {
                if (!captured[i])
                    printf(" %zu (C/H/S %u/%u/%u)\n",
                        i,
                        i / heads / sectors,
                        (i / sectors) % heads,
                        (i % sectors) + 1);
            }
        }
    }

    fclose(dsk_fp);
    return 0;
}

