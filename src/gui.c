#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <GL/freeglut.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Only the GUI child touches this state. The supervisor owns all process control. */
typedef struct {
    int status, lots, alert;
    double temp, pressure, ms, alert_until;
} StageView;
typedef struct {
    Config cfg;
    int fd, belt, paused, finished, eof, line_head, line_count;
    double belt_phase, t_start, finish_at, container, capacity;
    double oil, water, pomace;
    char result[48], lines[6][120];
    int line_level[6];
    StageView stages[NUM_STAGES];
} View;
static View view;

static void color(float r, float g, float b) { glColor3f(r,g,b); }
static void rect(float x,float y,float w,float h) {
    glBegin(GL_QUADS); glVertex2f(x,y); glVertex2f(x+w,y);
    glVertex2f(x+w,y+h); glVertex2f(x,y+h); glEnd();
}
static void outline(float x,float y,float w,float h) {
    glBegin(GL_LINE_LOOP); glVertex2f(x,y); glVertex2f(x+w,y);
    glVertex2f(x+w,y+h); glVertex2f(x,y+h); glEnd();
}
static void circle(float x,float y,float r) {
    glBegin(GL_TRIANGLE_FAN); glVertex2f(x,y);
    for(int i=0;i<=32;i++) { float a=(float)(i*6.283185307179586/32.0);
        glVertex2f(x+r*cosf(a),y+r*sinf(a)); }
    glEnd();
}
static void text(float x,float y,const char *fmt,...) {
    char buf[200]; va_list ap; va_start(ap,fmt);
    vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    glRasterPos2f(x,y);
    for(const unsigned char *p=(const unsigned char *)buf;*p;p++)
        glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12,*p);
}
static void log_line(int level,const char *fmt,...) {
    char line[120]; va_list ap; va_start(ap,fmt);
    vsnprintf(line,sizeof line,fmt,ap); va_end(ap);
    snprintf(view.lines[view.line_head],sizeof view.lines[0],"%s",line);
    view.line_level[view.line_head]=level;
    view.line_head=(view.line_head+1)%6;
    if(view.line_count<6)view.line_count++;
}
static const char *status_name(int s) {
    switch(s) { case ST_RUNNING:return "RUNNING"; case ST_STOPPED:return "STOPPED";
    case ST_PAUSED:return "PAUSED"; case ST_ERROR:return "ERROR";
    case ST_DONE:return "DONE"; default:return "IDLE"; }
}
static void stage_color(int status) {
    switch(status) {
    case ST_RUNNING:color(.20f,.56f,.39f);break;
    case ST_STOPPED:color(.81f,.47f,.18f);break;
    case ST_PAUSED:color(.75f,.65f,.24f);break;
    case ST_ERROR:color(.77f,.23f,.25f);break;
    case ST_DONE:color(.33f,.39f,.44f);break;
    default:color(.28f,.41f,.52f);break;
    }
}
/* This is the sole location where external messages change GUI model state. */
static void apply_event(const EventMsg *e) {
    int s=e->stage;
    StageView *st=(s>=0&&s<NUM_STAGES)?&view.stages[s]:NULL;
    switch(e->type) {
    case EV_STAGE_STATUS:
        if(st) { st->status=e->status; if(e->status==ST_PAUSED)view.paused=1;
            else if(view.paused&&e->status==ST_RUNNING)view.paused=0; }
        break;
    case EV_TELEMETRY:
        if(st) { st->temp=e->v1;st->pressure=e->v2;st->ms=e->v3;st->lots++; }
        break;
    case EV_ALERT_WARNING: case EV_ALERT_CRITICAL:
        if(st) {
            int lev=e->type==EV_ALERT_CRITICAL?2:1;
            if(lev>st->alert || now_s()>st->alert_until)st->alert=lev;
            st->alert_until=now_s()+3;
        }
        log_line(e->type==EV_ALERT_CRITICAL?2:1,
            "%s %s %s %.2f (allowed %.2f to %.2f)",
            e->type==EV_ALERT_CRITICAL?"CRITICAL":"WARNING",stage_name(s),
            e->text,e->v1,e->v2,e->v3);
        break;
    case EV_LOT_FINISHED:
        view.oil+=e->v1;view.water+=e->v2;view.pomace+=e->v3;
        log_line(0,"Lot %u finished: oil %.1f kg",e->lot_id,e->v1);
        break;
    case EV_BELT: view.belt=e->v1>0;log_line(0,"Conveyor belt %s",view.belt?"running":"stopped");break;
    case EV_CONTAINER:view.container=e->v1;view.capacity=e->v2;break;
    case EV_STAGE_DONE:
        if(st)st->status=ST_DONE;
        if(s==STAGE_RECEPTION)view.belt=0;
        break;
    case EV_INFO:log_line(0,"%s",e->text);break;
    case EV_SHUTDOWN:
        view.finished=1;view.oil=e->v1;view.water=e->v2;view.pomace=e->v3;
        snprintf(view.result,sizeof view.result,"%s",e->text);
        log_line(0,"Production %s",view.result);
        break;
    default:break;
    }
}
static void gauge(float x,float y,float w,double value,Range limit,int is_pressure) {
    double span=limit.max-limit.min, frac=span>0?(value-limit.min)/span:0;
    if(frac<0)frac=0;
    if(frac>1)frac=1;
    color(.19f,.27f,.32f);rect(x,y,w,6);
    color(is_pressure?.35f:.45f,is_pressure?.67f:.76f,.63f);rect(x,y,(float)(w*frac),6);
    color(.91f,.95f,.92f);rect(x+(float)(w*frac)-1,y-3,3,12);
}
static void draw_hopper(void) {
    color(.16f,.22f,.25f);rect(33,443,155,116);
    color(.67f,.73f,.67f);
    glBegin(GL_LINE_LOOP);glVertex2f(48,545);glVertex2f(173,545);
    glVertex2f(156,470);glVertex2f(67,470);glEnd();
    double frac=view.capacity>0?view.container/view.capacity:0;
    if(frac<0)frac=0;
    if(frac>1)frac=1;
    color(.50f,.63f,.31f);rect(68,474,86,(float)(65*frac));
    color(.92f,.92f,.87f);text(43,571,"OLIVE CONTAINER");
    text(62,452,"%.0f / %.0f kg",view.container,view.capacity);
}
static void draw_belt(void) {
    color(.18f,.24f,.27f);rect(205,478,124,26);
    color(.42f,.52f,.54f);circle(211,491,14);circle(321,491,14);
    color(.67f,.72f,.70f);
    for(int i=0;i<7;i++) {
        float x=214+i*18+(float)view.belt_phase;
        if(x>319)x-=126;
        glBegin(GL_LINES);glVertex2f(x,482);glVertex2f(x+8,500);glEnd();
    }
    color(.91f,.94f,.88f);text(209,522,"BELT: %s",view.belt?"RUN":"STOP");
}
static void draw_stages(void) {
    const float x0=38, step=174, y=270, w=150, h=145;
    color(.93f,.94f,.88f);text(38,424,"PRODUCTION STAGES  /  LIVE SENSORS");
    for(int i=0;i<NUM_STAGES;i++) {
        float x=x0+step*i;
        const StageView *s=&view.stages[i];
        color(.13f,.20f,.23f);rect(x,y,w,h);
        stage_color(s->status);rect(x,y+h-30,w,30);
        color(.95f,.96f,.93f);text(x+8,y+h-19,"%d  %s",i+1,stage_name(i));
        color(.84f,.88f,.85f);text(x+8,y+90,"%s  |  lots %d",status_name(s->status),s->lots);
        text(x+8,y+71,"T: %.1f C",s->temp);
        text(x+8,y+42,"P: %.2f bar",s->pressure);
        text(x+8,y+9,"Time: %.0f ms",s->ms);
        gauge(x+83,y+69,58,s->temp,view.cfg.temp_limit_c[i],0);
        gauge(x+83,y+40,58,s->pressure,view.cfg.pressure_limit_bar[i],1);
        if(s->status==ST_RUNNING) {
            float cx=x+127,cy=y+100;
            color(.85f,.80f,.48f);
            circle(cx,cy,7);
            glPushMatrix();glTranslatef(cx,cy,0);glRotatef((float)(view.belt_phase*18+i*27),0,0,1);
            for(int k=0;k<4;k++) {glRotatef(90,0,0,1);rect(5,-2,9,4);}glPopMatrix();
        }
        if(s->alert && now_s()<s->alert_until) {
            glLineWidth(3);if(s->alert==2)color(.95f,.25f,.29f);
            else color(.98f,.68f,.19f);
            outline(x+1,y+1,w-2,h-2);glLineWidth(1);
        }
        if(i<NUM_STAGES-1) {
            color(.59f,.70f,.64f);
            glBegin(GL_TRIANGLES);glVertex2f(x+w+6,y+70);
            glVertex2f(x+w+18,y+76);glVertex2f(x+w+6,y+82);glEnd();
        }
    }
}
static void draw_tanks(void) {
    double values[3]={view.oil,view.water,view.pomace};
    const char *names[3]={"OLIVE OIL","WATER","POMACE"};
    const float cols[3][3]={{.84f,.69f,.27f},{.42f,.65f,.73f},{.57f,.42f,.31f}};
    double total=values[0]+values[1]+values[2];
    color(.91f,.94f,.88f);text(38,247,"EXTRACTION OUTPUT");
    for(int i=0;i<3;i++) {
        float x=44+168*i;
        double f=total>0?values[i]/total:0;
        color(.19f,.26f,.29f);rect(x,122,80,98);
        color(cols[i][0],cols[i][1],cols[i][2]);rect(x+3,125,74,(float)(92*f));
        color(.86f,.90f,.86f);outline(x,122,80,98);
        text(x+87,192,"%s",names[i]);text(x+87,169,"%.1f kg",values[i]);
    }
}
static void draw_logs(void) {
    color(.92f,.94f,.89f);text(552,246,"RECENT EVENTS");
    color(.11f,.18f,.21f);rect(548,66,512,162);
    for(int j=0;j<view.line_count;j++) {
        int i=(view.line_head-view.line_count+j+6)%6;
        if(view.line_level[i]==2)color(.99f,.45f,.45f);
        else if(view.line_level[i]==1)color(.98f,.75f,.37f);
        else color(.78f,.87f,.81f);
        text(561,205-j*23,"%s",view.lines[i]);
    }
}
static void display(void) {
    glClearColor(.075f,.125f,.15f,1);glClear(GL_COLOR_BUFFER_BIT);
    color(.91f,.94f,.86f);text(35,665,"OLIVE PRESS  /  REAL-TIME CONTROL DASHBOARD");
    color(.43f,.63f,.49f);rect(35,648,1030,2);
    draw_hopper();draw_belt();draw_stages();draw_tanks();draw_logs();
    color(.71f,.78f,.73f);text(38,38,"[p] pause   [s] status   [q] quit    |   Elapsed: %.0f s",
        now_s()-view.t_start);
    if(view.paused) {color(.95f,.75f,.35f);text(805,38,"PAUSED");}
    if(view.finished) {
        color(.09f,.17f,.18f);rect(275,555,575,65);
        color(.98f,.89f,.57f);text(301,595,"PRODUCTION %s",view.result[0]?view.result:"COMPLETE");
        text(301,573,"Oil %.1f kg  |  Water %.1f kg  |  Pomace %.1f kg",
             view.oil,view.water,view.pomace);
    }
    glutSwapBuffers();
}
static void reshape(int w,int h) {
    if(w<=0||h<=0)return;
    glViewport(0,0,w,h);glMatrixMode(GL_PROJECTION);glLoadIdentity();
    gluOrtho2D(0,1100,0,700);glMatrixMode(GL_MODELVIEW);glLoadIdentity();
}
static void keyboard(unsigned char key,int x,int y) {
    (void)x;(void)y;
    if(key=='q'||key==27) {glutLeaveMainLoop();return;}
    if(key=='p') {if(kill(getppid(),SIGUSR1)==0)view.paused=!view.paused;
        else perror("GUI: send pause");}
    if(key=='s'&&kill(getppid(),SIGUSR2)<0)perror("GUI: send status");
}
static void tick(int ignored) {
    (void)ignored;
    if(g_terminate) {glutLeaveMainLoop();return;}
    for(int n=0;n<200;n++) {
        struct pollfd pfd={.fd=view.fd,.events=POLLIN|POLLHUP};
        int r=poll(&pfd,1,0);
        if(r<0) {if(errno==EINTR)break;perror("GUI poll");view.eof=1;break;}
        if(r==0)break;
        if(pfd.revents&(POLLIN|POLLHUP|POLLERR)) {
            EventMsg e;int got=read_msg(view.fd,&e,sizeof e);
            if(got==1)apply_event(&e);
            else if(got==-2)break;
            else {if(got<0)perror("GUI read");view.eof=1;break;}
        } else break;
    }
    if(view.belt)view.belt_phase+=.7;
    if(view.belt_phase>=126)view.belt_phase-=126;
    if(view.eof) {
        if(view.finish_at==0)view.finish_at=now_s()+1.0;
        if(now_s()>=view.finish_at) {glutLeaveMainLoop();return;}
    }
    glutPostRedisplay();glutTimerFunc(16,tick,0);
}
void gui_main(const Config *cfg,const StageIO *io) {
    int argc=1;char arg0[]="olive-gui";char *argv[]={arg0,NULL};
    if(!cfg||!io||io->in_fd<0)return;
    stage_signals_init();stage_signals_unblock();
    if(!getenv("DISPLAY")&&!getenv("WAYLAND_DISPLAY")) {
        fprintf(stderr,"GUI: no graphical display; skipping OpenGL window\n");
        close(io->in_fd);return;
    }
    memset(&view,0,sizeof view);
    view.cfg=*cfg;view.fd=io->in_fd;view.t_start=now_s();
    view.capacity=cfg->container_capacity_kg;
    glutInit(&argc,argv);glutInitDisplayMode(GLUT_RGB|GLUT_DOUBLE);
    glutInitWindowSize(cfg->window_w,cfg->window_h);
    glutCreateWindow("Olive Press Simulation");
    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE,GLUT_ACTION_GLUTMAINLOOP_RETURNS);
    glutDisplayFunc(display);glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);glutTimerFunc(16,tick,0);
    //glutMainLoop();close(io->in_fd);
    glutMainLoop();
    /* user pressed q / closed the window: stop the whole simulation */
    if(!view.eof && !g_terminate) kill(getppid(), SIGTERM);
    close(io->in_fd);
}
