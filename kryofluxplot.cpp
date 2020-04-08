
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

    FILE *csv_fp = fopen("graph.csv","w");
    if (csv_fp == NULL) return 1;

    struct kryoflux_event ev;
    unsigned long count = 0;

    fprintf(csv_fp,"# index, flux\n");
    while (kryoflux_read(ev,fp)) {
        if (ev.message == MSG_FLUX) {
            fprintf(csv_fp,"%lu, %lu\n",count,ev.flux_interval);
            count++;
        }
    }

    FILE *graph_fp = fopen("graph.gnuplot","w");
    if (graph_fp == NULL) return 1;

    fprintf(graph_fp,"reset\n");
    fprintf(graph_fp,"set term png size 1024,256\n");
    fprintf(graph_fp,"set output 'graph.png'\n");
    fprintf(graph_fp,"set grid\n");
    fprintf(graph_fp,"set autoscale\n");
    fprintf(graph_fp,"set title 'Flux reversals'\n");
    fprintf(graph_fp,"set xlabel 'Sample'\n");
    fprintf(graph_fp,"set ylabel 'Flux reversal distance'\n");
    fprintf(graph_fp,"plot 'graph.csv' using 1:2 with points pointtype 3 pointsize 0.1 title 'Distance'\n");

    fclose(graph_fp);
    fclose(csv_fp);
    fclose(fp);
    return 0;
}

