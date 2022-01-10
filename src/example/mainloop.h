#ifndef MAINLOOP_H
#define MAINLOOP_H

typedef enum events_enum {
    EVENT_IN    =     0x01,
    EVENT_OUT   =     0x02,
    EVENT_ERR   =     0x04,
}events_t;

typedef void (*mainloop_destroy_func) (void *user_data);
typedef void (*mainloop_event_func) (int fd, events_t events, void *user_data);
typedef void (*mainloop_timeout_func) (int fd, void *user_data);

int mainloop_init();
void mainloop_run();
void mainloop_quit();

int mainloop_add_fd(int fd, events_t events, 
                    mainloop_event_func callback, 
                    mainloop_destroy_func destroy,
                    void *user_data);
int mainloop_modify_fd(int fd, events_t events);
int mainloop_remove_fd(int fd);

int mainloop_add_timeout(uint32_t msec, 
                        mainloop_timeout_func callback,
                        mainloop_destroy_func destroy,
                        void *user_data);
int mainloop_modify_timeout(int fd, uint32_t msec);
int mainloop_remove_timeout(int fd);

#endif