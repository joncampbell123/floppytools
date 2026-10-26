
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <algorithm>
#include <string>
#include <vector>

#include "kryocomm.h"
#include "kryomfm.h"

/*-----------------------------------------------------------------------------------------------*/
/* flux loading */


/* Estimate the MFM cell period from the histogram of flux intervals.
 * MFM intervals are 2, 3 or 4 cells. The first strong peak is 2 cells, the estimate is then
 * refined by fitting all intervals that are near 2, 3 or 4 cells. */
double estimate_cell_period(const std::vector<uint32_t> &flux) {
    static const unsigned int maxbin = 2048;
    std::vector<unsigned long> hist(maxbin,0),sm(maxbin,0);
    unsigned long total = 0,max = 0;

    for (size_t i=0;i < flux.size();i++) {
        if (flux[i] < maxbin) {
            hist[flux[i]]++;
            total++;
        }
    }

    for (unsigned int i=2;i < (maxbin-2);i++) {
        sm[i] = hist[i-2] + hist[i-1] + hist[i] + hist[i+1] + hist[i+2];
        if (max < sm[i]) max = sm[i];
    }

    if (max < 100ul)
        return 0;

    unsigned int i = 0;
    while (i < maxbin && sm[i] < (max / 8ul)) i++;
    while ((i+1) < maxbin && sm[i+1] >= sm[i]) i++;

    double cell = (double)i / 2.0;
    if (cell < 4.0)
        return 0;

    unsigned long fit = 0;
    for (unsigned int iter=0;iter < 4;iter++) {
        double num = 0,den = 0;

        fit = 0;
        for (unsigned int b=1;b < maxbin;b++) {
            if (hist[b] == 0) continue;

            double k = floor(((double)b / cell) + 0.5);
            if (k < 2 || k > 4) continue;

            num += (double)hist[b] * (double)b;
            den += (double)hist[b] * k;
            fit += hist[b];
        }

        if (den == 0)
            return 0;

        cell = num / den;
    }

    /* most intervals must fit MFM closely, else this is not an MFM track (unformatted, noise, FM, etc.) */
    fit = 0;
    for (unsigned int b=1;b < maxbin;b++) {
        const double k = floor(((double)b / cell) + 0.5);
        if (k >= 2 && k <= 4 && fabs((double)b - (k * cell)) <= (cell / 4.0))
            fit += hist[b];
    }

    if (fit < (total / 2ul))
        return 0;

    return cell;
}

/* Same stream format as kryoflux_read(), but parsed from memory. kryoflux_read() calls ftell()
 * for every flux event which is too slow when every track is read several times. */
bool load_flux_track(flux_track &ft,const std::string &cappath,unsigned int track,unsigned int head) {
    struct kryoflux_stream_info si;
    std::vector<unsigned char> raw;

    ft.flux.clear();
    ft.cell = 0;
    ft.sck = si.sck;

    FILE *fp = kryo_fopen(cappath,track,head);
    if (fp == NULL)
        return false;

    {
        unsigned char tmp[65536];
        size_t rd;

        while ((rd=fread(tmp,1,sizeof(tmp),fp)) > 0)
            raw.insert(raw.end(),tmp,tmp+rd);
    }

    fclose(fp);

    const size_t len = raw.size();
    size_t i = 0;
    uint32_t ovl = 0;

    ft.flux.reserve(len);
    while (i < len) {
        const unsigned char c = raw[i];

        if (c <= 0x07) { /* Flux2 */
            if ((i+2) > len) break;
            ft.flux.push_back(ovl + ((uint32_t)c << 8u) + raw[i+1]);
            ovl = 0;
            i += 2;
        }
        else if (c == 0x08) { /* Nop1 */
            i += 1;
        }
        else if (c == 0x09) { /* Nop2 */
            i += 2;
        }
        else if (c == 0x0A) { /* Nop3 */
            i += 3;
        }
        else if (c == 0x0B) { /* Ovl16 */
            ovl += 0x10000u;
            i += 1;
        }
        else if (c == 0x0C) { /* Flux3 */
            if ((i+3) > len) break;
            ft.flux.push_back(ovl + ((uint32_t)raw[i+1] << 8u) + raw[i+2]);
            ovl = 0;
            i += 3;
        }
        else if (c == 0x0D) { /* OOB */
            if ((i+4) > len) break;

            const unsigned char type = raw[i+1];
            const size_t olen = (size_t)raw[i+2] + ((size_t)raw[i+3] << 8u);

            if (type == 0x0D) /* EOF */
                break;
            if ((i+4+olen) > len)
                break;

            if (type == 0x04) { /* KFInfo */
                std::vector<unsigned char> msg(raw.begin()+i+4,raw.begin()+i+4+olen);
                kryoflux_update_stream_info(si,msg);
            }

            i += 4 + olen;
        }
        else { /* Flux1 */
            ft.flux.push_back(ovl + c);
            ovl = 0;
            i += 1;
        }
    }

    ft.sck = si.sck;
    ft.cell = estimate_cell_period(ft.flux);
    return !ft.flux.empty();
}

