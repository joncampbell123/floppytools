
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <queue>
#include <map>
#include <vector>

#include "kryocomm.h"
#include "kryomfm.h"

// Macintosh 400K/800K GCR floppy decoder for KryoFlux raw stream captures.
//
// The Mac drive spins at 5 speeds (zones of 16 tracks) with 12, 11, 10, 9 and 8 sectors per track
// and a constant 2us cell. Captured in a 300 RPM drive the cell is longer (about 2.6us to 3.9us)
// and differs per zone, so it is estimated per track. There are no clock bits: every cell is a
// data bit, a flux transition is a 1, and a disk byte (nibble) always starts with a 1 bit, which
// the IWM uses to find byte boundaries (leading zeros are skipped).
//
// Each sector:
//
// FF FF ...            self-sync bytes (10 bits each)
// D5 AA 96             address field prologue
// track,sector,side,format,checksum     6-and-2 GCR nibbles. side has bit 5 = side, bit 0 = track bit 6.
//                      checksum = track ^ sector ^ side ^ format
// DE AA                epilogue
// FF FF ...
// D5 AA AD             data field prologue
// sector               GCR nibble
// 699 nibbles          524 bytes (12 tag bytes + 512 data bytes), 3 bytes per 4 nibbles, scrambled
//                      with a running checksum
// 4 nibbles            the 3 byte checksum
// DE AA                epilogue
//
// Recovery: every read of a sector (all revolutions, capture directories, PLL presets) is kept as
// nibbles with a confidence (flux timing margin) and an alternative value (the nibble with its
// least certain transition moved by a cell). Bad sectors are recovered by voting per nibble, then
// by trying the alternatives of the least certain nibbles. The 24 bit data checksum makes a false
// match unlikely (about 1 in 16 million per trial).

std::string             output_path = "disk.dsk";
std::string             dc42_path;              /* also write a DiskCopy 4.2 image (with tags) */
int                     sides = 0;              /* 0 = autodetect */
unsigned int            guess_budget = 4096;    /* max checksum trial corrections per bad sector */
bool                    bad_zero = false;
bool                    verbose = false;

static const unsigned int tracks = 80;

static unsigned int track_sectors(unsigned int track) {
    return 12u - (track / 16u);
}

/*-----------------------------------------------------------------------------------------------*/
/* GCR */

static const uint8_t gcr_encode[64] = {
    0x96,0x97,0x9A,0x9B,0x9D,0x9E,0x9F,0xA6,0xA7,0xAB,0xAC,0xAD,0xAE,0xAF,0xB2,0xB3,
    0xB4,0xB5,0xB6,0xB7,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,0xCB,0xCD,0xCE,0xCF,0xD3,
    0xD6,0xD7,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,0xE5,0xE6,0xE7,0xE9,0xEA,0xEB,0xEC,
    0xED,0xEE,0xEF,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF
};

static int8_t gcr_decode[256];

static void gcr_init(void) {
    memset(gcr_decode,-1,sizeof(gcr_decode));
    for (unsigned int i=0;i < 64;i++)
        gcr_decode[gcr_encode[i]] = (int8_t)i;
}

static const size_t data_nibbles = 704;         /* sector + 699 data + 4 checksum */
static const size_t extra_nibbles = 16;         /* read past the field, for aligning reads that lost a nibble */
static const size_t tagged_size = 524;          /* 12 tag bytes + 512 data bytes */

/* Decode the 703 nibbles following the sector nibble into 524 bytes. Returns true if the
 * checksum matches. (Same algorithm as the Sony driver.) */
static bool decode_data_nibbles(const uint8_t *in,uint8_t *out) {
    uint8_t b1[175],b2[175],b3[175];

    for (unsigned int i=0;i < 175;i++) {
        const uint8_t w4 = *in++;
        const uint8_t w1 = *in++;
        const uint8_t w2 = *in++;
        const uint8_t w3 = (i != 174) ? *in++ : 0;

        b1[i] = (uint8_t)((w1 & 0x3F) | ((w4 << 2) & 0xC0));
        b2[i] = (uint8_t)((w2 & 0x3F) | ((w4 << 4) & 0xC0));
        b3[i] = (uint8_t)((w3 & 0x3F) | ((w4 << 6) & 0xC0));
    }

    uint32_t c1 = 0,c2 = 0,c3 = 0;
    unsigned int count = 0,o = 0;

    while (1) {
        c1 = (c1 & 0xFF) << 1;
        if (c1 & 0x100) c1++;

        uint8_t val = (uint8_t)(b1[count] ^ c1);
        c3 += val;
        if (c1 & 0x100) {
            c3++;
            c1 &= 0xFF;
        }
        out[o++] = val;

        val = (uint8_t)(b2[count] ^ c3);
        c2 += val;
        if (c3 > 0xFF) {
            c2++;
            c3 &= 0xFF;
        }
        out[o++] = val;

        if (o == tagged_size)
            break;

        val = (uint8_t)(b3[count] ^ c2);
        c1 += val;
        if (c2 > 0xFF) {
            c1++;
            c2 &= 0xFF;
        }
        out[o++] = val;
        count++;
    }

    const uint8_t c4 = (uint8_t)(((c1 & 0xC0) >> 6) | ((c2 & 0xC0) >> 4) | ((c3 & 0xC0) >> 2));

    return in[0] == (c4 & 0x3F) && in[1] == (c3 & 0x3F) && in[2] == (c2 & 0x3F) && in[3] == (c1 & 0x3F);
}

