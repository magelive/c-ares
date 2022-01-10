#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/timerfd.h>
#include "mainloop.h"

#define USE_EPOLL

#define mainloop_malloc malloc
#define mainloop_free free

#define MAINLOOP_TIMEOUT_MSEC 500       //mainloop timeout 1/2s (500ms)

typedef struct {

}mainloop_t;

static int mainloop_terminate;

typedef enum {
    GENERAL,
    TIMER,
}mainloop_data_type;

typedef struct mainloop_data {
    struct mainloop_data *next, *prev;
    int fd;
    mainloop_data_type type;
    events_t events;
    mainloop_timeout_func timeout_callback;
    mainloop_event_func event_callback;

    mainloop_destroy_func destroy_callback;
    
    void *user_data;
}mainloop_data_t;

static mainloop_data_t *mainloop_data_head = NULL;
static mainloop_data_t *mainloop_free_head = NULL;
static uint32_t mainloop_free_count = 0;
#define MAINLOOP_FREE_COUNT_MAX 8

void mainloop_data_add(mainloop_data_t *data)
{
    if (mainloop_data_head) {
        data->next = mainloop_data_head;
        mainloop_data_head->prev = data;
    }
    data->prev = NULL;
    mainloop_data_head = data;
}

void mainloop_free_add(mainloop_data_t *data)
{
    if (mainloop_free_count >= MAINLOOP_FREE_COUNT_MAX) {
        mainloop_free(data);
    }

    if (mainloop_free_head) {
        data->next = mainloop_free_head;
        mainloop_data_head->prev = data;
    }
    data->prev = NULL;
    mainloop_free_head = data;
    mainloop_free_count ++;
}

void mainloop_data_remove(mainloop_data_t *data)
{
    if (!data || !mainloop_data_head) {
        return;
    }
    if (mainloop_data_head == data) {
        mainloop_data_head = mainloop_data_head->next;
        if (mainloop_data_head)
            mainloop_data_head->prev = NULL;
    } else {
        if (data->prev)
            data->prev->next = data->next;
        if (data->next)
            data->next->prev = data->prev;
    }
    mainloop_free_count --;
    data->prev = data->next = NULL;
}

void mainloop_free_remove(mainloop_data_t *data)
{
    if (!data || !mainloop_free_head) {
        return;
    }

    if (mainloop_free_head == data) {
        mainloop_free_head = mainloop_free_head->next;
        if (mainloop_free_head)
            mainloop_free_head->prev = NULL;
    } else {
        if (data->prev)
            data->prev->next = data->next;
        if (data->next)
            data->next->prev = data->prev;
    }
    data->prev = data->next = NULL;
}

static mainloop_data_t *get_mainloop_data_by_fd(int fd)
{
    mainloop_data_t *data = mainloop_data_head;
    while(data != NULL) {
        if (data->fd == fd) {
            break;
        }
        data = data->next;
    }
    return data;
}

static int mainloop_timeout_process(mainloop_data_t *data)
{
    if (data->type != TIMER) {
        return -EINVAL;
    }
    uint64_t expired;
    if (read(data->fd, &expired, sizeof(expired)) != sizeof(expired)) {
        return -EIO;
    }
    if (data->timeout_callback) {
        data->timeout_callback(data->fd, data->user_data);
    }
    return 0;
}

#ifdef USE_EPOLL
#include <sys/epoll.h>
static int epoll_fd;

static int epoll_init()
{
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        return epoll_fd;
    }
    return 0;
}

static int epoll_add_fd(events_t events, mainloop_data_t *data)
{
    struct epoll_event ev = {0x00};
    if (events & EVENT_IN) {
        ev.events |= EPOLLIN;
    }
    if (events & EVENT_OUT) {
        ev.events |= EPOLLOUT;
    }
    if (data->type == TIMER) {
        ev.events |= EPOLLONESHOT;
    }
    ev.data.ptr = data;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, data->fd, &ev);
}

