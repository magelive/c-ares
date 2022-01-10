
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

#include "mainloop.h"

static void dns_callback (void* arg, int status, int timeouts, struct hostent* hptr)
{
	int i;
	char **pptr;
	char *url = (char *)arg;
	if(status != ARES_SUCCESS) {
		fprintf(stderr, "Lookup %s(%s) failed: %s\n",
				(char *)arg,
				hptr?(hptr->h_name?hptr->h_name:"none"):"null",
				ares_strerror(status));
		return;
	}
	fprintf(stdout, "host %s(%s) ip list:\n", url, hptr->h_name);
	for (pptr=hptr->h_addr_list; *pptr != NULL; pptr++) {
		fprintf(stdout, "\t%s\n", inet_ntoa(*(struct in_addr*)*pptr));
	}

	

	return;
}

char *url[]= {
	"www.baidu.com",
	"www.google.com",
	"www.hello.com",
	"www.bb.com",
	"www.aadfasdfasdfasdfad.com"
};

struct dns_resolve {
	ares_channel channel;
	ares_socket_t ares_socks[ARES_GETSOCK_MAXNUM];
};

void dns_timeout(int fd, void *user_data)
{
	struct dns_resolve *dr = (struct dns_resolve *)user_data;

	ares_process_fd(dr->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
	mainloop_remove_timeout(fd);
	return;
}

void dns_process(int fd, events_t events, void *user_data)
{
	struct dns_resolve *dr = (struct dns_resolve *)user_data;
	ares_socket_t read_fd = ARES_SOCKET_BAD, write_fd = ARES_SOCKET_BAD;
	if (events | EVENT_IN) {
		read_fd = fd;
	}
	
	if (events | EVENT_OUT) {
		write_fd = fd;
	}
	
	ares_process_fd(dr->channel, read_fd, write_fd);
	return;
}

int main(int argc, char *argv[])
{
	int status, i;
	mainloop_init();
	struct dns_resolve dr = {0x00};
	status = ares_library_init(ARES_LIB_INIT_ALL);
	if (status != ARES_SUCCESS) {
		goto ares_error;
	}
	struct ares_options options = {0x00};
	int optmask = 0;
	optmask |= ARES_OPT_TIMEOUTMS;
	options.timeout = 200;
	status = ares_init_options(&dr.channel, &options, optmask);
	if (status != ARES_SUCCESS) {
		goto ares_error;
	}
	for (i = 0; i < ARES_GETSOCK_MAXNUM; i++) {
		dr.ares_socks[i] = ARES_SOCKET_BAD;
	}
	mainloop_add_timeout(200,
			dns_timeout, NULL, &dr);
	//ares_set_servers_csv(channel, "114.114.114.114");
	for (i = 0; i < sizeof(url)/sizeof(char *); i++) {
	ares_gethostbyname(dr.channel, url[i], AF_INET, 
				dns_callback, url[i]);
	}
	events_t events = 0;
	int fd_bitmap = ares_getsock(dr.channel, dr.ares_socks, ARES_GETSOCK_MAXNUM);
    for (i = 0; i < ARES_GETSOCK_MAXNUM; i++) {
		events = 0;
        if (ARES_GETSOCK_READABLE(fd_bitmap, i) || ARES_GETSOCK_WRITABLE(fd_bitmap, i)) {
			printf("%d, fd = %d\n", i, dr.ares_socks[i]);
            if (ARES_GETSOCK_READABLE(fd_bitmap, i)) {
                events |= EVENT_IN;
            }

            if (ARES_GETSOCK_WRITABLE(fd_bitmap, i)) {
                events |= EVENT_OUT;
            }
			mainloop_add_fd(dr.ares_socks[i], events,
					dns_process, NULL, &dr);
        }
    }

	mainloop_run();

ares_error:

	if ( status != ARES_SUCCESS) {
		fprintf(stderr, "ares_library_init error: %s\n",
				ares_strerror(status));
		return 0;
	}
	ares_destroy(dr.channel);
	return 0;
}
