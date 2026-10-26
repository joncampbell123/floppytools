
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <math.h>

#include <algorithm>
#include <string>
#include <queue>
#include <map>
#include <vector>

#include "kryocomm.h"
#include "kryomfm.h"

// IBM PC MFM floppy decoder for KryoFlux raw stream captures.
//
// Track layout:
//
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
// F8-FB (FB = normal data, F8 = deleted data)
// <SECTOR DATA>
// <TWO BYTE CRC>
//
// (inter-sector gap filled with 4E)
//
// Decoding is done in stages:
//
// 1. The flux intervals of a track are loaded into memory and the MFM cell period is estimated
//    from the interval histogram.
// 2. A software PLL converts flux intervals into a cell (bit) stream. The PLL tracks rotational
//    speed variation across the track. For every flux transition, the timing margin (how far it
//    was from the cell boundary) is kept so that uncertain bits can be identified later.
// 3. The cell stream is scanned for A1 sync marks. ID and data fields are decoded without
//    regard to MFM clock bit errors, the CRC is what decides whether the data is good.
// 4. Every read of every sector (all revolutions, all capture directories, several PLL settings)
//    is kept. A sector with no good read is recovered by soft-decision voting across all reads,
//    then if necessary by flipping the least certain bits until the CRC matches.

// geometry, 0 = autodetect
unsigned int            sectors = 0;
unsigned int            heads = 0;
unsigned int            tracks = 0;
unsigned int            sector_size = 0;
unsigned int            double_track = 0;       /* i.e. 360KB DD disk captured in 1.2MB HD drive. 0=unknown 1=single 2=double */
int                     sector_base = -1;       /* first sector number on a track, normally 1. -1 = autodetect */
int                     side_id[2] = {-1,-1};   /* side number recorded in sector IDs for each head. -1 = autodetect */
unsigned int            sector_size_code = 0;

std::string             output_path = "disk.img";
unsigned int            guess_budget = 64;      /* max CRC trial corrections per bad sector, 0 to disable */
bool                    bad_zero = false;       /* write zeros instead of best guess data for unrecovered sectors */
bool                    verbose = false;

static const unsigned int max_phys_track = 90;

/* CRC over A1 A1 A1 <mark> <buf incl. CRC>, 0 if the CRC is correct */
static uint16_t crc_residual(uint8_t mark,const uint8_t *buf,size_t len) {
    const uint8_t pre[4] = { MFM_A1_SYNC_BYTE, MFM_A1_SYNC_BYTE, MFM_A1_SYNC_BYTE, mark };

    return mfm_crc16fd_update(mfm_crc16fd_update(0xFFFF,pre,4),buf,len);
}

/* CRC residual change caused by flipping the bit at position j counted from the last bit of the CRC.
 * The CRC is linear, so for a single bit error this is x^(j+16) mod G. */
static std::vector<uint16_t> crc_syndrome;

static void build_syndromes(size_t nbits) {
    if (crc_syndrome.size() >= nbits) return;

    uint16_t s = 0x1021;
    crc_syndrome.resize(nbits);
    for (size_t j=0;j < nbits;j++) {
        crc_syndrome[j] = s;
        s = (uint16_t)((s << 1u) ^ ((s & 0x8000u) ? 0x1021u : 0u));
    }
}

struct flip_candidate {
    unsigned int    bit;
    int             cost;

    bool operator<(const flip_candidate &o) const { return cost < o.cost; }
};

struct flip_trial {
    int             cost;
    uint32_t        mask;
    unsigned int    last;

    bool operator<(const flip_trial &o) const { return cost > o.cost; } /* min-heap */
};

/* Try to fix a bad CRC by flipping the least certain bits. Combinations are tried in order of
 * increasing total confidence, at most 'budget' of them. Every trial carries a ~1/65536 chance
 * of a false match, which is why the budget is limited and the result is reported as a guess.
 * soft[] and tsoft[] are per bit, the magnitude is the confidence (all and timing only). Only bits
 * with |soft| < threshold are candidates, ranked by |soft| + |tsoft| so that bits with ambiguous
 * timing come before the bits that are merely near a clock violation.
 * On success buf[] is corrected and the number of flipped bits returned, else 0. The number of
 * trials made is returned in *trials_used if not NULL (the chance of a false match is about
 * trials / 65536). */
