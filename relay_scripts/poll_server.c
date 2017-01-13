/**
 * [Title]: poll_server.c -- calculate the one way delays of the other VMs
 * [Author]: Dimitris Mavrommatis (mavromat@ics.forth.gr) -- @inspire_forth
 * -----------------------------------------------------------------------------------------------------------
 * [Details]: 
 * A C program that connects to other VMs and requests for timestamps in order to calculate the one way delay.
 * The results are then send to the CXP Controller which is going to save them and use them in order to path
 * stich the lowest latency path when needed.
 * 
 * This is an experimental program that uses polling to achieve less overhead than the other version.
 * -----------------------------------------------------------------------------------------------------------
 * [Warning]:
 * This script comes as-is with no promise of functionality or accuracy. I did not write it to be efficient nor 
 * secured. Feel free to change or improve it any way you see fit.
 * -----------------------------------------------------------------------------------------------------------   
 * [Modification, Distribution, and Attribution]:
 * You are free to modify and/or distribute this script as you wish. I only ask that you maintain original
 * author attribution.
**/

#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/time.h>

char *serverName, **names, *serverIp;
double *delays;
volatile int ping_requests;
volatile int interrupt_occured = 0;
int total_servers;

typedef double elem_type ;

pthread_mutex_t lock;

#define ELEM_SWAP(a,b) { register elem_type t=(a);(a)=(b);(b)=t; }

double quick_select_median(double arr[], uint16_t n)
{
    uint16_t low, high ;
    uint16_t median;
    uint16_t middle, ll, hh;
    low = 0 ; high = n - 1 ; median = (low + high) / 2;
    for (;;) {
        if (high <= low) /* One element only */
            return arr[median] ;
        if (high == low + 1) { /* Two elements only */
            if (arr[low] > arr[high])
                ELEM_SWAP(arr[low], arr[high]) ;
            return arr[median] ;
        }
        /* Find median of low, middle and high items; swap into position low */
        middle = (low + high) / 2;
        if (arr[middle] > arr[high])
            ELEM_SWAP(arr[middle], arr[high]) ;
        if (arr[low] > arr[high])
            ELEM_SWAP(arr[low], arr[high]) ;
        if (arr[middle] > arr[low])
            ELEM_SWAP(arr[middle], arr[low]) ;
        /* Swap low item (now in position middle) into position (low+1) */
        ELEM_SWAP(arr[middle], arr[low + 1]) ;
        /* Nibble from each end towards middle, swapping items when stuck */
        ll = low + 1;
        hh = high;
        for (;;) {
            do ll++; while (arr[low] > arr[ll]) ;
            do hh--; while (arr[hh] > arr[low]) ;
            if (hh < ll)
                break;
            ELEM_SWAP(arr[ll], arr[hh]) ;
        }
        /* Swap middle item (in position low) back into correct position */
        ELEM_SWAP(arr[low], arr[hh]) ;
        /* Re-set active partition */
        if (hh <= median)
            low = ll;
        if (hh >= median)
            high = hh - 1;
    }
    return arr[median] ;
}


double timeval_diff(struct timeval * tv0, struct timeval * tv1)
{
    double time1, time2;

    time1 = tv0->tv_sec + (tv0->tv_usec / 1000000.0);
    time2 = tv1->tv_sec + (tv1->tv_usec / 1000000.0);

    time1 = time1 - time2;
    if (time1 < 0)
        time1 = -time1;
    return time1;
}

typedef struct ip_name {
    char *name;
    char *ip;
    int id;
} ipname;

void alarm_sig_handler (int sig_no, siginfo_t* info, void *vcontext)
{
    ucontext_t *context = (ucontext_t*)vcontext;
    int sockfd;
    char buffer[100];
    struct sockaddr_in servaddr, cliaddr;
    int i;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(serverIp);
    servaddr.sin_port = htons(32032);

    sprintf(buffer, "%s ", serverName);
    for (i = 0; i < total_servers; i++) {
        strcat(buffer, names[i]);
        sprintf(buffer, "%s:%f ", buffer, delays[i]);
    }
    sprintf(buffer, "%send ", buffer);
    printf("buffer: %s\n", buffer);
    sendto(sockfd, buffer, 20 * total_servers * sizeof(char), 0,
           (struct sockaddr *)&servaddr, sizeof(servaddr));

    interrupt_occured = 1;
}

unsigned int interrupt_enable ()
{
    struct itimerval timer;
    /* install signal handler */
    struct sigaction sig_action;
    memset(&sig_action, 0, sizeof(sig_action));
    sig_action.sa_sigaction = alarm_sig_handler;
    sig_action.sa_flags = SA_NODEFER | SA_SIGINFO;
    sigemptyset(&sig_action.sa_mask);
    if ( sigaction(SIGALRM, &sig_action, NULL) < 0 )
        return 0;
    /* install timer */
    timer.it_interval.tv_usec = 0;
    timer.it_interval.tv_sec = 20;
    timer.it_value.tv_usec = 0;
    timer.it_value.tv_sec = 20;
    if ( setitimer (ITIMER_REAL, &timer, NULL) < 0 )
        return 0;
    return 1;
}

