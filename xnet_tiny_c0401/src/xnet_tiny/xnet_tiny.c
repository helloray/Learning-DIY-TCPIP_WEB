﻿/**
 * 用1500行代码从0开始实现TCP/IP协议栈+WEB服务器
 *
 * 本源码旨在用最简单、最易懂的方式帮助你快速地了解TCP/IP以及HTTP工作原理的主要核心知识点。
 * 所有代码经过精心简化设计，避免使用任何复杂的数据结构和算法，避免实现其它无关紧要的细节。
 *
 * 本源码配套高清的视频教程，免费提供下载！具体的下载网址请见下面。
 * 视频中的PPT暂时提供下载，但配套了学习指南，请访问下面的网址。
 *
 * 作者：李述铜
 * 网址: http://01ketang.cc/tcpip
 * QQ群：524699753（加群时请注明：tcpip），免费提供关于该源码的支持和问题解答。
 * 微信公众号：请搜索 01课堂
 *
 * 版权声明：源码仅供学习参考，请勿用于商业产品，不保证可靠性。二次开发或其它商用前请联系作者。
 * 注：
 * 1.源码不断升级中，该版本可能非最新版。如需获取最新版，请访问上述网址获取最新版本的代码
 * 2.1500行代码指未包含注释的代码。
 *
 * 如果你在学习本课程之后，对深入研究TCP/IP感兴趣，欢迎关注我的后续课程。我将开发出一套更加深入
 * 详解TCP/IP的课程。采用多线程的方式，实现更完善的功能，包含但不限于
 * 1. IP层的分片与重组
 * 2. Ping功能的实现
 * 3. TCP的流量控制等
 * 4. 基于UDP的TFTP服务器实现
 * 5. DNS域名接触
 * 6. DHCP动态地址获取
 * 7. HTTP服务器
 * ..... 更多功能开发中...........
 * 如果你有兴趣的话，欢迎关注。
 */
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "xnet_tiny.h"

#define min(a, b)               ((a) > (b) ? (b) : (a))

static const xipaddr_t netif_ipaddr = XNET_CFG_NETIF_IP;
static const uint8_t ether_broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t netif_mac[XNET_MAC_ADDR_SIZE];                   // mac地址
static xnet_packet_t tx_packet, rx_packet;                      // 接收与发送缓冲区
static xarp_entry_t arp_entry;                                  // 节省内存，只使用一个ARP表项
static xnet_time_t arp_timer;                                   // ARP扫描定时

static void update_arp_entry(uint8_t* src_ip, uint8_t* mac_addr);

#define swap_order16(v)   ((((v) & 0xFF) << 8) | (((v) >> 8) & 0xFF))
#define xipaddr_is_equal_buf(addr, buf)      (memcmp((addr)->array, (buf), XNET_IPV4_ADDR_SIZE) == 0)
#define xipaddr_is_equal(addr1, addr2)       ((addr1)->addr == (addr2)->addr)
#define xipaddr_from_buf(dest, buf)          ((dest)->addr = *(uint32_t *)(buf))

/**
 * 检查是否超时
 * @param time 前一时间
 * @param sec 预期超时时间，值为0时，表示获取当前时间
 * @return 0 - 未超时，1-超时
 */
int xnet_check_tmo(xnet_time_t * time, uint32_t sec) {
    xnet_time_t curr = xsys_get_time();
    if (sec == 0) {          // 0，取当前时间
        *time = curr;
        return 0;
    } else if (curr - *time >= sec) {   // 非0检查超时
        *time = curr;       // 当超时时，才更新时间
        return 1;
    }
    return 0;
}

/**
 * 分配一个网络数据包用于发送数据
 * @param data_size 数据空间大小
 * @return 分配得到的包结构
 */
xnet_packet_t * xnet_alloc_for_send(uint16_t data_size) {
    // 从tx_packet的后端往前分配，因为前边要预留作为各种协议的头部数据存储空间
    tx_packet.data = tx_packet.payload + XNET_CFG_PACKET_MAX_SIZE - data_size;
    tx_packet.size = data_size;
    return &tx_packet;
}

