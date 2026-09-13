#include "tju_tcp.h"

char TraceFile_name[20] = "./trance.txt"; //一个tcp记录
uint32_t recv_trance = 0;
/*
创建 TCP socket 
初始化对应的结构体
设置初始状态为 CLOSED
*/

/*
比较难绷的一点是，源代码发送消息的时候没有包含tcp远程ip,也完全没有实现标准传输协议
东北人写的代码就是这样唐诗
f8fq.
*/
//判断该窗口是否可继续滑动 返回滑动距离并弹出值
uint16_t tcp_FindMark(tju_tcp_t* sock, uint32_t seq)
{
    //这个mark其实可以优化成链表 但是我懒
    recv_mark* ptr = sock->window.wnd_recv->mark;
    //只能使用笨方法搜寻了,因为
    for (int i = 0; i < TCP_RECV_PACK_NUM; i++)
    {
        if (ptr[i].seq == seq)
        {
            ptr[i].seq = 0;
            return ptr[i].len;
        }
    }
    return 0;
}

//记录可滑动
void tcp_WriteMark(tju_tcp_t* sock, uint32_t seq,uint16_t len)
{
    recv_mark* ptr = sock->window.wnd_recv->mark;
    for (int i = 0; i < TCP_RECV_PACK_NUM; i++)
    {
        if (ptr[i].seq == 0)
        {
            ptr[i].seq=seq;
            ptr[i].len=len;
            return;
        }
    }
    return ;
}

uint32_t now_us() 
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

void tcp_send_window_reset(send_window* wind)
{
    free(wind->msg);
    wind->msg = NULL;
    wind->send_time = 0;
    wind->send_ok = 0;
    wind->send_lenth = 0;
    return;
}

//正确就返回1 否则返回0
uint8_t tcp_checksum(uint16_t* data, int len) 
{// 两字节两字节求和
    uint32_t sum = 0;

    // ① 所有16位字按大端累加
    while (len > 1) {
        sum += *data++;
        len -= 2;
    }
    // ② 奇数尾部补0（最后一个字节补成16位）
    if (len == 1) {
        sum += *(uint8_t*)data << 8;   // 低位补0
    }
    // ③ 进位回卷（carry 回加到低位，直到无进位）
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    // ④ 取反（one's complement）
    return (uint16_t)~sum;
}//问题来了 你这包头的checksum在哪呢?   经典草台班子

void tcp_rto_count(tju_tcp_t* sock,uint32_t RTT_sample)
{
    uint32_t rto;
    sock->RTTVAR = (1-sock->b) *sock->RTTVAR + sock->b*fabs(sock->SRTT - RTT_sample);
    sock->SRTT = (1-sock->a) * sock->SRTT + sock->a * RTT_sample;
    rto= sock->SRTT + 4*sock->RTTVAR;
    if (rto < RTO_down)        rto = RTO_down;    // 下限100ms（你原来的RTO_SET）
    if (rto > RTO_up)      rto = RTO_up;
    sock->RTO = rto;
    printf("rto : %d\n",rto);
    return ;
}

void tcp_rto_backoff(tju_tcp_t* sock)
{
    uint32_t rto = sock->RTO;
    rto *=2;
    if (rto < RTO_down)        rto = RTO_down;    // 下限100ms（你原来的RTO_SET）

    if (rto > RTO_up)      rto = RTO_up;
    sock->RTO = rto;
    return;
}

