
#include <vector>
#include <string>
#include <stdlib.h>
#include <stdint.h>

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

struct kryoflux_stream_info {
    std::string         name;
    double              sck;
    double              ick;

    kryoflux_stream_info();
    void clear();
};

bool kryoflux_update_stream_info(struct kryoflux_stream_info &si,std::vector<unsigned char> &msg);
void kryoflux_parse_stream_nv_pair(std::string &name,std::string &value,std::vector<unsigned char>::iterator &mi,const std::vector<unsigned char>::iterator mend);

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

int flux_bits_mfm_decode_bit(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp);
int flux_bits_mfm_decode(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp);

#define MFM_A1_SYNC         0x4489
#define MFM_A1_SYNC_BYTE    0xA1
#define MFM_A1_SYNC_LENGTH  16

/*-----*/
/**
 * \file
 * Functions and types for CRC checks.
 *
 * Generated on Thu Oct 11 13:49:52 2018
 * by pycrc v0.9.1, https://pycrc.org
 * using the configuration:
 *  - Width         = 16
 *  - Poly          = 0x1021
 *  - XorIn         = 0x1d0f
 *  - ReflectIn     = False
 *  - XorOut        = 0x0000
 *  - ReflectOut    = False
 *  - Algorithm     = table-driven
 */

typedef uint16_t mfm_crc16fd_t;

mfm_crc16fd_t mfm_crc16fd_update(mfm_crc16fd_t crc, const void *data, size_t data_len);

FILE *kryo_fopen(const std::string &cappath,unsigned int track,unsigned int head);

