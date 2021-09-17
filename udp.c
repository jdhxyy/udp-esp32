// Copyright 2021-2021 The jdh99 Authors. All rights reserved.
// UDP收发模块
// Authors: jdh99 <jdh821@163.com>

#include "udp.h"
#include "bror.h"
#include "lagan.h"
#include "async.h"
#include "tzmalloc.h"
#include "tzlist.h"

#include "lwip/sockets.h"


#define TAG "udp"
#define THREAD_SIZE 4096

// tzmalloc字节数
#define MALLOC_TOTAL 4096

#pragma pack(1)

// 观察者回调函数
typedef struct {
    TZNetDataFunc callback;
} tItem;

// 接收缓存数据
typedef struct {
    uint32_t ip;
    uint16_t port;
    TZBufferDynamic* buffer;
} tRxBuffer;

#pragma pack()

static int mid = -1;

// 存储观察者列表
static intptr_t list = 0;

// 本地端口
static int sock = -1;

static uint16_t localPort = 0;

// 接收缓存
static tRxBuffer rxBuffer;

static int task(void);
static void notifyObserver(void);
static void rxThread(void* param);
static bool isObserverExist(TZNetDataFunc callback);
static TZListNode* createNode(void);

// UdpLoad 模块载入
// 载入之前需初始化nvs_flash_init,esp_netif_init,esp_event_loop_create_default
bool UdpLoad(void) {
    mid = TZMallocRegister(0, TAG, MALLOC_TOTAL);
    if (mid == -1) {
        LE(TAG, "load failed!malloc failed");
        return false;
    }
    list = TZListCreateList(mid);
    if (list == 0) {
        LE(TAG, "load failed!create list failed");
        return false;
    }
    rxBuffer.buffer = TZMalloc(mid, sizeof(TZBufferDynamic) + UDP_RX_LEN_MAX);
    if (rxBuffer.buffer == NULL) {
        LE(TAG, "load failed!malloc rx buffer failed");
        return false;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        LE(TAG, "bind failed!create socket failed");
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

    PT_WAIT_UNTIL(&pt, rxBuffer.buffer->len > 0);

    notifyObserver();

    PT_END(&pt);
}

static void notifyObserver(void) {
    TZListNode* node = TZListGetHeader(list);
    tItem* item = NULL;
    for (;;) {
        if (node == NULL) {
            break;
        }

        item = (tItem*)node->Data;
        if (item->callback) {
            item->callback(rxBuffer.buffer->buf, rxBuffer.buffer->len, 
                rxBuffer.ip, rxBuffer.port);
        }

        node = node->Next;
    }
    rxBuffer.buffer->len = 0;
}

static void rxThread(void* param) {
    uint8_t buffer[UDP_RX_LEN_MAX] = {0};
    int bufferLen = 0;
    struct sockaddr_in sourceAddr;
    socklen_t socklen = sizeof(sourceAddr);

    while (1) {
        bufferLen = recvfrom(sock, buffer, UDP_RX_LEN_MAX, 0, 
            (struct sockaddr *)&sourceAddr, &socklen);

        LD(TAG, "rx frame.ip:%d.%d.%d.%d,port:%d len:%d", 
            (uint8_t)(sourceAddr.sin_addr.s_addr), 
            (uint8_t)(sourceAddr.sin_addr.s_addr >> 8), 
            (uint8_t)(sourceAddr.sin_addr.s_addr >> 16), 
            (uint8_t)(sourceAddr.sin_addr.s_addr >> 24), sourceAddr.sin_port, bufferLen);
        LaganPrintHex(TAG, LAGAN_LEVEL_DEBUG, buffer, bufferLen);

        if (bufferLen <= 0) {
            LE(TAG, "rx buffer len is wrong:%d", bufferLen);
            continue;
        }
        if (rxBuffer.buffer->len > 0) {
            LW(TAG, "deal data is too slow.throw frame!");
            continue;
        }
        memcpy(rxBuffer.buffer->buf, buffer, bufferLen);
        rxBuffer.ip = htonl(sourceAddr.sin_addr.s_addr);
        rxBuffer.port = htons(sourceAddr.sin_port);
        rxBuffer.buffer->len = bufferLen;
    }
    BrorThreadDeleteMe();
}

// UdpBind 绑定端口
bool UdpBind(uint16_t port) {
    static bool isBind = false;

    if (isBind) {
        if (localPort == port) {
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
    
    int err = bind(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
    if (err < 0) {
        LE(TAG, "Socket unable to bind: errno %d", errno);
    }
    LI(TAG, "socket bound.port:%d", port);

    isBind = true;
    localPort = port;
    return true;
}

// UdpRegisterObserver 注册接收观察者
// callback是回调函数,接收到数据会回调此函数
bool UdpRegisterObserver(TZNetDataFunc callback) {
    if (mid < 0 || callback == NULL) {
        LE(TAG, "register observer failed!mid is wrong or callback is null");
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
    TZListAppend(list, node);
    return true;
}

static bool isObserverExist(TZNetDataFunc callback) {
    TZListNode* node = TZListGetHeader(list);
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
    TZListNode* node = TZListCreateNode(list);
    if (node == NULL) {
        return NULL;
    }
    node->Data = TZMalloc(mid, sizeof(tItem));
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

    LD(TAG, "tx frame.ip:%d.%d.%d.%d,port:%d len:%d", 
        (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip, 
        port, size);
    LaganPrintHex(TAG, LAGAN_LEVEL_DEBUG, bytes, size);

    int err = sendto(sock, bytes, size, 0, (struct sockaddr *)&destAddr, sizeof(destAddr));
    if (err < 0) {
        LE(TAG, "Error occurred during sending: errno %d", errno);
        return;
    }
}