tju_tcp_t* tju_socket(){
    tju_tcp_t* sock = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    sock->state = CLOSED;
    
    pthread_mutex_init(&(sock->send_lock), NULL);
    sock->sending_buf = (char*)malloc(sizeof(char) * send_buff_lenth ) ;
    sock->sending_len = 0;

    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = (char*)malloc(sizeof(char) * recv_buff_lenth);
    sock->received_len = 0;
    
    if(pthread_cond_init(&sock->wait_cond, NULL) != 0){
        perror("ERROR condition variable not set\n");
        exit(-1);
    }

    sock->window.wnd_recv = NULL;
    
    //初始化发送窗口
    sock->window.wnd_recv = (receiver_window_t*)calloc(sizeof(receiver_window_t),sizeof(char));
    sock->window.wnd_recv->receving = 0;


    sock->window.wnd_send = (sender_window_t*)calloc(sizeof(sender_window_t),sizeof(char)); //默认所有数据为0
    sock->window.wnd_send->window_size = TCP_SENDWN_SIZE;
    //初始化rto
    
    sock->a = 0.125;
    sock->b = 0.25;
    sock->SRTT = RTO_SET;
    sock->RTTVAR = RTO_SET/2;
    sock->RTO = sock->SRTT + 4*sock->RTTVAR;

    srand(now_us());
    sock->seq = rand()%300;  //随机确定一个seq
    sock->ack = 0;
    return sock;
}

/*
绑定监听的地址 包括ip和端口
*/
int tju_bind(tju_tcp_t* sock, tju_sock_addr bind_addr){
    sock->bind_addr = bind_addr;
    return 0;
}

/*
被动打开 监听bind的地址和端口
设置socket的状态为LISTEN
注册该socket到内核的监听socket哈希表
*/
int tju_listen(tju_tcp_t* sock){
    sock->state = LISTEN;
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
    listen_socks[hashval] = sock;
    return 0;
}

/*
接受连接 
返回与客户端通信用的socket
这里返回的socket一定是已经完成3次握手建立了连接的socket
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
tju_tcp_t* tju_accept(tju_tcp_t* listen_sock){
    /*
    tju_tcp_t* new_conn = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    memcpy(new_conn, listen_sock, sizeof(tju_tcp_t));

    tju_sock_addr local_addr, remote_addr;
    */

    /*
     这里涉及到TCP连接的建立
     正常来说应该是收到客户端发来的SYN报文
     从中拿到对端的IP和PORT
     换句话说 下面的处理流程其实不应该放在这里 应该在tju_handle_packet中
     我还是另起一个函数放里边吧
    */ 
   /*
    remote_addr.ip = inet_network(CLIENT_IP);  //具体的IP地址 也就是说似乎只能链接到固定ip
    remote_addr.port = 5678;  //端口

    local_addr.ip = listen_sock->bind_addr.ip;  //具体的IP地址
    local_addr.port = listen_sock->bind_addr.port;  //端口

    new_conn->established_local_addr = local_addr;
    new_conn->established_remote_addr = remote_addr;

    // 这里应该是经过三次握手后才能修改状态为ESTABLISHED
    new_conn->state = ESTABLISHED; // 这里真的握手了吗 
    */
    uint8_t waiting_tcp_recv = 0;
    //老实说我不想造一个新线程 所以就在这里设置超时重传
    uint32_t time;
    uint32_t rto= listen_sock->RTO;
    while (listen_sock->state != ESTABLISHED)
    {
        if (listen_sock->state == SYN_RECV)
        {
            if ((waiting_tcp_recv == 1) && (now_us() - time >= rto))
            {
                waiting_tcp_recv = 0; //超时重传
                char* head;
                head = create_packet_buf(listen_sock->bind_addr.port,listen_sock->established_remote_addr.port,listen_sock->seq,
                listen_sock->ack,DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,SYN_FLAG_MASK | ACK_FLAG_MASK,1,0,NULL,0);  //有啥比要模拟客户端发送吗
                sendToLayer3(head,DEFAULT_HEADER_LEN);//将TCP第二次握手消息发送
                free(head);
            }
            else if(waiting_tcp_recv == 0)
            {
                waiting_tcp_recv = 1 ;
                time = now_us();   //获取当前时间戳
                //@Todo 思考 这里是共用时间变量好还是互斥锁更快呢?
            }
            
        }
    }
    // 将新的conn放到内核建立连接的socket哈希表中
    //int hashval = cal_hash(local_addr.ip, local_addr.port, remote_addr.ip, remote_addr.port);
    int hashval = cal_hash(listen_sock->established_local_addr.ip,listen_sock->established_local_addr.port,
        listen_sock->established_remote_addr.ip,listen_sock->established_remote_addr.port);
    established_socks[hashval] = listen_sock;
 //  printf("\nsocket已放入内核: %d %d %d %d %d\n",listen_sock->established_local_addr.ip,listen_sock->established_local_addr.port,
  //      listen_sock->established_remote_addr.ip,listen_sock->established_remote_addr.port,hashval);

    // 如果new_conn的创建过程放到了tju_handle_packet中 那么accept怎么拿到这个new_conn呢
    // 在linux中 每个listen socket都维护一个已经完成连接的socket队列   完成在哪 listen_socK实际上是监听队列
    // 每次调用accept 实际上就是取出这个队列中的一个元素
    // 队列为空,则阻塞 
    return established_socks[cal_hash(listen_sock->bind_addr.ip,listen_sock->bind_addr.port,
        listen_sock->established_remote_addr.ip,listen_sock->established_remote_addr.port)];
}