static int epoll_modify_fd(events_t events, mainloop_data_t *data)
{
    struct epoll_event ev = {0x00};
    if (events & EVENT_IN) {
        ev.events |= EPOLLIN;
    }
    if (events & EVENT_OUT) {
        ev.events |= EPOLLOUT;
    }
    ev.data.ptr = data;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, data->fd, &ev);
}

static int epoll_remove_fd(int fd)
{
    return epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}
 
static int epoll_run()
{
    #define MAX_EPOLL_EVENTS 10 
    int i;
    struct epoll_event events[MAX_EPOLL_EVENTS] = {0x00};
    int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, MAINLOOP_TIMEOUT_MSEC);
    for (i = 0; i < nfds; i++) {
        events_t e = 0;
        if (events[i].events & EPOLLIN) {
            e |= EVENT_IN;
        }
        if (events[i].events & EPOLLOUT) {
            e |= EVENT_OUT;
        }
        if ((events[i].events & EPOLLERR) || 
            (events[i].events & EPOLLRDHUP)) {
            e |= EVENT_ERR;
        }
        mainloop_data_t *data = events[i].data.ptr;
        switch(data->type) {
            case GENERAL:
                data->event_callback(data->fd, e, data->user_data);
                break;
            case TIMER:
                if (!(e & EVENT_ERR)) {
					printf("...aaabb\n");
                    mainloop_timeout_process(data);
                }
                break;
        }
    }
    return nfds;
}

static int epoll_destroy()
{
    close(epoll_fd);
}

#elif USE_SELECT
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static fd_set rfds;
static fd_set wfds;
static int maxfd = 0;

static int select_init()
{
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    maxfd = 0;
    return 0;
}

static int select_add_fd(events_t events, mainloop_data_t *data)
{
    if (events & EVENT_IN) {
        FD_SET(data->fd, &rfds);
    }
    if (events & EVENT_OUT) {
        FD_SET(data->fd, &wfds);
    }
    maxfd = (maxfd <= data->fd) ? (data->fd + 1) : maxfd;
    return 0;
}

static int select_remove_fd(int fd)
{
    FD_CLR(fd, &rfds);
    FD_CLR(fd, &wfds);
    return 0;
}

static int select_modify_fd(events_t events, mainloop_data_t *data)
{
    FD_CLR(data->fd, &rfds);
    FD_CLR(data->fd, &wfds);
    if (events & EVENT_IN) {
        FD_SET(data->fd, &rfds);
    }
    if (events & EVENT_OUT) {
        FD_SET(data->fd, &wfds);
    }
    return 0;    
}

static int select_run()
{
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = MAINLOOP_TIMEOUT_MSEC*1000,
    };
    fd_set orfds = rfds, owfds = wfds;
    int ret = select(maxfd, &orfds, &owfds, NULL, &timeout);
    if (ret <= 0) {
        return ret;
    }
    int fc = 0;
    events_t e = 0;
    mainloop_data_t *data = mainloop_data_head;
    while(data) {
        fc = 0;
        e = 0;
        if (FD_ISSET(data->fd, &orfds)) {
            fc = 1;
            e |= EVENT_IN;
        }
        if (FD_ISSET(data->fd, &orfds)) {
            fc = 1;
            e |= EVENT_OUT;
        }
        if (fc) {
            switch(data->type) {
                case GENERAL:
                    data->event_callback(data->fd, e, data->user_data);
                    break;
                case TIMER: 
                    mainloop_timeout_process(data);
                    break;
            }
        }
        data = data->next;
    }
    return ret;
}

static int select_destroy()
{
    select_init();
    return 0;
}
#endif

typedef int (*platform_init)(void);
typedef int (*platform_add_fd)(events_t events, mainloop_data_t *data);
typedef int (*platform_modify_fd)(events_t events, mainloop_data_t *data);
typedef int (*platform_remove_fd)(int fd);
typedef int (*platform_run)(void);
typedef int (*platform_destroy)(void);