static unsigned int crc_guess(uint16_t residual,uint8_t *buf,const std::vector<int> &soft,const std::vector<int> &tsoft,size_t nbytes,unsigned int budget,int threshold,unsigned int *trials_used=NULL) {
    static const unsigned int max_candidates = 32;
    const size_t nbits = nbytes * 8u;
    std::vector<flip_candidate> cand;

    if (budget == 0 || residual == 0)
        return 0;

    build_syndromes(nbits);

    for (size_t b=0;b < nbits;b++) {
        const int c = abs(soft[b]);
        if (c < threshold) {
            flip_candidate fc;
            fc.bit = (unsigned int)b;
            fc.cost = c + abs(tsoft[b]) + 1;
            cand.push_back(fc);
        }
    }

    if (cand.empty())
        return 0;

    std::stable_sort(cand.begin(),cand.end());
    if (cand.size() > max_candidates)
        cand.resize(max_candidates);

    /* best-first enumeration of subsets by total cost */
    std::priority_queue<flip_trial> heap;
    unsigned int trials = 0;

    {
        flip_trial t;
        t.cost = cand[0].cost;
        t.mask = 1u;
        t.last = 0;
        heap.push(t);
    }

    while (!heap.empty() && trials < budget) {
        const flip_trial t = heap.top();
        heap.pop();
        trials++;
        if (trials_used != NULL) *trials_used = trials;

        uint16_t r = residual;
        for (unsigned int i=0;i <= t.last;i++) {
            if (t.mask & (1u << i))
                r ^= crc_syndrome[nbits - 1u - cand[i].bit];
        }

        if (r == 0) {
            unsigned int flips = 0;

            for (unsigned int i=0;i <= t.last;i++) {
                if (t.mask & (1u << i)) {
                    buf[cand[i].bit >> 3u] ^= (uint8_t)(0x80u >> (cand[i].bit & 7u));
                    flips++;
                }
            }

            return flips;
        }

        if ((t.last + 1u) < cand.size()) {
            flip_trial n;

            n.last = t.last + 1u;
            n.mask = t.mask | (1u << n.last);
            n.cost = t.cost + cand[n.last].cost;
            heap.push(n);

            n.mask = (t.mask & ~(1u << t.last)) | (1u << n.last);
            n.cost = t.cost - cand[t.last].cost + cand[n.last].cost;
            heap.push(n);
        }
    }

    return 0;
}

/*-----------------------------------------------------------------------------------------------*/
/* track scanning */

struct mark_pos {
    size_t          pos;        /* cell index of the first cell following the mark byte */
    uint8_t         mark;       /* 0xFE (ID) or 0xF8-0xFB (data) */
};

struct sector_read {
    uint8_t                 cyl,side,sector,size_code;
    bool                    id_crc_ok;          /* ID CRC is good (possibly after correction) */
    bool                    id_corrected;
    bool                    has_data;
    bool                    data_crc_ok;
    uint8_t                 mark;               /* data address mark, 0xFB normal 0xF8 deleted */
    std::vector<uint8_t>    data;               /* sector data followed by 2 CRC bytes */
    std::vector<int8_t>     soft;               /* soft decision per bit of data[] */
    std::vector<int8_t>     tsoft;              /* soft decision from flux timing only */
    std::vector<uint8_t>    raw;                /* cells around the data field for realignment (bad CRC only) */

    sector_read() : cyl(0), side(0), sector(0), size_code(0), id_crc_ok(false), id_corrected(false), has_data(false), data_crc_ok(false), mark(0) { }
};

static void find_marks(const std::vector<uint8_t> &cells,std::vector<mark_pos> &marks) {
    unsigned int sr = 0;

    marks.clear();
    for (size_t i=0;i < cells.size();i++) {
        sr = ((sr << 1u) | (cell_bit(cells,i) ? 1u : 0u)) & 0xFFFFu;
        if (sr != MFM_A1_SYNC) continue;

        /* 0x4489 has a missing clock bit so it cannot occur in normal data.
         * Allow any number of A1 syncs before the mark, the CRC decides whether it is valid. */
        const size_t p = i + 1u;
        if ((p + 17u) > cells.size()) break;

        unsigned int nx = 0;
        for (unsigned int j=0;j < 16u;j++)
            nx = (nx << 1u) | (cell_bit(cells,p+j) ? 1u : 0u);
        if (nx == MFM_A1_SYNC) continue; /* another sync follows, the mark comes after that */

        const uint8_t m = decode_byte(cells,p);
        if (m == 0xFE || (m >= 0xF8 && m <= 0xFB)) {
            mark_pos mp;
            mp.pos = p + 16u;
            mp.mark = m;
            marks.push_back(mp);
        }
    }
}

/* distance in bytes from the end of the ID field to the start of sector data within which
 * the data mark must appear. IBM format is normally 22 + 12 + 4 = 38, but sectors rewritten by
 * another drive can have it much later (74 bytes has been seen). The search also stops at the
 * next ID mark. */
static const size_t max_id_to_data_bytes = 128;

