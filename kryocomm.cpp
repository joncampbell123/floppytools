
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"

bool kryoflux_read(struct kryoflux_event &ev,FILE *fp) {
    int c;

    ev.msg.clear();
    ev.oob_code = 0;
    ev.message = MSG_FLUX;
    ev.flux_interval = 0;
    ev.offset = ftell(fp);

    do {
        if ((c = fgetc(fp)) < 0)
            return false;

        if (c <= 0x07) {
            ev.flux_interval += ((unsigned char)c << 8u);

            if ((c = fgetc(fp)) < 0)
                return false;
            ev.flux_interval +=  (unsigned char)c;

            return true;
        }
        else if (c == 0x08) {
            // ignore
        }
        else if (c == 0x09) {
            if ((c = fgetc(fp)) < 0)
                return false;

            // ignore
        }
        else if (c == 0x0A) {
            if ((c = fgetc(fp)) < 0)
                return false;
            if ((c = fgetc(fp)) < 0)
                return false;

            // ignore
        }
        else if (c == 0x0B) {
            // overflow16
            ev.flux_interval += 0x10000u;
        }
        else if (c == 0x0C) {
            // value16
            if ((c = fgetc(fp)) < 0)
                return false;
            ev.flux_interval += ((unsigned char)c << 8u);

            if ((c = fgetc(fp)) < 0)
                return false;
            ev.flux_interval +=  (unsigned char)c;

            return true;
        }
        else if (c == 0x0D) {
            unsigned long oob_len;

            // OOB
            if ((c = fgetc(fp)) < 0)
                return false;
            unsigned char type = (unsigned char)c;

            if ((c = fgetc(fp)) < 0)
                return false;
            oob_len  = (unsigned char)c;

            if ((c = fgetc(fp)) < 0)
                return false;
            oob_len += (unsigned char)c << 8u;

            ev.msg.clear();
            ev.msg.resize(oob_len);
            for (unsigned long i=0;i < oob_len;i++) {
                if ((c = fgetc(fp)) < 0)
                    return false;

                ev.msg[i] = (unsigned char)c;
            }

            ev.message = MSG_OOB;
            ev.oob_code = type;

            return true;
        }
        else {
            ev.flux_interval +=  (unsigned char)c;

            return true;
        }
    } while (1);

    return false;
}

bool kryoflux_bits_refill(flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    while (fb.avail() <= 24) {
        if (!kryoflux_read(ev,fp))
            return false;

        if (ev.message == MSG_FLUX) {
            long len;

            len = ((((long)ev.flux_interval - (long)fb.shortest) + (long)(fb.dist / 2l)) / (long)fb.dist) + 1l;
            if (len < 1) len = 1;
            if (len > 8) len = 8;

            fb.add((unsigned int)len);
        }
    }

    return true;
}

void flux_bits::clear(void) {
    bits = 0;
    left = 0;
}

void flux_bits::add(unsigned int len) {
    if (len != 0) {
        left  += len;
        if (left > (sizeof(unsigned int) * 8u)) fprintf(stderr,"flux_bits::add() overrun\n");
        bits <<= len;
        bits  += 1u << (len - 1u);
    }
}

unsigned int flux_bits::avail(void) const {
    return left;
}

unsigned int flux_bits::peek(unsigned int bc) const {
    if (left >= bc)
        return (bits >> (left-bc)) & ((1u << bc) - 1u);

    fprintf(stderr,"flux_bits::peek() overrun\n");
    return 0;
}

unsigned int flux_bits::get(unsigned int bc) {
    if (left >= bc) {
        unsigned int r = peek(bc);
        left -= bc;
        return r;
    }
    else {
        fprintf(stderr,"flux_bits::get() overrun\n");
    }

    return 0;
}