void * one_way_client(void * ptr)
{
    ipname *k;
    int sockfd, i, timeout, preceived = 0, psend = 0, nfds = 1, ret, finished_clients = 0;
    struct sockaddr_in servaddr, cliaddr;
    struct timeval before, arrival_time, received_time;
    char * buf;
    uint32_t sec, usec;
    double first_trip, second_trip, ping, drift;
    double avg_rtt[10], avg_forward[10], avg_reverse[10];
    FILE *fptr;
    time_t timer;
    char buffer[26];
    struct tm* tm_info;
    char *dir;
    struct pollfd *fds;
    size_t msgsize = sizeof(uint32_t);
    k = (ipname *) ptr;

    if ( ( sockfd = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP ) ) < 0 ) {
        perror("one_way_client socket");
        exit( EXIT_FAILURE );
    }

    memset(&servaddr, 0, sizeof(struct sockaddr_in));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(k->ip);
    servaddr.sin_port = htons(32000);

    if ( connect( sockfd, (struct sockaddr *) &servaddr, sizeof(struct sockaddr_in) ) < 0 ) {
        perror("one_way_client connect");
        exit(EXIT_FAILURE);
    }

    buf = (unsigned char *) malloc ( 4 * msgsize );

    fds = (struct pollfd*) malloc (sizeof(struct pollfd));
    fds->fd = sockfd;
    fds->events = POLLIN | POLLOUT;
    timeout = ( 5000 );

    time(&timer);
    tm_info = localtime(&timer);

    strftime(buffer, 26, "%Y:%m:%d %H:%M:%S", tm_info);
    dir = strdup("./logs/");
    strcat(dir, k->name);
    fptr = fopen( dir, "a");
    fprintf(fptr, "%s\n", buffer);
    fprintf(fptr, "RTT/forward/reverse delays\n");

    do {
        // printf("Waiting on poll...\n");
        if ( (ret = poll( fds, nfds, timeout )) < 0 )
            perror("one_way_client poll");
        else if (ret == 0) {
            printf("time out occured\n");
            fds->events = 0 | POLLOUT;
        }

        if ( fds->revents & POLLIN ) {

            if ( ( ret = recvfrom( fds->fd, buf,  4 * msgsize, 0, NULL, NULL ) ) < 0 )
                perror("one_way_client recv");
            // printf("one_way_client recv %s\n", k->name);

            gettimeofday(&arrival_time, 0);

            memcpy( &sec, buf, msgsize );
            memcpy( &usec, buf + msgsize, msgsize );
            sec = ntohl(sec);
            usec = ntohl(usec);
            before.tv_sec = sec;
            before.tv_usec = usec;

            memcpy( &sec, buf + 2 * msgsize, msgsize );
            memcpy( &usec, buf + 3 * msgsize, msgsize );
            sec = ntohl(sec);
            usec = ntohl(usec);
            received_time.tv_sec = sec;
            received_time.tv_usec = usec;

            first_trip = 1000. * timeval_diff(&received_time, &before);
            ping = 1000.*(timeval_diff(&arrival_time, &before));
            second_trip = 1000.*(timeval_diff(&arrival_time, &received_time));

            drift = (first_trip + second_trip) / ping;

            avg_rtt[preceived] = ping  * 100.;
            if ( drift >= 1.0f ) {
                avg_forward[preceived] = first_trip / drift * 100.;
                avg_reverse[preceived] = second_trip / drift * 100.;
            } else {
                avg_forward[preceived] = first_trip * 100.;
                avg_reverse[preceived] = second_trip  * 100.;
            }

            fprintf(fptr, "%f / %f / %f\n", avg_rtt[preceived] / 100., avg_forward[preceived] / 100., avg_reverse[preceived] / 100.);
            delays[k->id] = quick_select_median(avg_forward, preceived + 1) / 100.;

            if ( ++preceived == 10 ) {
                printf("%s finished\n", k->name);
                finished_clients++;
                break;
            }

            fds->events = 0 | POLLOUT;
        }
        else if ( fds->revents & POLLOUT ) {
            gettimeofday(&before, 0);
            sec = htonl(before.tv_sec);
            usec = htonl(before.tv_usec);

            memcpy( buf, &sec, msgsize);
            memcpy( buf + msgsize, &usec, msgsize);

            // printf("one_way_client send %s\n", k->name);
            if ( ( ret = sendto( fds->fd, buf, 2 * msgsize, 0, (struct sockaddr *)&servaddr, sizeof(servaddr) ) ) < 0 )
                perror("one_way_client send");

            fds->events = 0 | POLLIN;
        }
    } while ( 1 );

    close( sockfd );
    fclose(fptr);
}

