
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

