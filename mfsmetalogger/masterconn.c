/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA.

   This file is part of MooseFS.

   MooseFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   MooseFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with MooseFS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>

#include "MFSCommunication.h"
#include "datapack.h"
#include "masterconn.h"
#include "crc.h"
#include "cfg.h"
#include "main.h"
#include "sockets.h"

#define MaxPacketSize 1500000
#define META_DL_BLOCK 1000000

#define NOT_USED(x) ( (void)(x) )

typedef struct masterconn {
	int mode;
	int sock;
	int32_t pdescpos;
	time_t lastread,lastwrite;
	uint8_t hdrbuff[8];
	packetstruct inputpacket;
	packetstruct *outputhead,**outputtail;
	uint32_t bindip;
	uint32_t masterip;
	uint16_t masterport;
	uint8_t masteraddrvalid;

	uint8_t retrycnt;
	uint8_t downloading;
	FILE *logfd;	// using stdio because this is text file
	int metafd;	// using standard unix I/O because this is binary file
	uint64_t filesize;
	uint64_t dloffset;
	uint64_t dlstartuts;
} masterconn;

static masterconn *masterconnsingleton=NULL;

// from config
static uint32_t BackLogsNumber;
static char *MasterHost;
static char *MasterPort;
static char *BindHost;
static uint32_t Timeout;
static int first_add;

static uint32_t stats_bytesout=0;
static uint32_t stats_bytesin=0;

void masterconn_stats(uint32_t *bin,uint32_t *bout) {
	*bin = stats_bytesin;
	*bout = stats_bytesout;
	stats_bytesin = 0;
	stats_bytesout = 0;
}

uint8_t* masterconn_createpacket(masterconn *eptr,uint32_t type,uint32_t size) {
	packetstruct *outpacket;
	uint8_t *ptr;
	uint32_t psize;

	outpacket=(packetstruct*)malloc(sizeof(packetstruct));
	if (outpacket==NULL) {
		return NULL;
	}
	psize = size+8;
	outpacket->packet=malloc(psize);
	outpacket->bytesleft = psize;
	if (outpacket->packet==NULL) {
		free(outpacket);
		return NULL;
	}
	ptr = outpacket->packet;
	put32bit(&ptr,type);
	put32bit(&ptr,size);
	outpacket->startptr = (uint8_t*)(outpacket->packet);
	outpacket->next = NULL;
	*(eptr->outputtail) = outpacket;
	eptr->outputtail = &(outpacket->next);
	return ptr;
}

void masterconn_sendregister(masterconn *eptr) {
	uint8_t *buff;

	eptr->downloading=0;
	eptr->metafd=-1;
	eptr->logfd=NULL;

	buff = masterconn_createpacket(eptr,MLTOMA_REGISTER,1+4+2);
	if (buff==NULL) {
		eptr->mode=KILL;
		return;
	}
	put8bit(&buff,1);
	put16bit(&buff,VERSMAJ);
	put8bit(&buff,VERSMID);
	put8bit(&buff,VERSMIN);
	put16bit(&buff,Timeout);
}

void masterconn_metachanges_log(masterconn *eptr,const uint8_t *data,uint32_t length) {
	char logname1[100],logname2[100];
	uint32_t i;
	uint64_t version;
	if (length==1 && data[0]==0x55) {
		if (eptr->logfd!=NULL) {
			fclose(eptr->logfd);
			eptr->logfd=NULL;
		}
		if (BackLogsNumber>0) {
			for (i=BackLogsNumber ; i>0 ; i--) {
				snprintf(logname1,100,"changelog_ml.%"PRIu32".mfs",i);
				snprintf(logname2,100,"changelog_ml.%"PRIu32".mfs",i-1);
				rename(logname2,logname1);
			}
		} else {
			unlink("changelog_ml.0.mfs");
		}
		return;
	}
	if (length<10) {
		syslog(LOG_NOTICE,"MATOML_METACHANGES_LOG - wrong size (%"PRIu32"/9+data)",length);
		eptr->mode = KILL;
		return;
	}
	if (data[0]!=0xFF) {
		syslog(LOG_NOTICE,"MATOML_METACHANGES_LOG - wrong packet");
		eptr->mode = KILL;
		return;
	}
	if (data[length-1]!='\0') {
		syslog(LOG_NOTICE,"MATOML_METACHANGES_LOG - invalid string");
		eptr->mode = KILL;
		return;
	}

	if (eptr->logfd==NULL) {
		eptr->logfd = fopen("changelog_ml.0.mfs","a");
	}

	data++;
	version = get64bit(&data);
	if (eptr->logfd) {
		fprintf(eptr->logfd,"%"PRIu64": %s\n",version,data);
	} else {
		syslog(LOG_NOTICE,"lost MFS change %"PRIu64": %s",version,data);
	}
}

