
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"
#include "kryomfm.h"

// Amiga (AmigaDOS trackdisk) MFM floppy decoder for KryoFlux raw stream captures, to an ADF image.
//
// Track layout: 11 sectors (DD) or 22 sectors (HD) of 512 bytes, no index alignment, one gap.
// Each sector, as raw MFM longwords:
//
// AAAA AAAA            two bytes of 00
// 4489 4489            two A1 sync words
// info      (2 longs)  odd/even encoded: format (FF), track (cyl*2+head), sector, sectors to gap
// label     (8 longs)  odd/even encoded 16 bytes, normally 0
// hdr csum  (2 longs)  odd/even encoded: XOR of the info and label raw longs & 0x55555555
// data csum (2 longs)  odd/even encoded: XOR of the data raw longs & 0x55555555
// data    (256 longs)  odd bits of the 512 bytes (128 longs), then the even bits (128 longs)
//
// The odd/even encoding stores the odd bits of a long in one raw longword and the even bits in
// the next, so every raw longword carries 16 data bits. The 270 raw longwords after the sync are
// decoded here as 540 bytes in raw order (2 per raw longword) and combined across reads like
// any other MFM field (see kryomfm.h), then the sector data is put back together.

unsigned int            sectors = 0;            /* 0 = autodetect (11 DD, 22 HD) */
unsigned int            tracks = 0;             /* cylinders, 0 = autodetect */
static const unsigned int heads = 2;

std::string             output_path = "disk.adf";
bool                    column_fix = true;      /* fix single bit errors located by the checksum */
bool                    bad_zero = false;       /* write zeros instead of best guess data for unrecovered sectors */
bool                    verbose = false;

static const unsigned int max_phys_track = 90;

static const size_t     field_longs = 270;      /* raw longwords after the sync */
static const size_t     field_bytes = field_longs * 2u;
static const size_t     data_first = 14;        /* raw longword index of the data (odd bits) */

/*-----------------------------------------------------------------------------------------------*/
/* field decoding */

/* the 16 data bits of raw longword i of a field decoded in raw order */
static inline uint16_t field_word(const std::vector<uint8_t> &f,size_t i) {
    return (uint16_t)((f[i*2u] << 8u) | f[(i*2u)+1u]);
}

/* odd/even decode: o has bits 31,29,...,1 and e has bits 30,28,...,0 */
static uint32_t interleave(uint16_t o,uint16_t e) {
    uint32_t v = 0;

    for (unsigned int j=0;j < 16;j++) {
        if (o & (1u << j)) v |= 1u << ((j * 2u) + 1u);
        if (e & (1u << j)) v |= 1u << (j * 2u);
    }

    return v;
}

/* checksum as AmigaDOS computes it (XOR of raw longs & 0x55555555), as the 16 bits it carries */
static uint16_t field_xor(const std::vector<uint8_t> &f,size_t first,size_t count) {
    uint16_t x = 0;

    for (size_t i=first;i < (first+count);i++)
        x ^= field_word(f,i);

    return x;
}

/* stored checksum, as the 16 bits it should carry, and whether its other bits are clear */
static uint16_t stored_checksum(const std::vector<uint8_t> &f,size_t at,bool *clean) {
    if (clean != NULL) *clean = (field_word(f,at) == 0);
    return field_word(f,at+1u); /* the even bits */
}

static bool header_checksum_ok(const std::vector<uint8_t> &f) {
    bool clean;
    return field_xor(f,0,10) == stored_checksum(f,10,&clean) && clean;
}

static bool data_checksum_ok(const std::vector<uint8_t> &f) {
    bool clean;
    return field_xor(f,data_first,256) == stored_checksum(f,12,&clean) && clean;
}

static void field_to_sector(const std::vector<uint8_t> &f,uint8_t *out) {
    for (size_t k=0;k < 128;k++) {
        const uint32_t v = interleave(field_word(f,data_first+k),field_word(f,data_first+128u+k));

        out[(k*4u)+0u] = (uint8_t)(v >> 24u);
        out[(k*4u)+1u] = (uint8_t)(v >> 16u);
        out[(k*4u)+2u] = (uint8_t)(v >> 8u);
        out[(k*4u)+3u] = (uint8_t)v;
    }
}