/*
连接到服务端
该函数以一个socket为参数
调用函数前, 该socket还未建立连接
函数正常返回后, 该socket一定是已经完成了3次握手, 建立了连接
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
int tju_connect(tju_tcp_t* sock, tju_sock_addr target_addr){

    sock->established_remote_addr = target_addr;

    tju_sock_addr local_addr;
    local_addr.ip = inet_network(CLIENT_IP);    //只能客户端连服务器用
    local_addr.port = 5678; // 连接方进行connect连接的时候 内核中是随机分配一个可用的端口
    sock->established_local_addr.ip = local_addr.ip;
    sock->established_local_addr.port = local_addr.port;
    // 这里也不能直接建立连接 需要经过三次握手
    // 实际在linux中 connect调用后 会进入一个while循环
    // 循环跳出的条件是socket的状态变为ESTABLISHED 表面看上去就是 正在连接中 阻塞
    // 而状态的改变在别的地方进行 在我们这就是tju_handle_packet    啥比,这样耦合度太高了

    int hashval = cal_hash(sock->established_local_addr.ip,sock->established_local_addr.port,0,0);
    listen_socks[hashval] = sock;  //注册客户端在tcp握手段监听服务器消息的哈希表
    printf("已加入监听：%d\n",hashval);
    uint8_t waiting_tcp_recv = 0;
    int last_state = sock->state;
    //老实说我不想造一个新线程 所以就在这里设置超时重传
    uint32_t time;
    uint32_t rto= sock->RTO;
    char* head;
    head = create_packet_buf(sock->established_local_addr.port,target_addr.port,sock->seq,
                sock->ack,DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,SYN_FLAG_MASK,1,0,NULL,0);  //有啥比要模拟客户端发送吗
    sendToLayer3(head,DEFAULT_HEADER_LEN);//将TCP第一次握手消息发送
    free(head);

    sock->state = SYN_SENT;
    while (sock->state != ESTABLISHED)
    {
        if(sock->state == SYN_SENT)
        {
            if ((waiting_tcp_recv == 1) && (now_us() - time >= rto))
            {
                waiting_tcp_recv = 0; //超时重传
                
                head = create_packet_buf(sock->established_local_addr.port,target_addr.port,sock->seq,
                sock->ack,DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,SYN_FLAG_MASK,1,0,NULL,0);  //有啥比要模拟客户端发送吗
                sendToLayer3(head,DEFAULT_HEADER_LEN);//将TCP第一次握手消息发送
                free(head);
             }
            else if(waiting_tcp_recv == 0)
            {
                waiting_tcp_recv = 1 ;
                time = now_us();   //获取当前时间戳
                //@Todo 思考 这里是共用时间变量好还是互斥锁更快呢?
            }
        }
    }
    
    //sock->state = ESTABLISHED;

    // 将建立了连接的socket放入内核 已建立连接哈希表中
   //  printf("\nsocket已放入内核: %d %d %d %d %d\n",sock->established_local_addr.ip,sock->established_local_addr.port,
    //    sock->established_remote_addr.ip,sock->established_remote_addr.port,hashval);
    hashval = cal_hash(local_addr.ip, local_addr.port, sock->established_local_addr.ip, sock->established_local_addr.port);
    established_socks[hashval] = sock;

    return 0;
}

int tju_send(tju_tcp_t* sock, const void *buffer, int len){
    // 这里当然不能直接简单地调用sendToLayer3
    //谁写的唐氏代码
   // printf("发送消息中\n");
    char* data = malloc(len);
    memcpy(data, buffer, len);

    char* msg;
    uint16_t plen = DEFAULT_HEADER_LEN + len;
    sock->window.wnd_send->ack_cnt = sock->seq;//期望收到的ack

    uint32_t restLen = len; //记录剩下数据长度
    uint32_t ackLen =  len; //还剩多少数据待确认

    int send_len= 0;
    uint32_t rto = sock->RTO;
    uint16_t i = 0;
    uint32_t now_time;
    //开始进行窗口传输数据 阻塞
    while (ackLen !=0)
    {
        now_time = now_us();
        if (sock->window.wnd_send->packs[i].send_time == 0 && restLen!=0)
        {
                //如果窗口记录中为0则开始首次进行传输与窗口注册
            send_len = (restLen >= (MAX_LEN-DEFAULT_HEADER_LEN)) ? MAX_LEN-DEFAULT_HEADER_LEN:restLen;
            restLen -= send_len;
            plen = DEFAULT_HEADER_LEN + send_len;
            

            msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, sock->seq, sock->ack, 
            DEFAULT_HEADER_LEN,plen, ACK_FLAG_MASK, 1, 0, data+(len - (restLen+send_len)),send_len); //
            sendToLayer3(msg,plen);         //进行该数据的首次发送
            sock->window.wnd_send->ack_cnt +=  send_len;//注册的时候  期望ack步进
            sock->seq += send_len;  //发送seq步进

            sock->window.wnd_send->packs[i].msg = msg;  //不释放的便利性
            sock->window.wnd_send->packs[i].send_time = now_time; 
            sock->window.wnd_send->packs[i].send_time_base = now_time; 
            sock->window.wnd_send->packs[i].send_lenth = send_len;
            sock->window.wnd_send->packs[i].send_waiting_ack = sock->window.wnd_send->ack_cnt; //这里可以再探讨一下
            sock->window.wnd_send->packs[i].send_ok = 0;
        }
        if (sock->window.wnd_send->packs[i].send_ok == 1 && msg!=NULL)
        {//移动窗口
            ackLen -= sock->window.wnd_send->packs[i].send_lenth;//确认了这一部分的消息
            tcp_rto_count(sock,now_time - sock->window.wnd_send->packs[i].send_time_base);
            rto = sock->RTO;
            tcp_send_window_reset(&sock->window.wnd_send->packs[i]);//重置该窗口
        }

        if ((now_time - sock->window.wnd_send->packs[i].send_time >= rto)&&(sock->window.wnd_send->packs[i].send_ok == 0)&&(sock->window.wnd_send->packs[i].send_lenth>0))
        {//超时重传
            tcp_rto_backoff(sock);
            rto = sock->RTO;
            sock->window.wnd_send->packs[i].send_time = now_time;
            sendToLayer3(sock->window.wnd_send->packs[i].msg,sock->window.wnd_send->packs[i].send_lenth + DEFAULT_HEADER_LEN); 
           // printf("重传中%d %d %d\n",now_us() , sock->window.wnd_send->packs[i].send_waiting_ack,sock->window.wnd_send->packs[i].send_lenth);
        }
        
        
        i++;
        if (i == TCP_SENDWN_SIZE)
        {
            i =0;//循环查询窗口
        }
        
    }
    return len;
}

int tju_recv(tju_tcp_t* sock, void *buffer, int len){
    //printf("\n开启接收\n");
    while(sock->received_len<=0){
        // 阻塞
    }
        //buffer:接收数组
    int restLen = len;  //意义不明的int uint32_t更好其实
    int read_len=0;
    char* ptr=buffer;  //ptr拼接数据
    uint32_t time =now_us();
    uint32_t rto = sock->RTO * 2;//以重传时间的两倍判断链接断开

    while (restLen != 0)
    {
        while(pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁  这里有必要加锁吗?就两个线程
        if (sock->received_len == 0)
        {
            pthread_mutex_unlock(&(sock->recv_lock)); // 解锁
            continue;//如果缓冲区没有数据则等待
        }
        
        if (sock->received_len >= restLen)
        {
          //received_len不会超过 MAX_LEN - 包头长
            read_len = restLen;
            restLen = 0;
        }
        else
        {
            read_len = sock->received_len; // 读取sock->received_len长度的数据(全读出来)
            restLen -= sock->received_len;
        }

        memcpy(ptr, sock->received_buf, read_len);//拼接数据
        ptr += read_len;
       // printf("已阅读%d\n",len-restLen);
        if(read_len < sock->received_len) 
        { // 还剩下一些
            char* new_buf = malloc(sock->received_len - read_len);
            memcpy(new_buf, sock->received_buf + read_len, sock->received_len - read_len); //将剩下的数据复制到缓冲区开头相当于,但是此处实现得很烂就是
            free(sock->received_buf);               //哈哈你这个崽种就喜欢释放对吗
            sock->received_len -= read_len;
            sock->received_buf = new_buf;
        }
        else
        {
            free(sock->received_buf);
            sock->received_buf = NULL;          //结果就是大家设计的缓冲区被释放,难绷
            sock->received_len = 0;            //缓冲区没东西了,本次接收结束上了吗
        }
        pthread_mutex_unlock(&(sock->recv_lock)); // 解锁
    }
    return len;
}


//这个函数应该是处理接收消息至缓冲区(丢弃包头)
int tju_handle_packet(tju_tcp_t* sock, char* pkt,uint32_t dst_IP){
    
    uint32_t data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;//这样写是不规范的     东北人写的代码吧我看

   // printf("收到消息, %d %d %d %d %d %d %d\n",get_hlen(pkt),get_plen(pkt),get_seq(pkt),sock->ack, sock->window.wnd_recv->expect_seq,sock->window.wnd_recv->max_seq,sock->window.wnd_recv->base_seq);
    // 把收到的数据放到接受缓冲区
    while(pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁

    if ( (get_hlen(pkt) != get_plen(pkt))&&(get_seq(pkt) == sock->window.wnd_recv->expect_seq)
    &&(get_seq(pkt) == sock->window.wnd_recv->max_seq))
    {      //这里处理正常接收的情况 不需要滑动窗口         如果exp和max不重合证明中间有等待填补的数据
       // printf("正常处理 %d %d %d\n",get_seq(pkt),sock->window.wnd_recv->expect_seq,data_len);
        if(sock->received_buf == NULL)
        {
            sock->received_buf = malloc(data_len);
        }else 
        {
            sock->received_buf = realloc(sock->received_buf, sock->received_len + data_len); //这是将缓冲区拓展了 
        }
        memcpy(sock->received_buf + sock->received_len, pkt + DEFAULT_HEADER_LEN, data_len); //从上一次接收的末尾开始 将数据拷贝至缓冲区:sock->recvbuff 也就是说将包头丢弃了
        sock->received_len += data_len; //这里也增加了累计接收量
        sock->window.wnd_recv->expect_seq += data_len;//正常接收了这么多,那么期望的seq就增长
        sock->window.wnd_recv->base_seq +=data_len;
        sock->window.wnd_recv->max_seq +=data_len;
        sock->ack += data_len;
        char *back = create_packet_buf(sock->established_local_addr.port,sock->established_remote_addr.port,sock->seq,
                   get_seq(pkt) + get_plen(pkt) - get_hlen(pkt),DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,1,0,NULL,0); 
        sendToLayer3(back,DEFAULT_HEADER_LEN);  
        free(back);//按协议返回ack
        
    }
    else
    {
          //这里单独分离出来处理 需要tcp协议的部分
        tju_TCP_handler(sock,pkt,dst_IP);  //优雅的写法 每次触发接收时都进行TCP处理
    }    
    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    return 0;
}

int tju_close (tju_tcp_t* sock){
    return 0;
}

int tju_TCP_handler(tju_tcp_t* sock,char* head,uint32_t dst_IP)
{

    switch (sock->state)
    {
        case LISTEN:
            //监听到链接方请求
                sock->established_remote_addr.port  =  get_src(head); //拿到源发送端口
                sock->established_remote_addr.ip    =  inet_network(CLIENT_IP); // 提供的代码根本没做到符合tcp规范 草台班子

                sock->ack = get_seq(head) + 1;
                char* send_handshack_buff = create_packet_buf(sock->bind_addr.port,sock->established_remote_addr.port,sock->seq,
                sock->ack,DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,SYN_FLAG_MASK | ACK_FLAG_MASK,1,0,NULL,0);//ack+1 回传seq
                sendToLayer3(send_handshack_buff,DEFAULT_HEADER_LEN);//将TCP第二次握手消息发送
                free(send_handshack_buff);
                sock->state = SYN_RECV;  //等待接收同步消息    
            break;
        case SYN_RECV:
                    //比较难绷的一点是这里不用设置ACK和Seq=1
                if ((get_ack(head) == (sock->seq + 1)))
                {//这里由于不需要确定任何消息,ack不变,get_seq(head) == sock->ack
                    sock->seq++; //确认上一次带SYN的消息以后seq步进
                    printf("已建立链接");
                    sock->established_local_addr.ip = sock->bind_addr.ip;
                    sock->established_local_addr.port = sock->bind_addr.port;
                    
                    sock->window.wnd_recv->base_seq = sock->ack; //开始接收那一刻,我们设置起始接收seq来确保后续传来的信息拼接
                    sock->window.wnd_recv->expect_seq = sock->ack;//期望下一次接收的seq
                    sock->window.wnd_recv->max_seq = sock->ack;//首先初始化接收窗口
                    sock->state = ESTABLISHED; //服务器完成三次握手
                } 
            break;
        case SYN_SENT:
                if (get_ack(head) == sock->seq + 1)
                {
                   //printf("收到服务端ip:%d\n",dst_IP);
                    sock->established_remote_addr.ip =   inet_network(SERVER_IP);
                    sock->established_remote_addr.port = get_src(head);
                    sock->ack = get_seq(head) +1; //确认服务器带SYN的seq
                    sock->seq++; // 确认上一次发送的SYN后seq自动+1

                    sock->window.wnd_recv->base_seq = sock->ack; //开始接收那一刻,我们设置起始接收seq来确保后续传来的信息拼接
                    sock->window.wnd_recv->expect_seq = sock->ack;//期望下一次接收的seq
                    sock->window.wnd_recv->max_seq = sock->ack;//首先初始化接收窗口
                    sock->state = ESTABLISHED;//客户端完成三次握手
                    char* send_handshack_buff = create_packet_buf(sock->established_local_addr.port,sock->established_remote_addr.port,sock->seq,
                    sock->ack,DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,1,0,NULL,0);
                    sendToLayer3(send_handshack_buff,DEFAULT_HEADER_LEN);  //进行最后一次握手,不需要等待返回
                    free(send_handshack_buff);
                }
            break;
        case ESTABLISHED:
                
               // printf("TCP处理, %d %d %d %d %d %d %d %d\n",get_hlen(head),get_plen(head),get_seq(head),get_ack(head),sock->ack, sock->window.wnd_recv->expect_seq,sock->window.wnd_recv->max_seq,sock->window.wnd_recv->base_seq);
                if (get_plen(head) == get_hlen(head))
                {//证明这是一个确认接收包,不需要返回任何消息,只需要移动窗口
                    for (int i = 0; i < TCP_SENDWN_SIZE; i++)
                    {
                        if (sock->window.wnd_send->packs[i].send_waiting_ack == get_ack(head))
                        {
                            sock->window.wnd_send->packs[i].send_ok = 1; //如果ack匹配则认为确认发送
                        }
                    }
                }
                else
                {//正常收到消息
                    if (get_seq(head) < sock->window.wnd_recv->expect_seq)
                    {//如果是重复接收
                        char *pkt = create_packet_buf(sock->established_local_addr.port,sock->established_remote_addr.port,sock->seq,
                        get_seq(head) + get_plen(head) - get_hlen(head),DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,1,0,NULL,0); 
                        sendToLayer3(pkt,DEFAULT_HEADER_LEN);  
                        free(pkt);
                        //仅返回ack
                    }
                    else if (get_seq(head) > sock->window.wnd_recv->expect_seq )
                    {//如果超前接收了
                        //首先判断是否超出窗口 虽然这不大可能发生    
                        if ((get_seq(head) - sock->window.wnd_recv->base_seq + get_plen(head) - get_hlen(head)) > TCP_RECVWN_SIZE )
                        {
                            return 0;
                        }
                        sock->window.wnd_recv->max_seq = sock->window.wnd_recv->max_seq > get_seq(head)?sock->window.wnd_recv->max_seq : get_seq(head);
                        //更新接收窗口最大seq
                        
                        memcpy(&sock->window.wnd_recv->received[get_seq(head) - sock->window.wnd_recv->base_seq],
                        head+get_hlen(head),get_plen(head) - get_hlen(head));//检查基准seq偏移然后复制到窗口临时数组中等待拼接 
                        tcp_WriteMark(sock,get_seq(head),get_plen(head) - get_hlen(head));//记录可滑动
                        //收到消息以后按照协议要返回ack 
                        char *pkt = create_packet_buf(sock->established_local_addr.port,sock->established_remote_addr.port,sock->seq,
                        get_seq(head) + get_plen(head) - get_hlen(head),DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,1,0,NULL,0); 
                        sendToLayer3(pkt,DEFAULT_HEADER_LEN);  
                        free(pkt);
                    }
                    else
                    {//此处expect_seq == get_seq(head) 填补数据 expect_seq步进 并且检查滑动 
                        memcpy(&sock->window.wnd_recv->received[get_seq(head) - sock->window.wnd_recv->base_seq],
                        head+DEFAULT_HEADER_LEN,get_plen(head) - get_hlen(head));//检查基准seq偏移然后复制到窗口临时数组中等待拼接 

                        uint16_t step = get_plen(head) - get_hlen(head);//查询seq是否可以继续滑动
                        while (step)
                        {
                            sock->window.wnd_recv->expect_seq+=step;
                            //检查滑动后是否追上max
                            if (sock->window.wnd_recv->expect_seq == sock->window.wnd_recv->max_seq)
                            {//终于追上你了 真是绕了好大一圈啊 .jonny go!
                                step = tcp_FindMark(sock,sock->window.wnd_recv->expect_seq);//将最后的max弹出
                                uint32_t data_len = sock->window.wnd_recv->expect_seq + step - sock->window.wnd_recv->base_seq;
                                if(sock->received_buf == NULL)
                                {
                                    sock->received_buf = malloc(data_len);
                                }else 
                                {
                                    sock->received_buf = realloc(sock->received_buf, sock->received_len +data_len); //这是将缓冲区拓展了 
                                }
                                memcpy(sock->received_buf + sock->received_len, sock->window.wnd_recv->received, data_len); //将拼接好的数据放入缓冲区
                                sock->received_len += data_len;
                                //处理窗口
                                sock->window.wnd_recv->base_seq+=data_len;
                                sock->window.wnd_recv->expect_seq+=step;
                                sock->window.wnd_recv->max_seq+=step;
                                sock->ack+=data_len;
                                break;
                            }//进if后step必然为0
                            step = tcp_FindMark(sock,sock->window.wnd_recv->expect_seq);//查询seq是否可以继续滑动
                        }            
                        //收到消息以后按照协议要返回ack 
                        char *pkt = create_packet_buf(sock->established_local_addr.port,sock->established_remote_addr.port,sock->seq,
                        get_seq(head) + get_plen(head) - get_hlen(head),DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,1,0,NULL,0); 
                        sendToLayer3(pkt,DEFAULT_HEADER_LEN);  
                        free(pkt);
                    }
                    
                }
            break;
        default:
            break;
    }
    
    return 0;
}