/*-----------------------------------------------------------------------------------------------*/
/* nibble reading */

struct nibble_field {
    std::vector<int8_t>     val;        /* 6 bit value, -1 if not a valid GCR nibble */
    std::vector<uint8_t>    conf;       /* 0..63 timing confidence */
    std::vector<int8_t>     alt;        /* value with the least certain transition moved, -1 if none */
};

static inline uint8_t cells_byte(const std::vector<uint8_t> &cells,size_t p) {
    unsigned int v = 0;

    for (unsigned int i=0;i < 8;i++)
        v = (v << 1u) | (cell_bit(cells,p+i) ? 1u : 0u);

    return (uint8_t)v;
}

/* Read n nibbles starting at cell p the way the IWM does: skip leading zero cells, then shift
 * in 8 cells. This resynchronizes after a gained or lost cell. Returns false if out of cells. */
static bool read_nibbles(const std::vector<uint8_t> &cells,size_t p,size_t n,nibble_field &f) {
    f.val.resize(n);
    f.conf.resize(n);
    f.alt.resize(n);

    for (size_t i=0;i < n;i++) {
        unsigned int skip = 0;

        while (p < cells.size() && !cell_bit(cells,p) && skip < 4) {
            p++;
            skip++;
        }
        if ((p + 9u) > cells.size())
            return false;

        const uint8_t b = cells_byte(cells,p);
        unsigned int conf = 63,weak = 0;

        for (unsigned int c=0;c < 8;c++) {
            if (cells[p+c] & CELL_FLUX) {
                const unsigned int m = cell_margin(cells[p+c]);
                if (m < conf) {
                    conf = m;
                    weak = c;
                }
            }
        }

        f.val[i] = gcr_decode[b];
        f.conf[i] = (uint8_t)conf;
        f.alt[i] = -1;

        /* alternative: move the weakest transition one cell the way its timing leaned.
         * not the first cell, which marks the start of the nibble */
        if (weak != 0 && conf < 48) {
            const int to = (int)weak + ((cells[p+weak] & CELL_LATE) ? 1 : -1);

            if (to >= 1 && to <= 7 && !cell_bit(cells,p+(size_t)to)) {
                const uint8_t ab = (uint8_t)((b & ~(0x80u >> weak)) | (0x80u >> (unsigned int)to));
                f.alt[i] = gcr_decode[ab];
            }
        }

        p += 8u;
    }

    return true;
}

/*-----------------------------------------------------------------------------------------------*/
/* track scanning */

struct mac_read {
    uint8_t                 track,sector,side,format;
    bool                    hdr_ok;
    bool                    data_ok;
    nibble_field            nib;            /* the 704 data field nibbles */

    mac_read() : track(0), sector(0), side(0), format(0), hdr_ok(false), data_ok(false) { }
};

/* data field must follow the address field within this many cells */
static const size_t max_addr_to_data_cells = 600;

static bool nibbles_checksum_ok(const std::vector<int8_t> &v) {
    uint8_t in[data_nibbles],out[tagged_size];

    for (size_t i=0;i < data_nibbles;i++) {
        if (v[i] < 0) return false;
        in[i] = (uint8_t)v[i];
    }

    return decode_data_nibbles(in+1,out);
}

static void scan_track(const std::vector<uint8_t> &cells,std::vector<mac_read> &reads,bool hdr_only) {
    uint32_t sr = 0;

    for (size_t i=0;i < cells.size();i++) {
        sr = ((sr << 1u) | (cell_bit(cells,i) ? 1u : 0u)) & 0xFFFFFFu;
        if (sr != 0xD5AA96u) continue;

        nibble_field h;
        mac_read r;

        if (!read_nibbles(cells,i+1u,5,h))
            break;

        r.track = (uint8_t)h.val[0];
        r.sector = (uint8_t)h.val[1];
        r.side = (uint8_t)h.val[2];
        r.format = (uint8_t)h.val[3];
        r.hdr_ok = h.val[0] >= 0 && h.val[1] >= 0 && h.val[2] >= 0 && h.val[3] >= 0 && h.val[4] >= 0 &&
            ((h.val[0] ^ h.val[1] ^ h.val[2] ^ h.val[3]) & 0x3F) == h.val[4];

        if (!hdr_only) {
            uint32_t ds = 0;

            for (size_t j=i+41u;j < cells.size() && j < (i + max_addr_to_data_cells);j++) {
                ds = ((ds << 1u) | (cell_bit(cells,j) ? 1u : 0u)) & 0xFFFFFFu;
                if (ds == 0xD5AA96u) break; /* next address field, this sector has no data */
                if (ds != 0xD5AAADu) continue;

                if (read_nibbles(cells,j+1u,data_nibbles+extra_nibbles,r.nib) || read_nibbles(cells,j+1u,data_nibbles,r.nib)) {
                    r.data_ok = nibbles_checksum_ok(r.nib.val);
                    reads.push_back(r);
                }
                break;
            }
        }
        else {
            reads.push_back(r);
        }

        i += 40u;
        sr = 0;
    }
}

