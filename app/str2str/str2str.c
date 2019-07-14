/*------------------------------------------------------------------------------
* str2str.c : console version of stream server
*
*          Copyright (C) 2007-2018 by T.TAKASU, All rights reserved.
*
* version : $Revision: 1.1 $ $Date: 2008/07/17 21:54:53 $
* history : 2009/06/17  1.0  new
*           2011/05/29  1.1  add -f, -l and -x option
*           2011/11/29  1.2  fix bug on recognize ntrips:// (rtklib_2.4.1_p4)
*           2012/12/25  1.3  add format conversion functions
*                            add -msg, -opt and -sta options
*                            modify -p option
*           2013/01/25  1.4  fix bug on showing message
*           2014/02/21  1.5  ignore SIG_HUP
*           2014/08/10  1.5  fix bug on showing message
*           2014/08/26  1.6  support input format gw10, binex and rt17
*           2014/10/14  1.7  use stdin or stdout if option -in or -out omitted
*           2014/11/08  1.8  add option -a, -i and -o
*           2015/03/23  1.9  fix bug on parsing of command line options
*           2018/01/29  1.10 fix bug on invalid sta position by option -p (#126)
*-----------------------------------------------------------------------------*/
#include <signal.h>
#include <unistd.h>
#include "rtklib.h"

static const char rcsid[]="$Id:$";

#define PRGNAME     "str2str"          /* program name */
#define MAXSTR      5                  /* max number of streams */
#define MAXRCVCMD   4096               /* max length of receiver command */
#define TRFILE      "str2str.trace"    /* trace file */

/* global variables ----------------------------------------------------------*/
static strsvr_t strsvr;                /* stream server */
static volatile int intrflg=0;         /* interrupt flag */

