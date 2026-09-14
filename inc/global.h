#ifndef _GLOBAL_H_
#define _GLOBAL_H_

#include <netinet/in.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "global.h"
#include <pthread.h>
#include <sys/select.h>
#include <arpa/inet.h>

// 单位是byte
#define SIZE32 4
#define SIZE16 2
#define SIZE8  1

// 一些Flag
#define NO_FLAG 0
#define NO_WAIT 1
#define TIMEOUT 2
#define TRUE 1
#define FALSE 0

// 定义最大包长 防止IP层分片
#define MAX_DLEN 1375 	// 最大包内数据长度
#define MAX_LEN 1400 	// 最大包长度

// TCP socket 状态定义
#define CLOSED 0
#define LISTEN 1
#define SYN_SENT 2
#define SYN_RECV 3
#define ESTABLISHED 4
#define FIN_WAIT_1 5
#define FIN_WAIT_2 6
#define CLOSE_WAIT 7
#define CLOSING 8
#define LAST_ACK 9
#define TIME_WAIT 10

#define RTO_SET 5000  //重传时间
#define RTO_up 2000000
#define RTO_down 1000
// TCP 拥塞控制状态
#define SLOW_START 0
#define CONGESTION_AVOIDANCE 1
#define FAST_RECOVERY 2

#define send_buff_lenth 0
#define recv_buff_lenth 0
// TCP 接受窗口大小
#define TCP_RECV_PACK_NUM 52
#define TCP_RECVWN_SIZE TCP_RECV_PACK_NUM*MAX_DLEN // 比如最多放32个满载数据包

// TCP 发送窗口
#define TCP_SENDWN_SIZE 50  //发送窗口大小
// 注释的内容如果想用就可以用 不想用就删掉 仅仅提供思路和灵感

typedef struct Send_window
{
	uint8_t  send_ok;     //是否已确认
	uint16_t send_lenth;//发送数据的长度
	char*    msg; //数据包
	uint32_t send_waiting_ack;//等待ack编号
	uint32_t send_time;   //上一次发送时间
	uint32_t send_time_base;
	uint8_t quick_ok;
}send_window;  //窗口结构体

typedef struct {
	uint16_t window_size;
	uint16_t sending; //记录正在发送窗口的哪一条数据
	
    uint32_t base;
    uint32_t nextseq;
//   uint32_t estmated_rtt;
     int ack_cnt;
     pthread_mutex_t ack_cnt_lock; //维护ack
	send_window packs[TCP_SENDWN_SIZE];
	
//   struct timeval send_time;
//   struct timeval timeout;
//   uint16_t rwnd; 
//   int congestion_status;
//   uint16_t cwnd; 
//   uint16_t ssthresh; 
} sender_window_t;

typedef struct recv_Mark
{
	uint32_t seq;
	uint16_t len;
}recv_mark;
// TCP 接受窗口
// 注释的内容如果想用就可以用 不想用就删掉 仅仅提供思路和灵感
typedef struct {
	uint8_t receving;
	uint32_t expect_seq;//期望seq
	uint32_t base_seq; //接收窗口的基准ack 以判断偏移量
	uint32_t max_seq; //记录本次窗口最大接收到的seq
	recv_mark mark[TCP_RECV_PACK_NUM]; //标记窗口可滑动区域
//   received_packet_t* head;
//   char buf[TCP_RECVWN_SIZE];
} receiver_window_t;

// TCP 窗口 每个建立了连接的TCP都包括发送和接受两个窗口
typedef struct {
	sender_window_t* wnd_send;
  	receiver_window_t* wnd_recv;
} window_t;

typedef struct {
	uint32_t ip;
	uint16_t port;
} tju_sock_addr;


// TJU_TCP 结构体 保存TJU_TCP用到的各种数据
typedef struct {
	int state; // TCP的状态

	tju_sock_addr bind_addr; // 存放bind和listen时该socket绑定的IP和端口
	tju_sock_addr established_local_addr; // 存放建立连接后 本机的 IP和端口
	tju_sock_addr established_remote_addr; // 存放建立连接后 连接对方的 IP和端口

	pthread_mutex_t send_lock; // 发送数据锁
	char* sending_buf; // 发送数据缓存区
	int sending_len; // 发送数据缓存长度

	pthread_mutex_t recv_lock; // 接收数据锁
	char* received_buf; // 接收数据缓存区
	int received_len; // 接收数据缓存长度

	pthread_cond_t wait_cond; // 可以被用来唤醒recv函数调用时等待的线程

	window_t window; // 发送和接受窗口

	uint32_t RTO ;//超时重传时间 应该是微秒级别的

	uint32_t SRTT;
	uint32_t RTTVAR;
	float a;
	float b;

	uint32_t seq; 
	uint32_t ack;
} tju_tcp_t;

#endif