bool autodetect_flux_bits_mfm(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    fseek(fp,0,SEEK_SET);
    fb.clear();

    unsigned long hist[256];
    for (unsigned long i=0;i < 256;i++) hist[i] = 0;

    while (kryoflux_read(ev,fp)) {
        if (ev.message == MSG_FLUX) {
            unsigned long ofs = ev.flux_interval;
            if (ofs < 256ul) hist[ofs]++;
        }
    }

    fseek(fp,0,SEEK_SET);

    unsigned long min = ~0UL,max = 0;
    for (unsigned int i=0;i < 256;i++) {
        if (hist[i] != 0ul) {
            if (min > hist[i]) min = hist[i];
            if (max < hist[i]) max = hist[i];
        }
    }

    if (min == ~0UL)
        return false;

    if (max < (min * 4ul))
        return false;

    unsigned long cutoff = min + ((max - min) / 100ul);

    /* look for the first peak, which represents 1010101010101010101010 */
    unsigned int scan = 0;

    unsigned long peak_max[3];
    unsigned int peak_idx[3];

    fprintf(stderr,"autoscan: min=%lu max=%lu cutoff=%lu\n",min,max,cutoff);

    for (unsigned int peak=0;peak < 3;peak++) {
        peak_max[peak] = 0;
        peak_idx[peak] = 0;
        while (scan < 256 && hist[scan] < cutoff) scan++;
        while (scan < 256 && hist[scan] >= cutoff) {
            if (peak_max[peak] < hist[scan]) {
                peak_max[peak] = hist[scan];
                peak_idx[peak] = scan;
            }
            scan++;
        }
    }

    printf("autoscan: peaks at %u, %u, %u [%lu, %lu, %lu]\n",
        peak_idx[0],peak_idx[1],peak_idx[2],
        peak_max[0],peak_max[1],peak_max[2]);

    /* we require two peaks at least */
    if (peak_idx[0] == 0 || peak_idx[1] == 0)
        return false;

    fb.dist = peak_idx[1] - peak_idx[0];
    if (fb.dist < 8)
        return false;

    if (peak_idx[2] != 0) {
        unsigned long dist2 = peak_idx[2] - peak_idx[1];

        printf("autoscan: dist=%lu dist2=%lu\n",fb.dist,dist2);
        if (labs((signed long)dist2 - (signed long)fb.dist) < (fb.dist / 4ul)) {
            printf("autoscan: using peaks 1-2 to average dist (diff=%ld)\n",(signed long)dist2 - (signed long)fb.dist);
            fb.dist = (fb.dist + dist2) / 2ul;
        }
    }

    fb.shortest = peak_idx[0];
    if (fb.shortest <= fb.dist)
        return false;

    /* peak[0] represents 101010101010101010 so to get the shortest (1111111)
     * subtract dist from it */
    fb.shortest -= fb.dist;

    printf("autoscan: final params shortest=%lu dist=%lu\n",fb.shortest,fb.dist);
    return true;
}

static int flux_bits_pbit = 0;

int flux_bits_mfm_decode_bit(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    unsigned int raw,bit;

    if (fb.avail() < 2) {
        if (!kryoflux_bits_refill(fb,ev,fp))
            return -1;
    }

    raw = fb.peek(2);
    if (raw == 1/*01*/) {
        bit = 1;
    }
    else if (raw == 3/*11*/) {
        fb.get(1);
        return -1;
    }
    else {
        /* 10 if previous bit 0
         * 00 if previous bit 1 */
        raw ^= (1u ^ flux_bits_pbit) << 1u;
        if (raw != 0) {
            fb.get(1);
            return -1;
        }
        bit = 0;
    }

    fb.get(2);
    flux_bits_pbit = bit;
    return bit;
}

int flux_bits_mfm_decode(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    unsigned int byte = 0,i;
    int c;

    if (fb.avail() < 16) {
        if (!kryoflux_bits_refill(fb,ev,fp))
            return -1;
    }

    if (fb.peek(MFM_A1_SYNC_LENGTH) == MFM_A1_SYNC) {
        fb.get(MFM_A1_SYNC_LENGTH);
        flux_bits_pbit = 1; /* last bit of 0xA1 is 1 */
        return -MFM_A1_SYNC_BYTE;
    }

    for (i=0;i < 8;i++) {
        c = flux_bits_mfm_decode_bit(fb,ev,fp);
        if (c < 0 || c >= 2) return -1;
        byte |= (unsigned int)c << (7u - i);
    }

    return byte;
}