/*-----------------------------------------------------------------------------------------------*/
/* track scanning */

struct amiga_read {
    uint8_t                 format,track,sector,gap;
    bool                    hdr_ok;
    bool                    data_ok;
    std::vector<uint8_t>    field;      /* 540 bytes, raw order */
    std::vector<int8_t>     soft,tsoft;
    std::vector<uint8_t>    raw;        /* cells around the field for realignment (bad data only) */

    amiga_read() : format(0), track(0), sector(0), gap(0), hdr_ok(false), data_ok(false) { }
};

static void scan_track(const std::vector<uint8_t> &cells,std::vector<amiga_read> &reads) {
    uint32_t sr = 0;

    for (size_t i=0;i < cells.size();i++) {
        sr = (sr << 1u) | (cell_bit(cells,i) ? 1u : 0u);
        if (sr != 0x44894489u) continue;

        const size_t p = i + 1u;
        if ((p + (field_bytes * 16u) + 1u) > cells.size()) break;

        amiga_read r;
        r.field.resize(field_bytes);
        r.soft.resize(field_bytes * 8u);
        r.tsoft.resize(field_bytes * 8u);
        decode_soft(cells,p,field_bytes,&r.field[0],&r.soft[0],&r.tsoft[0]);

        const uint32_t info = interleave(field_word(r.field,0),field_word(r.field,1));
        r.format = (uint8_t)(info >> 24u);
        r.track = (uint8_t)(info >> 16u);
        r.sector = (uint8_t)(info >> 8u);
        r.gap = (uint8_t)info;
        r.hdr_ok = header_checksum_ok(r.field);
        r.data_ok = data_checksum_ok(r.field);

        if (!r.data_ok && p >= mfm_align_pad && (p + (field_bytes * 16u) + mfm_align_pad) <= cells.size())
            r.raw.assign(cells.begin()+(p-mfm_align_pad),cells.begin()+(p+(field_bytes*16u)+mfm_align_pad));

        reads.push_back(r);
    }
}

/* does this read belong to the given track, with a good or at least plausible header */
static bool read_matches(const amiga_read &r,unsigned int track,unsigned int spt) {
    if (r.track != track || r.sector >= spt) return false;
    if (r.hdr_ok) return true;
    /* damaged header: accept if every field is what is expected */
    return r.format == 0xFF && r.gap >= 1 && (r.sector + r.gap) <= spt;
}

/*-----------------------------------------------------------------------------------------------*/
/* sector recovery */

enum {
    SEC_MISSING=0,      /* no sector header found */
    SEC_OK,             /* at least one read with good checksum */
    SEC_VOTED,          /* good checksum after combining multiple bad reads */
    SEC_FIXED,          /* good checksum after fixing bits located by the checksum */
    SEC_BAD             /* no good checksum, best guess data */
};

static const char sec_status_char[] = { '-', 'O', 'V', 'F', 'X' };
static const char *sec_status_name[] = { "missing", "ok", "voted", "fixed", "bad" };

struct sector_result {
    int                     status;
    unsigned int            reads;
    unsigned int            flips;
    bool                    inconsistent;
    uint8_t                 data[512];

    sector_result() : status(SEC_MISSING), reads(0), flips(0), inconsistent(false) { memset(data,0,sizeof(data)); }
};

/* The data checksum is a parity per bit column of the 256 data words. For each column that
 * fails, flip the least confident bit of that column, if it is clearly the least confident.
 * Returns the number of bits flipped, 0 if not possible. */