bool track_file_exists(const std::string &cappath,unsigned int track,unsigned int head) {
    FILE *fp = kryo_fopen(cappath,track,head);
    if (fp == NULL) return false;
    fclose(fp);
    return true;
}

/*-----------------------------------------------------------------------------------------------*/
/* PLL flux to cell decoding */


/* first entry is the normal decode, the rest are retries for tracks with missing sectors */
const pll_params pll_presets[] = {
    { 1.00, 0.05, 0.60, 0.10 },
    { 1.00, 0.02, 0.30, 0.10 },
    { 1.00, 0.10, 0.85, 0.15 },
    { 0.98, 0.05, 0.60, 0.10 },
    { 1.02, 0.05, 0.60, 0.10 },
    { 1.00, 0.00, 0.50, 0.00 },
    { 1.00, 0.04, 0.15, 0.08 }
};
const unsigned int pll_preset_count = sizeof(pll_presets) / sizeof(pll_presets[0]);


void pll_decode(std::vector<uint8_t> &cells,const flux_track &ft,const pll_params &p) {
    const double centre = ft.cell * p.clock_scale;
    const double clock_min = centre * (1.0 - p.max_adj);
    const double clock_max = centre * (1.0 + p.max_adj);
    double clock = centre;
    double flux = 0;

    cells.clear();
    if (centre <= 0) return;
    cells.reserve((size_t)((ft.flux.size() * 3u) + 16u));

    for (size_t i=0;i < ft.flux.size();i++) {
        flux += (double)ft.flux[i];

        /* transitions too close to the previous one are noise, merge them into the next interval */
        if (flux < (clock / 2.0))
            continue;

        unsigned int zeros = 0;
        while (1) {
            flux -= clock;
            if (flux >= (clock / 2.0)) {
                cells.push_back(0);
                zeros++;
            }
            else {
                break;
            }
        }

        /* flux is now the phase error, in the range [-clock/2,clock/2) */
        double margin = 1.0 - ((2.0 * fabs(flux)) / clock);
        int m = (int)((margin * 63.0) + 0.5);
        if (m < 0) m = 0;
        if (m > 63) m = 63;
        cells.push_back(CELL_FLUX | (flux > 0 ? CELL_LATE : 0) | (uint8_t)m);

        if (zeros <= 3) /* in sync, follow frequency error */
            clock += flux * p.period_adj;
        else /* out of sync, drift back to centre */
            clock += (centre - clock) * p.period_adj;

        if (clock < clock_min) clock = clock_min;
        if (clock > clock_max) clock = clock_max;

        flux = flux * (1.0 - p.phase_adj);
    }
}

/*-----------------------------------------------------------------------------------------------*/
/* MFM field decoding */


uint8_t decode_byte(const std::vector<uint8_t> &cells,size_t pos) {
    unsigned int v = 0;

    for (unsigned int b=0;b < 8;b++)
        v = (v << 1u) | (cell_bit(cells,pos + (b * 2u) + 1u) ? 1u : 0u);

    return (uint8_t)v;
}

/* Data bits near an MFM clock rule violation get little confidence, rising with the distance from
 * it (violation_floor + violation_slope * distance, out of 63) out to violation_radius bits. A violation means a
 * missing or extra transition, or that the PLL lost the clock/data alignment, and the bits around
 * it are unreliable. With little confidence there, the bits of other reads that decoded that
 * area cleanly decide the vote. The floor is kept above the confidence of a transition that is
 * truly ambiguous in timing so that CRC trial correction tries those bits first. */
static const unsigned int violation_floor = 12;
static const unsigned int violation_radius = 24;
static const unsigned int violation_slope = 2;

/* Decode nbytes starting at cell pos (the clock cell of the first bit).
 * Fills out[] with the bytes and soft[] (one per bit, MSB first) with the soft decision:
 * sign is the bit value, magnitude 0..126 is the confidence. tsoft[] is the same but from the
 * flux timing alone, without the clock rule violations, which points more precisely at the
 * bits that were misread. */