int masterconn_download_end(masterconn *eptr) {
	uint8_t *buff;
	eptr->downloading=0;
	buff = masterconn_createpacket(eptr,MLTOMA_DOWNLOAD_END,0);
	if (buff==NULL) {
		eptr->mode=KILL;
		return -1;
	}
	if (eptr->metafd>0) {
		if (close(eptr->metafd)<0) {
			syslog(LOG_NOTICE,"error closing metafile: %m");
			eptr->metafd=-1;
			return -1;
		}
		eptr->metafd=-1;
	}
	return 0;
}

void masterconn_download_init(masterconn *eptr,uint8_t filenum) {
	uint8_t *ptr;
//	syslog(LOG_NOTICE,"download_init %d",filenum);
	if ((eptr->mode==HEADER || eptr->mode==DATA) && eptr->downloading==0) {
//		syslog(LOG_NOTICE,"sending packet");
		ptr = masterconn_createpacket(eptr,MLTOMA_DOWNLOAD_START,1);
		if (ptr==NULL) {
			eptr->mode=KILL;
			return;
		}
		put8bit(&ptr,filenum);
		eptr->downloading=filenum;
	}
}

void masterconn_metadownloadinit(void) {
	masterconn_download_init(masterconnsingleton,1);
}

void masterconn_sessionsdownloadinit(void) {
	masterconn_download_init(masterconnsingleton,2);
}

void masterconn_download_next(masterconn *eptr) {
	uint8_t *ptr;
	uint8_t filenum;
	uint64_t dltime;
	if (eptr->dloffset>=eptr->filesize) {	// end of file
		filenum = eptr->downloading;
		if (masterconn_download_end(eptr)<0) {
			return;
		}
		dltime = main_utime()-eptr->dlstartuts;
		syslog(LOG_NOTICE,"%s downloaded %"PRIu64"B/%"PRIu64".%06"PRIu32"s (%.3lf MB/s)",(filenum==1)?"metadata":(filenum==2)?"sessions":"???",eptr->filesize,dltime/1000000,(uint32_t)(dltime%1000000),(double)(eptr->filesize)/(double)(dltime));
		if (filenum==1) {
			if (rename("metadata_ml.tmp","metadata_ml.mfs.back")<0) {
				syslog(LOG_NOTICE,"can't rename downloaded metadata - do it manually before next download");
			}
		} else if (filenum==2) {
			if (rename("sessions_ml.tmp","sessions_ml.mfs")<0) {
				syslog(LOG_NOTICE,"can't rename downloaded sessions - do it manually before next download");
			}
		}
	} else {	// send request for next data packet
		ptr = masterconn_createpacket(eptr,MLTOMA_DOWNLOAD_DATA,12);
		if (ptr==NULL) {
			eptr->mode=KILL;
			return;
		}
		put64bit(&ptr,eptr->dloffset);
		if (eptr->filesize-eptr->dloffset>META_DL_BLOCK) {
			put32bit(&ptr,META_DL_BLOCK);
		} else {
			put32bit(&ptr,eptr->filesize-eptr->dloffset);
		}
	}
}