static unsigned int column_parity_fix(std::vector<uint8_t> &f,const mfm_consensus &v) {
    const uint16_t bad = field_xor(f,data_first,256) ^ stored_checksum(f,12,NULL);
    const int full = std::max(1,v.total_w * 126);
    unsigned int flips = 0;
    std::vector< std::pair<size_t,unsigned int> > todo;

    if (bad == 0 || field_word(f,12) != 0) return 0;

    for (unsigned int j=0;j < 16;j++) {
        if (!(bad & (1u << j))) continue;

        /* bit j of a word is data bit (15-j) of its byte pair, MSB first */
        int best = -1,second = -1;
        size_t best_word = 0;

        /* the data words and the stored checksum (even bits, word 13) */
        for (size_t w=13;w < (data_first+256u);w++) {
            const size_t k = (w * 16u) + (15u - j);
            const int c = abs(v.sum[k]);

            if (best < 0 || c < best) {
                second = best;
                best = c;
                best_word = w;
            }
            else if (second < 0 || c < second) {
                second = c;
            }
        }

        /* must be uncertain and clearly the most uncertain bit of the column */
        if (best < 0 || (best * 10) >= (full * 6) || (best * 2) >= second)
            return 0;

        todo.push_back(std::pair<size_t,unsigned int>(best_word,j));
    }

    for (size_t i=0;i < todo.size();i++) {
        const size_t k = (todo[i].first * 16u) + (15u - todo[i].second);
        f[k >> 3u] ^= (uint8_t)(0x80u >> (k & 7u));
        flips++;
    }

    return data_checksum_ok(f) ? flips : 0;
}

static void resolve_sector(sector_result &res,const std::vector<const amiga_read*> &reads) {
    res = sector_result();
    res.reads = (unsigned int)reads.size();
    if (reads.empty())
        return;

    /* any good read? pick the most common good content */
    {
        const amiga_read *best = NULL;
        unsigned int best_w = 0,distinct = 0;
        std::map< std::vector<uint8_t>, unsigned int > votes;

        for (size_t i=0;i < reads.size();i++) {
            if (reads[i]->data_ok) {
                uint8_t d[512];
                field_to_sector(reads[i]->field,d);

                unsigned int &v = votes[std::vector<uint8_t>(d,d+512)];
                if (v == 0) distinct++;
                v += reads[i]->hdr_ok ? 2u : 1u;
                if (best_w < v) {
                    best_w = v;
                    best = reads[i];
                }
            }
        }

        if (best != NULL) {
            res.status = SEC_OK;
            res.inconsistent = (distinct > 1);
            field_to_sector(best->field,res.data);
            return;
        }
    }

    /* combine all reads, realigning bit slipped reads */
    std::vector<mfm_copy> wr(reads.size());
    mfm_consensus v;

    for (size_t i=0;i < reads.size();i++) {
        wr[i].raw = &reads[i]->raw;
        wr[i].weight = reads[i]->hdr_ok ? 2 : 1;
        wr[i].data = reads[i]->field;
        wr[i].soft = reads[i]->soft;
        wr[i].tsoft = reads[i]->tsoft;
    }

    for (unsigned int iter=0;iter < 4;iter++) {
        if (iter != 0 && !mfm_realign(wr,field_bytes,v))
            break;

        mfm_vote(wr,field_bytes,v);
        if (data_checksum_ok(v.buf))
            break;
    }

    std::vector<uint8_t> f = v.buf;

    if (data_checksum_ok(f)) {
        res.status = SEC_VOTED;
    }
    else if (column_fix && (res.flips = column_parity_fix(f,v)) != 0) {
        res.status = SEC_FIXED;
    }
    else {
        res.status = SEC_BAD;
    }

    field_to_sector(f,res.data);
}

/*-----------------------------------------------------------------------------------------------*/
/* format detection */

/* good headers on a track from the first capture that has it, normal decode only */
static void track_headers(std::vector<amiga_read> &hdrs,const std::vector<std::string> &cappaths,unsigned int track,unsigned int head) {
    hdrs.clear();

    for (size_t capidx=0;capidx < cappaths.size();capidx++) {
        flux_track ft;
        std::vector<uint8_t> cells;
        std::vector<amiga_read> reads;

        if (!load_flux_track(ft,cappaths[capidx],track,head) || ft.cell <= 0) continue;

        pll_decode(cells,ft,pll_presets[0]);
        scan_track(cells,reads);
        for (size_t i=0;i < reads.size();i++) {
            if (reads[i].hdr_ok && reads[i].format == 0xFF) {
                reads[i].field.clear();
                reads[i].soft.clear();
                reads[i].tsoft.clear();
                reads[i].raw.clear();
                hdrs.push_back(reads[i]);
            }
        }

        if (!hdrs.empty()) break;
    }
}