/* track number as recorded: low 6 bits in the track nibble, bit 6 in bit 0 of the side nibble */
static unsigned int read_track_number(const mac_read &r) {
    return (unsigned int)(r.track & 0x3F) | ((r.side & 1u) ? 64u : 0u);
}

static bool read_matches(const mac_read &r,unsigned int track,unsigned int head,int format) {
    const unsigned int spt = track_sectors(track);

    if (read_track_number(r) != track || ((r.side >> 5u) & 1u) != head || r.sector >= spt) return false;
    if (r.hdr_ok) return true;
    /* damaged header checksum: accept if the format is what the rest of the disk uses */
    return format >= 0 && r.format == (uint8_t)format;
}

/*-----------------------------------------------------------------------------------------------*/
/* sector recovery */

enum {
    SEC_MISSING=0,
    SEC_OK,
    SEC_VOTED,
    SEC_GUESSED,
    SEC_BAD
};

static const char sec_status_char[] = { '-', 'O', 'V', 'G', 'X' };
static const char *sec_status_name[] = { "missing", "ok", "voted", "guessed", "bad" };

struct sector_result {
    int                     status;
    unsigned int            reads;
    unsigned int            changed;        /* nibbles changed by trial correction */
    unsigned int            trials;
    bool                    inconsistent;
    uint8_t                 data[tagged_size];

    sector_result() : status(SEC_MISSING), reads(0), changed(0), trials(0), inconsistent(false) { memset(data,0,sizeof(data)); }
};

struct nib_candidate {
    size_t          pos;
    int8_t          val;
    int             cost;

    bool operator<(const nib_candidate &o) const { return cost < o.cost; }
};

struct nib_trial {
    int             cost;
    uint32_t        mask;
    unsigned int    last;

    bool operator<(const nib_trial &o) const { return cost > o.cost; } /* min-heap */
};

static void nibbles_to_data(const std::vector<int8_t> &v,uint8_t *out) {
    uint8_t in[data_nibbles];

    for (size_t i=0;i < data_nibbles;i++)
        in[i] = (v[i] >= 0) ? (uint8_t)v[i] : 0;

    decode_data_nibbles(in+1,out);
}

/* Vote per nibble: a read gives its value its confidence, and its alternative value the rest.
 * Reads whose confident nibbles disagree a lot are dropped, if enough remain. Fills the best and
 * second best value per nibble and the score margin between them. */
static void vote_nibbles(const std::vector<nibble_field> &f,const std::vector<int> &weight,std::vector<int8_t> &best,std::vector<int8_t> &second,std::vector<int> &margin) {
    std::vector<bool> use(f.size(),true);
    std::vector<int> score(data_nibbles * 64u);
    unsigned int used = (unsigned int)f.size();

    best.resize(data_nibbles);
    second.resize(data_nibbles);
    margin.resize(data_nibbles);

    for (unsigned int iter=0;iter < 2;iter++) {
        std::fill(score.begin(),score.end(),0);

        for (size_t r=0;r < f.size();r++) {
            if (!use[r]) continue;

            const int w = weight[r];

            for (size_t i=0;i < data_nibbles;i++) {
                if (f[r].val[i] >= 0) score[(i * 64u) + (size_t)f[r].val[i]] += w * (1 + (int)f[r].conf[i]);
                if (f[r].alt[i] >= 0) score[(i * 64u) + (size_t)f[r].alt[i]] += w * ((64 - (int)f[r].conf[i]) / 2);
            }
        }

        for (size_t i=0;i < data_nibbles;i++) {
            int b = -1,s = -1;

            for (int v=0;v < 64;v++) {
                const int sc = score[(i * 64u) + (size_t)v];
                if (sc <= 0) continue;
                if (b < 0 || sc > score[(i * 64u) + (size_t)b]) {
                    s = b;
                    b = v;
                }
                else if (s < 0 || sc > score[(i * 64u) + (size_t)s]) {
                    s = v;
                }
            }

            best[i] = (int8_t)b;
            second[i] = (int8_t)s;
            margin[i] = (b >= 0) ? (score[(i * 64u) + (size_t)b] - ((s >= 0) ? score[(i * 64u) + (size_t)s] : 0)) : 0;
        }

        if (iter != 0 || used <= 2)
            break;

        for (size_t r=0;r < f.size() && used > 2;r++) {
            unsigned int confident = 0,diff = 0;

            for (size_t i=1;i < data_nibbles;i++) {
                if (f[r].val[i] < 0 || f[r].conf[i] < 40) continue;
                confident++;
                if (f[r].val[i] != best[i]) diff++;
            }

            if ((diff * 10u) > confident) {
                use[r] = false;
                used--;
            }
        }
    }
}

