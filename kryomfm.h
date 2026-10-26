
#ifndef KRYOMFM_H
#define KRYOMFM_H

#include <stdint.h>
#include <stddef.h>

#include <string>
#include <vector>

// Common MFM decoding for KryoFlux raw stream captures (IBM PC, Amiga):
// flux loading, cell period estimation, PLL decoding to a cell stream with timing confidence,
// soft decision field decoding, and combining several reads of the same field with bit slip
// realignment.

struct flux_track {
    std::vector<uint32_t>   flux;       /* flux intervals in sample clock ticks */
    double                  sck;        /* sample clock rate */
    double                  cell;       /* estimated MFM cell period in sample ticks, 0 if unknown */

    flux_track() : sck(0), cell(0) { }
};

/* Estimate the cell period from the histogram of flux intervals, which are kmin..kmax cells
 * long (MFM 2..4, GCR 1..3). Returns 0 if the track does not look like that encoding. */
double estimate_cell_period(const std::vector<uint32_t> &flux,unsigned int kmin=2,unsigned int kmax=4);
bool load_flux_track(flux_track &ft,const std::string &cappath,unsigned int track,unsigned int head);
bool track_file_exists(const std::string &cappath,unsigned int track,unsigned int head);

struct pll_params {
    double      clock_scale;    /* multiplier applied to the estimated cell period */
    double      period_adj;     /* how fast the PLL follows frequency error */
    double      phase_adj;      /* how fast the PLL follows phase error */
    double      max_adj;        /* max deviation of the clock from centre, as a fraction */
};

/* first entry is the normal decode, the rest are retries for tracks with missing sectors */
extern const pll_params pll_presets[];
extern const unsigned int pll_preset_count;

/* cell stream encoding: 0 = no flux transition, else CELL_FLUX | timing info */
#define CELL_FLUX       0x80
#define CELL_LATE       0x40    /* transition arrived late, alternative position is the next cell. else the previous cell */
#define CELL_MARGIN     0x3F    /* 63 = dead centre of the cell, 0 = exactly between two cells */

void pll_decode(std::vector<uint8_t> &cells,const flux_track &ft,const pll_params &p);

static inline bool cell_bit(const std::vector<uint8_t> &cells,size_t i) {
    return (cells[i] & CELL_FLUX) != 0;
}

static inline unsigned int cell_margin(uint8_t c) {
    return c & CELL_MARGIN;
}

/* decode the byte whose first (clock) cell is at pos. 16 cells per byte, data in the odd cells */
uint8_t decode_byte(const std::vector<uint8_t> &cells,size_t pos);

/* Decode nbytes starting at cell pos with soft decisions per bit (MSB first): sign is the bit
 * value, magnitude 0..126 the confidence. soft[] includes MFM clock violations, tsoft[] is from
 * the flux timing alone. Needs cells pos-1 .. pos+nbytes*16. */
void decode_soft(const std::vector<uint8_t> &cells,size_t pos,size_t nbytes,uint8_t *out,int8_t *soft,int8_t *tsoft);

/* soft value at or above which a bit is considered decoded cleanly (out of 126) */
extern const int confident_soft;

/* Bit slip realignment. A damaged spot can make the PLL gain or lose cells, after which the rest
 * of the read is shifted. Shifts by an odd number of cells swap clock and data and show up as
 * clock violations, but shifts by an even number (whole bits) decode cleanly to wrong data.
 * Reads are realigned against the consensus of all reads with a per byte cell offset. For that
 * a read keeps the cells of its field plus align_pad cells before and after. */
static const size_t mfm_align_pad = 48;

/* one read of a field, for combining */
struct mfm_copy {
    const std::vector<uint8_t>  *raw;       /* cells from field start - mfm_align_pad, NULL if not kept */
    int                         weight;
    std::vector<uint8_t>        data;
    std::vector<int8_t>         soft,tsoft;
    std::vector<int>            ofs;        /* cell offset per byte (realignment) */

    mfm_copy() : raw(NULL), weight(1) { }
};

struct mfm_consensus {
    std::vector<int>            sum,tsum;   /* weighted soft sums per bit */
    std::vector<uint8_t>        buf;        /* hard decision */
    std::vector<bool>           use;        /* copies that took part (outliers dropped) */
    int                         total_w;
};

/* Soft decision combining. Damaged areas of a read carry little confidence so each area is
 * decided by the reads that decoded it cleanly. Reads whose confident bits disagree a lot are
 * dropped as long as enough remain. */
void mfm_vote(const std::vector<mfm_copy> &c,size_t nbytes,mfm_consensus &v);

/* Realign the copies that have raw cells against the consensus. Returns true if any changed. */
bool mfm_realign(std::vector<mfm_copy> &c,size_t nbytes,const mfm_consensus &v);

#endif
