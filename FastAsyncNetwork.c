/*
 * create by pycai at 2022-12-10
 * */

/*includes*/
#include "FastAsyncNetwork.h"
#include <pthread.h>

/*pre-define*/
#define SKT_CNT 65536 /*from 0 to 65535*/
#define SEND_SIZE 16384 /*16KB*/

#define LOG_ERROR(fmt, ...) /**/
#define LOG_INFO(fmt, ...)  /**/
#define LOG_DEBUG(fmt, ...) /**/

/*data-define*/
typedef struct
{
    int isCbLock;
    uint64_t useCount;
    pthread_mutex sendMutex;
    pthread_mutex recvMutex;
} Binder;

/*socket references*/
static FANConfig g_config = {0};
static Binder g_binders[SKT_CNT] = {0};
static uint64_t g_sendQueues[SKT_CNT] = {0};
static uint64_t g_recvQueues[SKT_CNT] = {0};

/*implement*/

static void HandleEvents(int skt, uint32_t events, uint64_t queue, uint32_t targetEvents)
{
    if (!(events&targetEvents || events&EPOLLHUP || events&EPOLLERR))
    {
        LOG_ERROR("events[%u] error", events);
        return;
    }
    
    QueueData data = {0};
    if (QueuePopFront(queue, &data) != 0)
    {
        LOG_ERROR("queue is empty for targetEvents[%u]", targetEvents);
        return;
    }
    
    pthread_mutex_t* mtx = ((data.type == SEND || data.type == SEND_TO) ? &g_binders[skt].sendMutex : &g_binders[skt].recvMutex);
    int isCbLock = g_binders[skt].isCbLock;
    switch(data.type)
    {
        case SEND:
        case SEND_TO:
        {
            struct sockaddr* pAddr = (struct sockaddr*)(data.type == SEND ? 0 : &data.addr);
            socklen_t addrLen = pAddr == 0 ? 0 : pAddr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
            int ret = sendto(data.skt, data.buf+data.hdrLen, data.len-data.hdrLen, 0, pAddr, addrLen);
            int err = errno;
            if (ret > 0 || (ret == 0 && (err == EAGAIN || err == EWOULDBLOCK)))
            {
                data.hdrLen += ret;
                if (data.hdrLen < data.len) // finish some part
                {
                    QueuePushFront(queue, &data);
                    return;
                }
            }
            else
            {
                data.retErr = (err != 0 ? err : ret); 
            }
            break;
        }
        case RECV:
        case RECV_FROM:
        {
            struct sockaddr* pAddr = (struct sockaddr*)(data.type == RECV ? 0 : data.pAddr);
            socklen_t addrLen = pAddr == 0 ? 0 : pAddr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
            int ret = recvfrom(data.skt, data.buf, data.len, 0, pAddr, addrLen);
            int err = errno;
            if (ret > 0)
            {
                data.hdrLen = ret;
            }
            else
            {
                data.retErr = (err != 0 ? err : ret);
            }
        }
        default:
            break;
    }
    
    // callback
    if (isCbLock) LOCK(mtx);
    if (g_binders[skt].useCount != data.useCount)
    {
        // may unregister, most time is bigger, when useCount over MAX_INT64 may smaller.
    }
    else
    {
        data.cb(data.retErr, data.hdrLen, data.usr);
    }
    if (isCbLock) UNLOCK(mtx);
}

static void* AsyncLoop(void* arg)
{
    int epfdIndex = (int)(unsigned long)arg;
    int epollCount = g_config.sendEpollCount + g_config.recvEpollCount;
    if (epfdIndex < 0 || epfdIndex >= epollCount)
    {
        LOG_ERROR("epfdIndex error");
        return 0;
    }
    
    int epfd = g_epfds[epfdIndex];
    if (epfd < 0)
    {
        LOG_ERROR("epfd < 0");
        return 0;
    }
    
    int isSender = (epfdIndex < g_config.sendEpollCount ? 1 : 0);
    
    while (1)
    {
        struct epoll_event e = {0};
        int ret = epoll_wait(ptr->epfd, &e, 1, g_config.epollWaitTime);
        if (ret < 0)
        {
            int err = errno;
            LOG_ERROR("epoll_wait failed, ret[%d], errno[%d], msg[%s]", ret, err, strerror(err));
            break;
        }
        
        if (ret == 0)
        {
            int err = errno;
            LOG_DEBUG("epoll_wait may timeout, ret[0], errno[%d], msg[%s]", err, strerror(err));
            continue;
        }
        
        int skt = e.data.fd;
        uint32_t events = e.events;
        if (skt < 0 || skt >= SKT_CNT)
        {
            LOG_ERROR("skt from epoll is error");
            continue;
        }
        
        uint64_t queue = (isSender ? g_sendQueues[skt] : g_recvQueues[skt]);
        uint32_t targetEvents = (isSender ? EPOLLOUT : EPOLLIN);
        HandleEvents(skt, events, queue, targetEvents);
    }
    
    free(ptr);
    return 0;
}

