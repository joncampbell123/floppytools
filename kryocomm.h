
#include <vector>

enum {
    MSG_NONE,
    MSG_FLUX,
    MSG_OOB
};

struct kryoflux_event {
    unsigned int                flux_interval;
    unsigned char               message;
    unsigned char               oob_code;
    unsigned long               offset;
    std::vector<unsigned char>  msg;
};

bool kryoflux_read(struct kryoflux_event &ev,FILE *fp);

struct flux_bits {
    unsigned int        bits;
    unsigned int        left;

    unsigned int        shortest;
    unsigned int        dist;

    void                clear(void);
    void                add(unsigned int len);
    unsigned int        avail(void) const;
    unsigned int        peek(unsigned int bc) const;
    unsigned int        get(unsigned int bc);
};

bool kryoflux_bits_refill(flux_bits &fb,struct kryoflux_event &ev,FILE *fp);
bool autodetect_flux_bits_mfm(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp);

#define MFM_A1_SYNC         0x4489
#define MFM_A1_SYNC_LENGTH  16

