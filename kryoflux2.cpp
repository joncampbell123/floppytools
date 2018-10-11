
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <map>
#include <vector>

#include "kryocomm.h"

static unsigned long histogram[256];

int main(int argc,char **argv) {
    if (argc < 2) {
        fprintf(stderr,"%s <raw>\n",argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1],"rb");
    if (fp == NULL) return 1;

    struct kryoflux_event ev;

    for (size_t i=0;i < 256;i++) histogram[i] = 0;

    while (kryoflux_read(ev,fp)) {
        if (ev.message == MSG_FLUX) {
            unsigned long ofs = ev.flux_interval;
            if (ofs > 255ul) ofs = 255ul;
            histogram[ofs]++;
        }
    }

    printf("Histogram:\n");
    for (size_t i=0;i < 256;i++) {
        if (histogram[i] != 0ul)
            printf("%zu: %lu\n",i,histogram[i]);
    }

    fclose(fp);
    return 0;
}