static bool detect_format(const std::vector<std::string> &cappaths) {
    unsigned int max_phys = 0;

    for (size_t capidx=0;capidx < cappaths.size();capidx++) {
        for (unsigned int t=0;t < max_phys_track;t++) {
            if (track_file_exists(cappaths[capidx],t,0) && max_phys < (t+1u))
                max_phys = t+1u;
        }
    }

    if (max_phys == 0) {
        fprintf(stderr,"No trackNN.0.raw files found\n");
        return false;
    }

    /* sectors per track: the most common highest sector number + 1 over a few tracks */
    if (sectors == 0) {
        std::map<unsigned int,unsigned int> h;

        printf("Auto-detecting sectors per track...\n");

        for (unsigned int cyl=0;cyl < max_phys && cyl < 10;cyl++) {
            for (unsigned int head=0;head < heads;head++) {
                std::vector<amiga_read> hdrs;
                unsigned int mx = 0;
                bool any = false;

                track_headers(hdrs,cappaths,cyl,head);
                for (size_t i=0;i < hdrs.size();i++) {
                    if (hdrs[i].track != ((cyl * 2u) + head)) continue;
                    if (mx < hdrs[i].sector) mx = hdrs[i].sector;
                    any = true;
                }

                if (any) h[mx+1u]++;
            }
        }

        unsigned int sel = 0,sel_count = 0;
        for (std::map<unsigned int,unsigned int>::iterator i=h.begin();i!=h.end();i++) {
            if (sel_count <= i->second) {
                sel_count = i->second;
                sel = i->first;
            }
        }

        if (sel == 0)
            return false;

        sectors = sel;
    }

    /* number of tracks: the last cylinder with a reasonable number of sectors on both heads */
    if (tracks == 0) {
        const unsigned int need = std::max(1u,sectors / 2u);

        printf("Auto-detecting tracks...\n");

        for (unsigned int cyl=0;cyl < max_phys;cyl++) {
            unsigned int hok = 0;

            for (unsigned int head=0;head < heads;head++) {
                std::vector<amiga_read> hdrs;
                std::vector<bool> seen(sectors,false);
                unsigned int count = 0;

                printf("\x0D" "t=%u h=%u ",cyl,head);
                fflush(stdout);

                track_headers(hdrs,cappaths,cyl,head);
                for (size_t i=0;i < hdrs.size();i++) {
                    if (hdrs[i].track != ((cyl * 2u) + head) || hdrs[i].sector >= sectors) continue;
                    if (!seen[hdrs[i].sector]) {
                        seen[hdrs[i].sector] = true;
                        count++;
                    }
                }

                if (count >= need) hok++;
            }

            if (hok == heads) tracks = cyl + 1u;
        }

        printf("\x0D            \x0D");
        fflush(stdout);

        /* a disk with a damaged end still is a standard 80 cylinder disk */
        if (tracks != 0 && tracks < 80 && max_phys >= 80)
            tracks = 80;
    }

    return sectors != 0 && tracks != 0;
}

/*-----------------------------------------------------------------------------------------------*/

static void help(const char *argv0) {
    fprintf(stderr,"%s [options] <raw capture directory> [dir...]\n",argv0);
    fprintf(stderr,"\n");
    fprintf(stderr,"Decodes AmigaDOS format floppies to an ADF image. Multiple capture directories\n");
    fprintf(stderr,"of the same disk are combined to recover bad sectors.\n");
    fprintf(stderr,"\n");
    fprintf(stderr,"  -o <file>         Output image (default disk.adf)\n");
    fprintf(stderr,"  -dd -hd           11 (880KB) or 22 (1760KB) sectors per track instead of autodetecting\n");
    fprintf(stderr,"  -tracks <n>       Number of cylinders (normally 80)\n");
    fprintf(stderr,"  -nofix            Do not fix bits located by the data checksum\n");
    fprintf(stderr,"  -badzero          Write zeros for unrecovered sectors instead of the best guess\n");
    fprintf(stderr,"  -v                Verbose\n");
}