void masterconn_download_start(masterconn *eptr,const uint8_t *data,uint32_t length) {
	if (length!=1 && length!=8) {
		syslog(LOG_NOTICE,"MATOML_DOWNLOAD_START - wrong size (%"PRIu32"/1|8)",length);
		eptr->mode = KILL;
		return;
	}
	if (length==1) {
		syslog(LOG_NOTICE,"download start error");
		return;
	}
	eptr->filesize = get64bit(&data);
	eptr->dloffset = 0;
	eptr->retrycnt = 0;
	eptr->dlstartuts = main_utime();
	if (eptr->downloading==1) {
		eptr->metafd = open("metadata_ml.tmp",O_WRONLY | O_TRUNC | O_CREAT,0666);
	} else if (eptr->downloading==2) {
		eptr->metafd = open("sessions_ml.tmp",O_WRONLY | O_TRUNC | O_CREAT,0666);
	} else {
		syslog(LOG_NOTICE,"unexpected MATOML_DOWNLOAD_START packet");
		eptr->mode = KILL;
		return;
	}
	if (eptr->metafd<0) {
		syslog(LOG_NOTICE,"error opening metafile: %m");
		masterconn_download_end(eptr);
		return;
	}
	masterconn_download_next(eptr);
}

void masterconn_download_data(masterconn *eptr,const uint8_t *data,uint32_t length) {
	uint64_t offset;
	uint32_t leng;
	uint32_t crc;
	ssize_t ret;
	if (eptr->metafd<0) {
		syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - file not opened");
		eptr->mode = KILL;
		return;
	}
	if (length<16) {
		syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - wrong size (%"PRIu32"/16+data)",length);
		eptr->mode = KILL;
		return;
	}
	offset = get64bit(&data);
	leng = get32bit(&data);
	crc = get32bit(&data);
	if (leng+16!=length) {
		syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - wrong size (%"PRIu32"/16+%"PRIu32")",length,leng);
		eptr->mode = KILL;
		return;
	}
	if (offset!=eptr->dloffset) {
		syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - unexpected file offset (%"PRIu64"/%"PRIu64")",offset,eptr->dloffset);
		eptr->mode = KILL;
		return;
	}
	if (offset+leng>eptr->filesize) {
		syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - unexpected file size (%"PRIu64"/%"PRIu64")",offset+leng,eptr->filesize);
		eptr->mode = KILL;
		return;
	}
#ifdef HAVE_PWRITE
	ret = pwrite(eptr->metafd,data,leng,offset);
#else /* HAVE_PWRITE */
	lseek(eptr->metafd,offset,SEEK_SET);
	ret = write(eptr->metafd,data,leng);
#endif /* HAVE_PWRITE */
	if (ret!=(ssize_t)leng) {
		syslog(LOG_NOTICE,"error writing metafile: %m");
		if (eptr->retrycnt>=5) {
			masterconn_download_end(eptr);
		} else {
			eptr->retrycnt++;
			masterconn_download_next(eptr);
		}
		return;
	}
	if (crc!=mycrc32(0,data,leng)) {
		syslog(LOG_NOTICE,"metafile data crc error");
		if (eptr->retrycnt>=5) {
			masterconn_download_end(eptr);
		} else {
			eptr->retrycnt++;
			masterconn_download_next(eptr);
		}
		return;
	}
	if (fsync(eptr->metafd)<0) {
		syslog(LOG_NOTICE,"error syncing metafile: %m");
		if (eptr->retrycnt>=5) {
			masterconn_download_end(eptr);
		} else {
			eptr->retrycnt++;
			masterconn_download_next(eptr);
		}
		return;
	}
	eptr->dloffset+=leng;
	eptr->retrycnt=0;
	masterconn_download_next(eptr);
}

void masterconn_beforeclose(masterconn *eptr) {
	if (eptr->metafd>0) {
		close(eptr->metafd);
		unlink("metadata_ml.tmp");
		unlink("sessions_ml.tmp");
	}
	if (eptr->logfd) {
		fclose(eptr->logfd);
	}
}

void masterconn_gotpacket(masterconn *eptr,uint32_t type,const uint8_t *data,uint32_t length) {
	switch (type) {
		case ANTOAN_NOP:
			break;
		case MATOML_METACHANGES_LOG:
			masterconn_metachanges_log(eptr,data,length);
			break;
		case MATOML_DOWNLOAD_START:
			masterconn_download_start(eptr,data,length);
			break;
		case MATOML_DOWNLOAD_DATA:
			masterconn_download_data(eptr,data,length);
			break;
		default:
			syslog(LOG_NOTICE,"got unknown message (type:%"PRIu32")",type);
			eptr->mode = KILL;
	}
}