/**
 * 分配一个网络数据包用于读取
 * @param data_size 数据空间大小
 * @return 分配得到的数据包
 */
xnet_packet_t * xnet_alloc_for_read(uint16_t data_size) {
    // 从最开始进行分配，用于最底层的网络数据帧读取
    rx_packet.data = rx_packet.payload;
    rx_packet.size = data_size;
    return &rx_packet;
}

/**
 * 为发包添加一个头部
 * @param packet 待处理的数据包
 * @param header_size 增加的头部大小
 */
static void add_header(xnet_packet_t *packet, uint16_t header_size) {
    packet->data -= header_size;
    packet->size += header_size;
}

/**
 * 为接收向上处理移去头部
 * @param packet 待处理的数据包
 * @param header_size 移去的头部大小
 */
static void remove_header(xnet_packet_t *packet, uint16_t header_size) {
    packet->data += header_size;
    packet->size -= header_size;
}

/**
 * 将包的长度截断为size大小
 * @param packet 待处理的数据包
 * @param size 最终大小
 */
static void truncate_packet(xnet_packet_t *packet, uint16_t size) {
    packet->size = min(packet->size, size);
}

/**
 * 以太网初始化
 * @return 初始化结果
 */
static xnet_err_t ethernet_init (void) {
    xnet_err_t err = xnet_driver_open(netif_mac);
    if (err < 0) return err;

    // 开启抓包工具wireshark，能在窗口发现如下数据包抓取
    // 1	0.000000	Dell_f9:e6:77	Broadcast	ARP	42	ARP Announcement for 192.168.254.2
    return xarp_make_request(&netif_ipaddr);
}

/**
 * 发送一个以太网数据帧
 * @param protocol 上层数据协议，IP或ARP
 * @param mac_addr 目标网卡的mac地址
 * @param packet 待发送的数据包
 * @return 发送结果
 */
static xnet_err_t ethernet_out_to(xnet_protocol_t protocol, const uint8_t *mac_addr, xnet_packet_t * packet) {
    xether_hdr_t* ether_hdr;

    // 添加头部
    add_header(packet, sizeof(xether_hdr_t));
    ether_hdr = (xether_hdr_t*)packet->data;
    memcpy(ether_hdr->dest, mac_addr, XNET_MAC_ADDR_SIZE);
    memcpy(ether_hdr->src, netif_mac, XNET_MAC_ADDR_SIZE);
    ether_hdr->protocol = swap_order16(protocol);

    // 数据发送
    return xnet_driver_send(packet);
}

/**
 * 将IP数据包通过以太网发送出去
 * @param dest_ip 目标IP地址
 * @param packet 待发送IP数据包
 * @return 发送结果
 */
static xnet_err_t ethernet_out (xipaddr_t * dest_ip, xnet_packet_t * packet) {
    xnet_err_t err;
    uint8_t * mac_addr;

    if ((err = xarp_resolve(dest_ip, &mac_addr) == XNET_ERR_OK)) {
        return ethernet_out_to(XNET_PROTOCOL_IP, mac_addr, packet);
    }
    return err;
}

/**
 * 以太网数据帧输入输出
 * @param packet 待处理的包
 */
static void ethernet_in (xnet_packet_t * packet) {
    // 至少要比头部数据大
    if (packet->size <= sizeof(xether_hdr_t)) {
        return;
    }

    // 往上分解到各个协议处理
    xether_hdr_t* hdr = (xether_hdr_t*)packet->data;
    switch (swap_order16(hdr->protocol)) {
        case XNET_PROTOCOL_ARP:
            remove_header(packet, sizeof(xether_hdr_t));
            xarp_in(packet);
            break;
        case XNET_PROTOCOL_IP: {
            // 以下代码是从IP包头中提取IP地址，以及从以太网包头中提取mac地址
            // 然后用其更新ARP表
#if 0
            xip_hdr_t *iphdr = (xip_hdr_t *) (packet->data + sizeof(xether_hdr_t));
            if (packet->size >= sizeof(xether_hdr_t) + sizeof(xip_hdr_t)) {
                if (memcmp(iphdr->dest_ip, &netif_ipaddr.array, XNET_IPV4_ADDR_SIZE) == 0) {
                    update_arp_entry(iphdr->src_ip, hdr->src);
                }
            }
#endif
            remove_header(packet, sizeof(xether_hdr_t));
            xip_in(packet);
            break;
        }
    }
}

