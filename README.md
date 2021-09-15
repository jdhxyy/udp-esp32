# udp-esp32

## 1. 介绍
esp32下的udp驱动

## 2. 功能
- 接收数据，推送给应用模块
- 发送数据

## 3. 初始化API
```c
// UdpLoad 模块载入
// 载入之前需初始化nvs_flash_init,esp_netif_init,esp_event_loop_create_default
bool UdpLoad(void);
```

注意：UdpLoad之前需初始：
```c
ESP_ERROR_CHECK(nvs_flash_init());
ESP_ERROR_CHECK(esp_netif_init());
ESP_ERROR_CHECK(esp_event_loop_create_default());
```

## 4. 绑定端口
```c
// UdpBind 绑定端口
bool UdpBind(uint16_t port);
```

## 5. 注册接收回调
```c
// UdpRegisterObserver 注册接收观察者
// callback是回调函数,接收到数据会回调此函数
bool UdpRegisterObserver(TZNetDataFunc callback);
```

## 6. 发送数据
```c
// UdpTx 发送数据
// ip是4字节数组
void UdpTx(uint8_t* bytes, int size, uint32_t ip, uint16_t port);
```
