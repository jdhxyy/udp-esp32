// Copyright 2021-2021 The jdh99 Authors. All rights reserved.
// UDP收发模块
// Authors: jdh99 <jdh821@163.com>
// FIFO序列号存储的字节流格式:
// 标识符(4)+IP(4)+PORT(2)+数据长度(2)+数据(N)
// 帧头:0x20230604

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

// 标志符
#define MAGIC_WORD 0x20230604

#pragma pack(1)

// 观察者回调函数
typedef struct {
    TZNetDataFunc callback;
} tItem;

typedef struct {
    uint32_t magicWord;
    uint32_t ip;
    uint16_t port;
    uint16_t len;
    uint8_t data[];
} tFrame;

#pragma pack()

static int gMid = -1;

// 存储观察者列表
static intptr_t gList = 0;

// 本地端口
static int gSock = -1;

static uint16_t gLocalPort = 0;

// 接收缓存
static intptr_t gFifo = 0;
static tFrame* gFrame = NULL;

static int task(void);
static void notifyObserver(void);
static bool getFrameHead(int count);
static void clearFifo(int count);
static void rxThread(void* param);
static bool isObserverExist(TZNetDataFunc callback);
static TZListNode* createNode(void);

// UdpLoad 模块载入
// 载入之前需初始化nvs_flash_init,esp_netif_init,esp_event_loop_create_default
bool UdpLoad(void) {
    gMid = TZMallocRegister(0, TAG, UDP_MALLOC_TOTAL);
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

    gFrame = TZMalloc(gMid, sizeof(tFrame) + UDP_RX_LEN_MAX);
    if (gFrame == NULL) {
        LE(TAG, "load failed!malloc gFrame failed");
        return false;
    }

    gFifo = TZFifoCreate(gMid, UDP_RX_FIFO_SIZE, 1);
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
    TZListNode* node = TZListGetHeader(gList);
    tItem* item = NULL;

    // 反序列化读取数据
    int count = TZFifoReadableItemCount(gFifo);
    if (count == 0) {
        return;
    }

    if (getFrameHead(count) == false) {
        // 读取头部失败,fifo数据有问题,全部清空
        LW(TAG, "clear fifo!");
        clearFifo(count);
        return;
    }

    if (gFrame->len == 0) {
        LW(TAG, "frame len is 0!");
        return;
    }
    if (TZFifoReadBatch(gFifo, (uint8_t*)gFrame->data, gFrame->len, gFrame->len) == false) {
        LW(TAG, "fifo read batch failed!read data failed!");
        return;
    }

    for (;;) {
        if (node == NULL) {
            break;
        }

        item = (tItem*)node->Data;
        if (item->callback) {
            item->callback(gFrame->data, gFrame->len, gFrame->ip, gFrame->port);
        }

        node = node->Next;
    }
}

static bool getFrameHead(int count) {
    // 反序列化读取数据
    if (count < sizeof(tFrame)) {
        LW(TAG, "fifo data format is wrong.count is too short:%d", count);
        return false;
    }
    if (TZFifoReadBatch(gFifo, (uint8_t*)gFrame, sizeof(tFrame), sizeof(tFrame)) == false) {
        LE(TAG, "fifo read batch is failed!");
        return false;
    }
    if (gFrame->magicWord != MAGIC_WORD) {
        LW(TAG, "magic word is wrong:0x%x", gFrame->magicWord);
        return false;
    }
    if (count - sizeof(tFrame) < gFrame->len) {
        LW(TAG, "count is too short:%d %d", count, gFrame->len);
        return false;
    }
    return true;
}

static void clearFifo(int count) {
    int frameSize = sizeof(tFrame) + UDP_RX_LEN_MAX;

    for (;;) {
        if (count <= frameSize) {
            TZFifoReadBatch(gFifo, (uint8_t*)gFrame, count, count);
            break;
        } else {
            TZFifoReadBatch(gFifo, (uint8_t*)gFrame, frameSize, frameSize);
            count -= frameSize;
        }
    }
}

static void rxThread(void* param) {
    uint8_t buffer[UDP_RX_LEN_MAX] = {0};
    int bufferLen = 0;
    struct sockaddr_in sourceAddr;
    socklen_t socklen = sizeof(sourceAddr);
    tFrame* frame = NULL;
    int count = 0;

    while (1) {
        bufferLen = recvfrom(gSock, buffer + sizeof(tFrame), UDP_RX_LEN_MAX - sizeof(tFrame), 0, 
            (struct sockaddr *)&sourceAddr, &socklen);

        LD(TAG, "rx frame.ip:%d.%d.%d.%d,port:%d len:%d",
            (uint8_t)(sourceAddr.sin_addr.s_addr),
            (uint8_t)(sourceAddr.sin_addr.s_addr >> 8),
            (uint8_t)(sourceAddr.sin_addr.s_addr >> 16),
            (uint8_t)(sourceAddr.sin_addr.s_addr >> 24), sourceAddr.sin_port, bufferLen);
        LaganPrintHex(TAG, LAGAN_LEVEL_DEBUG, buffer + sizeof(tFrame), bufferLen);

        if (bufferLen <= 0) {
            LE(TAG, "rx buffer len is wrong:%d", bufferLen);
            continue;
        }

        // 序列号存储
        count = TZFifoWriteableItemCount(gFifo);
        if (count < sizeof(tFrame) + bufferLen) {
            LW(TAG, "receive failed!fifo is full:%d %d", count, bufferLen);
            continue;
        }

        frame = (tFrame*)buffer;
        frame->magicWord = MAGIC_WORD;
        frame->ip = htonl(sourceAddr.sin_addr.s_addr);
        frame->port = htons(sourceAddr.sin_port);
        frame->len = bufferLen;

        if (TZFifoWriteBatch(gFifo, buffer, sizeof(tFrame) + bufferLen) == false) {
            LW(TAG, "receive failed!write fifo frame failed!");
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