/* Align the nibbles of a read to a reference (banded edit distance, inserted or dropped nibbles
 * allowed) and return them at the reference positions. Unmatched positions get no value. */
static void align_nibbles(const nibble_field &r,const std::vector<int8_t> &ref,nibble_field &out) {
    static const int band = 16;
    static const int gap_cost = 2;
    const int n = (int)data_nibbles;
    const int nr = (int)r.val.size();
    const int width = (band * 2) + 1;
    const int inf = 1 << 28;
    std::vector<int> cost((size_t)(nr + 1) * (size_t)width,inf);
    std::vector<uint8_t> from((size_t)(nr + 1) * (size_t)width,0); /* 0 diagonal, 1 skip read nibble, 2 skip ref nibble */

    /* cell (i,j): i read nibbles and j reference nibbles consumed, stored at d = j - i + band */
#define AL(i,j) (((size_t)(i) * (size_t)width) + (size_t)((j) - (i) + band))
    cost[AL(0,0)] = 0;

    for (int i=0;i <= nr;i++) {
        for (int j=std::max(0,i-band);j <= std::min(n,i+band);j++) {
            const int c = cost[AL(i,j)];
            if (c >= inf) continue;

            if (i < nr && j < n) {
                const int m = c + ((r.val[i] >= 0 && r.val[i] == ref[j]) ? 0 : 1);
                if (m < cost[AL(i+1,j+1)]) {
                    cost[AL(i+1,j+1)] = m;
                    from[AL(i+1,j+1)] = 0;
                }
            }
            if (i < nr && (j - (i+1)) >= -band) {
                const int m = c + gap_cost;
                if (m < cost[AL(i+1,j)]) {
                    cost[AL(i+1,j)] = m;
                    from[AL(i+1,j)] = 1;
                }
            }
            if (j < n && ((j+1) - i) <= band) {
                const int m = c + gap_cost;
                if (m < cost[AL(i,j+1)]) {
                    cost[AL(i,j+1)] = m;
                    from[AL(i,j+1)] = 2;
                }
            }
        }
    }

    out.val.assign(data_nibbles,-1);
    out.conf.assign(data_nibbles,0);
    out.alt.assign(data_nibbles,-1);

    /* the end of the reference may be matched anywhere in the read */
    int i = -1,j = n;
    for (int k=std::max(0,n-band);k <= std::min(nr,n+band);k++) {
        if (cost[AL(k,n)] < inf && (i < 0 || cost[AL(k,n)] < cost[AL(i,n)])) i = k;
    }
    if (i < 0) i = std::min(nr,n);
    while (i > 0 || j > 0) {
        const uint8_t f = from[AL(i,j)];

        if (f == 0) {
            i--;
            j--;
            out.val[(size_t)j] = r.val[(size_t)i];
            out.conf[(size_t)j] = r.conf[(size_t)i];
            out.alt[(size_t)j] = r.alt[(size_t)i];
        }
        else if (f == 1) {
            i--;
        }
        else {
            j--;
        }
    }
#undef AL
}

