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

