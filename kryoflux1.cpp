
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"

int main(int argc,char **argv) {
    if (argc < 2) {
        fprintf(stderr,"%s <raw>\n",argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1],"rb");
    if (fp == NULL) return 1;

    struct kryoflux_event ev;

    while (kryoflux_read(ev,fp)) {
        fprintf(stderr,"flux=%lu(0x%lx) oob=%u ooblen=%zu offset=%lu\n",
            ev.flux_interval,ev.flux_interval,ev.oob_code,ev.msg.size(),ev.offset);

        if (ev.msg.size() != 0 && ev.oob_code == 4) {
            fprintf(stderr,"  oobmsg='");
            fwrite(&ev.msg[0],ev.msg.size(),1,stderr);
            fprintf(stderr,"'\n");
        }
    }

    fclose(fp);
    return 0;
}