void decode_soft(const std::vector<uint8_t> &cells,size_t pos,size_t nbytes,uint8_t *out,int8_t *soft,int8_t *tsoft) {
    const size_t nbits = nbytes * 8u;
    std::vector<unsigned int> conf(nbits),tconf;

    for (size_t k=0;k < nbits;k++) {
        const size_t d = pos + (k * 2u) + 1u;
        unsigned int c = 63;

        /* a transition in this data cell might belong in a neighboring clock cell */
        if (cells[d] & CELL_FLUX)
            c = std::min(c,cell_margin(cells[d]));
        /* a late transition in the previous clock cell might belong in this data cell */
        if ((cells[d-1] & CELL_FLUX) && (cells[d-1] & CELL_LATE))
            c = std::min(c,cell_margin(cells[d-1]));
        /* an early transition in the next clock cell might belong in this data cell */
        if ((cells[d+1] & CELL_FLUX) && !(cells[d+1] & CELL_LATE))
            c = std::min(c,cell_margin(cells[d+1]));

        conf[k] = c;
    }

    tconf = conf;

    /* MFM clock rule: the clock cell is 1 only if the data bits on either side are both 0.
     * The violation is between bits k-1 and k. */
    for (size_t k=0;k < nbits;k++) {
        const size_t d = pos + (k * 2u) + 1u;
        const bool expect = !cell_bit(cells,d-2) && !cell_bit(cells,d);

        if (cell_bit(cells,d-1) != expect) {
            for (size_t dist=0;dist < violation_radius;dist++) {
                const unsigned int c = violation_floor + (violation_slope * (unsigned int)dist);

                if ((k + dist) < nbits) conf[k + dist] = std::min(conf[k + dist],c);
                if (k >= (dist + 1u)) conf[k - 1u - dist] = std::min(conf[k - 1u - dist],c);
            }
        }
    }

    for (size_t i=0;i < nbytes;i++) {
        unsigned int v = 0;

        for (unsigned int b=0;b < 8;b++) {
            const size_t k = (i * 8u) + b;
            const bool bit = cell_bit(cells,pos + (k * 2u) + 1u);

            v = (v << 1u) | (bit ? 1u : 0u);
            soft[k] = (int8_t)(bit ? (int)(conf[k] * 2u) : -(int)(conf[k] * 2u));
            tsoft[k] = (int8_t)(bit ? (int)(tconf[k] * 2u) : -(int)(tconf[k] * 2u));
        }

        out[i] = (uint8_t)v;
    }
}

/*-----------------------------------------------------------------------------------------------*/
/* combining reads */

const int confident_soft = 76;

static const int    align_max = 24;         /* max offset from the sync aligned position, in cells */
static const int    align_step = 4;         /* max change of offset from one byte to the next */
static const int    align_change_cost = 24; /* cost of changing the offset, in mismatching bits */

void mfm_vote(const std::vector<mfm_copy> &wr,size_t nbytes,mfm_consensus &v) {
    std::vector<int> &sum = v.sum,&tsum = v.tsum;
    std::vector<uint8_t> &buf = v.buf;
    std::vector<bool> &use = v.use;
    int &total_w = v.total_w;
    const size_t nbits = nbytes * 8u;
    unsigned int used = (unsigned int)wr.size();

    use.assign(wr.size(),true);
    sum.resize(nbits);
    tsum.resize(nbits);
    buf.resize(nbytes);

    for (unsigned int iter=0;iter < 2;iter++) {
        std::fill(sum.begin(),sum.end(),0);
        std::fill(tsum.begin(),tsum.end(),0);
        total_w = 0;

        for (size_t i=0;i < wr.size();i++) {
            if (!use[i]) continue;
            const int w = wr[i].weight;
            for (size_t b=0;b < nbits;b++) {
                sum[b] += w * (int)wr[i].soft[b];
                tsum[b] += w * (int)wr[i].tsoft[b];
            }
            total_w += w;
        }

        for (size_t i=0;i < nbytes;i++) {
            unsigned int v = 0;
            for (unsigned int b=0;b < 8;b++) {
                const size_t k = (i * 8u) + b;
                /* tie: go with the first read */
                bool bit = sum[k] > 0 || (sum[k] == 0 && (wr[0].data[i] & (0x80u >> b)));
                v = (v << 1u) | (bit ? 1u : 0u);
            }
            buf[i] = (uint8_t)v;
        }

        if (iter != 0 || used <= 2)
            break;

        for (size_t i=0;i < wr.size() && used > 2;i++) {
            size_t diff = 0,confident = 0;
            for (size_t k=0;k < nbits;k++) {
                if (abs(wr[i].soft[k]) < confident_soft) continue;
                confident++;
                if (((buf[k >> 3u] ^ wr[i].data[k >> 3u]) & (0x80u >> (k & 7u))) != 0) diff++;
            }
            if ((diff * 20u) > confident) {
                use[i] = false;
                used--;
            }
        }
    }
}