/**
 * 查询网络接口，看看是否有数据包，有则进行处理
 */
static void ethernet_poll (void) {
    xnet_packet_t * packet;

    if (xnet_driver_read(&packet) == XNET_ERR_OK) {
        // 正常情况下，在此打个断点，全速运行
        // 然后在对方端ping 192.168.254.2，会停在这里
        ethernet_in(packet);
    }
}

/**
 * ARP初始化
 */
void xarp_init(void) {
    arp_entry.state = XARP_ENTRY_FREE;

    // 获取初始时间
    xnet_check_tmo(&arp_timer, 0);
}

/**
 * 查询ARP表项是否超时，超时则重新请求
 */
void xarp_poll(void) {
    if (xnet_check_tmo(&arp_timer, XARP_TIMER_PERIOD)) {
        switch (arp_entry.state) {
            case XARP_ENTRY_RESOLVING:
                if (--arp_entry.tmo == 0) {     // 重试完毕，回收
                    if (arp_entry.retry_cnt-- == 0) {
                        arp_entry.state = XARP_ENTRY_FREE;
                    } else {    // 继续重试
                        xarp_make_request(&arp_entry.ipaddr);
                        arp_entry.state = XARP_ENTRY_RESOLVING;
                        arp_entry.tmo = XARP_CFG_ENTRY_PENDING_TMO;
                    }
                }
                break;
            case XARP_ENTRY_OK:
                if (--arp_entry.tmo == 0) {     // 超时，重新请求
                    xarp_make_request(&arp_entry.ipaddr);
                    arp_entry.state = XARP_ENTRY_RESOLVING;
                    arp_entry.tmo = XARP_CFG_ENTRY_PENDING_TMO;
                }
                break;
        }
    }
}

/**
 * 生成一个ARP响应
 * @param arp_packet 接收到的ARP请求包
 * @return 生成结果
 */
 xnet_err_t xarp_make_response(xarp_packet_t * arp_packet) {
    xarp_packet_t* response_packet;
    xnet_packet_t * packet = xnet_alloc_for_send(sizeof(xarp_packet_t));

    response_packet = (xarp_packet_t *)packet->data;
    response_packet->hw_type = swap_order16(XARP_HW_ETHER);
    response_packet->pro_type = swap_order16(XNET_PROTOCOL_IP);
    response_packet->hw_len = XNET_MAC_ADDR_SIZE;
    response_packet->pro_len = XNET_IPV4_ADDR_SIZE;
    response_packet->opcode= swap_order16(XARP_REPLY);
    memcpy(response_packet->target_mac, arp_packet->sender_mac, XNET_MAC_ADDR_SIZE);
    memcpy(response_packet->target_ip, arp_packet->sender_ip, XNET_IPV4_ADDR_SIZE);
    memcpy(response_packet->sender_mac, netif_mac, XNET_MAC_ADDR_SIZE);
    memcpy(response_packet->sender_ip, netif_ipaddr.array, XNET_IPV4_ADDR_SIZE);
    return ethernet_out_to(XNET_PROTOCOL_ARP, ether_broadcast, packet);
}

/**
 * 产生一个ARP请求，请求网络指定ip地址的机器发回一个ARP响应
 * @param ipaddr 请求的IP地址
 * @return 请求结果
 */