static void scan_track(const std::vector<uint8_t> &cells,std::vector<sector_read> &reads,bool ids_only) {
    std::vector<mark_pos> marks;

    find_marks(cells,marks);

    for (size_t mi=0;mi < marks.size();mi++) {
        if (marks[mi].mark != 0xFE) continue;

        const size_t idp = marks[mi].pos;
        if ((idp + (6u * 16u) + 1u) > cells.size()) continue;

        sector_read r;
        uint8_t idb[6];
        int8_t idsoft[6*8],idtsoft[6*8];

        decode_soft(cells,idp,6,idb,idsoft,idtsoft);

        uint16_t res = crc_residual(0xFE,idb,6);
        if (res == 0) {
            r.id_crc_ok = true;
        }
        else {
            /* try a very small number of trial corrections on the ID field */
            std::vector<int> s(idsoft,idsoft+(6*8)),ts(idtsoft,idtsoft+(6*8));
            if (crc_guess(res,idb,s,ts,6,16,80) != 0) {
                r.id_crc_ok = true;
                r.id_corrected = true;
            }
        }

        r.cyl = idb[0];
        r.side = idb[1];
        r.sector = idb[2];
        r.size_code = idb[3];

        if (!ids_only && r.size_code <= 6) {
            const size_t id_end = idp + (6u * 16u);
            const size_t ssz = 128u << r.size_code;

            for (size_t mj=mi+1;mj < marks.size();mj++) {
                if (marks[mj].mark == 0xFE) break; /* next sector ID, this one has no data */
                if (marks[mj].pos > (id_end + (max_id_to_data_bytes * 16u))) break;

                const size_t dp = marks[mj].pos;
                if ((dp + ((ssz + 2u) * 16u) + 1u) > cells.size()) break;

                r.has_data = true;
                r.mark = marks[mj].mark;
                r.data.resize(ssz + 2u);
                r.soft.resize((ssz + 2u) * 8u);
                r.tsoft.resize((ssz + 2u) * 8u);
                decode_soft(cells,dp,ssz + 2u,&r.data[0],&r.soft[0],&r.tsoft[0]);
                r.data_crc_ok = (crc_residual(r.mark,&r.data[0],r.data.size()) == 0);

                /* keep the cells of a bad read so that it can be realigned after a bit slip */
                if (!r.data_crc_ok && dp >= mfm_align_pad && (dp + ((ssz + 2u) * 16u) + mfm_align_pad) <= cells.size())
                    r.raw.assign(cells.begin()+(dp-mfm_align_pad),cells.begin()+(dp+((ssz+2u)*16u)+mfm_align_pad));
                break;
            }
        }

        reads.push_back(r);
    }
}

/* decode a track and return all sector IDs with a good CRC (no data) */
static bool read_track_ids(std::vector<sector_read> &ids,const std::string &cappath,unsigned int phys_track,unsigned int head) {
    flux_track ft;
    std::vector<uint8_t> cells;
    std::vector<sector_read> reads;

    ids.clear();
    if (!load_flux_track(ft,cappath,phys_track,head))
        return false;
    if (ft.cell <= 0)
        return true;

    pll_decode(cells,ft,pll_presets[0]);
    scan_track(cells,reads,true);

    for (size_t i=0;i < reads.size();i++) {
        if (reads[i].id_crc_ok)
            ids.push_back(reads[i]);
    }

    return true;
}

/*-----------------------------------------------------------------------------------------------*/
/* sector recovery */

enum {
    SEC_MISSING=0,      /* no ID/data found at all */
    SEC_OK,             /* at least one read with good CRC */
    SEC_VOTED,          /* good CRC after combining multiple bad reads */
    SEC_GUESSED,        /* good CRC after flipping uncertain bits (small chance of being wrong) */
    SEC_BAD             /* no good CRC, best guess data */
};

static const char sec_status_char[] = { '-', 'O', 'V', 'G', 'X' };
static const char *sec_status_name[] = { "missing", "ok", "voted", "guessed", "bad" };

struct sector_result {
    int                     status;
    unsigned int            reads;
    unsigned int            flips;
    unsigned int            trials;         /* CRC trial corrections made (SEC_GUESSED) */
    bool                    deleted;
    bool                    inconsistent;   /* multiple good reads with different contents (weak bits?) */
    std::vector<uint8_t>    data;

    sector_result() : status(SEC_MISSING), reads(0), flips(0), trials(0), deleted(false), inconsistent(false) { }
};

static unsigned int read_weight(const sector_read &r) {
    return r.id_crc_ok ? 2u : 1u;
}