static int SyncRecv(int type, int skt, char* buf, int len, FANAddr* addr, FANCallback cb, void* usr)
{
    if ((type != RECV && type != RECV_FROM) || skt < 0 || skt >= SKT_CNT || buf == 0 || len <= 0 || addr == 0 || cb == 0 || usr == 0)
    {
        LOG_ERROR("input params error");
        return -1;
    }
    
    QueueData data = {0};
    data.skt = skt;
    data.buf = buf;
    data.len = len;
    data.pAddr = addr;
    data.cb = cb;
    data.usr = usr;
    data.type = type;
    data.useCount = g_binders[skt].useCount;
    uint64_t queue = g_recvQueues[skt];
    return QueuePushBack(queue, &data, 1);
}
static int SyncSend(int type, int skt, char* buf, int len, FANAddr* addr, FANCallback cb, void* usr)
{    
    if ((type != SEND && type != SEND_TO) || skt < 0 || skt >= SKT_CNT || buf == 0 || len <= 0 || cb == 0 || usr == 0)
    {
        LOG_ERROR("input params error");
        return -1;
    }
    
    int cnt = len / SEND_SIZE;
    int remain = len % SEND_SIZE;
    if (remain != 0) cnt++;
    QueueData* data = (QueueData*)malloc(cnt * sizeof(QueueData));
    if (data == 0)
    {
        LOG_ERROR("malloc failed");
        return -1;
    }
    memset(data, 0, cnt * sizeof(QueueData));
    
    for (int i = 0; i < cnt; i++)
    {
        data[i].buf = (char*)malloc(SEND_SIZE);
        if (data[i].buf != 0) continue;
        for (int j = 0; j < i; j++) free(data[j].buf);
        free(data);
        LOG_ERROR("malloc failed");
        return -1;
    }
    for (int i = 0; i < cnt; i++)
    {
        data[i].skt = skt;
        data[i].len = ((remain != 0 && i == cnt-1) ? remain : SEND_SIZE);
        memcpy(data[i].buf, (buf + i * SEND_SIZE), data[i].len);
        if (addr != 0) data[i].addr = *addr;
        data[i].cb = cb;
        data[i].usr = usr;
        data[i].type = type;
        data[i].useCount = g_binders[skt].useCount;
    }
    uint64_t queue = g_sendQueues[skt];
    int ret = QueuePushBack(queue, data, cnt);
    if (ret != 0) for (int i = 0; i < cnt; i++) free(data[i].buf);
    free(data);
    return ret;
}

/*interfaces*/

int FANInit(FANConfig cfg)
{
    g_config = cfg;
    return 0;
}

int FanRegister(int skt, int isCbLock)
{
    if (skt < 0 || sdk >= SKT_CNT)
    {
        LOG_ERROR("input params error");
        return -1;
    }
    
    __sync_add_and_fetch(&g_binders[skt].useCount, 1);
    g_binders[skt].isCbLock = isCbLock;
    return 0;
}

int FanUnregister(int skt)
{
    if (skt < 0 || sdk >= SKT_CNT)
    {
        LOG_ERROR("input params error");
        return -1;
    }
    
    __sync_add_and_fetch(&g_binders[skt].useCount, 1);

    MUTEX_LOCK(&g_binders[skt].sendMutex);
    MUTEX_UNLOCK(&g_binders[skt].sendMutex);
    MUTEX_LOCK(&g_binders[skt].recvMutex);
    MUTEX_UNLOCK(&g_binders[skt].recvMutex);
    
    QueueClearup(g_sendQueues[skt]);
    QueueClearup(g_recvQueues[skt]);
    
    return 0;
}

int FANSend(int skt, char* buf, int len, FANCallback cb, void* usr)
{    
    return SyncSend(SEND, skt, buf, len, 0, cb, usr);
}
int FANSendTo(int skt, char* buf, int len, FANAddr addr, FANCallback cb, void* usr)
{
    return SyncSend(SEND_TO, skt, buf, len, &addr, cb, usr);
}

int FANRecv(int skt, char* buf, int len, FANCallback cb, void* usr)
{
    return SyncRecv(RECV, skt, buf, len, 0, cb, usr);
}
int FANRecvFrom(int skt, char* buf, int len, FANAddr* addr, FANCallback cb, void* usr)
{
    return SyncRecv(RECV_FROM, skt, buf, len, addr, cb, usr);
}