xnet_err_t xarp_make_request(const xipaddr_t * ipaddr) {
    xarp_packet_t* arp_packet;
    xnet_packet_t * packet = xnet_alloc_for_send(sizeof(xarp_packet_t));

    arp_packet = (xarp_packet_t *)packet->data;
    arp_packet->hw_type = swap_order16(XARP_HW_ETHER);
    arp_packet->pro_type = swap_order16(XNET_PROTOCOL_IP);
    arp_packet->hw_len = XNET_MAC_ADDR_SIZE;
    arp_packet->pro_len = XNET_IPV4_ADDR_SIZE;
    arp_packet->opcode = swap_order16(XARP_REQUEST);
    memcpy(arp_packet->sender_mac, netif_mac, XNET_MAC_ADDR_SIZE);
    memcpy(arp_packet->sender_ip, netif_ipaddr.array, XNET_IPV4_ADDR_SIZE);
    memset(arp_packet->target_mac, 0, XNET_MAC_ADDR_SIZE);
    memcpy(arp_packet->target_ip, ipaddr->array, XNET_IPV4_ADDR_SIZE);
    return ethernet_out_to(XNET_PROTOCOL_ARP, ether_broadcast, packet);
}

/**
 * 解析指定的IP地址，如果不在ARP表项中，则发送ARP请求
 * @param ipaddr 查找的ip地址
 * @param mac_addr 返回的mac地址存储区
 * @return XNET_ERR_OK 查找成功，XNET_ERR_NONE 查找失败
 */
xnet_err_t xarp_resolve(const xipaddr_t * ipaddr, uint8_t ** mac_addr) {
    if ((arp_entry.state == XARP_ENTRY_OK) && xipaddr_is_equal(ipaddr, &arp_entry.ipaddr)) {
        *mac_addr = arp_entry.macaddr;
        return XNET_ERR_OK;
    }

    xarp_make_request(ipaddr);
    return XNET_ERR_NONE;
}

/**
 * 更新ARP表项
 * @param src_ip 源IP地址
 * @param mac_addr 对应的mac地址
 */
static void update_arp_entry(uint8_t * src_ip, uint8_t * mac_addr) {
    memcpy(arp_entry.ipaddr.array, src_ip, XNET_IPV4_ADDR_SIZE);
    memcpy(arp_entry.macaddr, mac_addr, 6);
    arp_entry.state = XARP_ENTRY_OK;
    arp_entry.tmo = XARP_CFG_ENTRY_OK_TMO;
    arp_entry.retry_cnt = XARP_CFG_MAX_RETRIES;
}

/**
 * ARP输入处理
 * @param packet 输入的ARP包
 */
void xarp_in(xnet_packet_t * packet) {
    if (packet->size >= sizeof(xarp_packet_t)) {
        xarp_packet_t * arp_packet = (xarp_packet_t *) packet->data;
        uint16_t opcode = swap_order16(arp_packet->opcode);

        // 包的合法性检查
        if ((swap_order16(arp_packet->hw_type) != XARP_HW_ETHER) ||
            (arp_packet->hw_len != XNET_MAC_ADDR_SIZE) ||
            (swap_order16(arp_packet->pro_type) != XNET_PROTOCOL_IP) ||
            (arp_packet->pro_len != XNET_IPV4_ADDR_SIZE)
            || ((opcode != XARP_REQUEST) && (opcode != XARP_REPLY))) {
            return;
        }

        // 只处理发给自己的请求或响应包
        if (!xipaddr_is_equal_buf(&netif_ipaddr, arp_packet->target_ip)) {
            return;
        }

        // 根据操作码进行不同的处理
        switch (swap_order16(arp_packet->opcode)) {
            case XARP_REQUEST:  // 请求，回送响应
                // 在对方机器Ping 自己，然后看wireshark，能看到ARP请求和响应
                // 接下来，很可能对方要与自己通信，所以更新一下
                xarp_make_response(arp_packet);
                update_arp_entry(arp_packet->sender_ip, arp_packet->sender_mac);
                break;
            case XARP_REPLY:    // 响应，更新自己的表
                update_arp_entry(arp_packet->sender_ip, arp_packet->sender_mac);
                break;
        }
    }
}

