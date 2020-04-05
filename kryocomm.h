
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

struct kryo_savestate {
    off_t               last_event_offset;
    struct flux_bits    fb;

                        kryo_savestate();
    void                clear(void);
};

struct mfm_sector_id { // 0xFE type packets
    uint8_t         track;
    uint8_t         side;
    uint8_t         sector;
    uint8_t         sector_size_code;       // bytes = 128 << sector_size_code
    bool            crc_ok;

    mfm_sector_id();
    void clear(void);
    unsigned int sector_size(void) const;
};

void kryo_save_state(struct kryo_savestate &st,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp);
void kryo_restore_state(const struct kryo_savestate &st,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp);

bool kryoflux_update_stream_info(struct kryoflux_stream_info &si,std::vector<unsigned char> &msg);
void kryoflux_parse_stream_nv_pair(std::string &name,std::string &value,std::vector<unsigned char>::iterator &mi,const std::vector<unsigned char>::iterator mend);

bool kryoflux_bits_refill(flux_bits &fb,struct kryoflux_event &ev,FILE *fp);
bool autodetect_flux_bits_mfm(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp);

int flux_bits_mfm_decode_bit(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp);
int flux_bits_mfm_decode(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp);
int flux_bits_mfm_skip_sync(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp);
int flux_bits_mfm_read_sync_and_byte(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp);
int flux_bits_mfm_read_sector_id(mfm_sector_id &sid,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp);
int flux_bits_mfm_read_sector_data(unsigned char *buf,unsigned int sector_size,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp,int c);

#define MFM_A1_SYNC         0x4489
#define MFM_A1_SYNC_BYTE    0xA1
#define MFM_A1_SYNC_LENGTH  16

#define MFM_CRC_ERROR               0x100
#define MFM_READ_DECODE_ERROR       0x101

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
bool mfm_find_sync(flux_bits &fb,struct kryoflux_event ev,FILE *fp);

FILE *kryo_fopen(const std::string &cappath,unsigned int track,unsigned int head);