static void resolve_sector(sector_result &res,const std::vector<const sector_read*> &reads,size_t ssz) {
    const size_t nbytes = ssz + 2u;

    res = sector_result();
    res.reads = (unsigned int)reads.size();
    if (reads.empty())
        return;

    /* any good read? pick the most common good content */
    {
        const sector_read *best = NULL;
        unsigned int best_w = 0,distinct = 0;
        std::map< std::vector<uint8_t>, unsigned int > votes;

        for (size_t i=0;i < reads.size();i++) {
            if (reads[i]->data_crc_ok) {
                unsigned int &v = votes[reads[i]->data];
                if (v == 0) distinct++;
                v += read_weight(*reads[i]);
                if (best_w < v) {
                    best_w = v;
                    best = reads[i];
                }
            }
        }

        if (best != NULL) {
            res.status = SEC_OK;
            res.deleted = (best->mark == 0xF8 || best->mark == 0xF9);
            res.inconsistent = (distinct > 1);
            res.data.assign(best->data.begin(),best->data.begin()+ssz);
            return;
        }
    }

    /* soft decision combining of all reads, then realign the bad reads against the result
     * to undo bit slips and combine again */
    std::vector<mfm_copy> wr(reads.size());
    mfm_consensus v;
    uint8_t mark = 0xFB;

    for (size_t i=0;i < reads.size();i++) {
        wr[i].raw = &reads[i]->raw;
        wr[i].weight = (int)read_weight(*reads[i]);
        wr[i].data = reads[i]->data;
        wr[i].soft = reads[i]->soft;
        wr[i].tsoft = reads[i]->tsoft;
    }

    for (unsigned int iter=0;iter < 4;iter++) {
        if (iter != 0 && !mfm_realign(wr,nbytes,v))
            break;

        mfm_vote(wr,nbytes,v);

        /* data mark by weighted majority */
        {
            unsigned int mw[4] = {0,0,0,0};
            unsigned int best = 0;

            mark = 0xFB;
            for (size_t i=0;i < reads.size();i++) {
                if (v.use[i]) mw[reads[i]->mark & 3u] += read_weight(*reads[i]);
            }
            for (unsigned int i=0;i < 4;i++) {
                if (best < mw[i]) {
                    best = mw[i];
                    mark = (uint8_t)(0xF8 + i);
                }
            }
        }

        if (crc_residual(mark,&v.buf[0],nbytes) == 0)
            break;
    }

    std::vector<uint8_t> &buf = v.buf;

    res.deleted = (mark == 0xF8 || mark == 0xF9);

    const uint16_t residual = crc_residual(mark,&buf[0],nbytes);
    if (residual == 0) {
        res.status = SEC_VOTED;
    }
    else {
        /* candidates: bits with combined confidence below 60% */
        const int threshold = (v.total_w * 126 * 6) / 10;

        res.flips = crc_guess(residual,&buf[0],v.sum,v.tsum,nbytes,guess_budget,threshold,&res.trials);
        res.status = (res.flips != 0) ? SEC_GUESSED : SEC_BAD;
    }

    res.data.assign(buf.begin(),buf.begin()+ssz);
}

/*-----------------------------------------------------------------------------------------------*/
/* format detection */

struct id_cache_key {
    size_t          capidx;
    unsigned int    phys,head;

    bool operator<(const id_cache_key &o) const {
        if (capidx != o.capidx) return capidx < o.capidx;
        if (phys != o.phys) return phys < o.phys;
        return head < o.head;
    }
};

static std::map<id_cache_key, std::vector<sector_read> > id_cache;

static const std::vector<sector_read> *cached_track_ids(const std::vector<std::string> &cappaths,size_t capidx,unsigned int phys,unsigned int head) {
    id_cache_key k;
    k.capidx = capidx;
    k.phys = phys;
    k.head = head;

    std::map<id_cache_key, std::vector<sector_read> >::iterator i = id_cache.find(k);
    if (i == id_cache.end()) {
        std::vector<sector_read> ids;
        if (!read_track_ids(ids,cappaths[capidx],phys,head))
            return NULL;
        i = id_cache.insert(std::pair<id_cache_key, std::vector<sector_read> >(k,ids)).first;
    }

    return &(i->second);
}

/* most common value in a histogram, ties going to the larger value */
static int histogram_mode(const std::map<int,unsigned long> &h) {
    int sel = -1;
    unsigned long sel_count = 0;

    for (std::map<int,unsigned long>::const_iterator i=h.begin();i!=h.end();i++) {
        if (sel_count <= i->second) {
            sel_count = i->second;
            sel = i->first;
        }
    }

    return sel;
}

static unsigned int count_distinct_sectors(const std::vector<sector_read> &ids,unsigned int cyl,int side) {
    std::vector<bool> seen(256,false);
    unsigned int count = 0;

    for (size_t i=0;i < ids.size();i++) {
        const sector_read &r = ids[i];
        if (r.cyl != cyl || r.size_code != sector_size_code) continue;
        if (side >= 0 && r.side != (unsigned int)side) continue;
        if ((int)r.sector < sector_base || (int)r.sector >= (sector_base + (int)sectors)) continue;
        if (!seen[r.sector]) {
            seen[r.sector] = true;
            count++;
        }
    }

    return count;
}