static void resolve_sector(sector_result &res,const std::vector<const mac_read*> &reads) {
    res = sector_result();
    res.reads = (unsigned int)reads.size();
    if (reads.empty())
        return;

    /* any good read? pick the most common good content */
    {
        const mac_read *best = NULL;
        unsigned int best_w = 0,distinct = 0;
        std::map< std::vector<int8_t>, unsigned int > votes;

        for (size_t i=0;i < reads.size();i++) {
            if (reads[i]->data_ok) {
                std::vector<int8_t> v(reads[i]->nib.val.begin()+1,reads[i]->nib.val.begin()+data_nibbles);
                unsigned int &c = votes[v];
                if (c == 0) distinct++;
                c += reads[i]->hdr_ok ? 2u : 1u;
                if (best_w < c) {
                    best_w = c;
                    best = reads[i];
                }
            }
        }

        if (best != NULL) {
            res.status = SEC_OK;
            res.inconsistent = (distinct > 1);
            nibbles_to_data(best->nib.val,res.data);
            return;
        }
    }

    /* vote per nibble, then align every read to the result (a damaged spot can make a read gain
     * or lose a nibble, which shifts everything after it) and vote again */
    std::vector<nibble_field> fields(reads.size());
    std::vector<int> weight(reads.size());
    std::vector<int8_t> best,second;
    std::vector<int> margin;

    for (size_t r=0;r < reads.size();r++) {
        fields[r] = reads[r]->nib;
        weight[r] = reads[r]->hdr_ok ? 2 : 1;
    }

    vote_nibbles(fields,weight,best,second,margin);

    for (unsigned int iter=0;iter < 3 && !nibbles_checksum_ok(best);iter++) {
        bool changed = false;

        for (size_t r=0;r < reads.size();r++) {
            nibble_field a;

            align_nibbles(reads[r]->nib,best,a);
            if (a.val != fields[r].val) {
                fields[r] = a;
                changed = true;
            }
        }

        if (!changed)
            break;

        vote_nibbles(fields,weight,best,second,margin);
    }

    if (nibbles_checksum_ok(best)) {
        res.status = SEC_VOTED;
        nibbles_to_data(best,res.data);
        return;
    }

    /* splice search: where the reads disagree, the nibbles are grouped into clusters, and each
     * cluster can take the version of any read. Combinations of versions are tried, most common
     * versions first. This handles damage that reads differently every revolution. */
    {
        std::vector<size_t> weak;
        int total_w = 0;

        for (size_t r=0;r < fields.size();r++) total_w += weight[r];
        for (size_t k=1;k < data_nibbles;k++) {
            int agree = 0;
            for (size_t r=0;r < fields.size();r++) {
                if (fields[r].val[k] == best[k]) agree += weight[r];
            }
            if (best[k] < 0 || (agree * 2) <= total_w) weak.push_back(k);
        }

        /* clusters of weak nibbles no more than 3 apart */
        std::vector< std::pair<size_t,size_t> > clusters;
        for (size_t i=0;i < weak.size();i++) {
            if (!clusters.empty() && weak[i] <= (clusters.back().second + 3u))
                clusters.back().second = weak[i];
            else
                clusters.push_back(std::pair<size_t,size_t>(weak[i],weak[i]));
        }

        /* versions of each cluster, most common (by weight) first */
        std::vector< std::vector< std::vector<int8_t> > > versions(clusters.size());
        unsigned long combos = 1;

        for (size_t c=0;c < clusters.size() && combos <= guess_budget;c++) {
            std::map< std::vector<int8_t>, int > count;

            for (size_t r=0;r < fields.size();r++) {
                std::vector<int8_t> v(fields[r].val.begin()+clusters[c].first,fields[r].val.begin()+clusters[c].second+1u);
                bool valid = true;
                for (size_t i=0;i < v.size();i++) {
                    if (v[i] < 0) valid = false;
                }
                if (valid) count[v] += weight[r];
            }

            std::vector< std::pair<int, std::vector<int8_t> > > order;
            for (std::map< std::vector<int8_t>, int >::iterator i=count.begin();i!=count.end();i++)
                order.push_back(std::pair<int, std::vector<int8_t> >(-i->second,i->first));
            std::stable_sort(order.begin(),order.end());

            for (size_t i=0;i < order.size() && i < 6;i++)
                versions[c].push_back(order[i].second);

            if (versions[c].empty()) combos = guess_budget + 1ul; /* no read has the cluster intact */
            else combos *= versions[c].size();
        }

        if (!clusters.empty() && combos <= guess_budget) {
            /* best-first by the sum of version ranks */
            std::vector<unsigned int> pick(clusters.size(),0);
            unsigned int trials = 0;

            for (unsigned int rank_sum=0;trials < combos;rank_sum++) {
                /* enumerate all picks with this rank sum (odometer, filtered) */
                std::fill(pick.begin(),pick.end(),0u);
                while (1) {
                    unsigned int sum = 0;
                    for (size_t c=0;c < pick.size();c++) sum += pick[c];

                    if (sum == rank_sum) {
                        std::vector<int8_t> v = best;

                        trials++;
                        for (size_t c=0;c < clusters.size();c++)
                            std::copy(versions[c][pick[c]].begin(),versions[c][pick[c]].end(),v.begin()+clusters[c].first);

                        if (nibbles_checksum_ok(v)) {
                            res.status = SEC_VOTED;
                            res.trials = trials;
                            nibbles_to_data(v,res.data);
                            return;
                        }
                    }

                    size_t c = 0;
                    while (c < pick.size() && ++pick[c] >= versions[c].size()) {
                        pick[c] = 0;
                        c++;
                    }
                    if (c == pick.size()) break;
                }
            }
        }
    }

    /* trial correction: the second choice of the least certain nibbles, best-first by cost */
    std::vector<nib_candidate> cand;
    static const unsigned int max_candidates = 32;

    for (size_t i=1;i < data_nibbles;i++) {
        if (best[i] < 0) continue;
        if (second[i] >= 0) {
            nib_candidate c;
            c.pos = i;
            c.val = second[i];
            c.cost = margin[i] + 1;
            cand.push_back(c);
        }
    }

    std::stable_sort(cand.begin(),cand.end());
    if (cand.size() > max_candidates)
        cand.resize(max_candidates);

    bool missing_nibble = false;
    for (size_t i=1;i < data_nibbles;i++) {
        if (best[i] < 0) missing_nibble = true;
    }

    if (!cand.empty() && !missing_nibble && guess_budget != 0) {
        std::priority_queue<nib_trial> heap;
        unsigned int trials = 0;
        nib_trial t;

        t.cost = cand[0].cost;
        t.mask = 1u;
        t.last = 0;
        heap.push(t);

        while (!heap.empty() && trials < guess_budget) {
            t = heap.top();
            heap.pop();
            trials++;

            std::vector<int8_t> v = best;
            for (unsigned int i=0;i <= t.last;i++) {
                if (t.mask & (1u << i)) v[cand[i].pos] = cand[i].val;
            }

            if (nibbles_checksum_ok(v)) {
                res.status = SEC_GUESSED;
                res.trials = trials;
                for (unsigned int i=0;i <= t.last;i++) {
                    if (t.mask & (1u << i)) res.changed++;
                }
                nibbles_to_data(v,res.data);
                return;
            }

            if ((t.last + 1u) < cand.size()) {
                nib_trial n;

                n.last = t.last + 1u;
                n.mask = t.mask | (1u << n.last);
                n.cost = t.cost + cand[n.last].cost;
                heap.push(n);

                n.mask = (t.mask & ~(1u << t.last)) | (1u << n.last);
                n.cost = t.cost - cand[t.last].cost + cand[n.last].cost;
                heap.push(n);
            }
        }
    }

    res.status = SEC_BAD;
    nibbles_to_data(best,res.data);
}

