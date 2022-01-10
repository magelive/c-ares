
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

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
		fprintf(stdout, "\t%d: %s\n", i, ips->iplist[i]);
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
		return;
	}
	ips->count = 0;
	strncpy(ips->host, hptr->h_name, sizeof(ips->host));
	for (pptr=hptr->h_addr_list; 
			*pptr != NULL && ips->count < 16; 
			pptr++, ips->count++) {
		inet_ntop(hptr->h_addrtype, *pptr, ips->iplist[ips->count], IP_LEN);
	}

	print_dns(ips);
	return;
}

int main(int argc, char *argv[])
{
	ares_channel channel;
	int status = ares_library_init(ARES_LIB_INIT_ALL);
	if (status != ARES_SUCCESS) {
		goto ares_error;
	}

	status = ares_init(&channel);
	if (status != ARES_SUCCESS) {
		goto ares_error;
	}
	ares_set_servers_csv(channel, "114.114.114.114");

	iplist ips = {0x00};
	ares_gethostbyname(channel, argv[1], AF_INET, dns_callback, (void*)(&ips));

	int nfds;
	fd_set readers, writers;	
	struct timeval *tvp, tv;
	for (;;) {  
        FD_ZERO(&readers);  
        FD_ZERO(&writers);  
        nfds = ares_fds(channel, &readers, &writers);
        if (nfds == 0) break;  
        tvp = ares_timeout(channel, NULL, &tv);      
        select(nfds, &readers, &writers, NULL, tvp);
        ares_process(channel, &readers, &writers);
    }

ares_error:

	if ( status != ARES_SUCCESS) {
		fprintf(stderr, "ares_library_init error: %s\n",
				ares_strerror(status));
		return 0;
	}
	ares_destroy(channel);
	return 0;
}