/**
 * 校验和计算
 * @param buf 校验数据区的起始地址
 * @param len 数据区的长度，以字节为单位
 * @param pre_sum 累加的之前的值，用于多次调用checksum对不同的的数据区计算出一个校验和
 * @param complement 是否对累加和的结果进行取反
 * @return 校验和结果
 */
static uint16_t checksum16(uint16_t * buf, uint16_t len, uint16_t pre_sum, int complement) {
    uint32_t checksum = pre_sum;
    uint16_t high;

    while (len > 1) {
        checksum += *buf++;
        len -= 2;
    }
    if (len > 0) {
        checksum += *(uint8_t *)buf;
    }

    // 注意，这里要不断累加。不然结果在某些情况下计算不正确
    while ((high = checksum >> 16) != 0) {
        checksum = high + (checksum & 0xffff);
    }
    return complement ? (uint16_t)~checksum : (uint16_t)checksum;
}

/**
 * IP层的初始化
 */
void xip_init(void) {}

/**
 * IP层的输入处理
 * @param packet 输入的IP数据包
 */
void xip_in(xnet_packet_t * packet) {
    xip_hdr_t* iphdr = (xip_hdr_t*)packet->data;
    uint32_t total_size, header_size;
    uint16_t pre_checksum;
    xipaddr_t src_ip;

    // 进行一些必要性的检查：版本号要求
    if (iphdr->version != XNET_VERSION_IPV4) {
        return;
    }

    // 长度要求检查
    header_size = iphdr->hdr_len * 4;
    total_size = swap_order16(iphdr->total_len);
    if ((header_size < sizeof(xip_hdr_t)) || ((total_size < header_size) || (packet->size < total_size))) {
        return;
    }

    // 校验和要求检查
    pre_checksum = iphdr->hdr_checksum;
    iphdr->hdr_checksum = 0;
    if (pre_checksum != checksum16((uint16_t*)iphdr, header_size, 0, 1)) {
        return;
    }
    iphdr->hdr_checksum = pre_checksum;

    // 只处理目标IP为自己的数据包，其它广播之类的IP全部丢掉
    if (!xipaddr_is_equal_buf(&netif_ipaddr, iphdr->dest_ip)) {
        return;
    }

    xipaddr_from_buf(&src_ip, iphdr->src_ip);
    switch(iphdr->protocol) {
        case XNET_PROTOCOL_ICMP:
            remove_header(packet, header_size);
            xicmp_in(&src_ip, packet);
            break;
        default:
            // 这里应当写成协议不可达，因为没有任何协议能处理输入数据包
            xicmp_dest_unreach(XICMP_CODE_PRO_UNREACH, iphdr);
            break;
    }
}

/**
 * IP包的输出
 * @param protocol 上层协议，ICMP、UDP或TCP
 * @param dest_ip
 * @param packet
 * @return
 */
xnet_err_t xip_out(xnet_protocol_t protocol, xipaddr_t* dest_ip, xnet_packet_t * packet) {
    static uint32_t ip_packet_id = 0;
    xip_hdr_t * iphdr;

    add_header(packet, sizeof(xip_hdr_t));
    iphdr = (xip_hdr_t*)packet->data;
    iphdr->version = XNET_VERSION_IPV4;
    iphdr->hdr_len = sizeof(xip_hdr_t) / 4;
    iphdr->tos = 0;
    iphdr->total_len = swap_order16(packet->size);
    iphdr->id = swap_order16(ip_packet_id);
    iphdr->flags_fragment = 0;
    iphdr->ttl = XNET_IP_DEFAULT_TTL;
    iphdr->protocol = protocol;
    memcpy(iphdr->dest_ip, dest_ip->array, XNET_IPV4_ADDR_SIZE);
    memcpy(iphdr->src_ip, netif_ipaddr.array, XNET_IPV4_ADDR_SIZE);
    iphdr->hdr_checksum = 0;
    iphdr->hdr_checksum = checksum16((uint16_t *)iphdr, sizeof(xip_hdr_t), 0, 1);;

    ip_packet_id++;
    return ethernet_out(dest_ip, packet);
}

