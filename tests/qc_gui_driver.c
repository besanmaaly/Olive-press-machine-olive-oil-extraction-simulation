#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int send_telemetry(int fd, int stage, uint32_t id, double temp,
                          double pressure, double ms)
{
    EventMsg e = make_event(EV_TELEMETRY, stage, 0, id,
                            temp, pressure, ms, NULL);
    return write_full(fd, &e, sizeof e);
}

int main(int argc, char **argv)
{
    int tel[2], evt[2], failures = 0;
    pid_t pid;
    Config cfg;
    const struct {
        double temp, pressure, ms;
        int warnings, criticals;
    } cases[] = {
        {25,1.0,500,0,0}, {36,1.0,500,0,1},
        {9.9,1.0,500,0,1}, {33,1.0,500,1,0},
        {32.5,1.0,500,0,0}, {25,2.5,500,0,1},
        {25,1.95,500,1,0}, {25,1.0,2500,1,0},
        {40,3.0,5000,1,2}
    };
    const int ncases = (int)(sizeof cases / sizeof cases[0]);
    int got_warn[9]={0},got_crit[9]={0},got_tel[9]={0};
    if (argc != 2 || strcmp(argv[1],"qc") != 0) {
        fprintf(stderr,"Usage: %s qc\n",argv[0]);
        return 2;
    }
    config_set_defaults(&cfg);
    cfg.time_scale = 5;
    if (pipe(tel) || pipe(evt)) { perror("pipe"); return 1; }
    pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        StageIO io = {.in_fd=tel[0],.out_fd=-1,.telemetry_fd=-1,
                      .event_fd=evt[1],.peer_pid=-1};
        close(tel[1]); close(evt[0]);
        quality_control_main(&cfg,&io);
        _exit(0);
    }
    close(tel[0]); close(evt[1]);
    for (int i=0;i<ncases;i++) {
        if (send_telemetry(tel[1],STAGE_CRUSHING,(uint32_t)i+1,
                           cases[i].temp,cases[i].pressure,cases[i].ms) < 0) {
            perror("send telemetry"); return 1;
        }
    }
    close(tel[1]);
    for (;;) {
        EventMsg e;
        int r=read_msg(evt[0],&e,sizeof e);
        if (r == 0) break;
        if (r == -2) continue;
        if (r < 0) { perror("read event"); failures++; break; }
        if (e.lot_id>=1 && e.lot_id<=(unsigned)ncases) {
            int i=(int)e.lot_id-1;
            if (e.type==EV_TELEMETRY) got_tel[i]++;
            if (e.type==EV_ALERT_WARNING) got_warn[i]++;
            if (e.type==EV_ALERT_CRITICAL) got_crit[i]++;
        }
    }
    close(evt[0]);
    waitpid(pid,NULL,0);
    for (int i=0;i<ncases;i++) {
        int pass=got_tel[i]==1 && got_warn[i]==cases[i].warnings &&
                 got_crit[i]==cases[i].criticals;
        printf("QC case %d: %s (telemetry=%d warnings=%d criticals=%d)\n",
               i+1, pass?"PASS":"FAIL",got_tel[i],got_warn[i],got_crit[i]);
        if (!pass) failures++;
    }
    printf("QC RESULT: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
