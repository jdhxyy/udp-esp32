// Copyright 2021-2023 The jdh99 Authors. All rights reserved.
// UDP收发模块
// Authors: jdh99 <jdh821@163.com>

#include "udp.h"
#include "bror.h"
#include "lagan.h"
#include "async.h"
#include "tzmalloc.h"
#include "kuggis.h"
#include "tzlist.h"

#include "lwip/sockets.h"

#define TAG "udp"
#define THREAD_SIZE 4096

#pragma pack(1)

// 观察者回调函数
typedef struct {
    TZNetDataFunc callback;
} tItem;

typedef struct {
    uint32_t ip;
    uint16_t port;
} tRxTag;

#pragma pack()

static int gMid = -1;

// 存储观察者列表
static intptr_t gList = 0;

// 本地端口
static int gSock = -1;

static uint16_t gLocalPort = 0;

// 接收缓存
static intptr_t gFifo = 0;
static uint8_t* gTxFrame = NULL;
static uint8_t* gRxFrame = NULL;
static int gFrameLenMax = 0;

static int task(void);
static void notifyObserver(void);
static void rxThread(void* param);
static bool isObserverExist(TZNetDataFunc callback);
static TZListNode* createNode(void);

// UdpLoad 模块载入
// 载入之前需初始化nvs_flash_init,esp_netif_init,esp_event_loop_create_default
// frameLenMax是最大帧长.fifoSize是接收缓存的大小
bool UdpLoad(int frameLenMax, int fifoSize) {
    gMid = TZMallocRegister(0, TAG, frameLenMax * 2 + fifoSize + 1024);
    if (gMid == -1) {
        LE(TAG, "load failed!malloc failed");
        return false;
    }
    gList = TZListCreateList(gMid);
    if (gList == 0) {
        LE(TAG, "load failed!create gList failed");
        return false;
    }

    gSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (gSock < 0) {
        LE(TAG, "bind failed!create socket failed");
        return false;
    }

    gFrameLenMax = frameLenMax;
    gTxFrame = TZMalloc(gMid, frameLenMax);
    if (gTxFrame == NULL) {
        LE(TAG, "load failed!malloc gTxFrame failed");
        return false;
    }
    gRxFrame = TZMalloc(gMid, frameLenMax);
    if (gRxFrame == NULL) {
        LE(TAG, "load failed!malloc gRxFrame failed");
        return false;
    }

    gFifo = KuggisCreate(gMid, fifoSize);
    if (gFifo == 0) {
        LE(TAG, "load failed!create rx fifo failed");
        return false;
    }
    
    LI(TAG, "socket created");

    AsyncStart(task, ASYNC_NO_WAIT);
    BrorThreadCreate(rxThread, "rxThread", BROR_THREAD_PRIORITY_MIDDLE, THREAD_SIZE);
    return true;
}

static int task(void) {
    static struct pt pt = {0};

    PT_BEGIN(&pt);

    notifyObserver();

    PT_END(&pt);
}

static void notifyObserver(void) {
    tRxTag tag;
    int rxLen = KuggisRead(gFifo, gRxFrame, gFrameLenMax, (uint8_t*)&tag, sizeof(tRxTag));
    if (rxLen == 0) {
        return;
    }

    TZListNode* node = TZListGetHeader(gList);
    tItem* item = NULL;

    for (;;) {
        if (node == NULL) {
            break;
        }

        item = (tItem*)node->Data;
        if (item->callback) {
            item->callback(gRxFrame, rxLen, tag.ip, tag.port);
        }

        node = node->Next;
    }
}