/*-----------------------------------------------------------------------------------------------*/

static bool load_gcr_track(flux_track &ft,const std::string &cappath,unsigned int track,unsigned int head) {
    if (!load_flux_track(ft,cappath,track,head))
        return false;

    ft.cell = estimate_cell_period(ft.flux,1,3);
    return true;
}

/* format byte and sides from the address fields of the first tracks */
static bool detect_format(const std::vector<std::string> &cappaths,int &format) {
    std::map<int,unsigned int> fmt;
    unsigned int side1 = 0,hdrs = 0;

    for (unsigned int track=0;track < 4;track++) {
        for (unsigned int head=0;head < 2;head++) {
            for (size_t capidx=0;capidx < cappaths.size();capidx++) {
                flux_track ft;
                std::vector<uint8_t> cells;
                std::vector<mac_read> reads;

                if (!load_gcr_track(ft,cappaths[capidx],track,head) || ft.cell <= 0) continue;

                pll_decode(cells,ft,pll_presets[0]);
                scan_track(cells,reads,true);

                for (size_t i=0;i < reads.size();i++) {
                    if (!reads[i].hdr_ok || read_track_number(reads[i]) != track) continue;
                    if (((reads[i].side >> 5u) & 1u) != head) continue;

                    fmt[reads[i].format]++;
                    hdrs++;
                    if (head == 1) side1++;
                }
            }
        }
    }

    if (hdrs < 8)
        return false;

    unsigned int sel_count = 0;
    for (std::map<int,unsigned int>::iterator i=fmt.begin();i!=fmt.end();i++) {
        if (sel_count < i->second) {
            sel_count = i->second;
            format = i->first;
        }
    }

    if (sides == 0)
        sides = (side1 >= 8 || (format & 0x20)) ? 2 : 1;

    return true;
}

/* volume name and file system from block 2: the MFS or HFS master directory block, or the
 * ProDOS volume directory header (Apple II 800K disks use the same GCR format) */
static std::string volume_name(const std::vector<uint8_t> &image,std::string &fs) {
    std::string name;

    fs.clear();
    if (image.size() < 2048) return name;

    const uint8_t *blk = &image[1024];
    const unsigned int sig = ((unsigned int)blk[0] << 8u) | blk[1];

    if (sig == 0x4244 || sig == 0xD2D7) { /* HFS 'BD', MFS */
        const unsigned int len = std::min(27u,(unsigned int)blk[0x24]);
        name.assign((const char*)blk + 0x25,len);
        fs = (sig == 0x4244) ? "HFS" : "MFS";
    }
    else if (blk[0] == 0 && blk[1] == 0 && (blk[4] & 0xF0) == 0xF0 && (blk[4] & 0x0F) != 0) {
        name.assign((const char*)blk + 5,blk[4] & 0x0Fu);
        fs = "ProDOS";
    }

    return name;
}

/* volume name for printing, non-ASCII (MacRoman) characters as '?' */
static std::string printable(const std::string &s) {
    std::string r = s;

    for (size_t i=0;i < r.size();i++) {
        if ((unsigned char)r[i] < 0x20 || (unsigned char)r[i] > 0x7E) r[i] = '?';
    }

    return r;
}

static uint32_t dc42_checksum(const uint8_t *p,size_t len) {
    uint32_t sum = 0;

    for (size_t i=0;(i+1) < len;i += 2) {
        sum += ((uint32_t)p[i] << 8u) | p[i+1];
        sum = (sum >> 1u) | (sum << 31u);
    }

    return sum;
}

static void put32(uint8_t *p,uint32_t v) {
    p[0] = (uint8_t)(v >> 24u);
    p[1] = (uint8_t)(v >> 16u);
    p[2] = (uint8_t)(v >> 8u);
    p[3] = (uint8_t)v;
}