/**
 * Static table used for the table_driven implementation.
 */
static const mfm_crc16fd_t mfm_crc16fd_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
};

mfm_crc16fd_t mfm_crc16fd_update(mfm_crc16fd_t crc, const void *data, size_t data_len)
{
    const unsigned char *d = (const unsigned char *)data;
    unsigned int tbl_idx;

    while (data_len--) {
        tbl_idx = ((crc >> 8) ^ *d) & 0xff;
        crc = (mfm_crc16fd_table[tbl_idx] ^ (crc << 8)) & 0xffff;
        d++;
    }
    return crc & 0xffff;
}
/*-----*/

kryoflux_stream_info::kryoflux_stream_info() {
    clear();
}

void kryoflux_stream_info::clear() {
    name.clear();
    sck = 24027428.57142857;
    ick = 3003428.571428571;
}

void kryoflux_parse_stream_nv_pair(std::string &name,std::string &value,std::vector<unsigned char>::iterator &mi,const std::vector<unsigned char>::iterator mend) {
    name.clear();
    value.clear();

    /* leading space */
    while (mi != mend && *mi == ' ') mi++;

    /* name */
    while (mi != mend) {
        if (*mi == 0) return; /* end of string, return now */
        if (*mi == '=') {
            mi++; /* skip it and break to next loop */
            break;
        }
        if (*mi == ',') {
            mi++; /* skip it and return */
            return;
        }

        name += *mi;
        mi++;
    }

    /* value */
    while (mi != mend) {
        if (*mi == 0) return; /* end of string, return now */
        if (*mi == ',') {
            mi++; /* skip it and return */
            return;
        }

        value += *mi;
        mi++;
    }
}

bool kryoflux_update_stream_info(struct kryoflux_stream_info &si,std::vector<unsigned char> &msg) {
    std::string name,value;

    std::vector<unsigned char>::iterator mi = msg.begin();
    while (mi != msg.end() && *mi != 0) {
        kryoflux_parse_stream_nv_pair(name,value,mi,msg.end());

        if (name == "name")
            si.name = value;
        else if (name == "sck")
            si.sck = atof(value.c_str());
        else if (name == "ick")
            si.ick = atof(value.c_str());
    }

    return true;
}

FILE *kryo_fopen(const std::string &cappath,unsigned int track,unsigned int head) {
    std::string path;
    char tmp[128];

    sprintf(tmp,"track%02u.%u.raw",track,head);
    path = cappath + "/" + tmp;

    return fopen(path.c_str(),"rb");
}

bool mfm_find_sync(flux_bits &fb,struct kryoflux_event ev,FILE *fp) {
    do {
        kryoflux_bits_refill(fb,ev,fp);

        while (fb.avail() >= MFM_A1_SYNC_LENGTH) {
            if (fb.peek(MFM_A1_SYNC_LENGTH) == MFM_A1_SYNC) {
                return true;
            }
            else {
                fb.get(1);
            }
        }

        if (!kryoflux_bits_refill(fb,ev,fp))
            break;
    } while (fb.avail() > 0);

    return false;
}

void kryo_save_state(struct kryo_savestate &st,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    st.last_event_offset = ev.offset;
    st.fb = fb;
}

void kryo_restore_state(const struct kryo_savestate &st,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    if (fp != NULL) fseek(fp,st.last_event_offset,SEEK_SET);
    fb = st.fb;
}

kryo_savestate::kryo_savestate() {
    clear();
}

void kryo_savestate::clear(void) {
    fb.clear();
    last_event_offset = 0;
}

// at call:
// fb.peek() == A1 sync
// returns:
// number of sync codes
int flux_bits_mfm_skip_sync(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    int count=0,c;

    kryoflux_bits_refill(fb,ev,fp);
    while (fb.avail() >= MFM_A1_SYNC_LENGTH && fb.peek(MFM_A1_SYNC_LENGTH) == MFM_A1_SYNC) {
        c=flux_bits_mfm_decode(fb,ev,fp);
        if (c != -MFM_A1_SYNC_BYTE) {
            if (c < 0) break;
        }

        kryoflux_bits_refill(fb,ev,fp);
        count++;
    }

    return count;
}

