
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

struct mfm_sector_id { // 0xFE type packets
    uint8_t         track;
    uint8_t         side;
    uint8_t         sector;
    uint8_t         sector_size_code;       // bytes = 128 << sector_size_code
    bool            crc_ok;

    mfm_sector_id();
    void clear(void);
};

mfm_sector_id::mfm_sector_id() {
    clear();
}

void mfm_sector_id::clear(void) {
    track = side = sector = sector_size_code = 0;
    crc_ok = false;
}

// at call:
// caller just read A1 A1 A1 <byte> where <byte> == 0xFE
// and the next bytes are the track, side, sector, sector_size and CRC fields.
int flux_bits_mfm_read_sector_id(mfm_sector_id &sid,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    unsigned char tmp[8];
    mfm_crc16fd_t check;
    int c;

    sid.clear();

    tmp[0] = (unsigned char)MFM_A1_SYNC_BYTE;
    tmp[1] = (unsigned char)MFM_A1_SYNC_BYTE;
    tmp[2] = (unsigned char)MFM_A1_SYNC_BYTE;
    tmp[3] = (unsigned char)0xFE;

    int track = flux_bits_mfm_decode(fb,ev,fp);
    if (track < 0) return -1;
    tmp[4] = sid.track = (unsigned char)track;

    int side = flux_bits_mfm_decode(fb,ev,fp);
    if (side < 0) return -1;
    tmp[5] = sid.side = (unsigned char)side;

    int sector = flux_bits_mfm_decode(fb,ev,fp);
    if (sector < 0) return -1;
    tmp[6] = sid.sector = (unsigned char)sector;

    int ssize = flux_bits_mfm_decode(fb,ev,fp);
    if (ssize < 0) return -1;
    tmp[7] = sid.sector_size_code = (unsigned char)ssize;

    unsigned int crc;
    if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0) return -1;//CRC-hi
    crc  = (unsigned int)c << 8;

    if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0) return -1;//CRC-hi
    crc += (unsigned int)c;

    check = mfm_crc16fd_update(0xffff,tmp,8);
    if (check != crc) return -1;

    sid.crc_ok = true;
    return 0;
}

// at call:
// fb.peek() == A1 sync
void process_sync(FILE *dsk_fp,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    unsigned char tmp[128];
    mfm_crc16fd_t check;
    unsigned int count;
    mfm_sector_id sid;
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

    // Read A1 sync bytes (min 3) followed by first byte after
    if ((c=flux_bits_mfm_read_sync_and_byte(fb,ev,fp)) != 0xFE) return;
    // then read the rest of the sector ID
    if (flux_bits_mfm_read_sector_id(sid,fb,ev,fp) < 0) return;

    if ((128 << sid.sector_size_code) != sector_size) {
//        printf("Not what we're looking for, wrong sector size\n");
        return;
    }
    if (sid.sector < 1 || sid.sector > sectors ||
        sid.side < 0   || sid.side >= heads ||
        sid.track < 0  || sid.track >= tracks) {
//        printf("Not what we're looking for, track/side/sector %d/%d/%d out of range\n",track,side,sector);
        return;
    }

    unsigned long sector_num;

    sector_num  = (unsigned long)sid.track;
    sector_num *= (unsigned long)heads;
    sector_num += (unsigned long)sid.side;
    sector_num *= (unsigned long)sectors;
    sector_num += (unsigned long)sid.sector - 1;

    assert(sector_num < captured.size());
    if (captured[sector_num]) {
//        printf("Already captured\n");
        return;
    }

    printf("Sector: track=%u side=%u sector=%u ssize=%u\n",sid.track,sid.side,sid.sector,128 << sid.sector_size_code);

    // Find next sync header
    if (!mfm_find_sync(fb,ev,fp)) {
        fprintf(stderr,"Failed to find sync for #2\n");
        return;
    }

    // Read A1 sync bytes (min 3) followed by first byte after
    c = flux_bits_mfm_read_sync_and_byte(fb,ev,fp);
    if (c < 0) return;
    if ((c&0xFE) != 0xFA) return;

    // A1 A1 A1 FA/FB
    tmp[0] = (unsigned char)MFM_A1_SYNC_BYTE;
    tmp[1] = (unsigned char)MFM_A1_SYNC_BYTE;
    tmp[2] = (unsigned char)MFM_A1_SYNC_BYTE;
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

    unsigned int crc;
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
                printf("Track %u head %u\n",track,head);

                FILE *fp = kryo_fopen(cappaths[capidx],track,head);
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
                unsigned int capcount = 0;

                {
                    unsigned long snum = ((track * heads) + head) * sectors;

                    for (size_t i=0;i < sectors;i++)
                        capcount += captured[i+snum];

                    if (capcount >= sectors) {
                        printf("Track %u head %u already captured\n",track,head);
                        break;
                    }
                    else {
                        printf("Track %u head %u capture progress: %u/%u ",track,head,capcount,sectors);
                        for (size_t i=0;i < sectors;i++) printf("%u",captured[i+snum]?1:0);
                        printf("\n");
                    }

                    fseek(fp,0,SEEK_SET);
                    fb.clear();

                    while (mfm_find_sync(fb,ev,fp))
                        process_sync(dsk_fp,fb,ev,fp);
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