void * one_way_server( void * ptr ) {
    int sockfd = -1;
    struct sockaddr_in local_addr, remote_addr;
    socklen_t remote_addr_len = sizeof(remote_addr), local_addr_len = sizeof(local_addr);
    char * buf;
    uint32_t sec, usec;
    struct timeval arrival_time, received_time;
    double first_trip;
    struct pollfd *fds;
    int nfds = 1, timeout, ret;
    int on = 1;
    size_t msgsize = sizeof(uint32_t);

    buf = (unsigned char *) malloc ( 4 * msgsize );

    if ( ( sockfd = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP ) ) < 0 ) {
        perror("one_way_server socket");
        exit( EXIT_FAILURE );
    }

    if ( setsockopt( sockfd, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on) ) < 0)
        perror("one_way_server setsockopt");

    memset( &local_addr, 0, sizeof( local_addr ) );
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(32000);

    if ( bind( sockfd, ( struct sockaddr * ) &local_addr, sizeof( local_addr ) ) < 0 )
        perror("one_way_server bind");

    fds = (struct pollfd*) malloc (sizeof(struct pollfd));
    fds->fd = sockfd;
    fds->events = POLLIN;
    timeout = ( -1 );

    pthread_mutex_unlock( &lock );

    do {
        if ( (ret = poll( fds, nfds, timeout )) < 0 )
            perror("one_way_server poll");

        if ( fds->revents & POLLIN )
        {
            do {
                if ( ( ret = recvfrom( fds->fd, buf, 2 * msgsize, 0, (struct sockaddr *)&remote_addr, &remote_addr_len ) ) < 0 ) {
                    perror("one_way_server recv");
                }
            } while ( ret == 0 );
            // printf("one_way_server recv %s\n", serverName);

            gettimeofday(&arrival_time, 0);
            sec = htonl(arrival_time.tv_sec);
            usec = htonl(arrival_time.tv_usec);
            memcpy( buf + 2 * msgsize, &sec, msgsize);
            memcpy( buf + 3 * msgsize, &usec, msgsize);

            if ( ( ret = sendto( fds->fd, buf, 4 * msgsize, 0,  (struct sockaddr *)&remote_addr, sizeof(remote_addr) ) ) < 0 ) {
                perror("one_way_server send");
            } else {
                // printf("one_way_server send %s\n", serverName);
                ping_requests++;
            }

        }

    } while ( 1 );

    close( sockfd );
    return NULL;

}

int main(int argc, char**argv)
{
    pthread_t *client_threads;
    pthread_t server_thread;
    ipname * k;
    int i, j;
    char *ptr, **ips, **tmp;

    printf("Arguments:\n");
    for (i=0;i<argc;i++) {
        printf("\targv[%d]: %s\n",i,argv[i]);
    }

    if ( pthread_mutex_init(&lock, NULL) != 0 )
    {
        printf("Mutex init failed\n");
        exit(EXIT_FAILURE);
    }

    ping_requests = 0;
    total_servers = atoi(argv[1]);

    tmp = (char **) malloc (total_servers * sizeof(char *));
    names = (char **) malloc (total_servers * sizeof(char *));
    ips = (char **) malloc (total_servers * sizeof(char *));
    delays = (double *) calloc (total_servers, sizeof(double));

    serverName = strdup(argv[2]);
    serverIp = strdup(argv[4]);

    i = 0;
    ptr = strtok(argv[3], "|");
    while ( ptr ) {
        tmp[i++] = strdup(ptr);
        ptr = strtok(NULL, "|");
    }

    for (j = 0; j < i ; j++) {
        ptr = strtok(tmp[j], ":");
        while ( ptr ) {
            names[j] = strdup(ptr);
            ptr = strtok(NULL, ":");
            ips[j] = strdup(ptr);
            ptr = strtok(NULL, ":");
        }
    }

    pthread_mutex_lock(&lock);
    pthread_create( &server_thread, NULL, one_way_server, (void *) NULL );
    sleep(1);
    client_threads = (pthread_t *) malloc ( total_servers * sizeof(pthread_t));
    for ( i = 0; i < total_servers; i++ ) {
        k = (ipname *) malloc (sizeof(ipname));
        k->name = strdup(names[i]);
        k->ip = strdup(ips[i]);
        k->id = i;
        pthread_create( &client_threads[i], NULL, one_way_client, (void *) k );
    }

    interrupt_enable();

    while(interrupt_occured == 0);
    printf("exiting\n");

    return 0;
}

