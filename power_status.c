#define _GNU_SOURCE

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/user.h>
#include <sys/un.h>
#include <linux/types.h>
#include <linux/netlink.h>

#define HOTPLUG_BUFFER_SIZE     1024
#define HOTPLUG_NUM_ENVP        32
#define OBJECT_SIZE         512

int uevent_flag = 0;  //uevent status  1:power, 2:usb add, 3:usb remove

struct uevent {
    void *next;
    char buffer[HOTPLUG_BUFFER_SIZE + OBJECT_SIZE];
    char *devpath;
    char *action;
    char *envp[HOTPLUG_NUM_ENVP];
};

static struct uevent * alloc_uevent (void);
static int open_netlink_socket(void);
static int power_status_event(int sock_id);

int main(int argc, char *argv[])
{ 
    int device_id = 0;

    device_id = open_netlink_socket();
    power_status_event(device_id);

    return 0;
}

static struct uevent * alloc_uevent (void)
{
    return (struct uevent *)malloc(sizeof(struct uevent));
}

static int open_netlink_socket(void)
{
    int sock;
    socklen_t addrlen;
    int retval;
    int rcvbufsz = 128*1024;
    int rcvsz = 0;
    int rcvszsz = sizeof(rcvsz);
    const int feature_on = 1;
    unsigned int *prcvszsz = (unsigned int *)&rcvszsz;
    struct sockaddr_nl snl;
    struct sockaddr_un sun;

    memset(&snl, 0x00, sizeof(struct sockaddr_nl));
    snl.nl_family = AF_NETLINK;
    snl.nl_pid = getpid();
    snl.nl_groups = 0x01;

    sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (sock == -1) {
        printf("error getting socket, exit\n");
        return 1;
    }

    //printf("reading events from kernel.\n");

    retval = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbufsz, sizeof(rcvbufsz));
    if (retval < 0) {
        printf("error setting receive buffer size for socket, exit\n");
        exit(1);
    }
    retval = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvsz, prcvszsz);
    if (retval < 0) {
        printf("error setting receive buffer size for socket, exit\n");
        exit(1);
    }
   // printf("receive buffer size for socket is %u.\n", rcvsz);

    /*  enable receiving of the sender credentials */
    setsockopt(sock, SOL_SOCKET, SO_PASSCRED, &feature_on, sizeof(feature_on));
    retval = bind(sock, (struct sockaddr *) &snl, sizeof(struct sockaddr_nl));
    if (retval < 0) {
        printf("bind failed, exit\n");
    	close(sock);
    	return -1;
    }
   return sock;
}

static int power_status_event(int sock_id)
{
    pthread_attr_t attr;
    struct uevent *uev;
    int i;
    char *pos;
    size_t bufpos;
    ssize_t buflen;
    char *buffer;
    struct msghdr smsg;
    struct iovec iov;
    struct cmsghdr *cmsg;
    struct ucred *cred;
    char cred_msg[CMSG_SPACE(sizeof(struct ucred))];
    static char buf[HOTPLUG_BUFFER_SIZE + OBJECT_SIZE];

    memset(buf, 0x00, sizeof(buf));
    iov.iov_base = &buf;
    iov.iov_len = sizeof(buf);
    memset (&smsg, 0x00, sizeof(struct msghdr));
    smsg.msg_iov = &iov;
    smsg.msg_iovlen = 1;
    smsg.msg_control = cred_msg;
    smsg.msg_controllen = sizeof(cred_msg);
	
    while(1) {
        buflen = recvmsg(sock_id, &smsg, 0);
        if (buflen < 0) {
            if (errno != EINTR)
                printf("error receiving message.\n");
            continue;
        }
        cmsg = CMSG_FIRSTHDR(&smsg);
        if (cmsg == NULL || cmsg->cmsg_type != SCM_CREDENTIALS) {
            printf("no sender credentials received, message ignored\n");
            continue;
        }
        cred = (struct ucred *)CMSG_DATA(cmsg);
        if (cred->uid != 0) {
            printf("sender uid=%d, message ignored\n", cred->uid);
            continue;
        }
        /* skip header */
        bufpos = strlen(buf) + 1;
        if (bufpos < sizeof("a@/d") || bufpos >= sizeof(buf)) {
            printf("invalid message length\n");
            continue;
        }
        /* check message header */
        if (strstr(buf, "@/") == NULL) {
            printf("unrecognized message header");
            continue;
        }
        uev = alloc_uevent();
        if (!uev) {
            printf("lost uevent, oom");
            continue;
        }
        if ((size_t)buflen > sizeof(buf)-1)
            buflen = sizeof(buf)-1;
        /*
         * Copy the shared receive buffer contents to buffer private
         * to this uevent so we can immediately reuse the shared buffer.
         */
        memcpy(uev->buffer, buf, HOTPLUG_BUFFER_SIZE + OBJECT_SIZE);
        buffer = uev->buffer;
        buffer[buflen] = '\0';

        /* save start of payload */
        bufpos = strlen(buffer) + 1;

        /* action string */
        uev->action = buffer;
        pos = strchr(buffer, '@');
        if (!pos) {
            printf("bad action string '%s'", buffer);
            continue;
        }
        pos[0] = '\0';

        /* sysfs path */
        uev->devpath = &pos[1];

        /* hotplug events have the environment attached - reconstruct envp[] */
        for (i = 0; (bufpos < (size_t)buflen) && (i < HOTPLUG_NUM_ENVP-1); i++) {
            int keylen;
            char *key;

            key = &buffer[bufpos];
            keylen = strlen(key);
            uev->envp[i] = key;
            bufpos += keylen + 1;
        }
        uev->envp[i] = NULL;
        //printf("uevent '%s' from '%s'\n", uev->action, uev->devpath);
#if 1
	if (strncmp(uev->action, "change", 6) == 0) {
		uevent_flag = 1;
	}
	else if(strncmp(uev->action, "add", 3) == 0) {
		uevent_flag = 2;
	}
	else if(strncmp(uev->action, "remove", 6) == 0) {
		uevent_flag = 3;
	}
	switch (uevent_flag)
	{
		case 1:
        		for (i = 0; uev->envp[i] != NULL; i++) {
				if ((strncmp(uev->envp[i], "POWER_SUPPLY_STATUS=Charging", 28) == 0) || strncmp(uev->envp[i], "POWER_SUPPLY_ONLINE=1", 21) == 0) 
				{
					printf("Power Status is changed: Charging\n");
				}
				else if((strncmp(uev->envp[i], "POWER_SUPPLY_STATUS=Discharging", 31) == 0) || strncmp(uev->envp[i], "POWER_SUPPLY_ONLINE=0", 21) == 0) 
				{
					printf("Power Status is changed: Discharging\n");
				}
			}
			break;
		case 2:
			printf("usb is add\n");
			break;
		case 3:
			printf("usb removed\n");
			break;
		default:
			break;
	}
#endif
    }
}