static platform_init mainloop_platform_init = NULL;
static platform_add_fd mainloop_platform_add_fd = NULL;
static platform_modify_fd mainloop_platform_modify_fd = NULL;
static platform_remove_fd mainloop_platform_remove_fd = NULL;
static platform_run mainloop_platform_run = NULL;
static platform_destroy mainloop_platform_destroy = NULL;



int mainloop_add_fd(int fd, events_t events, 
                    mainloop_event_func callback, 
                    mainloop_destroy_func destroy,
                    void *user_data)
{
    int ret = 0;
    if (fd < 0 || !callback) {
        return -EINVAL;
    }
    mainloop_data_t *data = mainloop_free_head;
    mainloop_free_remove(data);
    if (!data) {
        data = mainloop_malloc(sizeof(mainloop_data_t));
        if (!data) {
            return -ENOMEM;
        }
    }

    bzero(data, sizeof(mainloop_data_t));  
    data->type = GENERAL;
    data->fd = fd;
    data->event_callback = callback;
    data->destroy_callback = destroy;
    data->user_data = user_data;

    mainloop_data_add(data);

    return mainloop_platform_add_fd(events, data);
}

int mainloop_modify_fd(int fd, events_t events)
{
    if (fd < 0) {
        return -EINVAL;
    }
    printf("modify fd = %d\n", fd);
    mainloop_data_t *data = get_mainloop_data_by_fd(fd);
    if (!data) {
        return -ENXIO;
    }
    printf(".....\n");
    return mainloop_platform_modify_fd(events, data);
}

int mainloop_remove_fd(int fd)
{
    if (fd < 0) {
        return -EINVAL;
    }
    mainloop_data_t *data = get_mainloop_data_by_fd(fd);
    if (!data) {
        return -ENXIO;
    }
    int ret = mainloop_platform_remove_fd(data->fd);
    
    if (data->type == TIMER) {
        close(data->fd);
        data->fd = -1;
    }
    if (data->destroy_callback) {
        data->destroy_callback(data->user_data);
    }
    
    mainloop_data_remove(data);
    mainloop_free_add(data);
    return ret;
}

int mainloop_init()
{
    int ret = 0;
    int i = 0;
    mainloop_data_head = NULL;
    mainloop_free_head = NULL;
    mainloop_free_count = 0;
    mainloop_terminate = 0;

#ifdef USE_EPOLL
    mainloop_platform_init = epoll_init;
    mainloop_platform_add_fd = epoll_add_fd;
    mainloop_platform_modify_fd = epoll_modify_fd;
    mainloop_platform_remove_fd = epoll_remove_fd;
    mainloop_platform_run = epoll_run;
    mainloop_platform_destroy = epoll_destroy;
#elif defined USE_SELECT
    mainloop_platform_init = select_init;
    mainloop_platform_add_fd = select_add_fd;
    mainloop_platform_modify_fd = select_modify_fd;
    mainloop_platform_remove_fd = select_remove_fd;
    mainloop_platform_run = select_run;
    mainloop_platform_destroy = select_destroy;
#endif 
  
    if (!mainloop_platform_init || 
        !mainloop_platform_add_fd ||
        !mainloop_platform_modify_fd||
        !mainloop_platform_remove_fd ||
        !mainloop_platform_run ||
        !mainloop_platform_destroy) {
            fprintf(stderr, "Lost the platform functions.\n");
            return -ENOSYS;
        }
    return mainloop_platform_init();
}

void mainloop_quit()
{
    mainloop_terminate = 1;
    mainloop_data_t *data = mainloop_free_head, *tmp;
    while(data) {
        tmp = data->next;
        mainloop_free(data);
        data = tmp;
    }
}

void mainloop_run()
{
    int ret = 0;
    while(!mainloop_terminate) {
        ret = mainloop_platform_run();    
    }
    
    mainloop_data_t *data = mainloop_data_head, *tmp;
    while(data) {
        tmp = data->next;
        if (data->type == TIMER) {
			printf("...close...\n");
            close(data->fd);
        }
        if (data->destroy_callback) {
            data->destroy_callback(data->user_data);
        }
        mainloop_free(data);
        data = tmp;
    }

    mainloop_platform_destroy();
    return;
}

