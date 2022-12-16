/*
 * brief: fast async network interfaces, short name 'fan'
 * author: pycai
 * time:  2022.02.19 10:00
 * */

#include <netinet/in.h>

typedef void (*FANCallback) (int ret, void* usr);

typedef union 
{ 
    struct sockaddr_in sa4;
    struct sockaddr_in6 sa6;
} FANAddr;

typedef struct
{
    int sendEpollCount;
    int recvEpollCount;
    int sendQueueLength;
    int recvQueueLength;
    int epollWaitTime;
    int maxCallbackTime;
    int checkTimeoutInterval;
} FANConfig;

int FANInit(FANConfig cfg);

int FanRegister(int skt, int isCbLock);

int FanUnregister(int skt);

int FANSend(int skt, char* buf, int len, FANCallback cb, void* usr);

int FANRecv(int skt, char* buf, int len, FANCallback cb, void* usr);

int FANSendTo(int skt, char* buf, int len, FANAddr addr, FANCallback cb, void* usr);

int FANRecvFrom(int skt, char* buf, int len, FANAddr* addr, FANCallback cb, void* usr);

