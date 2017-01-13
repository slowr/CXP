/*
 [Title]: server.c -- calculate the one way delays of the other VMs
 [Author]: Dimitris Mavrommatis (mavromat@ics.forth.gr) -- @inspire_forth
-------------------------------------------------------------------------------------------------------------
 [Details]: 
 A C program that connects to other VMs and requests for timestamps in order to calculate the one way delay.
 The results are then send to the CXP Controller which is going to save them and use them in order to path
 stich the lowest latency path when needed.
-------------------------------------------------------------------------------------------------------------
 [Warning]:
 This script comes as-is with no promise of functionality or accuracy. I did not write it to be efficient nor 
 secured. Feel free to change or improve it any way you see fit.
-------------------------------------------------------------------------------------------------------------   
 [Modification, Distribution, and Attribution]:
 You are free to modify and/or distribute this script as you wish.  I only ask that you maintain original
 author attribution.
*/

#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

char *serverName;
double *delays;

typedef double elem_type ;

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

void * one_way_client(void * ptr)
{
    ipname *k;
    int sockfd, n, i;
    struct sockaddr_in servaddr, cliaddr;
    struct timeval before, arrival_time, received_time;
    char * buf, * tmp;
    uint32_t sec, usec;
    double first_trip, second_trip, ping, drift;
    double avg_rtt[10], avg_forward[10], avg_reverse[10];
    FILE *fptr;

    k = (ipname *) ptr;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(k->ip);
    servaddr.sin_port = htons(32000);

    buf = (unsigned char *) malloc (4 * sizeof(uint32_t));

    for (;;) {
        for ( i = 0; i < 10 ; i++ ) {
            gettimeofday(&before, 0);
            sec = htonl(before.tv_sec);
            usec = htonl(before.tv_usec);

            memcpy( buf, &sec, sizeof(sec));
            memcpy( buf + sizeof(sec), &usec, sizeof(usec));
            sendto(sockfd, buf, 2 * sizeof(uint32_t), 0,
                   (struct sockaddr *)&servaddr, sizeof(servaddr));

            recvfrom(sockfd, buf, 4 * sizeof(uint32_t), 0, NULL, NULL);

            gettimeofday(&arrival_time, 0);

            memcpy( &sec, buf + 2 * sizeof(uint32_t), sizeof(sec) );
            memcpy( &usec, buf + 3 * sizeof(uint32_t), sizeof(usec) );
            sec = ntohl(sec);
            usec = ntohl(usec);
            received_time.tv_sec = sec;
            received_time.tv_usec = usec;

            memcpy( &sec, buf, sizeof(sec) );
            memcpy( &usec, buf + sizeof(uint32_t), sizeof(usec) );
            sec = ntohl(sec);
            usec = ntohl(usec);
            before.tv_sec = sec;
            before.tv_usec = usec;

            first_trip = 1000. * timeval_diff(&arrival_time, &received_time);
            ping = 1000.*(timeval_diff(&arrival_time, &before));
            second_trip = 1000.*(timeval_diff(&arrival_time, &received_time));

            drift = (first_trip + second_trip) / ping;
            // if ( drift < 0 ) drift = -drift;

            avg_rtt[i] = ping;
            if ( drift >= 1.0f ) {
                avg_forward[i] = first_trip / drift;
                avg_reverse[i] = second_trip / drift;
            } else {
                avg_forward[i] = first_trip;
                avg_reverse[i] = second_trip;
            }
        }

        delays[k->id] = quick_select_median(avg_forward, 10);

        sleep(10);
    }
}

void * one_way_server( void * ptr ) {
    int sockfd, n;
    struct sockaddr_in servaddr, cliaddr;
    socklen_t len;
    char * buf;
    uint32_t sec, usec;
    struct timeval arrival_time, received_time;
    double first_trip;

    buf = (unsigned char *) malloc (4 * sizeof(uint32_t));

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(32000);

    bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr));

    for (;;)
    {
        len = sizeof(cliaddr);

        recvfrom(sockfd, buf, 2 * sizeof(uint32_t), 0, (struct sockaddr *)&cliaddr, &len);

        gettimeofday(&arrival_time, 0);
        sec = htonl(arrival_time.tv_sec);
        usec = htonl(arrival_time.tv_usec);
        memcpy( buf + 2 * sizeof(uint32_t), &sec, sizeof(sec));
        memcpy( buf + 3 * sizeof(uint32_t), &usec, sizeof(usec));

        sendto(sockfd, buf, 3 * sizeof(uint32_t), 0, (struct sockaddr *)&cliaddr, sizeof(cliaddr));
    }
}

int main(int argc, char ** argv)
{
    pthread_t *client_threads;
    pthread_t server_thread;
    char buffer[100];
    int sockfd, n;
    struct sockaddr_in servaddr, cliaddr;
    ipname * k;
    int i, j, total_servers;
    char *ptr, **names, **ips, **tmp;

    total_servers = atoi(argv[1]);

    printf("Arguments:\n");
    for (i=0;i<argc;i++) {
        printf("\targv[%d]: %s\n",i,argv[i]);
    }

    tmp = (char **) malloc (total_servers * sizeof(char *));
    names = (char **) malloc (total_servers * sizeof(char *));
    ips = (char **) malloc (total_servers * sizeof(char *));
    delays = (double *) malloc (total_servers * sizeof(double));

    serverName = strdup(argv[2]);

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

    
    pthread_create( &server_thread, NULL, one_way_server, (void *) NULL );
    printf("one_way_server started\n");
    sleep(5);

    client_threads = (pthread_t *) malloc ( total_servers * sizeof(pthread_t));
    for ( i = 0; i < total_servers; i++ ) {
        k = (ipname *) malloc (sizeof(ipname));
        k->name = strdup(names[i]);
        k->ip = strdup(ips[i]);
        k->id = i;
        pthread_create( &client_threads[i], NULL, one_way_client, (void *) k );
    }
    printf("one_way_client started\n");

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(argv[4]);
    servaddr.sin_port = htons(32032);

    sleep(5);
    sprintf(buffer, "%s ", serverName);
    for (i = 0; i < total_servers; i++) {
        strcat(buffer, names[i]);
        sprintf(buffer, "%s:%f ", buffer, delays[i]);
    }
    sprintf(buffer, "%send ", buffer);
    printf("buffer: %s\n", buffer);
    sendto(sockfd, buffer, 20 * total_servers * sizeof(char), 0,
           (struct sockaddr *)&servaddr, sizeof(servaddr));

    return 0;
}