/* help text -----------------------------------------------------------------*/
static const char *help[]={
"",
" usage: str2str [-in stream] [-out stream [-out stream...]] [options]",
"",
" Input data from a stream and divide and output them to multiple streams",
" The input stream can be serial, tcp client, tcp server, ntrip client, or",
" file. The output stream can be serial, tcp client, tcp server, ntrip server,",
" or file. str2str is a resident type application. To stop it, type ctr-c in",
" console if run foreground or send signal SIGINT for background process.",
" if run foreground or send signal SIGINT for background process.",
" if both of the input stream and the output stream follow #format, the",
" format of input messages are converted to output. To specify the output",
" messages, use -msg option. If the option -in or -out omitted, stdin for",
" input or stdout for output is used.",
" Command options are as follows.",
"",
" -in  stream[#format] input  stream path and format",
" -out stream[#format] output stream path and format",
"",
"  stream path",
"    serial       : serial://port[:brate[:bsize[:parity[:stopb[:fctr]]]]]",
"    tcp server   : tcpsvr://:port",
"    tcp client   : tcpcli://addr[:port]",
"    ntrip client : ntrip://[user[:passwd]@]addr[:port][/mntpnt]",
"    ntrip server : ntrips://[:passwd@]addr[:port][/mntpnt[:str]] (only out)",
"    file         : [file://]path[::T][::+start][::xseppd][::S=swap]",
"",
"  format",
"    rtcm2        : RTCM 2 (only in)",
"    rtcm3        : RTCM 3",
"    nov          : NovAtel OEMV/4/6,OEMStar (only in)",
"    oem3         : NovAtel OEM3 (only in)",
"    ubx          : ublox LEA-4T/5T/6T (only in)",
"    ss2          : NovAtel Superstar II (only in)",
"    hemis        : Hemisphere Eclipse/Crescent (only in)",
"    stq          : SkyTraq S1315F (only in)",
"    gw10         : Furuno GW10 (only in)",
"    javad        : Javad (only in)",
"    nvs          : NVS BINR (only in)",
"    binex        : BINEX (only in)",
"    rt17         : Trimble RT17 (only in)",
"",
" -msg \"type[(tint)][,type[(tint)]...]\"",
"                   rtcm message types and output intervals (s)",
" -sta sta          station id",
" -opt opt          receiver dependent options",
" -s  msec          timeout time (ms) [10000]",
" -r  msec          reconnect interval (ms) [10000]",
" -n  msec          nmea request cycle (m) [0]",
" -f  sec           file swap margin (s) [30]",
" -c  file          receiver commands file [no]",
" -p  lat lon hgt   station position (latitude/longitude/height) (deg,m)",
" -a  antinfo       antenna info (separated by ,)",
" -i  rcvinfo       receiver info (separated by ,)",
" -o  e n u         antenna offst (e,n,u) (m)",
" -l  local_dir     ftp/http local directory []",
" -x  proxy_addr    http/ntrip proxy address [no]",
" -t  level         trace level [0]",
" -h                print help",
};
/* print help ----------------------------------------------------------------*/
static void printhelp(void)
{
    int i;
    for (i=0;i<sizeof(help)/sizeof(*help);i++) fprintf(stderr,"%s\n",help[i]);
    exit(0);
}
/* signal handler ------------------------------------------------------------*/
static void sigfunc(int sig)
{
    intrflg=1;
}
/* decode format -------------------------------------------------------------*/
static void decodefmt(char *path, int *fmt)
{
    char *p;
    
    *fmt=-1;
    
    if ((p=strrchr(path,'#'))) {
        if      (!strcmp(p,"#rtcm2")) *fmt=STRFMT_RTCM2;
        else if (!strcmp(p,"#rtcm3")) *fmt=STRFMT_RTCM3;
        else if (!strcmp(p,"#nov"  )) *fmt=STRFMT_OEM4;
        else if (!strcmp(p,"#oem3" )) *fmt=STRFMT_OEM3;
        else if (!strcmp(p,"#ubx"  )) *fmt=STRFMT_UBX;
        else if (!strcmp(p,"#ss2"  )) *fmt=STRFMT_SS2;
        else if (!strcmp(p,"#hemis")) *fmt=STRFMT_CRES;
        else if (!strcmp(p,"#stq"  )) *fmt=STRFMT_STQ;
        else if (!strcmp(p,"#gw10" )) *fmt=STRFMT_GW10;
        else if (!strcmp(p,"#javad")) *fmt=STRFMT_JAVAD;
        else if (!strcmp(p,"#nvs"  )) *fmt=STRFMT_NVS;
        else if (!strcmp(p,"#binex")) *fmt=STRFMT_BINEX;
        else if (!strcmp(p,"#rt17" )) *fmt=STRFMT_RT17;
        else return;
        *p='\0';
    }
}
/* decode stream path --------------------------------------------------------*/
static int decodepath(const char *path, int *type, char *strpath, int *fmt)
{
    char buff[1024],*p;
    
    strcpy(buff,path);
    
    /* decode format */
    decodefmt(buff,fmt);
    
    /* decode type */
    if (!(p=strstr(buff,"://"))) {
        strcpy(strpath,buff);
        *type=STR_FILE;
        return 1;
    }
    if      (!strncmp(path,"serial",6)) *type=STR_SERIAL;
    else if (!strncmp(path,"tcpsvr",6)) *type=STR_TCPSVR;
    else if (!strncmp(path,"tcpcli",6)) *type=STR_TCPCLI;
    else if (!strncmp(path,"ntrips",6)) *type=STR_NTRIPSVR;
    else if (!strncmp(path,"ntrip", 5)) *type=STR_NTRIPCLI;
    else if (!strncmp(path,"file",  4)) *type=STR_FILE;
    else {
        fprintf(stderr,"stream path error: %s\n",buff);
        return 0;
    }
    strcpy(strpath,p+3);
    return 1;
}
/* read receiver commands ----------------------------------------------------*/
static void readcmd(const char *file, char *cmd, int type)
{
    FILE *fp;
    char buff[MAXSTR],*p=cmd;
    int i=0;
    
    *p='\0';
    
    if (!(fp=fopen(file,"r"))) return;
    
    while (fgets(buff,sizeof(buff),fp)) {
        if (*buff=='@') i=1;
        else if (i==type&&p+strlen(buff)+1<cmd+MAXRCVCMD) {
            p+=sprintf(p,"%s",buff);
        }
    }
    fclose(fp);
}