/* Find the cell offset per byte of a read that best matches the consensus (Viterbi).
 * The cost of a byte is the mismatching bits weighted by the confidence of the consensus, plus
 * the clock violations (which catch clock/data swapped offsets). Changing offset costs extra. */
static void align_read(const std::vector<uint8_t> &raw,const std::vector<int> &sum,int total_w,size_t nbytes,std::vector<int> &ofs) {
    const int nstates = (align_max * 2) + 1;
    const int full = std::max(1,total_w * 126);
    std::vector<int> cost(nstates),ncost(nstates);
    std::vector<int8_t> back(nbytes * (size_t)nstates);

    for (int s=0;s < nstates;s++)
        cost[s] = (s == align_max) ? 0 : align_change_cost;

    for (size_t b=0;b < nbytes;b++) {
        for (int s=0;s < nstates;s++) {
            int best = -1,bestc = 0;

            for (int p=std::max(0,s-align_step);p <= std::min(nstates-1,s+align_step);p++) {
                const int c = cost[p] + ((p != s) ? align_change_cost : 0);
                if (best < 0 || c < bestc) {
                    best = p;
                    bestc = c;
                }
            }

            const size_t src = mfm_align_pad + (b * 16u) + (size_t)(s - align_max);
            int lc = 0;

            for (unsigned int k=0;k < 8;k++) {
                const size_t d = src + (k * 2u) + 1u;
                const int sv = sum[(b * 8u) + k];
                const bool bit = cell_bit(raw,d);
                const bool expect = !cell_bit(raw,d-2) && !bit;

                if (bit != (sv > 0)) lc += 1 + ((8 * abs(sv)) / full);
                if (cell_bit(raw,d-1) != expect) lc += 4;
            }

            ncost[s] = bestc + lc;
            back[(b * (size_t)nstates) + (size_t)s] = (int8_t)best;
        }

        cost.swap(ncost);
    }

    int s = 0;
    for (int i=1;i < nstates;i++) {
        if (cost[i] < cost[s]) s = i;
    }

    ofs.resize(nbytes);
    for (size_t b=nbytes;b-- > 0;) {
        ofs[b] = s - align_max;
        s = back[(b * (size_t)nstates) + (size_t)s];
    }
}

/* decode a read using a cell offset per byte */
static void decode_aligned(const std::vector<uint8_t> &raw,const std::vector<int> &ofs,size_t nbytes,std::vector<uint8_t> &data,std::vector<int8_t> &soft,std::vector<int8_t> &tsoft) {
    std::vector<uint8_t> cells;
    size_t src = (size_t)((int)mfm_align_pad + ofs[0]) - 16u;

    /* previous byte (context for the clock rule), the bytes, then one more cell */
    cells.reserve((nbytes + 1u) * 16u + 1u);
    cells.insert(cells.end(),raw.begin()+src,raw.begin()+src+16u);
    for (size_t b=0;b < nbytes;b++) {
        src = (size_t)((int)mfm_align_pad + ofs[b]) + (b * 16u);
        cells.insert(cells.end(),raw.begin()+src,raw.begin()+src+16u);
    }
    cells.push_back(raw[src+16u]);

    data.resize(nbytes);
    soft.resize(nbytes * 8u);
    tsoft.resize(nbytes * 8u);
    decode_soft(cells,16,nbytes,&data[0],&soft[0],&tsoft[0]);
}


bool mfm_realign(std::vector<mfm_copy> &c,size_t nbytes,const mfm_consensus &v) {
    bool changed = false;

    for (size_t i=0;i < c.size();i++) {
        std::vector<int> ofs;

        if (c[i].raw == NULL || c[i].raw->empty()) continue;

        align_read(*c[i].raw,v.sum,v.total_w,nbytes,ofs);
        if (c[i].ofs.size() != nbytes) c[i].ofs.assign(nbytes,0);
        if (ofs != c[i].ofs) {
            c[i].ofs = ofs;
            decode_aligned(*c[i].raw,ofs,nbytes,c[i].data,c[i].soft,c[i].tsoft);
            changed = true;
        }
    }

    return changed;
}