void masterconn_term(void) {
	packetstruct *pptr,*paptr;
	masterconn *eptr = masterconnsingleton;

	if (eptr->mode!=FREE) {
		tcpclose(eptr->sock);
	       	if (eptr->mode!=CONNECTING) {
			if (eptr->inputpacket.packet) {
				free(eptr->inputpacket.packet);
			}
			pptr = eptr->outputhead;
			while (pptr) {
				if (pptr->packet) {
					free(pptr->packet);
				}
				paptr = pptr;
				pptr = pptr->next;
				free(paptr);
			}
		}
	}

	free(eptr);
	masterconnsingleton = NULL;
}

void masterconn_connected(masterconn *eptr) {
	tcpnodelay(eptr->sock);
	eptr->mode=HEADER;
	eptr->inputpacket.next = NULL;
	eptr->inputpacket.bytesleft = 8;
	eptr->inputpacket.startptr = eptr->hdrbuff;
	eptr->inputpacket.packet = NULL;
	eptr->outputhead = NULL;
	eptr->outputtail = &(eptr->outputhead);

	masterconn_sendregister(eptr);
	eptr->lastread = eptr->lastwrite = get_current_time();
}

int masterconn_initconnect(FILE *msgfd,masterconn *eptr) {
	int status;
	if (eptr->masteraddrvalid==0) {
		uint32_t mip,bip;
		uint16_t mport;
		if (tcpresolve(BindHost,NULL,&bip,NULL,1)>=0) {
			eptr->bindip = bip;
		} else {
			eptr->bindip = 0;
		}
		if (tcpresolve(MasterHost,MasterPort,&mip,&mport,0)>=0) {
			eptr->masterip = mip;
			eptr->masterport = mport;
			eptr->masteraddrvalid = 1;
		} else {
			if (msgfd) {
				fprintf(msgfd,"can't resolve master host/port (%s:%s)",MasterHost,MasterPort);
			} else {
				syslog(LOG_WARNING,"can't resolve master host/port (%s:%s)",MasterHost,MasterPort);
			}
			return -1;
		}
	}
	eptr->sock=tcpsocket();
	if (eptr->sock<0) {
		if (msgfd) {
			fprintf(msgfd,"create socket error (errno:%d)\n",errno);
		} else {
			syslog(LOG_WARNING,"create socket, error: %m");
		}
		return -1;
	}
	if (tcpnonblock(eptr->sock)<0) {
		if (msgfd) {
			fprintf(msgfd,"set nonblock error (errno:%d)\n",errno);
		} else {
			syslog(LOG_WARNING,"set nonblock, error: %m");
		}
		tcpclose(eptr->sock);
		eptr->sock=-1;
		return -1;
	}
	if (eptr->bindip>0) {
		if (tcpnumbind(eptr->sock,eptr->bindip,0)<0) {
			if (msgfd) {
				fprintf(msgfd,"can't bind socket to given ip (errno:%d)\n",errno);
			} else {
				syslog(LOG_WARNING,"can't bind socket to given ip: %m");
			}
			tcpclose(eptr->sock);
			eptr->sock=-1;
			return -1;
		}
	}
	status = tcpnumconnect(eptr->sock,eptr->masterip,eptr->masterport);
	if (status<0) {
		if (msgfd) {
			fprintf(msgfd,"connect failed (errno:%d)\n",errno);
		} else {
			syslog(LOG_WARNING,"connect failed, error: %m");
		}
		tcpclose(eptr->sock);
		eptr->sock=-1;
		return -1;
	}
	if (status==0) {
		syslog(LOG_NOTICE,"connected to Master immediately");
		masterconn_connected(eptr);
	} else {
		eptr->mode = CONNECTING;
		syslog(LOG_NOTICE,"connecting ...");
	}
	return 0;
}