/**
 * icmp初始化
 */
void xicmp_init(void) {}

/**
 * 发送ICMP ECHO响应，即回应ping
 * @param icmp_hdr 收到的icmp包头
 * @param src_ip 包的来源ip
 * @param packet 收到的数据包
 * @return 处理结果
 */
static xnet_err_t reply_icmp_request(xicmp_hdr_t * icmp_hdr, xipaddr_t* src_ip, xnet_packet_t * packet) {
    xicmp_hdr_t * replay_hdr;
    xnet_packet_t * tx = xnet_alloc_for_send(packet->size);

    replay_hdr = (xicmp_hdr_t *)tx->data;
    replay_hdr->type = XICMP_CODE_ECHO_REPLY;
    replay_hdr->code = 0;
    replay_hdr->id = icmp_hdr->id;
    replay_hdr->seq = icmp_hdr->seq;
    replay_hdr->checksum = 0;
    memcpy(((uint8_t *)replay_hdr) + sizeof(xicmp_hdr_t), ((uint8_t *)icmp_hdr) + sizeof(xicmp_hdr_t),
            packet->size - sizeof(xicmp_hdr_t));
    replay_hdr->checksum = checksum16((uint16_t*)replay_hdr, tx->size, 0, 1);
    return xip_out(XNET_PROTOCOL_ICMP, src_ip, tx);
}

/**
 * ICMP包输入处理
 * @param src_ip 数据包来源
 * @param packet 待处理的数据包
 */
void xicmp_in(xipaddr_t *src_ip, xnet_packet_t * packet) {
    xicmp_hdr_t* icmphdr = (xicmp_hdr_t *)packet->data;

    if ((packet->size >= sizeof(xicmp_hdr_t)) && (icmphdr->type == XICMP_CODE_ECHO_REQUEST)) {
        reply_icmp_request(icmphdr, src_ip, packet);
    }
}

/**
 * 发送ICMP端口不可达或协议不可达的响应
 * @param code 不可达的类型码
 * @param ip_hdr 收到的ip包
 * @return 处理结果
 */
xnet_err_t xicmp_dest_unreach(uint8_t code, xip_hdr_t *ip_hdr) {
    xicmp_hdr_t * icmp_hdr;
    xipaddr_t dest_ip;
    xnet_packet_t* packet;

    // 计算要拷贝的ip数据量
    uint16_t ip_hdr_size = ip_hdr->hdr_len * 4;
    uint16_t ip_data_size = swap_order16(ip_hdr->total_len) - ip_hdr_size;

    // RFC文档里写的是8字节。但实际测试windows上发现复制了不止8个字节
    ip_data_size = ip_hdr_size + min(ip_data_size, 8);

    // 生成数据包，然后发送
    packet = xnet_alloc_for_send(ip_data_size + sizeof(xicmp_hdr_t));
    icmp_hdr = (xicmp_hdr_t*)packet->data;
    icmp_hdr->type = XICMP_TYPE_UNREACH;
    icmp_hdr->code = code;
    icmp_hdr->checksum = 0;
    icmp_hdr->id = icmp_hdr->seq = 0;
    memcpy(((uint8_t *)icmp_hdr) + sizeof(xicmp_hdr_t), ip_hdr, ip_data_size);
    icmp_hdr->checksum = 0;
    icmp_hdr->checksum = checksum16((uint16_t *)icmp_hdr, packet->size, 0, 1);
    xipaddr_from_buf(&dest_ip, ip_hdr->src_ip);
    return xip_out(XNET_PROTOCOL_ICMP, &dest_ip, packet);
}

/**
 * 协议栈的初始化
 */
void xnet_init (void) {
    ethernet_init();
    xarp_init();
    xip_init();
    xicmp_init();
}

/**
 * 轮询处理数据包，并在协议栈中处理
 */
void xnet_poll(void) {
    ethernet_poll();
    xarp_poll();
}