static bool detect_format(const std::vector<std::string> &cappaths) {
    unsigned int max_phys = 0;
    bool have_head1 = false;

    for (size_t capidx=0;capidx < cappaths.size();capidx++) {
        for (unsigned int t=0;t < max_phys_track;t++) {
            if (track_file_exists(cappaths[capidx],t,0)) {
                if (max_phys < (t+1u)) max_phys = t+1u;
            }
            if (track_file_exists(cappaths[capidx],t,1))
                have_head1 = true;
        }
    }

    if (max_phys == 0) {
        fprintf(stderr,"No trackNN.0.raw files found\n");
        return false;
    }

    /* single/double track stepping. Look at the cylinder numbers on even tracks past 0.
     * This is needed to support 360KB DD floppies imaged by a 1.2MB HD drive. */
    if (double_track == 0) {
        unsigned long count_1x = 0,count_2x = 0;

        printf("Auto-detecting double/single track...\n");

        for (unsigned int phys=2;phys <= 16 && phys < max_phys;phys += 2) {
            for (size_t capidx=0;capidx < cappaths.size();capidx++) {
                for (unsigned int head=0;head < 2;head++) {
                    const std::vector<sector_read> *ids = cached_track_ids(cappaths,capidx,phys,head);
                    if (ids == NULL) continue;

                    for (size_t i=0;i < ids->size();i++) {
                        if ((*ids)[i].cyl == phys) count_1x++;
                        else if ((*ids)[i].cyl == (phys/2u)) count_2x++;
                    }
                }
            }

            if ((count_1x + count_2x) >= 32) break;
        }

        if ((count_1x + count_2x) >= 4)
            double_track = (count_2x > count_1x) ? 2 : 1;
        else if (sectors != 0 && sector_size != 0)
            double_track = 1; /* user gave the format, assume normal stepping */
        else
            return false;
    }

    const unsigned int max_cyl = (max_phys + double_track - 1u) / double_track;

    /* sample cylinders for the format detection. several, in case of damage or odd tracks */
    std::vector<unsigned int> sample;
    for (unsigned int c=0;c < max_cyl;c++) {
        if (c < 10 || (c % 8u) == 0)
            sample.push_back(c);
    }

    /* sector size */
    if (sector_size == 0) {
        std::map<int,unsigned long> h;

        printf("Auto-detecting sector size...\n");

        for (size_t si=0;si < sample.size();si++) {
            for (size_t capidx=0;capidx < cappaths.size();capidx++) {
                const std::vector<sector_read> *ids = cached_track_ids(cappaths,capidx,sample[si]*double_track,0);
                if (ids == NULL) continue;

                for (size_t i=0;i < ids->size();i++) {
                    if ((*ids)[i].cyl == sample[si] && (*ids)[i].size_code <= 6)
                        h[(*ids)[i].size_code]++;
                }
            }
        }

        const int sel = histogram_mode(h);
        if (sel < 0 || h[sel] < 6)
            return false;

        sector_size = 128u << (unsigned int)sel;
    }

    sector_size_code = 0;
    while ((128u << sector_size_code) < sector_size) sector_size_code++;
    if ((128u << sector_size_code) != sector_size) {
        fprintf(stderr,"Invalid sector size %u\n",sector_size);
        return false;
    }

    /* side number recorded in the IDs of head 0 */
    if (side_id[0] < 0) {
        std::map<int,unsigned long> h;

        for (size_t si=0;si < sample.size();si++) {
            for (size_t capidx=0;capidx < cappaths.size();capidx++) {
                const std::vector<sector_read> *ids = cached_track_ids(cappaths,capidx,sample[si]*double_track,0);
                if (ids == NULL) continue;

                for (size_t i=0;i < ids->size();i++) {
                    if ((*ids)[i].cyl == sample[si] && (*ids)[i].size_code == sector_size_code)
                        h[(*ids)[i].side]++;
                }
            }
        }

        side_id[0] = histogram_mode(h);
        if (side_id[0] < 0) side_id[0] = 0;
    }

    /* first sector number and sectors per track. Use the most common lowest and highest sector
     * number of each track, which rejects copy protection sectors and damaged tracks. */
    if (sectors == 0 || sector_base < 0) {
        std::map<int,unsigned long> hmin,hcount;
        std::vector< std::pair<int,int> > ranges;

        printf("Auto-detecting sectors per track...\n");

        for (size_t si=0;si < sample.size();si++) {
            for (size_t capidx=0;capidx < cappaths.size();capidx++) {
                const std::vector<sector_read> *ids = cached_track_ids(cappaths,capidx,sample[si]*double_track,0);
                if (ids == NULL) continue;

                int mn = 256,mx = -1;
                for (size_t i=0;i < ids->size();i++) {
                    const sector_read &r = (*ids)[i];
                    if (r.cyl != sample[si] || r.side != (unsigned int)side_id[0] || r.size_code != sector_size_code) continue;
                    if (mn > (int)r.sector) mn = (int)r.sector;
                    if (mx < (int)r.sector) mx = (int)r.sector;
                }

                if (mx >= 0) {
                    hmin[mn]++;
                    ranges.push_back(std::pair<int,int>(mn,mx));
                }
            }
        }

        if (sector_base < 0) {
            sector_base = histogram_mode(hmin);
            if (sector_base < 0) sector_base = 1;
        }

        if (sectors == 0) {
            for (size_t i=0;i < ranges.size();i++) {
                if (ranges[i].second >= sector_base)
                    hcount[ranges[i].second + 1 - sector_base]++;
            }

            const int sel = histogram_mode(hcount);
            if (sel <= 0)
                return false;

            sectors = (unsigned int)sel;
        }
    }

    /* number of heads. look at side 2 (head == 1) and see whether the same format is there. */
    if (heads == 0 || (heads > 1 && side_id[1] < 0)) {
        std::map<int,unsigned long> hside;
        unsigned int found = 0,good = 0;
        const unsigned int need = std::max(1u,sectors / 2u);

        printf("Auto-detecting heads...\n");

        if (have_head1) {
            for (size_t si=0;si < sample.size();si++) {
                bool tfound = false,tgood = false;

                for (size_t capidx=0;capidx < cappaths.size();capidx++) {
                    const std::vector<sector_read> *ids = cached_track_ids(cappaths,capidx,sample[si]*double_track,1);
                    if (ids == NULL) continue;
                    tfound = true;

                    for (size_t i=0;i < ids->size();i++) {
                        const sector_read &r = (*ids)[i];
                        if (r.cyl == sample[si] && r.size_code == sector_size_code)
                            hside[r.side]++;
                    }

                    if (count_distinct_sectors(*ids,sample[si],-1) >= need)
                        tgood = true;
                }

                if (tfound) found++;
                if (tgood) good++;
            }
        }

        if (heads == 0)
            heads = (found != 0 && good >= std::max(1u,found / 2u)) ? 2 : 1;

        if (heads > 1 && side_id[1] < 0) {
            side_id[1] = histogram_mode(hside);
            if (side_id[1] < 0) side_id[1] = 1;
        }
    }

    /* number of tracks: the last cylinder that has a reasonable number of sectors on every head.
     * Cylinders past that formatted on only some heads (e.g. the disk manufacturer's
     * certification track on head 0 of cylinder 80) are left out, -tracks includes them. */
    if (tracks == 0) {
        const unsigned int need = std::min(4u,std::max(1u,sectors / 2u));
        std::vector<unsigned int> partial;

        printf("Auto-detecting tracks...\n");

        for (unsigned int cyl=0;cyl < max_cyl;cyl++) {
            unsigned int hok = 0;

            for (unsigned int head=0;head < heads;head++) {
                bool ok = false;

                printf("\x0D" "t=%u h=%u ",cyl,head);
                fflush(stdout);

                for (size_t capidx=0;capidx < cappaths.size() && !ok;capidx++) {
                    const std::vector<sector_read> *ids = cached_track_ids(cappaths,capidx,cyl*double_track,head);
                    if (ids == NULL) continue;

                    if (count_distinct_sectors(*ids,cyl,side_id[head]) >= need)
                        ok = true;
                }

                if (ok) hok++;
            }

            if (hok == heads) tracks = cyl + 1u;
            else if (hok != 0) partial.push_back(cyl);
        }

        printf("\x0D            \x0D");
        fflush(stdout);

        for (size_t i=0;i < partial.size();i++) {
            if (partial[i] >= tracks)
                printf("Note: cylinder %u is formatted on only some heads, not included (use -tracks %u to include it)\n",
                    partial[i],partial[i]+1u);
        }
    }

    id_cache.clear();
    return true;
}