void masterconn_connecttest(masterconn *eptr) {
	int status;

	status = tcpgetstatus(eptr->sock);
	if (status) {
		syslog(LOG_WARNING,"connection failed, error: %m");
		tcpclose(eptr->sock);
		eptr->sock=-1;
		eptr->mode=FREE;
	} else {
		syslog(LOG_NOTICE,"connected to Master");
		masterconn_connected(eptr);
	}
}

void masterconn_read(masterconn *eptr) {
	int32_t i;
	uint32_t type,size;
	const uint8_t *ptr;
	for (;;) {
		i=read(eptr->sock,eptr->inputpacket.startptr,eptr->inputpacket.bytesleft);
		if (i==0) {
			syslog(LOG_INFO,"Master connection lost");
			eptr->mode = KILL;
			return;
		}
		if (i<0) {
			if (errno!=EAGAIN) {
				syslog(LOG_INFO,"read from Master error: %m");
				eptr->mode = KILL;
			}
			return;
		}
		stats_bytesin+=i;
		eptr->inputpacket.startptr+=i;
		eptr->inputpacket.bytesleft-=i;

		if (eptr->inputpacket.bytesleft>0) {
			return;
		}

		if (eptr->mode==HEADER) {
			ptr = eptr->hdrbuff+4;
			size = get32bit(&ptr);

			if (size>0) {
				if (size>MaxPacketSize) {
					syslog(LOG_WARNING,"Master packet too long (%"PRIu32"/%u)",size,MaxPacketSize);
					eptr->mode = KILL;
					return;
				}
				eptr->inputpacket.packet = malloc(size);
				if (eptr->inputpacket.packet==NULL) {
					syslog(LOG_WARNING,"Master packet: out of memory");
					eptr->mode = KILL;
					return;
				}
				eptr->inputpacket.bytesleft = size;
				eptr->inputpacket.startptr = eptr->inputpacket.packet;
				eptr->mode = DATA;
				continue;
			}
			eptr->mode = DATA;
		}

		if (eptr->mode==DATA) {
			ptr = eptr->hdrbuff;
			type = get32bit(&ptr);
			size = get32bit(&ptr);

			eptr->mode=HEADER;
			eptr->inputpacket.bytesleft = 8;
			eptr->inputpacket.startptr = eptr->hdrbuff;

			masterconn_gotpacket(eptr,type,eptr->inputpacket.packet,size);

			if (eptr->inputpacket.packet) {
				free(eptr->inputpacket.packet);
			}
			eptr->inputpacket.packet=NULL;
		}
	}
}

void masterconn_write(masterconn *eptr) {
	packetstruct *pack;
	int32_t i;
	for (;;) {
		pack = eptr->outputhead;
		if (pack==NULL) {
			return;
		}
		i=write(eptr->sock,pack->startptr,pack->bytesleft);
		if (i<0) {
			if (errno!=EAGAIN) {
				syslog(LOG_INFO,"write to Master error: %m");
				eptr->mode = KILL;
			}
			return;
		}
		stats_bytesout+=i;
		pack->startptr+=i;
		pack->bytesleft-=i;
		if (pack->bytesleft>0) {
			return;
		}
		free(pack->packet);
		eptr->outputhead = pack->next;
		if (eptr->outputhead==NULL) {
			eptr->outputtail = &(eptr->outputhead);
		}
		free(pack);
	}
}


void masterconn_desc(int epoll_fd) {
	masterconn *eptr = masterconnsingleton;
	packetstruct *pptr,*paptr;	

	if (first_add==0) {
		struct epoll_event ev;
		ev.data.ptr = &eptr;
		ev.events = EPOLLIN|EPOLLOUT;
                epoll_ctl(epoll_fd,EPOLL_CTL_ADD,eptr->sock,&ev);
		first_add = 1;
	}
	if (eptr->mode == KILL) {
                masterconn_beforeclose(eptr);
                tcpclose(eptr->sock);
                if (eptr->inputpacket.packet) {
                      free(eptr->inputpacket.packet);
                }
                pptr = eptr->outputhead;
                while (pptr) {
                        if (pptr->packet) {
                                free(pptr->packet);
                        }
                        paptr = pptr;
                        pptr = pptr->next;
                        free(paptr);
                }
                eptr->mode = FREE;
        }
}


