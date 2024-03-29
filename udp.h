// Copyright 2021-2023 The jdh99 Authors. All rights reserved.
// UDP收发模块
// Authors: jdh99 <jdh821@163.com>

#ifndef UDP_H
#define UDP_H

#include "tztype.h"

// UdpLoad 模块载入
// 载入之前需初始化nvs_flash_init,esp_netif_init,esp_event_loop_create_default
// frameLenMax是最大帧长.fifoSize是接收缓存的大小
bool UdpLoad(int frameLenMax, int fifoSize);

// UdpBind 绑定端口
bool UdpBind(uint16_t port);

// UdpRegisterObserver 注册接收观察者
// callback是回调函数,接收到数据会回调此函数
bool UdpRegisterObserver(TZNetDataFunc callback);

// UdpTx 发送数据
// ip是4字节数组
void UdpTx(uint8_t* bytes, int size, uint32_t ip, uint16_t port);

#endif