static void rxThread(void* param) {
    int rxLen = 0;
    struct sockaddr_in sourceAddr;
    socklen_t socklen = sizeof(sourceAddr);
    int count = 0;
    tRxTag tag;

    for (;;) {
        rxLen = recvfrom(gSock, gRxFrame, gFrameLenMax, 0, (struct sockaddr *)&sourceAddr, &socklen);
        if (rxLen <= 0) {
            LE(TAG, "rx buffer len is wrong:%d", rxLen);
            continue;
        }

        LD(TAG, "rx frame.ip:%d.%d.%d.%d,port:%d len:%d", (uint8_t)(sourceAddr.sin_addr.s_addr), 
            (uint8_t)(sourceAddr.sin_addr.s_addr >> 8), (uint8_t)(sourceAddr.sin_addr.s_addr >> 16),
            (uint8_t)(sourceAddr.sin_addr.s_addr >> 24), sourceAddr.sin_port, rxLen);
        LaganPrintHex(TAG, LAGAN_LEVEL_DEBUG, gRxFrame, rxLen);

        count = KuggisWriteableCount(gFifo);
        if (count < rxLen) {
            LW(TAG, "receive failed!fifo is full:%d %d", count, rxLen);
            continue;
        }

        tag.ip = htonl(sourceAddr.sin_addr.s_addr);
        tag.port = htons(sourceAddr.sin_port);

        if (KuggisWrite(gFifo, gRxFrame, rxLen, (uint8_t*)&tag, sizeof(tRxTag)) == false) {
            LE(TAG, "receive failed!KuggisWrite fail");
            continue;
        }
    }
    BrorThreadDeleteMe();
}

// UdpBind 绑定端口
bool UdpBind(uint16_t port) {
    static bool isBind = false;

    if (isBind) {
        if (gLocalPort == port) {
            return true;
        } else {
            LE(TAG, "udp is bound,can not bind other port!");
            return false;
        }
    }

    struct sockaddr_in destAddr;
    destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(port);

    int err = bind(gSock, (struct sockaddr *)&destAddr, sizeof(destAddr));
    if (err < 0) {
        LE(TAG, "Socket unable to bind: errno %d", errno);
    }
    LI(TAG, "socket bound.port:%d", port);

    isBind = true;
    gLocalPort = port;
    return true;
}

// UdpRegisterObserver 注册接收观察者
// callback是回调函数,接收到数据会回调此函数
bool UdpRegisterObserver(TZNetDataFunc callback) {
    if (gMid < 0 || callback == NULL) {
        LE(TAG, "register observer failed!gMid is wrong or callback is null");
        return false;
    }

    if (isObserverExist(callback)) {
        return true;
    }

    TZListNode* node = createNode();
    if (node == NULL) {
        LE(TAG, "register observer failed!create node is failed");
        return false;
    }
    tItem* item = (tItem*)node->Data;
    item->callback = callback;
    TZListAppend(gList, node);
    return true;
}

static bool isObserverExist(TZNetDataFunc callback) {
    TZListNode* node = TZListGetHeader(gList);
    tItem* item = NULL;
    for (;;) {
        if (node == NULL) {
            break;
        }
        item = (tItem*)node->Data;
        if (item->callback == callback) {
            return true;
        }
        node = node->Next;
    }
    return false;
}

static TZListNode* createNode(void) {
    TZListNode* node = TZListCreateNode(gList);
    if (node == NULL) {
        return NULL;
    }
    node->Data = TZMalloc(gMid, sizeof(tItem));
    if (node->Data == NULL) {
        TZFree(node);
        return NULL;
    }
    return node;
}

// UdpTx 发送数据
// ip是4字节数组
void UdpTx(uint8_t* bytes, int size, uint32_t ip, uint16_t port) {
    struct sockaddr_in destAddr;
    destAddr.sin_addr.s_addr = htonl(ip);
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(port);

    LD(TAG, "tx frame.ip:%d.%d.%d.%d,port:%d len:%d", (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), 
        (uint8_t)ip, port, size);
    LaganPrintHex(TAG, LAGAN_LEVEL_DEBUG, bytes, size);

    int err = sendto(gSock, bytes, size, 0, (struct sockaddr *)&destAddr, sizeof(destAddr));
    if (err < 0) {
        LE(TAG, "Error occurred during sending: errno %d", errno);
        return;
    }
}