int StreamFilePath(char infile[][1024])
{
	int n = 0;
	strcpy(infile[n++], "ntrip://Peiyuan:zpy12345@165.206.203.10:31100/RTCM3_IAAM#rtcm3");
	strcpy(infile[n++], "ntrip://Peiyuan:zpy12345@products.igs-ip.net:2101/RTCM3EPH#rtcm3");
	strcpy(infile[n++], "ntrip://Peiyuan:zpy12345@products.igs-ip.net:2101/CLK93#rtcm3");
	return n;
}

int StreamFilePath_out(char infile[][1024])
{
	int n = 0;
	strcpy(infile[n++], "tcpsvr://:6661");
	strcpy(infile[n++], "tcpsvr://:6662");
	strcpy(infile[n++], "tcpsvr://:6663");
	return n;
}

/* str2str -------------------------------------------------------------------*/
int main()
{
	static char cmd[MAXRCVCMD] = "";
	const char ss[] = { 'E','-','W','C','C' };
	strconv_t *conv[MAXSTR] = { NULL };
	double pos[3], stapos[3] = { 0 }, off[3] = { 0 };
	char infile_[9][1024]    = { "" }, *infile[9]   , *paths   [MAXSTR], s[MAXSTR][MAXSTRPATH] = { {0} }, *cmdfile = "";
	char infile_out[9][1024] = { "" }, *infileout[9], *pathsout[MAXSTR], s2[MAXSTR][MAXSTRPATH] = { {0} };
	char *local = "", *proxy = "", *msg = "1004,1019", *opt = "", buff[256], *p;
	char strmsg[MAXSTRMSG] = "", *antinfo = "", *rcvinfo = "";
	char *ant[] = { "","","" }, *rcv[] = { "","","" };
	int i, j, n = 0, dispint = 5000, trlevel = 0, opts[] = { 10000,10000,2000,32768,10,0,30 };
	int types[MAXSTR] = { STR_FILE,STR_FILE }, typesout[MAXSTR] = { STR_FILE,STR_FILE }, stat[MAXSTR] = { 0 }, byte[MAXSTR] = { 0 };
	int bps[MAXSTR] = { 0 }, fmts[MAXSTR] = { 0 }, fmtsout[MAXSTR] = { 0 }, sta = 0;

	for (i = 0; i < MAXSTR; i++) paths[i] = s[i];
	for (i = 0; i < MAXSTR; i++) pathsout[i] = s2[i];
	// decode input streams
	int filenum = StreamFilePath(infile_);
	for (i = 0; i < filenum; i++)
	{
		infile[i] = infile_[i];
		if (!decodepath(infile[i], types+i, paths[i], fmts + i)) return -1;
	}
	// decode output streams
	int filenum_out = StreamFilePath_out(infile_out);
	for (i = 0; i < filenum_out; i++)
	{
		infileout[i] = infile_out[i];
		if (!decodepath(infileout[i], typesout + i, pathsout[i], fmtsout + i)) return -1;
	}
	if (filenum_out == filenum)
		strsvrinit(&strsvr, filenum);

	if (trlevel > 0) {
		traceopen(TRFILE);
		tracelevel(trlevel);
	}
	fprintf(stderr, "stream server start\n");

	strsetdir(local);
	strsetproxy(proxy);

	if (*cmdfile) readcmd(cmdfile, cmd, 0);

	/* start stream server */
	if (!strsvrstart2(&strsvr, opts, types, paths, typesout, pathsout, conv, *cmd ? cmd : NULL, stapos)) {
		fprintf(stderr, "stream server start error\n");
		return -1;
	}
	for (intrflg = 0; !intrflg;) {

		/* get stream server status */
		strsvrstat2(&strsvr, stat, byte, bps, strmsg);

		/* show stream server status */
		for (i = 0, p = buff; i < MAXSTR; i++) 
			p += sprintf(p, "%c", ss[stat[i] + 1]);

		fprintf(stderr, "%s [%s] %10d B %7d bps %s\n",
			time_str(utc2gpst(timeget()), 0), buff, byte[0], bps[0], strmsg);

		sleepms(dispint);
	}
	if (*cmdfile) readcmd(cmdfile, cmd, 1);

	/* stop stream server */
	strsvrstop(&strsvr, *cmd ? cmd : NULL);

	for (i = 0; i < n; i++) {
		strconvfree(conv[i]);
	}
	if (trlevel > 0) {
		traceclose();
	}
	fprintf(stderr, "stream server stop\n");
	return 0;
}