static inline int timeout_set(int fd, uint32_t msec)
{
    struct itimerspec itimer;
    unsigned int sec = msec / 1000;

    memset(&itimer, 0, sizeof(itimer));
    itimer.it_interval.tv_sec = 0;
    itimer.it_interval.tv_nsec = 0;
    itimer.it_value.tv_sec = sec;
    itimer.it_value.tv_nsec = (msec - (sec * 1000)) * 1000 * 1000;

    return timerfd_settime(fd, 0, &itimer, NULL);
}

int mainloop_add_timeout(uint32_t msec, 
                        mainloop_timeout_func callback,
                        mainloop_destroy_func destroy,
                        void *user_data)
{
    int ret = 0;
    if (!callback) {
        return -EINVAL;
    }
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        return -EIO;
    }
    if (msec > 0) {
        printf("msec = %d\n", msec);
        if (timeout_set(fd, msec) < 0) {
            close(fd);
            return -EIO;
        }
    }

    mainloop_data_t *data = mainloop_free_head;
    mainloop_free_remove(data);
    if (!data) {
        data = mainloop_malloc(sizeof(mainloop_data_t));
        if (!data) {
            return -ENOMEM;
        }
    }

    bzero(data, sizeof(mainloop_data_t));  
    data->type = TIMER;
    data->fd = fd;
    data->timeout_callback = callback;
    data->destroy_callback = destroy;
    data->user_data = user_data;
    events_t event = EVENT_IN;    

    ret = mainloop_platform_add_fd(event, data);

    if (ret < 0) {
        mainloop_free_add(data);
        close(fd);
        return ret;
    }
    mainloop_data_add(data);
    return fd;
}

int mainloop_modify_timeout(int fd, uint32_t msec)
{
    if (msec > 0 && timeout_set(fd, msec) < 0) {
        return -EIO;
    }
    events_t event = EVENT_IN;
    return mainloop_modify_fd(fd, event);
}

int mainloop_remove_timeout(int fd)
{
    return mainloop_remove_fd(fd);
}

#ifdef TEST

#include <sys/time.h>

void timer_destory(void *user_data)
{
    printf("timer_desotry.\n");
    free(user_data);
}

void timer_process(int fd, void *user_data)
{
    static int count = 0;
    struct timeval *old = (struct timeval *)user_data;
    struct timeval now = {0x00};
    gettimeofday(&now, NULL);
    long long diff = (now.tv_usec - old->tv_usec) / 1000 + 
                    (now.tv_sec - old->tv_sec) * 1000;
    printf("count = %d, old = %p, now = %p\n", ++count, old, &now);
    printf("%d The timer diff %lldms\n", fd, diff);
    printf("%d The old time is: %ld-%ld\n", fd, old->tv_sec, old->tv_usec/1000);
    printf("%d The now time is: %ld-%ld\n", fd, now.tv_sec, now.tv_usec/1000);
    printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n\n");
    
    if (count < 10 ) {
        mainloop_modify_timeout(fd, count * 100);
        printf(".......end fd \n");
    } else if (count < 11) {        
        struct timeval *new = malloc(sizeof(struct timeval));
        gettimeofday(new, NULL);
        mainloop_add_timeout(500, timer_process, timer_destory, new);
    } else {
        mainloop_quit();    
    }
    printf("timer_process end.\n");
    return;
}

int main()
{
    struct timeval tv = {0x00};
    gettimeofday(&tv, NULL);
    int ret = mainloop_init();
    if (ret < 0) {
        printf("mainloop_init error(%d): %s\n", ret, strerror(-ret));
        return -1;
    }
    mainloop_add_timeout(500, timer_process, NULL, &tv);
    mainloop_add_timeout(1000, timer_process, NULL, &tv);
    mainloop_run();
    printf("mainloop over.\n\n");
    return 0;
}
#endif