/*-----------------------------------------------------------------------------------------------*/

struct flux_source {
    size_t          capidx;
    unsigned int    phys;
    flux_track      ft;
};

static void help(const char *argv0) {
    fprintf(stderr,"%s [options] <raw capture directory> [dir...]\n",argv0);
    fprintf(stderr,"\n");
    fprintf(stderr,"Multiple capture directories of the same disk are combined to recover bad sectors.\n");
    fprintf(stderr,"\n");
    fprintf(stderr,"  -o <file>         Output image (default disk.img)\n");
    fprintf(stderr,"  -1.4mb -720k -1.2mb -360k -2.88mb\n");
    fprintf(stderr,"                    Use a standard format instead of autodetecting\n");
    fprintf(stderr,"  -sside -dside     Single or double sided\n");
    fprintf(stderr,"  -tracks <n>       Number of cylinders\n");
    fprintf(stderr,"  -sectors <n>      Sectors per track\n");
    fprintf(stderr,"  -ssize <n>        Sector size in bytes\n");
    fprintf(stderr,"  -base <n>         First sector number (normally 1)\n");
    fprintf(stderr,"  -single -double   Single or double track stepping (double = 40 track disk in 80 track drive)\n");
    fprintf(stderr,"  -guess <n>        Max CRC trial corrections per bad sector (default %u, 0 to disable)\n",guess_budget);
    fprintf(stderr,"  -badzero          Write zeros for unrecovered sectors instead of the best guess\n");
    fprintf(stderr,"  -v                Verbose\n");
}

