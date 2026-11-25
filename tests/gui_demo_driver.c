#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* Stand-alone GUI demo: parent simulates supervisor and writes real EventMsg. */
static int emit(int fd,int type,int stage,int status,unsigned lot,
                double a,double b,double c,const char *msg) {
    EventMsg e=make_event(type,stage,status,lot,a,b,c,msg);
    return send_event(fd,&e);
}
int main(void) {
    int p[2],status=0;pid_t child;Config cfg;
    config_set_defaults(&cfg);
    cfg.gui_auto_close_ms=2000;
    if(pipe(p)<0){perror("pipe");return 1;}
    child=fork();
    if(child<0){perror("fork");return 1;}
    if(child==0){
        StageIO io={.in_fd=p[0],.out_fd=-1,.telemetry_fd=-1,.event_fd=-1,.peer_pid=-1};
        close(p[1]);gui_main(&cfg,&io);_exit(0);
    }
    close(p[0]);
    /* The real GUI sends SIGUSR1 and SIGUSR2 to its supervisor (the parent). */
    struct sigaction sa={.sa_handler=SIG_IGN};
    sigaction(SIGUSR1,&sa,NULL);sigaction(SIGUSR2,&sa,NULL);
    sigaction(SIGPIPE,&sa,NULL);
    emit(p[1],EV_CONTAINER,-1,0,0,350,500,0,NULL);
    emit(p[1],EV_BELT,-1,0,0,1,0,0,NULL);
    for(int round=0;round<4;round++) {
        for(int s=0;s<NUM_STAGES;s++) {
            emit(p[1],EV_STAGE_STATUS,s,ST_RUNNING,(unsigned)(round+1),0,0,0,NULL);
            emit(p[1],EV_TELEMETRY,s,0,(unsigned)(round+1),
                 s==STAGE_CRUSHING&&round==2?42:25,
                 s==STAGE_PRESSING?51:1.1,500+s*70,NULL);
            if(s==STAGE_CRUSHING&&round==2)
                emit(p[1],EV_ALERT_CRITICAL,s,0,(unsigned)(round+1),42,10,35,"temperature");
        }
        emit(p[1],EV_CONTAINER,-1,0,0,350-round*90,500,0,NULL);
        emit(p[1],EV_LOT_FINISHED,STAGE_PRESSING,0,(unsigned)(round+1),
             13.2+round,38+round,48+round,NULL);
        emit(p[1],EV_INFO,-1,0,0,0,0,0,"Demo: processing lots");
        sleep_ms_scaled(350,1.0,NULL);
    }
    for(int s=0;s<NUM_STAGES;s++)emit(p[1],EV_STAGE_DONE,s,ST_DONE,0,0,0,0,NULL);
    emit(p[1],EV_BELT,-1,0,0,0,0,0,NULL);
    emit(p[1],EV_SHUTDOWN,-1,0,0,58.8,158,198,"complete");
    close(p[1]);
    if(waitpid(child,&status,0)<0){perror("waitpid");return 1;}
    printf("GUI demo exited: %s\n",WIFEXITED(status)?"normally":"abnormally");
    return WIFEXITED(status)&&WEXITSTATUS(status)==0?0:1;
}