// at call:
// fb.peek() == A1 sync
//
// Reads A1 A1 A1 <byte>
//
// Where byte is normally 0xFE (sector ID) or 0xFA/0xFB (sector data)
//
// The expectation is that you will read the rest of the data following it, into an unsigned
// char array, where the first four bytes are 0xA1 0xA1 0xA1 <byte> and fill the other bytes
// in to it so the checksum can be done.
int flux_bits_mfm_read_sync_and_byte(struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp) {
    int c;

    // Require at least 3 sync codes, allow more to occur
    if ((c=flux_bits_mfm_skip_sync(fb,ev,fp)) < 3)
        return -MFM_READ_SYNC_ERROR;

    return flux_bits_mfm_decode(fb,ev,fp);
}

mfm_sector_id::mfm_sector_id() {
    clear();
}

void mfm_sector_id::clear(void) {
    track = side = sector = sector_size_code = 0;
    crc_ok = false;
}

unsigned int mfm_sector_id::sector_size(void) const {
    if (sector_size_code < 8)
        return 128u << sector_size_code;

    return 0;
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
    if (track < 0)
        return -MFM_READ_DECODE_ERROR;
    tmp[4] = sid.track = (unsigned char)track;

    int side = flux_bits_mfm_decode(fb,ev,fp);
    if (side < 0)
        return -MFM_READ_DECODE_ERROR;
    tmp[5] = sid.side = (unsigned char)side;

    int sector = flux_bits_mfm_decode(fb,ev,fp);
    if (sector < 0)
        return -MFM_READ_DECODE_ERROR;
    tmp[6] = sid.sector = (unsigned char)sector;

    int ssize = flux_bits_mfm_decode(fb,ev,fp);
    if (ssize < 0)
        return -MFM_READ_DECODE_ERROR;
    tmp[7] = sid.sector_size_code = (unsigned char)ssize;

    unsigned int crc;
    if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0)
        return -MFM_READ_DECODE_ERROR;
    crc  = (unsigned int)c << 8;

    if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0)
        return -MFM_READ_DECODE_ERROR;
    crc += (unsigned int)c;

    check = mfm_crc16fd_update(0xffff,tmp,8);
    if (check != crc)
        return -MFM_CRC_ERROR;

    sid.crc_ok = true;
    return 0;
}

// at call:
// caller just read A1 A1 A1 <byte> where <byte> == 0xFA or 0xFB.
// The next N bytes are the sector contents.
int flux_bits_mfm_read_sector_data(unsigned char *buf,unsigned int sector_size,struct flux_bits &fb,struct kryoflux_event &ev,FILE *fp,int c) {
    mfm_crc16fd_t check;
    unsigned char tmp[4];

    // A1 A1 A1 FA/FB
    tmp[0] = (unsigned char)MFM_A1_SYNC_BYTE;
    tmp[1] = (unsigned char)MFM_A1_SYNC_BYTE;
    tmp[2] = (unsigned char)MFM_A1_SYNC_BYTE;
    tmp[3] = (unsigned char)c;

    /* begin checksum */
    check = mfm_crc16fd_update(0xffff,tmp,4);

    // sector data follows
    for (unsigned int b=0;b < sector_size;b++) {
        kryoflux_bits_refill(fb,ev,fp);

        if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0)
            return -MFM_READ_DECODE_ERROR;

        buf[b] = (unsigned char)c;
    }
    check = mfm_crc16fd_update(check,buf,sector_size);

    // followed by checksum
    for (unsigned int b=0;b < 2;b++) {
        kryoflux_bits_refill(fb,ev,fp);

        if ((c=flux_bits_mfm_decode(fb,ev,fp)) < 0)
            return -MFM_READ_DECODE_ERROR;

        tmp[b] = (unsigned char)c;
    }

    unsigned int crc;
    crc  = (unsigned int)tmp[0u] << 8u;
    crc += (unsigned int)tmp[1u];

    if (check != crc)
        return -MFM_CRC_ERROR;

    return 0;
}