int main(int argc,char **argv) {
    std::vector<std::string> cappaths;
    FILE *dsk_fp;

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
            else if (!strcmp(a,"2.88mb")) {
                sectors = 36;
                heads = 2;
                tracks = 80;
                sector_size = 512;
            }
            else if (!strcmp(a,"1.2mb")) {
                sectors = 15;
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
            else if (!strcmp(a,"360k")) {
                sectors = 9;
                heads = 2;
                tracks = 40;
                sector_size = 512;
            }
            else if (!strcmp(a,"sside")) {
                heads = 1;
            }
            else if (!strcmp(a,"dside")) {
                heads = 2;
            }
            else if (!strcmp(a,"single")) {
                double_track = 1;
            }
            else if (!strcmp(a,"double")) {
                double_track = 2;
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
            else if (!strcmp(a,"o") || !strcmp(a,"tracks") || !strcmp(a,"sectors") || !strcmp(a,"ssize") ||
                     !strcmp(a,"base") || !strcmp(a,"guess")) {
                if ((i+1) >= argc) {
                    fprintf(stderr,"Switch %s requires a value\n",a);
                    return 1;
                }

                const char *v = argv[++i];

                if (!strcmp(a,"o"))
                    output_path = v;
                else if (!strcmp(a,"tracks"))
                    tracks = (unsigned int)strtoul(v,NULL,0);
                else if (!strcmp(a,"sectors"))
                    sectors = (unsigned int)strtoul(v,NULL,0);
                else if (!strcmp(a,"ssize"))
                    sector_size = (unsigned int)strtoul(v,NULL,0);
                else if (!strcmp(a,"base"))
                    sector_base = (int)strtol(v,NULL,0);
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

    if (heads > 2) {
        fprintf(stderr,"At most 2 heads supported\n");
        return 1;
    }

    if (!detect_format(cappaths) || heads == 0 || sectors == 0 || tracks == 0 || sector_size == 0 || double_track == 0) {
        printf("Using disk geometry C/H/S/Sz %u/%u/%u/%u doubletrack=%u\n",tracks,heads,sectors,sector_size,double_track);
        fprintf(stderr,"Unable to detect format\n");
        return 1;
    }

    if (sector_base < 0) sector_base = 1;
    if (side_id[0] < 0) side_id[0] = 0;
    if (side_id[1] < 0) side_id[1] = 1;

    printf("Using disk geometry C/H/S/Sz %u/%u/%u/%u doubletrack=%u firstsector=%d sideids=%d",
        tracks,heads,sectors,sector_size,double_track,sector_base,side_id[0]);
    if (heads > 1) printf(",%d",side_id[1]);
    printf("\n");

    dsk_fp = fopen(output_path.c_str(),"wb");
    if (dsk_fp == NULL) {
        fprintf(stderr,"Cannot create %s\n",output_path.c_str());
        return 1;
    }

    const size_t total_sectors = (size_t)tracks * (size_t)heads * (size_t)sectors;
    std::vector<uint8_t> image(total_sectors * (size_t)sector_size,0);
    std::vector<int> status(total_sectors,SEC_MISSING);
    std::vector<unsigned int> status_flips(total_sectors,0);
    std::vector<unsigned int> status_trials(total_sectors,0);
    std::vector<bool> status_deleted(total_sectors,false);
    std::vector<bool> status_inconsistent(total_sectors,false);

    for (unsigned int cyl=0;cyl < tracks;cyl++) {
        for (unsigned int head=0;head < heads;head++) {
            std::vector<flux_source> sources;
            std::vector< std::vector<sector_read> > reads(sectors);
            std::vector<bool> good(sectors,false);
            unsigned int presets_used = 0;
            double cell_us = 0;

            /* the track itself in every capture */
            for (size_t capidx=0;capidx < cappaths.size();capidx++) {
                flux_source s;
                s.capidx = capidx;
                s.phys = cyl * double_track;
                if (!load_flux_track(s.ft,cappaths[capidx],s.phys,head)) continue;
                if (s.ft.cell > 0 && cell_us == 0) cell_us = (s.ft.cell * 1000000.0) / s.ft.sck;
                sources.push_back(s);
            }

            for (unsigned int preset=0;preset < pll_preset_count;preset++) {
                /* on retry, double stepped disks can also be read from the odd tracks between,
                 * which pick up the neighboring track. The cylinder ID check keeps it honest. */
                if (preset == 1 && double_track == 2) {
                    for (size_t capidx=0;capidx < cappaths.size();capidx++) {
                        for (int d=-1;d <= 1;d += 2) {
                            const int phys = (int)(cyl * 2u) + d;
                            if (phys < 0) continue;

                            flux_source s;
                            s.capidx = capidx;
                            s.phys = (unsigned int)phys;
                            if (!load_flux_track(s.ft,cappaths[capidx],s.phys,head)) continue;
                            sources.push_back(s);
                        }
                    }
                }

                presets_used++;

                for (size_t si=0;si < sources.size();si++) {
                    std::vector<uint8_t> cells;
                    std::vector<sector_read> tr;
                    unsigned int extra = 0;

                    if (sources[si].ft.cell <= 0) continue;

                    pll_decode(cells,sources[si].ft,pll_presets[preset]);
                    scan_track(cells,tr,false);

                    for (size_t i=0;i < tr.size();i++) {
                        sector_read &r = tr[i];

                        if (!r.has_data) continue;

                        /* a damaged ID is still accepted if every field matches what is expected */
                        if (r.cyl != cyl || r.side != (unsigned int)side_id[head] || r.size_code != sector_size_code ||
                            (int)r.sector < sector_base || (int)r.sector >= (sector_base + (int)sectors)) {
                            if (r.id_crc_ok) extra++;
                            continue;
                        }

                        const unsigned int sn = (unsigned int)((int)r.sector - sector_base);
                        if (r.data_crc_ok && r.id_crc_ok) good[sn] = true;

                        /* keep every read, bad reads are combined later for recovery */
                        reads[sn].push_back(r);
                    }

                    if (verbose && extra != 0 && preset == 0)
                        printf("  cyl %u head %u: %u sector IDs outside the disk geometry (capture %zu track %u)\n",
                            cyl,head,extra,sources[si].capidx,sources[si].phys);
                }

                bool complete = true;
                for (unsigned int s=0;s < sectors;s++) {
                    if (!good[s]) complete = false;
                }
                if (complete) break;
            }

            /* resolve each sector */
            unsigned int gcount = 0;
            std::string map;

            for (unsigned int s=0;s < sectors;s++) {
                const size_t sector_num = ((((size_t)cyl * heads) + head) * sectors) + s;
                std::vector<const sector_read*> rp;
                sector_result res;

                for (size_t i=0;i < reads[s].size();i++)
                    rp.push_back(&reads[s][i]);

                resolve_sector(res,rp,sector_size);

                status[sector_num] = res.status;
                status_flips[sector_num] = res.flips;
                status_trials[sector_num] = res.trials;
                status_deleted[sector_num] = res.deleted;
                status_inconsistent[sector_num] = res.inconsistent;

                if (res.status != SEC_MISSING && !(res.status == SEC_BAD && bad_zero))
                    memcpy(&image[sector_num * (size_t)sector_size],&res.data[0],sector_size);

                if (res.status == SEC_OK || res.status == SEC_VOTED || res.status == SEC_GUESSED)
                    gcount++;

                map += sec_status_char[res.status];

                if (verbose && res.status != SEC_OK)
                    printf("  C/H/S %u/%u/%u: %s, %u reads%s\n",cyl,head,s+(unsigned int)sector_base,
                        sec_status_name[res.status],res.reads,res.flips ? " (CRC trial correction)" : "");
            }

            printf("Cyl %2u head %u: %s %2u/%u",cyl,head,map.c_str(),gcount,sectors);
            if (cell_us > 0) printf("  cell %.3fus",cell_us);
            if (sources.empty()) printf("  (no capture)");
            else if (cell_us == 0) printf("  (no MFM signal)");
            if (presets_used > 1) printf("  retries %u",presets_used-1u);
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
        size_t deleted = 0,inconsistent = 0;

        for (size_t i=0;i < total_sectors;i++) {
            count[status[i]]++;
            if (status_deleted[i]) deleted++;
            if (status_inconsistent[i]) inconsistent++;
        }

        printf("Capture report: %zu captured / %zu total\n",count[SEC_OK]+count[SEC_VOTED]+count[SEC_GUESSED],total_sectors);
        printf("  ok %zu, recovered by voting %zu, recovered by CRC trial correction %zu, bad %zu, missing %zu\n",
            count[SEC_OK],count[SEC_VOTED],count[SEC_GUESSED],count[SEC_BAD],count[SEC_MISSING]);
        printf("  legend: O=ok V=voted G=CRC trial corrected X=bad (%s written) -=missing (zeros written)\n",
            bad_zero ? "zeros" : "best guess");

        if (count[SEC_OK] != total_sectors || deleted != 0 || inconsistent != 0) {
            printf("Sectors needing attention:\n");
            for (size_t i=0;i < total_sectors;i++) {
                if (status[i] == SEC_OK && !status_deleted[i] && !status_inconsistent[i]) continue;

                printf(" %zu (C/H/S %u/%u/%u) %s",
                    i,
                    (unsigned int)(i / heads / sectors),
                    (unsigned int)((i / sectors) % heads),
                    (unsigned int)((i % sectors) + (size_t)sector_base),
                    sec_status_name[status[i]]);
                if (status[i] == SEC_GUESSED)
                    printf(" (%u bits flipped, trial %u, ~1 in %u chance of a false match)",status_flips[i],status_trials[i],65536u / std::max(1u,status_trials[i]));
                if (status_deleted[i]) printf(" deleted-data-mark");
                if (status_inconsistent[i]) printf(" good-reads-differ");
                printf("\n");
            }
        }
    }

    printf("Using disk geometry C/H/S/Sz %u/%u/%u/%u doubletrack=%u\n",tracks,heads,sectors,sector_size,double_track);
    return 0;
}
