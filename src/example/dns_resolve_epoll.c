
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/un.h>

#include "ares_setup.h"

#if !defined(WIN32) || defined(WATT32)
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#include "ares.h"
#include "ares_dns.h"
#include "ares_ipv6.h"
#include "ares_nowarn.h"

#ifndef HAVE_STRDUP
#  include "ares_strdup.h"
#  define strdup(ptr) ares_strdup(ptr)
#endif

#ifndef HAVE_STRCASECMP
#  include "ares_strcasecmp.h"
#  define strcasecmp(p1,p2) ares_strcasecmp(p1,p2)
#endif

#ifndef HAVE_STRNCASECMP
#  include "ares_strcasecmp.h"
#  define strncasecmp(p1,p2,n) ares_strncasecmp(p1,p2,n)
#endif

#define IP_LEN 32

typedef struct {
	int is_get;
	char host[128];
	char iplist[16][IP_LEN];
	int count;
}iplist;

static void print_dns(iplist *ips)
{
	int i;
	fprintf(stdout, "host: %s\n", ips->host);
	fprintf(stdout, "ip count: %d\n", ips->count);
	fprintf(stdout, "ip list:\n");
	for (i = 0; i < ips->count; i++) {
		fprintf(stdout, "\t %d: %s\n", i, ips->iplist[i]);
	}
	return;
}

static void dns_callback (void* arg, int status, int timeouts, struct hostent* hptr)
{
	int i;
	char **pptr;
	iplist *ips = (iplist*)arg;
	if( ips == NULL )
		return;

	if(status != ARES_SUCCESS) {
		fprintf(stderr, "Lookup failed: %s\n",
				ares_strerror(status));
		ips->is_get = -1;
		return;
	}
	ips->count = 0;
	strncpy(ips->host, hptr->h_name, sizeof(ips->host));
	for (pptr=hptr->h_addr_list; 
			*pptr != NULL && ips->count < 16; 
			pptr++, ips->count++) {
		inet_ntop(hptr->h_addrtype, *pptr, ips->iplist[ips->count], IP_LEN);
	}

	ips->is_get = 1;

	//print_dns(ips);
	return;
}

int main(int argc, char *argv[])
{
	ares_channel channel;
	struct timeval *tvp, tv;
	struct timeval maxtv = {
		.tv_sec = 0,
		.tv_usec = 50
	};
	int ch ,status, i;
	ares_socket_t ares_socks[ARES_GETSOCK_MAXNUM] = {0x00};
	status = ares_library_init(ARES_LIB_INIT_ALL);
	if (status != ARES_SUCCESS) {
		goto ares_error;
	}
	struct ares_options options = {0x00};
	int optmask = ARES_OPT_TIMEOUTMS;
	options.timeout = 200;
	status = ares_init_options(&channel, &options, optmask);
	if (status != ARES_SUCCESS) {
		goto ares_error;
	}
	//ares_set_servers_csv(channel, "114.114.114.114");
	int _c = 0;	
	iplist ips = {0x00};
	ips.is_get = 0;
	ares_gethostbyname(channel, argv[1], AF_INET, dns_callback, (void*)(&ips));

	int fd_bitmap = 0;
	//unsigned int setbits = 0xffffffff;
	struct epoll_event events[ARES_GETSOCK_MAXNUM] = {0x00}, _event;
	int events_count;
	int event_fd = epoll_create(512);
	if (event_fd < 0) {
		fprintf(stderr, "event create error: %d\n", event_fd);
		goto ares_error;
	}
    events_count = 0;
    fd_bitmap = ares_getsock(channel, ares_socks, ARES_GETSOCK_MAXNUM);
	printf("fd_bitmap = 0x%x\n", fd_bitmap);
    for (i = 0; i < ARES_GETSOCK_MAXNUM; i++) {
		bzero(&_event, sizeof(struct epoll_event));
        if (ARES_GETSOCK_READABLE(fd_bitmap, i) || ARES_GETSOCK_WRITABLE(fd_bitmap, i)) {
			printf("%d, fd = %d\n", i, ares_socks[i]);
             _event.data.fd = (int)(ares_socks[i]);
            if (ARES_GETSOCK_READABLE(fd_bitmap, i)) {
				printf("....in\n");
                _event.events |= EPOLLIN;
            }

            if (ARES_GETSOCK_WRITABLE(fd_bitmap, i)) {
				printf("....out\n");
                _event.events |= EPOLLOUT;
            }

            epoll_ctl(event_fd, EPOLL_CTL_ADD, (int)ares_socks[i], &_event);
            events_count++;
        }
    }
	ares_socket_t read_fd = ARES_SOCKET_BAD, write_fd = ARES_SOCKET_BAD;

	while (ips.is_get == 0) {
		tvp = ares_timeout(channel, &maxtv, &tv);
		if (!tvp) {
			printf("No tvp....\n");
			break;
		}
		//int n = epoll_wait(event_fd, events, ARES_GETSOCK_MAXNUM, tvp?tvp->tv_sec*1000+tvp->tv_usec/1000:0);
		int n = epoll_wait(event_fd, events, ARES_GETSOCK_MAXNUM, 50);
		if (n > 0) {
			//printf("n = %d\n", n);
			for ( i = 0; i < n; i++) {
				read_fd = ARES_SOCKET_BAD;
				write_fd = ARES_SOCKET_BAD;

				if (events[i].events & EPOLLIN) {
					read_fd = events[i].data.fd;
				}
				
				if (events[i].events & EPOLLOUT) {
					write_fd = events[i].data.fd;
				}
				
				ares_process_fd(channel, read_fd, write_fd);
			}
		} else if (n == 0) {
			fprintf(stdout, "timeout....////////lll..\n");
			_c ++;
		} else {
			fprintf(stdout, "n < 0\n");
		}
	}

	if (ips.is_get == 1) {
		print_dns(&ips);
	}

ares_error:
	fprintf(stdout, "... timeout count is: %d\n", _c);
	if ( status != ARES_SUCCESS) {
		fprintf(stderr, "ares_library_init error: %s\n",
				ares_strerror(status));
		return 0;
	}
	ares_destroy(channel);
	return 0;
}
