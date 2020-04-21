
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
unsigned int            sectors = 0;
unsigned int            heads = 0;
unsigned int            tracks = 0;
unsigned int            sector_size = 0;
unsigned int            double_track = 0;       /* i.e. 360KB DD disk captured in 1.2MB HD drive. 0=unknown 1=single 2=double */

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

// at call:
// fb.peek() == A1 sync
void process_sync(FILE *dsk_fp,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp,unsigned int log_track,unsigned int log_head) {
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
    if (flux_bits_mfm_read_sync_and_byte(fb,ev,fp) != 0xFE) return;
    // then read the rest of the sector ID
    if (flux_bits_mfm_read_sector_id(sid,fb,ev,fp) < 0) return;

    if (sid.sector_size() != sector_size)
        return;
    if (sid.track != log_track || sid.side != log_head)
        return;

    if (sid.sector < 1 || sid.sector > sectors ||
        sid.side < 0   || sid.side >= heads ||
        sid.track < 0  || sid.track >= tracks) {
        return;
    }

    unsigned long sector_num;

    sector_num  = (unsigned long)sid.track;
    sector_num *= (unsigned long)heads;
    sector_num += (unsigned long)sid.side;
    sector_num *= (unsigned long)sectors;
    sector_num += (unsigned long)sid.sector - 1;

    assert(sector_num < captured.size());
    if (captured[sector_num])
        return;

    // Find next sync header
    if (!mfm_find_sync(fb,ev,fp)) {
//      printf("* Failed to find sync\n");
        return;
    }

    // Read A1 sync bytes (min 3) followed by first byte after. Store the value in 'c' because 0xFA/0xFB is part of the checksum.
    if (((c=flux_bits_mfm_read_sync_and_byte(fb,ev,fp))&0xFE) != 0xFA) {
//      printf("* Failed to find sector data sync, err=0x%x\n",-c);
        return;
    }

    // Then read the rest of the sector
    if ((c=flux_bits_mfm_read_sector_data(sector_buf,sid.sector_size(),fb,ev,fp,c)) < 0) {
//      printf("* Failed to read sector data, err=0x%x\n",-c);
        return;
    }

    printf("Sector: track=%u side=%u sector=%u ssize=%u\n",sid.track,sid.side,sid.sector,128 << sid.sector_size_code);

    unsigned long sector_ofs = sector_num * (unsigned long)sector_size;

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
            else if (!strcmp(a,"sside")) {
                heads = 1;
            }
            else if (!strcmp(a,"dside")) {
                heads = 2;
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

    /* sector size auto-detect */
    for (size_t capidx=0;capidx < cappaths.size();capidx++) {
        if (sector_size == 0) {
            printf("Auto-detecting sector size...\n");

            FILE *fp = kryo_fopen(cappaths[capidx],0/*track*/,0/*head*/);
            if (fp == NULL)
                continue;

            if (!autodetect_flux_bits_mfm(fb,ev,fp)) {
                fclose(fp);
                continue;
            }

            flux_bits ofb = fb;

            fseek(fp,0,SEEK_SET);
            fb.clear();

            static const unsigned int max_sector = 40; /* more than enough for even 2.88MB formats */
            unsigned long count_sszcode[max_sector] = {0};

            while (mfm_find_sync(fb,ev,fp)) {
                mfm_sector_id sid;

                if (flux_bits_mfm_read_sync_and_byte(fb,ev,fp) != 0xFE) continue;
                if (flux_bits_mfm_read_sector_id(sid,fb,ev,fp) < 0) continue;

                /* this is a read from track 0 head 0, make sure it matches */
                if (sid.track != 0 || sid.side != 0) continue;
                if (sid.sector_size_code < max_sector) count_sszcode[sid.sector_size_code]++;
            }

            /* scan the histogram for the most common sector size */
            {
                int sel = -1;
                unsigned long sel_count = 0;
                for (unsigned int i=0;i < max_sector;i++) {
                    if (sel_count < count_sszcode[i]) {
                        sel_count = count_sszcode[i];
                        sel = i;
                    }
                }

                if (sel_count >= 6 && sel >= 0) {
                    sector_size = 128u << (unsigned int)sel;
                }
            }

            fclose(fp);
        }
    }

    /* double track detect. Read track 2 and see how they are marked. This is needed to support 360KB DD floppies imaged by a 1.2MB HD drive. */
    for (size_t capidx=0;capidx < cappaths.size();capidx++) {
        if (sector_size != 0 && double_track == 0) {
            printf("Auto-detecting double/single track...\n");

            FILE *fp = kryo_fopen(cappaths[capidx],2/*track*/,0/*head*/);
            if (fp == NULL)
                continue;

            if (!autodetect_flux_bits_mfm(fb,ev,fp)) {
                fclose(fp);
                continue;
            }

            flux_bits ofb = fb;

            fseek(fp,0,SEEK_SET);
            fb.clear();

            unsigned long count_1x = 0;
            unsigned long count_2x = 0;

            while (mfm_find_sync(fb,ev,fp)) {
                mfm_sector_id sid;

                if (flux_bits_mfm_read_sync_and_byte(fb,ev,fp) != 0xFE) continue;
                if (flux_bits_mfm_read_sector_id(sid,fb,ev,fp) < 0) continue;

                /* this is a read from track 0 head 0, make sure it matches */
                if (sid.side != 0) continue;
                if (sid.sector_size() != sector_size) continue;

                if (sid.track == 1/*DD in HD case*/) count_2x++;
                else if (sid.track == 2/*HD in HD case or normal floppy*/) count_1x++;
            }

            /* scan the histogram for the most common sector size */
            if (count_1x >= 4 || count_2x >= 4) {
                if (count_2x > (count_1x / 2u))
                    double_track = 2;
                else if (count_1x > (count_2x / 2u))
                    double_track = 1;
            }

            fclose(fp);
        }
    }

    /* sectors per track autodetect. scan multiple tracks in case of missing/unreadable sectors in track 0 */
    for (size_t capidx=0;capidx < cappaths.size();capidx++) {
        if (sectors == 0 && sector_size != 0 && double_track != 0) {
            printf("Auto-detecting sectors per track...\n");

            for (unsigned int track=0;track < 20;track++) {
                FILE *fp = kryo_fopen(cappaths[capidx],track*double_track/*track*/,0/*head*/);
                if (fp == NULL)
                    continue;

                if (!autodetect_flux_bits_mfm(fb,ev,fp)) {
                    fclose(fp);
                    continue;
                }

                flux_bits ofb = fb;

                fseek(fp,0,SEEK_SET);
                fb.clear();

                while (mfm_find_sync(fb,ev,fp)) {
                    mfm_sector_id sid;

                    if (flux_bits_mfm_read_sync_and_byte(fb,ev,fp) != 0xFE) continue;
                    if (flux_bits_mfm_read_sector_id(sid,fb,ev,fp) < 0) continue;

                    /* this is a read from track <track> head 0, make sure it matches */
                    if (sid.track != track || sid.side != 0 || sid.sector_size() != sector_size) continue;

                    /* reject copy protection sectors that most likely have out of range numbers */
                    if (sid.sector > 24) continue; /* more than enough for 1.44MB and even DMF */

                    if (sectors < sid.sector)
                        sectors = sid.sector;
                }

                fclose(fp);
            }
        }
    }

    /* number of heads. look at side 2 (head == 1) and make sure the format matches. */
    for (size_t capidx=0;capidx < cappaths.size();capidx++) {
        if (sectors != 0 && sector_size != 0 && double_track != 0 && heads == 0) {
            unsigned int sectors2 = 0;

            printf("Auto-detecting heads...\n");

            for (unsigned int track=0;track < 20;track++) {
                FILE *fp = kryo_fopen(cappaths[capidx],track*double_track/*track*/,1/*head*/);
                if (fp == NULL)
                    continue;

                if (!autodetect_flux_bits_mfm(fb,ev,fp)) {
                    fclose(fp);
                    continue;
                }

                flux_bits ofb = fb;

                fseek(fp,0,SEEK_SET);
                fb.clear();

                while (mfm_find_sync(fb,ev,fp)) {
                    mfm_sector_id sid;

                    if (flux_bits_mfm_read_sync_and_byte(fb,ev,fp) != 0xFE) continue;
                    if (flux_bits_mfm_read_sector_id(sid,fb,ev,fp) < 0) continue;

                    /* this is a read from track <track> head 1, make sure it matches */
                    if (sid.track != track || sid.side != 1 || sid.sector_size() != sector_size) continue;

                    /* reject copy protection sectors that most likely have out of range numbers */
                    if (sid.sector > 24) continue; /* more than enough for 1.44MB and even DMF */

                    if (sectors2 < sid.sector)
                        sectors2 = sid.sector;
                }

                fclose(fp);
            }

            if (sectors2 == sectors)
                heads = 2;
            else
                heads = 1;
        }
    }

    /* number of tracks */
    for (size_t capidx=0;capidx < cappaths.size();capidx++) {
        if (sectors != 0 && sector_size != 0 && double_track != 0 && heads != 0 && tracks == 0) {
            printf("Auto-detecting tracks...\n");

            for (unsigned int track=0;track < (84/double_track);track++) {
                unsigned long sectcount = 0;
                unsigned long sectmin = (sectors / 4) * heads; /* in case of damaged or lost sectors */

                for (unsigned int head=0;head < heads;head++) {
                    FILE *fp = kryo_fopen(cappaths[capidx],track*double_track/*track*/,head/*head*/);
                    if (fp == NULL)
                        continue;

                    if (!autodetect_flux_bits_mfm(fb,ev,fp)) {
                        fclose(fp);
                        continue;
                    }

                    flux_bits ofb = fb;

                    fseek(fp,0,SEEK_SET);
                    fb.clear();

                    while (mfm_find_sync(fb,ev,fp)) {
                        mfm_sector_id sid;

                        if (flux_bits_mfm_read_sync_and_byte(fb,ev,fp) != 0xFE) continue;
                        if (flux_bits_mfm_read_sector_id(sid,fb,ev,fp) < 0) continue;

                        /* this is a read from track <track> head <head>, make sure it matches */
                        if (sid.track != track || sid.side != head || sid.sector_size() != sector_size || sid.sector > sectors) continue;

                        sectcount++;
                    }

                    fclose(fp);
                }

                if (sectcount >= sectmin) {
                    if (tracks <= track)
                        tracks = track + 1;
                }
            }
        }
    }

    printf("Using disk geometry C/H/S/Sz %u/%u/%u/%u doubletrack=%u\n",tracks,heads,sectors,sector_size,double_track);
    if (heads == 0 || sectors == 0 || tracks == 0 || sector_size == 0 || double_track == 0) {
        fprintf(stderr,"Unable to detect format\n");
        return 1;
    }

    captured.resize(heads * sectors * tracks);

    for (size_t pass=0;pass < 3;pass++) {
	    for (size_t capidx=0;capidx < cappaths.size();capidx++) {
		    for (unsigned int track=0;track < tracks;track++) {
			    for (unsigned int head=0;head < heads;head++) {
				    printf("Track %u head %u\n",track,head);

				    FILE *fp = kryo_fopen(cappaths[capidx],track*double_track,head);
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

				    int adj_span;
				    int dadj_span;

				    switch (pass) {
					    case 0:
						    adj_span = 0;
						    dadj_span = 0;
						    break;
					    case 1:
						    adj_span = 2;
						    dadj_span = 2;
						    break;
					    case 2:
						    adj_span = 4;
						    dadj_span = 4;
						    break;
					    default:
						    abort();
				    }

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

						    if (capcount >= sectors)
							    break;

						    fseek(fp,0,SEEK_SET);
						    fb.clear();

						    while (mfm_find_sync(fb,ev,fp))
							    process_sync(dsk_fp,fb,ev,fp,track,head);
					    }
				    }

				    {
					    unsigned long snum = ((track * heads) + head) * sectors;
					    printf("Track %u head %u capture progress: %u/%u ",track,head,capcount,sectors);
					    for (size_t i=0;i < sectors;i++) printf("%u",captured[i+snum]?1:0);
					    printf("\n");
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

    printf("Using disk geometry C/H/S/Sz %u/%u/%u/%u doubletrack=%u\n",tracks,heads,sectors,sector_size,double_track);
    fclose(dsk_fp);
    return 0;
}