static bool write_dc42(const std::string &path,const std::string &name,const std::vector<uint8_t> &data,const std::vector<uint8_t> &tags,int format) {
    uint8_t hdr[0x54];

    memset(hdr,0,sizeof(hdr));
    hdr[0] = (uint8_t)std::min((size_t)63,name.size());
    memcpy(hdr+1,name.data(),hdr[0]);
    put32(hdr+0x40,(uint32_t)data.size());
    put32(hdr+0x44,(uint32_t)tags.size());
    put32(hdr+0x48,dc42_checksum(&data[0],data.size()));
    put32(hdr+0x4C,tags.size() > 12 ? dc42_checksum(&tags[12],tags.size()-12) : 0); /* first sector's tags are not included */
    hdr[0x50] = (data.size() > 409600) ? 1 : 0;     /* 0 = 400K, 1 = 800K */
    hdr[0x51] = (uint8_t)format;
    hdr[0x52] = 0x01;
    hdr[0x53] = 0x00;

    FILE *fp = fopen(path.c_str(),"wb");
    if (fp == NULL) return false;

    bool ok = fwrite(hdr,sizeof(hdr),1,fp) == 1 && fwrite(&data[0],data.size(),1,fp) == 1 &&
        fwrite(&tags[0],tags.size(),1,fp) == 1;
    if (fclose(fp) != 0) ok = false;
    return ok;
}

static void help(const char *argv0) {
    fprintf(stderr,"%s [options] <raw capture directory> [dir...]\n",argv0);
    fprintf(stderr,"\n");
    fprintf(stderr,"Decodes Macintosh 400K/800K GCR floppies to a raw sector image. Multiple capture\n");
    fprintf(stderr,"directories of the same disk are combined to recover bad sectors.\n");
    fprintf(stderr,"\n");
    fprintf(stderr,"  -o <file>         Output raw image, 512 byte sectors (default disk.dsk)\n");
    fprintf(stderr,"  -dc42 <file>      Also write a DiskCopy 4.2 image, which keeps the tag bytes\n");
    fprintf(stderr,"  -ss -ds           Single (400K) or double (800K) sided instead of autodetecting\n");
    fprintf(stderr,"  -guess <n>        Max checksum trial corrections per bad sector (default %u, 0 to disable)\n",guess_budget);
    fprintf(stderr,"  -badzero          Write zeros for unrecovered sectors instead of the best guess\n");
    fprintf(stderr,"  -v                Verbose\n");
}