void masterconn_serve(int fd_num,int count, struct epoll_event *pdesc) {
	uint32_t now=get_current_time();
	masterconn *eptr = masterconnsingleton;	
	masterconn *weptr;

    NOT_USED(fd_num);
    NOT_USED(count);

	weptr = (masterconn *)pdesc[0].data.ptr;	
	if (pdesc[0].events & (EPOLLHUP | EPOLLERR)) {
		if (weptr->mode==CONNECTING) {
			masterconn_connecttest(eptr);
		} else {
			weptr->mode = KILL;
		}
	}
	if (weptr->mode==CONNECTING) {
		if (weptr->sock>=0 && (pdesc[0].events & EPOLLOUT)) { // FD_ISSET(eptr->sock,wset)) {
			masterconn_connecttest(weptr);
		}
	} else {
		if ((weptr->mode==HEADER || weptr->mode==DATA) && (pdesc[0].events & EPOLLIN)) { // FD_ISSET(eptr->sock,rset)) {
			masterconn_read(weptr);
			weptr->lastread = get_current_time();			
		}
		if ((weptr->mode==HEADER || weptr->mode==DATA) && (pdesc[0].events & EPOLLOUT)) { // FD_ISSET(eptr->sock,wset)) {
			masterconn_write(weptr);
			weptr->lastwrite = get_current_time();			
		}
		if ((weptr->mode==HEADER || weptr->mode==DATA) && weptr->lastread+Timeout<now) {
			weptr->mode = KILL;
		}
		if ((weptr->mode==HEADER || weptr->mode==DATA) && weptr->lastwrite+(Timeout/2)<now && weptr->outputhead==NULL) {
			masterconn_createpacket(weptr,ANTOAN_NOP,0);
		}
	}
}

void masterconn_reconnect(void) {
	masterconn *eptr = masterconnsingleton;
	if (eptr->mode==FREE) {
		masterconn_initconnect(NULL,eptr);
	}
}

void masterconn_reload(void) {
	masterconn *eptr = masterconnsingleton;
	eptr->masteraddrvalid=0;
}

int masterconn_init(FILE *msgfd) {
	uint32_t ReconnectionDelay;
	uint32_t MetaDLFreq;
	masterconn *eptr;

	ReconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY",5);
	MasterHost = cfg_getstr("MASTER_HOST","mfsmaster");
	MasterPort = cfg_getstr("MASTER_PORT","9419");
	BindHost = cfg_getstr("BIND_HOST","*");
	Timeout = cfg_getuint32("MASTER_TIMEOUT",60);
	BackLogsNumber = cfg_getuint32("BACK_LOGS",50);
	MetaDLFreq = cfg_getuint32("META_DOWNLOAD_FREQ",24);

	first_add = 0;
	if (Timeout>65536) {
		Timeout=65535;
	}
	if (Timeout<=1) {
		Timeout=2;
	}
	if (BackLogsNumber<5) {
		BackLogsNumber=5;
	}
	if (BackLogsNumber>10000) {
		BackLogsNumber=10000;
	}
	if (MetaDLFreq>(BackLogsNumber/2)) {
		MetaDLFreq=BackLogsNumber/2;
	}
	eptr = masterconnsingleton = malloc(sizeof(masterconn));

	eptr->masteraddrvalid = 0;
	eptr->mode = FREE;
	eptr->pdescpos = -1;
	eptr->logfd = NULL;
	eptr->metafd = -1;

	if (masterconn_initconnect(msgfd,eptr)<0) {
		return -1;
	}
	main_timeregister(TIMEMODE_RUNONCE,ReconnectionDelay,0,masterconn_reconnect);
	main_destructregister(masterconn_term);
	main_epollregister(masterconn_desc,masterconn_serve);
	main_reloadregister(masterconn_reload);
	main_timeregister(TIMEMODE_RUNONCE,MetaDLFreq*3600,630,masterconn_metadownloadinit);
	main_timeregister(TIMEMODE_RUNONCE,60,0,masterconn_sessionsdownloadinit);
	(void)msgfd;
	return 0;
}