int main(int argc,char **argv) {
    std::vector<std::string> cappaths;

    for (int i=1;i < argc;i++) {
        char *a = argv[i];

        if (*a == '-') {
            do { a++; } while (*a == '-');

            if (!strcmp(a,"dd")) {
                sectors = 11;
            }
            else if (!strcmp(a,"hd")) {
                sectors = 22;
            }
            else if (!strcmp(a,"nofix")) {
                column_fix = false;
            }
            else if (!strcmp(a,"badzero")) {
                bad_zero = true;
            }
            else if (!strcmp(a,"v")) {
                verbose = true;
            }
            else if (!strcmp(a,"h") || !strcmp(a,"help")) {
                help(argv[0]);
                return 1;
            }
            else if (!strcmp(a,"o") || !strcmp(a,"tracks")) {
                if ((i+1) >= argc) {
                    fprintf(stderr,"Switch %s requires a value\n",a);
                    return 1;
                }

                const char *v = argv[++i];

                if (!strcmp(a,"o"))
                    output_path = v;
                else if (!strcmp(a,"tracks"))
                    tracks = (unsigned int)strtoul(v,NULL,0);
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

    if (cappaths.empty()) {
        help(argv[0]);
        return 1;
    }

    if (!detect_format(cappaths)) {
        printf("Using disk geometry C/H/S %u/%u/%u\n",tracks,heads,sectors);
        fprintf(stderr,"Unable to detect format (not an AmigaDOS disk?)\n");
        return 1;
    }

    printf("Using disk geometry C/H/S %u/%u/%u\n",tracks,heads,sectors);
    if (tracks != 80)
        printf("Note: %u cylinders, a standard ADF has 80\n",tracks);

    FILE *dsk_fp = fopen(output_path.c_str(),"wb");
    if (dsk_fp == NULL) {
        fprintf(stderr,"Cannot create %s\n",output_path.c_str());
        return 1;
    }

    const size_t total_sectors = (size_t)tracks * heads * sectors;
    std::vector<uint8_t> image(total_sectors * 512u,0);
    std::vector<int> status(total_sectors,SEC_MISSING);
    std::vector<unsigned int> status_flips(total_sectors,0);
    std::vector<bool> status_inconsistent(total_sectors,false);

    for (unsigned int cyl=0;cyl < tracks;cyl++) {
        for (unsigned int head=0;head < heads;head++) {
            const unsigned int track = (cyl * 2u) + head;
            std::vector<flux_track> sources;
            std::vector< std::vector<amiga_read> > reads(sectors);
            std::vector<bool> good(sectors,false);
            unsigned int presets_used = 0,foreign = 0;
            double cell_us = 0;

            for (size_t capidx=0;capidx < cappaths.size();capidx++) {
                flux_track ft;
                if (!load_flux_track(ft,cappaths[capidx],cyl,head)) continue;
                if (ft.cell > 0 && cell_us == 0) cell_us = (ft.cell * 1000000.0) / ft.sck;
                sources.push_back(ft);
            }

            for (unsigned int preset=0;preset < pll_preset_count;preset++) {
                presets_used++;

                for (size_t si=0;si < sources.size();si++) {
                    std::vector<uint8_t> cells;
                    std::vector<amiga_read> tr;

                    if (sources[si].cell <= 0) continue;

                    pll_decode(cells,sources[si],pll_presets[preset]);
                    scan_track(cells,tr);

                    for (size_t i=0;i < tr.size();i++) {
                        if (!read_matches(tr[i],track,sectors)) {
                            if (tr[i].hdr_ok && preset == 0) foreign++;
                            continue;
                        }

                        if (tr[i].data_ok && tr[i].hdr_ok) good[tr[i].sector] = true;
                        reads[tr[i].sector].push_back(tr[i]);
                    }
                }

                bool complete = true;
                for (unsigned int s=0;s < sectors;s++) {
                    if (!good[s]) complete = false;
                }
                if (complete) break;
            }

            unsigned int gcount = 0;
            std::string map;

            for (unsigned int s=0;s < sectors;s++) {
                const size_t sector_num = ((size_t)track * sectors) + s;
                std::vector<const amiga_read*> rp;
                sector_result res;

                for (size_t i=0;i < reads[s].size();i++)
                    rp.push_back(&reads[s][i]);

                resolve_sector(res,rp);

                status[sector_num] = res.status;
                status_flips[sector_num] = res.flips;
                status_inconsistent[sector_num] = res.inconsistent;

                if (res.status != SEC_MISSING && !(res.status == SEC_BAD && bad_zero))
                    memcpy(&image[sector_num * 512u],res.data,512);

                if (res.status == SEC_OK || res.status == SEC_VOTED || res.status == SEC_FIXED)
                    gcount++;

                map += sec_status_char[res.status];

                if (verbose && res.status != SEC_OK)
                    printf("  C/H/S %u/%u/%u: %s, %u reads\n",cyl,head,s,sec_status_name[res.status],res.reads);
            }

            printf("Cyl %2u head %u: %s %2u/%u",cyl,head,map.c_str(),gcount,sectors);
            if (cell_us > 0) printf("  cell %.3fus",cell_us);
            if (sources.empty()) printf("  (no capture)");
            else if (cell_us == 0) printf("  (no MFM signal)");
            if (presets_used > 1) printf("  retries %u",presets_used-1u);
            if (gcount == 0 && foreign != 0) printf("  (sector headers of another track)");
            else if (gcount == 0 && cell_us > 0) printf("  (not AmigaDOS format?)");
            printf("\n");
        }
    }

    if (fwrite(&image[0],image.size(),1,dsk_fp) != 1) {
        fprintf(stderr,"Write error\n");
        fclose(dsk_fp);
        return 1;
    }
    fclose(dsk_fp);

    {
        size_t count[5] = {0,0,0,0,0};
        size_t inconsistent = 0;

        for (size_t i=0;i < total_sectors;i++) {
            count[status[i]]++;
            if (status_inconsistent[i]) inconsistent++;
        }

        printf("Capture report: %zu captured / %zu total\n",count[SEC_OK]+count[SEC_VOTED]+count[SEC_FIXED],total_sectors);
        printf("  ok %zu, recovered by voting %zu, fixed by checksum %zu, bad %zu, missing %zu\n",
            count[SEC_OK],count[SEC_VOTED],count[SEC_FIXED],count[SEC_BAD],count[SEC_MISSING]);
        printf("  legend: O=ok V=voted F=checksum fixed X=bad (%s written) -=missing (zeros written)\n",
            bad_zero ? "zeros" : "best guess");

        if (count[SEC_OK] != total_sectors || inconsistent != 0) {
            printf("Sectors needing attention:\n");
            for (size_t i=0;i < total_sectors;i++) {
                if (status[i] == SEC_OK && !status_inconsistent[i]) continue;

                printf(" %zu (C/H/S %u/%u/%u) %s",
                    i,
                    (unsigned int)(i / sectors / heads),
                    (unsigned int)((i / sectors) % heads),
                    (unsigned int)(i % sectors),
                    sec_status_name[status[i]]);
                if (status[i] == SEC_FIXED) printf(" (%u bits flipped)",status_flips[i]);
                if (status_inconsistent[i]) printf(" good-reads-differ");
                printf("\n");
            }
        }

        if (image.size() >= 4 && image[0] == 'D' && image[1] == 'O' && image[2] == 'S')
            printf("Boot block: DOS\\%u (%s)\n",image[3],(image[3] & 1) ? "FFS" : "OFS");
        else
            printf("Boot block: not DOS (non-AmigaDOS or custom boot disk)\n");
    }

    printf("Using disk geometry C/H/S %u/%u/%u\n",tracks,heads,sectors);
    return 0;
}