int main(int argc,char **argv) {
    std::vector<std::string> cappaths;
    int format = -1;

    gcr_init();

    for (int i=1;i < argc;i++) {
        char *a = argv[i];

        if (*a == '-') {
            do { a++; } while (*a == '-');

            if (!strcmp(a,"ss")) {
                sides = 1;
            }
            else if (!strcmp(a,"ds")) {
                sides = 2;
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
            else if (!strcmp(a,"o") || !strcmp(a,"dc42") || !strcmp(a,"guess")) {
                if ((i+1) >= argc) {
                    fprintf(stderr,"Switch %s requires a value\n",a);
                    return 1;
                }

                const char *v = argv[++i];

                if (!strcmp(a,"o"))
                    output_path = v;
                else if (!strcmp(a,"dc42"))
                    dc42_path = v;
                else if (!strcmp(a,"guess"))
                    guess_budget = (unsigned int)strtoul(v,NULL,0);
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

    printf("Auto-detecting format...\n");
    if (!detect_format(cappaths,format)) {
        fprintf(stderr,"Unable to detect format (not a Macintosh GCR disk?)\n");
        return 1;
    }

    printf("Using %u tracks, %d side%s, format byte 0x%02X (%s)\n",tracks,sides,sides > 1 ? "s" : "",format,
        sides > 1 ? "800K" : "400K");

    /* sector offsets: track by track, side 0 then side 1 */
    std::vector<size_t> track_first(tracks);
    size_t total_sectors = 0;
    for (unsigned int t=0;t < tracks;t++) {
        track_first[t] = total_sectors;
        total_sectors += (size_t)track_sectors(t) * (size_t)sides;
    }

    std::vector<uint8_t> image(total_sectors * 512u,0),tags(total_sectors * 12u,0);
    std::vector<int> status(total_sectors,SEC_MISSING);
    std::vector<unsigned int> status_changed(total_sectors,0),status_trials(total_sectors,0);
    std::vector<bool> status_inconsistent(total_sectors,false);
    std::vector<unsigned int> sector_track(total_sectors),sector_head(total_sectors),sector_num(total_sectors);

    for (unsigned int track=0;track < tracks;track++) {
        const unsigned int spt = track_sectors(track);

        for (unsigned int head=0;head < (unsigned int)sides;head++) {
            std::vector<flux_track> sources;
            std::vector< std::vector<mac_read> > reads(spt);
            std::vector<bool> good(spt,false);
            unsigned int presets_used = 0;
            double cell_us = 0;

            for (size_t capidx=0;capidx < cappaths.size();capidx++) {
                flux_track ft;
                if (!load_gcr_track(ft,cappaths[capidx],track,head)) continue;
                if (ft.cell > 0 && cell_us == 0) cell_us = (ft.cell * 1000000.0) / ft.sck;
                sources.push_back(ft);
            }

            for (unsigned int preset=0;preset < pll_preset_count;preset++) {
                presets_used++;

                for (size_t si=0;si < sources.size();si++) {
                    std::vector<uint8_t> cells;
                    std::vector<mac_read> tr;

                    if (sources[si].cell <= 0) continue;

                    pll_decode(cells,sources[si],pll_presets[preset]);
                    scan_track(cells,tr,false);

                    for (size_t i=0;i < tr.size();i++) {
                        if (!read_matches(tr[i],track,head,format)) continue;
                        if (tr[i].data_ok && tr[i].hdr_ok) good[tr[i].sector] = true;
                        reads[tr[i].sector].push_back(tr[i]);
                    }
                }

                bool complete = true;
                for (unsigned int s=0;s < spt;s++) {
                    if (!good[s]) complete = false;
                }
                if (complete) break;
            }

            unsigned int gcount = 0;
            std::string map;

            for (unsigned int s=0;s < spt;s++) {
                const size_t sn = track_first[track] + ((size_t)head * spt) + s;
                std::vector<const mac_read*> rp;
                sector_result res;

                for (size_t i=0;i < reads[s].size();i++)
                    rp.push_back(&reads[s][i]);

                resolve_sector(res,rp);

                status[sn] = res.status;
                status_changed[sn] = res.changed;
                status_trials[sn] = res.trials;
                status_inconsistent[sn] = res.inconsistent;
                sector_track[sn] = track;
                sector_head[sn] = head;
                sector_num[sn] = s;

                if (res.status != SEC_MISSING && !(res.status == SEC_BAD && bad_zero)) {
                    memcpy(&image[sn * 512u],res.data+12,512);
                    memcpy(&tags[sn * 12u],res.data,12);
                }

                if (res.status == SEC_OK || res.status == SEC_VOTED || res.status == SEC_GUESSED)
                    gcount++;

                map += sec_status_char[res.status];

                if (verbose && res.status != SEC_OK)
                    printf("  T/H/S %u/%u/%u: %s, %u reads\n",track,head,s,sec_status_name[res.status],res.reads);
            }

            printf("Track %2u head %u: %-12s %2u/%u",track,head,map.c_str(),gcount,spt);
            if (cell_us > 0) printf("  cell %.3fus",cell_us);
            if (sources.empty()) printf("  (no capture)");
            else if (cell_us == 0) printf("  (no GCR signal)");
            if (presets_used > 1) printf("  retries %u",presets_used-1u);
            printf("\n");
        }
    }

    {
        FILE *fp = fopen(output_path.c_str(),"wb");
        if (fp == NULL || fwrite(&image[0],image.size(),1,fp) != 1) {
            fprintf(stderr,"Cannot write %s\n",output_path.c_str());
            if (fp != NULL) fclose(fp);
            return 1;
        }
        fclose(fp);
    }

    std::string vfs;
    const std::string vname = volume_name(image,vfs);

    if (!dc42_path.empty() && !write_dc42(dc42_path,vname.empty() ? std::string("Untitled") : vname,image,tags,format)) {
        fprintf(stderr,"Cannot write %s\n",dc42_path.c_str());
        return 1;
    }

    {
        size_t count[5] = {0,0,0,0,0};
        size_t inconsistent = 0;

        for (size_t i=0;i < total_sectors;i++) {
            count[status[i]]++;
            if (status_inconsistent[i]) inconsistent++;
        }

        printf("Capture report: %zu captured / %zu total\n",count[SEC_OK]+count[SEC_VOTED]+count[SEC_GUESSED],total_sectors);
        printf("  ok %zu, recovered by voting %zu, recovered by checksum trial correction %zu, bad %zu, missing %zu\n",
            count[SEC_OK],count[SEC_VOTED],count[SEC_GUESSED],count[SEC_BAD],count[SEC_MISSING]);
        printf("  legend: O=ok V=voted G=checksum trial corrected X=bad (%s written) -=missing (zeros written)\n",
            bad_zero ? "zeros" : "best guess");

        if (count[SEC_OK] != total_sectors || inconsistent != 0) {
            printf("Sectors needing attention:\n");
            for (size_t i=0;i < total_sectors;i++) {
                if (status[i] == SEC_OK && !status_inconsistent[i]) continue;

                printf(" %zu (T/H/S %u/%u/%u) %s",i,sector_track[i],sector_head[i],sector_num[i],sec_status_name[status[i]]);
                if (status[i] == SEC_GUESSED)
                    printf(" (%u nibbles changed, trial %u)",status_changed[i],status_trials[i]);
                if (status_inconsistent[i]) printf(" good-reads-differ");
                printf("\n");
            }
        }

        if (!vname.empty())
            printf("Volume: %s (%s)\n",printable(vname).c_str(),vfs.c_str());
    }

    return 0;
}
