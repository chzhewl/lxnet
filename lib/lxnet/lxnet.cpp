
/*
 * Copyright (C) lcinx
 * lcinx@163.com
 */

#include <stdio.h>
#include <string.h>
#include "cthread.h"
#include "net_module.h"
#include "lxnet.h"
#include "net_buf.h"
#include "pool.h"
#include "msgbase.h"
#include "log.h"
#include "crosslib.h"
#include "lxnet_datainfo.h"

struct datainfomgr {
	bool isinit;
	int64 lasttime;
	struct datainfo datatable[enum_netdata_end];
};

static struct datainfomgr s_datamgr = {false};

struct infomgr {
	bool isinit;
	struct poolmgr *encrypt_pool;
	cspin encrypt_lock;

	struct poolmgr *socket_pool;
	cspin socket_lock;
	
	struct poolmgr *listen_pool;
	cspin listen_lock;
};

static struct infomgr s_infomgr = {false};

struct encryptinfo {
	enum {
		enum_encrypt_len = 32,
	};

	int maxidx;
	int nowidx;
	char buf[enum_encrypt_len];
};

static inline void on_sendmsg(size_t len) {
	assert(s_datamgr.isinit);
	catomic_inc(&s_datamgr.datatable[enum_netdata_total].sendmsgnum);
	catomic_fetch_add(&s_datamgr.datatable[enum_netdata_total].sendbytes, (int64)len);

	catomic_inc(&s_datamgr.datatable[enum_netdata_now].sendmsgnum);
	catomic_fetch_add(&s_datamgr.datatable[enum_netdata_now].sendbytes, (int64)len);
}

static inline void on_recvmsg(size_t len) {
	assert(s_datamgr.isinit);

	catomic_inc(&s_datamgr.datatable[enum_netdata_total].recvmsgnum);
	catomic_fetch_add(&s_datamgr.datatable[enum_netdata_total].recvbytes, (int64)len);

	catomic_inc(&s_datamgr.datatable[enum_netdata_now].recvmsgnum);
	catomic_fetch_add(&s_datamgr.datatable[enum_netdata_now].recvbytes, (int64)len);
}

static void datarun() {
	assert(s_datamgr.isinit);
	int64 currenttime = get_millisecond();
	if (currenttime - s_datamgr.lasttime < 1000)
		return;

	s_datamgr.lasttime = currenttime;

	time_t curtm = time(NULL);
	struct datainfo *maxinfo = &s_datamgr.datatable[enum_netdata_max];
	struct datainfo *nowinfo = &s_datamgr.datatable[enum_netdata_now];

	if (catomic_read(&maxinfo->sendmsgnum) < catomic_read(&nowinfo->sendmsgnum)) {
		catomic_set(&maxinfo->sendmsgnum, catomic_read(&nowinfo->sendmsgnum));
		maxinfo->tm_sendmsgnum = curtm;
	}

	if (catomic_read(&maxinfo->recvmsgnum) < catomic_read(&nowinfo->recvmsgnum)) {
		catomic_set(&maxinfo->recvmsgnum, catomic_read(&nowinfo->recvmsgnum));
		maxinfo->tm_recvmsgnum = curtm;
	}

	if (catomic_read(&maxinfo->sendbytes) < catomic_read(&nowinfo->sendbytes)) {
		catomic_set(&maxinfo->sendbytes, catomic_read(&nowinfo->sendbytes));
		maxinfo->tm_sendbytes = curtm;
	}

	if (catomic_read(&maxinfo->recvbytes) < catomic_read(&nowinfo->recvbytes)) {
		catomic_set(&maxinfo->recvbytes, catomic_read(&nowinfo->recvbytes));
		maxinfo->tm_recvbytes = curtm;
	}

	catomic_set(&nowinfo->sendmsgnum, 0);
	catomic_set(&nowinfo->recvmsgnum, 0);
	catomic_set(&nowinfo->sendbytes, 0);
	catomic_set(&nowinfo->recvbytes, 0);
}

static bool infomgr_init(size_t socketnum, size_t listennum) {
	if (s_infomgr.isinit)
		return false;

	s_infomgr.encrypt_pool = poolmgr_create(sizeof(struct encryptinfo), 8, socketnum * 2, 1, "encrypt buffer pool");
	s_infomgr.socket_pool = poolmgr_create(sizeof(lxnet::Socketer), 8, socketnum, 1, "Socketer obj pool");
	s_infomgr.listen_pool = poolmgr_create(sizeof(lxnet::Listener), 8, listennum, 1, "Listen obj pool");
	if (!s_infomgr.socket_pool || !s_infomgr.encrypt_pool || !s_infomgr.listen_pool) {
		poolmgr_release(s_infomgr.socket_pool);
		poolmgr_release(s_infomgr.encrypt_pool);
		poolmgr_release(s_infomgr.listen_pool);
		return false;
	}

	cspin_init(&s_infomgr.encrypt_lock);
	cspin_init(&s_infomgr.socket_lock);
	cspin_init(&s_infomgr.listen_lock);
	s_infomgr.isinit = true;

	s_datamgr.isinit = true;
	s_datamgr.lasttime = 0;

	time_t curtm = time(NULL);
	for (int i = 0; i < enum_netdata_end; ++i) {
		catomic_set(&s_datamgr.datatable[i].sendmsgnum, 0);
		catomic_set(&s_datamgr.datatable[i].recvmsgnum, 0);
		catomic_set(&s_datamgr.datatable[i].sendbytes, 0);
		catomic_set(&s_datamgr.datatable[i].recvbytes, 0);
		s_datamgr.datatable[i].tm_sendmsgnum = curtm;
		s_datamgr.datatable[i].tm_recvmsgnum = curtm;
		s_datamgr.datatable[i].tm_sendbytes = curtm;
		s_datamgr.datatable[i].tm_recvbytes = curtm;
	}
	return true;
}

static void infomgr_release() {
	if (!s_infomgr.isinit)
		return;

	s_infomgr.isinit = false;
	s_datamgr.isinit = false;
	poolmgr_release(s_infomgr.socket_pool);
	poolmgr_release(s_infomgr.encrypt_pool);
	poolmgr_release(s_infomgr.listen_pool);
	cspin_destroy(&s_infomgr.encrypt_lock);
	cspin_destroy(&s_infomgr.socket_lock);
	cspin_destroy(&s_infomgr.listen_lock);
}

static void encrypt_info_release(void *info) {
	if (!s_infomgr.isinit)
		return;

	cspin_lock(&s_infomgr.encrypt_lock);
	poolmgr_freeobject(s_infomgr.encrypt_pool, info);
	cspin_unlock(&s_infomgr.encrypt_lock);
}


namespace lxnet {



/* 创建一个用于监听的对象 */
Listener *Listener::Create() {
	if (!s_infomgr.isinit) {
		assert(false && "Listener Create not init!");
		return NULL;
	}

	struct listener *ls = listener_create();
	if (!ls)
		return NULL;

	cspin_lock(&s_infomgr.listen_lock);
	Listener *self = (Listener *)poolmgr_getobject(s_infomgr.listen_pool);
	cspin_unlock(&s_infomgr.listen_lock);
	if (!self) {
		listener_release(ls);
		return NULL;
	}

	self->m_self = ls;
	return self;
}

/* 释放一个用于监听的对象 */
void Listener::Release(Listener *self) {
	if (!self)
		return;

	if (self->m_self) {
		listener_release(self->m_self);
		self->m_self = NULL;
	}

	cspin_lock(&s_infomgr.listen_lock);
	poolmgr_freeobject(s_infomgr.listen_pool, self);
	cspin_unlock(&s_infomgr.listen_lock);
}

/* 监听 */
bool Listener::Listen(unsigned short port, int backlog) {
	return listener_listen(m_self, port, backlog);
}

/* 关闭用于监听的套接字，停止监听 */
void Listener::Close() {
	listener_close(m_self);
}

/* 测试是否已关闭 */
bool Listener::IsClose() {
	return listener_isclose(m_self);
}

/* 在指定的监听socket上接受连接 */
Socketer *Listener::Accept(bool bigbuf) {
	struct socketer *sock = listener_accept(m_self, bigbuf);
	if (!sock)
		return NULL;

	cspin_lock(&s_infomgr.socket_lock);
	Socketer *self = (Socketer *)poolmgr_getobject(s_infomgr.socket_pool);
	cspin_unlock(&s_infomgr.socket_lock);
	if (!self) {
		socketer_release(sock);
		return NULL;
	}

	self->m_encrypt = NULL;
	self->m_decrypt = NULL;
	self->m_self = sock;
	return self;
}

/* 检测是否有新的连接 */
bool Listener::CanAccept() {
	return listener_can_accept(m_self);
}



/* 创建一个Socketer对象 */
Socketer *Socketer::Create(bool bigbuf) {
	if (!s_infomgr.isinit) {
		assert(false && "Socketer Create not init!");
		return NULL;
	}

	struct socketer *so = socketer_create(bigbuf);
	if (!so)
		return NULL;

	cspin_lock(&s_infomgr.socket_lock);
	Socketer *self = (Socketer *)poolmgr_getobject(s_infomgr.socket_pool);
	cspin_unlock(&s_infomgr.socket_lock);
	if (!self) {
		socketer_release(so);
		return NULL;
	}

	self->m_encrypt = NULL;
	self->m_decrypt = NULL;
	self->m_self = so;
	return self;
}

/* 释放Socketer对象，会自动调用关闭等善后操作 */
void Socketer::Release(Socketer *self) {
	if (!self)
		return;
	
	if (self->m_self) {
		socketer_release(self->m_self);
		self->m_self = NULL;
	}

	self->m_encrypt = NULL;
	self->m_decrypt = NULL;

	cspin_lock(&s_infomgr.socket_lock);
	poolmgr_freeobject(s_infomgr.socket_pool, self);
	cspin_unlock(&s_infomgr.socket_lock);
}

/* 设置接收数据字节的临界值，超过此值，则停止接收，若小于等于0，则视为不限制 */
void Socketer::SetRecvLimit(int size) {
	socketer_set_recv_limit(m_self, size);
}

/* 设置发送数据字节的临界值，若缓冲中数据长度大于此值，则断开此连接，若为0，则视为不限制 */
void Socketer::SetSendLimit(int size) {
	socketer_set_send_limit(m_self, size);
}

/* (对发送数据起作用)设置启用压缩，若要启用压缩，则此函数在创建socket对象后即刻调用 */
void Socketer::UseCompress() {
	socketer_use_compress(m_self);
}

/* (慎用)(对接收的数据起作用)启用解压缩，网络库会负责解压缩操作，仅供客户端使用 */
void Socketer::UseUncompress() {
	socketer_use_uncompress(m_self);
}

/*
 * 设置加密/解密函数， 以及特殊用途的参与加密/解密逻辑的数据。
 * 若加密/解密函数为NULL，则保持默认。
 */
void Socketer::SetEncryptDecryptFunction(void (*encryptfunc)(void *logicdata, char *buf, int len), void (*release_encrypt_logicdata)(void *), void *encrypt_logicdata, void (*decryptfunc)(void *logicdata, char *buf, int len), void (*release_decrypt_logicdata)(void *), void *decrypt_logicdata) {
	socketer_set_encrypt_function(m_self, encryptfunc, release_encrypt_logicdata, encrypt_logicdata);
	socketer_set_decrypt_function(m_self, decryptfunc, release_decrypt_logicdata, decrypt_logicdata);
}

static void encrypt_decrypt_as_key_do_func(void *logicdata, char *buf, int len) {
	struct encryptinfo *o = (struct encryptinfo *)logicdata;

	int i;
	for (i = 0; i < len; i++) {
		if (o->nowidx >= o->maxidx)
			o->nowidx = 0;

		buf[i] ^= o->buf[o->nowidx];
		o->nowidx++;
	}
}

/* 设置加密key */
void Socketer::SetEncryptKey(const char *key, int key_len) {
	if (!key || key_len < 0)
		return;

	if (!m_encrypt) {
		cspin_lock(&s_infomgr.encrypt_lock);
		m_encrypt = (struct encryptinfo *)poolmgr_getobject(s_infomgr.encrypt_pool);
		cspin_unlock(&s_infomgr.encrypt_lock);

		if (m_encrypt) {
			m_encrypt->maxidx = 0;
			m_encrypt->nowidx = 0;
			memset(m_encrypt->buf, 0, sizeof(m_encrypt->buf));
			socketer_set_encrypt_function(m_self, encrypt_decrypt_as_key_do_func, encrypt_info_release, m_encrypt);
		}
	}

	if (m_encrypt) {
		m_encrypt->maxidx = key_len > encryptinfo::enum_encrypt_len ? encryptinfo::enum_encrypt_len : key_len;
		memcpy(&m_encrypt->buf, key, m_encrypt->maxidx);
	}
}

/* 设置解密key */
void Socketer::SetDecryptKey(const char *key, int key_len) {
	if (!key || key_len < 0)
		return;

	if (!m_decrypt) {
		cspin_lock(&s_infomgr.encrypt_lock);
		m_decrypt = (struct encryptinfo *)poolmgr_getobject(s_infomgr.encrypt_pool);
		cspin_unlock(&s_infomgr.encrypt_lock);

		if (m_decrypt) {
			m_decrypt->maxidx = 0;
			m_decrypt->nowidx = 0;
			memset(m_decrypt->buf, 0, sizeof(m_decrypt->buf));
			socketer_set_decrypt_function(m_self, encrypt_decrypt_as_key_do_func, encrypt_info_release, m_decrypt);
		}
	}

	if (m_decrypt) {
		m_decrypt->maxidx = key_len > encryptinfo::enum_encrypt_len ? encryptinfo::enum_encrypt_len : key_len;
		memcpy(&m_decrypt->buf, key, m_decrypt->maxidx);
	}
	
}

/* (启用加密) */
void Socketer::UseEncrypt() {
	socketer_use_encrypt(m_self);
}

/* (启用解密) */
void Socketer::UseDecrypt() {
	socketer_use_decrypt(m_self);
}

/* 启用TGW接入 */
void Socketer::UseTGW() {
	socketer_use_tgw(m_self);
}

/* 关闭用于连接的socket对象 */
void Socketer::Close() {
	socketer_close(m_self);
}

/* 连接指定的服务器 */
bool Socketer::Connect(const char *ip, short port) {
	return socketer_connect(m_self, ip, port);
}

/* 测试socket套接字是否已关闭 */
bool Socketer::IsClose() {
	return socketer_isclose(m_self);
}

/* 获取此客户端ip地址 */
void Socketer::GetIP(char *ip, size_t len) {
	socketer_getip(m_self, ip, len);
}

/* 获取发送缓冲待发送字节数(若为0表示不存在待发送数据或数据已写入系统缓冲) */
int Socketer::GetSendBufferByteSize() {
	return socketer_get_send_buffer_byte_size(m_self);
}

/*
 * 发送数据，仅仅是把数据压入包队列中，
 * adddata为附加到pMsg后面的数据，当然会自动修改pMsg的长度，addsize指定adddata的长度
 */
bool Socketer::SendMsg(Msg *pMsg, void *adddata, size_t addsize) {
	if (!pMsg)
		return false;

	if (adddata && addsize == 0) {
		assert(false);
		return false;
	}

	if (!adddata && addsize != 0) {
		assert(false);
		return false;
	}

	if (pMsg->GetLength() <= (int)sizeof(int))
		return false;

	if (pMsg->GetLength() + addsize >= _MAX_MSG_LEN) {
		assert(false && "if (pMsg->GetLength() + addsize >= _MAX_MSG_LEN)");
		log_error("	if (pMsg->GetLength() + addsize >= _MAX_MSG_LEN)");
		return false;
	}

	if (socketer_send_islimit(m_self, pMsg->GetLength() + addsize)) {
		Close();
		return false;
	}

	on_sendmsg(pMsg->GetLength() + addsize);

	if (adddata && addsize != 0) {
		bool res1, res2;
		int onesend = pMsg->GetLength();
		pMsg->SetLength(onesend + addsize);
		res1 = socketer_sendmsg(m_self, pMsg, onesend);
		
		//这里切记要修改回去。 例：对于同一个包遍历发送给一个列表，然后每次都附带不同尾巴。。。这种情景，那么必须如此恢复。
		pMsg->SetLength(onesend);
		res2 = socketer_sendmsg(m_self, adddata, addsize);
		return (res1 && res2);
	} else {
		return socketer_sendmsg(m_self, pMsg, pMsg->GetLength());
	}
}

/* 对as3发送策略文件 */
bool Socketer::SendPolicyData() {
	//as3套接字策略文件
	char buf[512] = "<cross-domain-policy> <allow-access-from domain=\"*\" secure=\"false\" to-ports=\"*\"/> </cross-domain-policy> ";
	size_t datasize = strlen(buf);
	if (socketer_send_islimit(m_self, datasize)) {
		Close();
		return false;
	}

	on_sendmsg(datasize + 1);
	return socketer_sendmsg(m_self, buf, datasize + 1);
}

/* 发送TGW信息头 */
bool Socketer::SendTGWInfo(const char *domain, int port) {
	char buf[1024] = {};
	size_t datasize;
	snprintf(buf, sizeof(buf) - 1, "tgw_l7_forward\r\nHost: %s:%d\r\n\r\n", domain, port);
	buf[sizeof(buf) - 1] = '\0';
	datasize = strlen(buf);
	if (socketer_send_islimit(m_self, datasize)) {
		Close();
		return false;
	}

	socketer_set_raw_datasize(m_self, datasize);
	on_sendmsg(datasize);
	return socketer_sendmsg(m_self, buf, datasize);
}

/* 触发真正的发送数据 */
void Socketer::CheckSend() {
	socketer_checksend(m_self);
}

/* 尝试投递接收操作 */
void Socketer::CheckRecv() {
	socketer_checkrecv(m_self);
}

/* 接收数据 */
Msg *Socketer::GetMsg(char *buf, size_t bufsize) {
	Msg *obj = (Msg *)socketer_getmsg(m_self, buf, bufsize);
	if (obj) {
		if (obj->GetLength() < (int)sizeof(Msg)) {
			Close();
			return NULL;
		}

		on_recvmsg(obj->GetLength());
	}
	return obj;
}

/* 发送数据 */
bool Socketer::SendData(const char *data, size_t datasize) {
	if (!data)
		return false;

	if (socketer_send_islimit(m_self, datasize)) {
		Close();
		return false;
	}

	on_sendmsg(datasize);

	return socketer_sendmsg(m_self, (void *)data, datasize);
}

/* 接收数据 */
char *Socketer::GetData(char *buf, size_t bufsize, int *datalen) {
	char *data = (char *)socketer_getdata(m_self, buf, bufsize, datalen);
	if (data) {
		on_recvmsg(*datalen);
	}

	return data;
}



/*
 * 初始化网络，
 * bigbufsize指定大块的大小，bigbufnum指定大块的数目，
 * smallbufsize指定小块的大小，smallbufnum指定小块的数目
 * listen num指定用于监听的套接字的数目，socket num用于连接的总数目
 * threadnum指定网络线程数目，若设置为小于等于0，则会开启cpu个数的线程数目
 */
bool net_init(size_t bigbufsize, size_t bigbufnum, size_t smallbufsize, size_t smallbufnum,
		size_t listenernum, size_t socketnum, int threadnum) {
	
	if (!infomgr_init(socketnum, listenernum))
		return false;

	if (!netinit(bigbufsize, bigbufnum, smallbufsize, smallbufnum,
			listenernum, socketnum, threadnum)) {
		infomgr_release();
		return false;
	}

	return true;
}

/* 释放网络相关 */
void net_release() {
	infomgr_release();
	netrelease();
}

/* 执行相关操作，需要在主逻辑中调用此函数 */
void net_run() {
	datarun();
	netrun();
}


/* 获取此进程所在的机器名 */
bool GetHostName(char *buf, size_t buflen) {
	return socketer_gethostname(buf, buflen);
}

/* 根据域名获取ip地址 */
bool GetHostIPByName(const char *hostname, char *buf, size_t buflen, bool ipv6) {
	return socketer_gethostbyname(hostname, buf, buflen, ipv6);
}


/* 启用/禁用接受的连接导致的错误日志，并返回之前的值 */
bool SetEnableErrorLog(bool flag) {
	return buf_set_enable_errorlog(flag);
}

/* 获取当前启用或禁用接受的连接导致的错误日志 */
bool GetEnableErrorLog() {
	return buf_get_enable_errorlog();
}



static char s_memory_info[1024*64];

/* 获取socket对象池，listen对象池，大块池，小块池的使用情况 */
const char *GetNetMemoryInfo() {
	size_t index = 0;

	snprintf(&s_memory_info[index], sizeof(s_memory_info) - 1 - index, "%s", "lxnet lib memory pool info:\n<+++++++++++++++++++++++++++++++++++++++++++++++++++++>");
	index = strlen(s_memory_info);

	cspin_lock(&s_infomgr.encrypt_lock);
	poolmgr_getinfo(s_infomgr.encrypt_pool, &s_memory_info[index], sizeof(s_memory_info) - 1 - index);
	cspin_unlock(&s_infomgr.encrypt_lock);

	index = strlen(s_memory_info);

	cspin_lock(&s_infomgr.socket_lock);
	poolmgr_getinfo(s_infomgr.socket_pool, &s_memory_info[index], sizeof(s_memory_info) - 1 - index);
	cspin_unlock(&s_infomgr.socket_lock);
	
	index = strlen(s_memory_info);

	cspin_lock(&s_infomgr.listen_lock);
	poolmgr_getinfo(s_infomgr.listen_pool, &s_memory_info[index], sizeof(s_memory_info) - 1 - index);
	cspin_unlock(&s_infomgr.listen_lock);

	index = strlen(s_memory_info);
	netmemory_info(&s_memory_info[index], sizeof(s_memory_info) - 1 - index);

	index = strlen(s_memory_info);
	snprintf(&s_memory_info[index], sizeof(s_memory_info) - 1 - index, "%s", "<+++++++++++++++++++++++++++++++++++++++++++++++++++++>");
	return s_memory_info;
}

//获取当前时间。格式为"2010-09-16 23:20:20"
static const char *get_current_time_str(time_t tval, char *buf, size_t buflen) {
	if (buflen < 32)
		return "null";

	struct tm tm_result;
	struct tm *currTM = safe_localtime(&tval, &tm_result);
	snprintf(buf, buflen - 1, "%d-%02d-%02d %02d:%02d:%02d", currTM->tm_year + 1900, currTM->tm_mon + 1, currTM->tm_mday, currTM->tm_hour, currTM->tm_min, currTM->tm_sec);
	buf[buflen - 1] = '\0';
	return buf;
}

/* 获取网络库通讯详情 */
const char *GetNetDataAllInfo() {
	static char infostr[1024*8];
	if (!s_datamgr.isinit)
		return "not init!";

	struct datainfo *totalinfo = &s_datamgr.datatable[enum_netdata_total];
	struct datainfo *maxinfo = &s_datamgr.datatable[enum_netdata_max];
	struct datainfo *nowinfo = &s_datamgr.datatable[enum_netdata_now];

	double numunit = 1000*1000;
	double bytesunit = 1024*1024;
	double totalsendmsgnum = double(catomic_read(&totalinfo->sendmsgnum) / numunit);
	double totalsendbytes = double(catomic_read(&totalinfo->sendbytes) / bytesunit);
	double totalrecvmsgnum = double(catomic_read(&totalinfo->recvmsgnum) / numunit);
	double totalrecvbytes = double(catomic_read(&totalinfo->recvbytes) / bytesunit);

	double maxsendmsgnum = (double)catomic_read(&maxinfo->sendmsgnum);
	double maxsendbytes = (double)catomic_read(&maxinfo->sendbytes) / bytesunit;
	double maxrecvmsgnum = (double)catomic_read(&maxinfo->recvmsgnum);
	double maxrecvbytes = (double)catomic_read(&maxinfo->recvbytes) / bytesunit;

	double nowsendmsgnum = (double)catomic_read(&nowinfo->sendmsgnum);
	double nowsendbytes = (double)catomic_read(&nowinfo->sendbytes) / bytesunit;
	double nowrecvmsgnum = (double)catomic_read(&nowinfo->recvmsgnum);
	double nowrecvbytes = (double)catomic_read(&nowinfo->recvbytes) / bytesunit;

	char buf_sendmsgnum[128] = {0};
	char buf_sendbytes[128] = {0};
	char buf_recvmsgnum[128] = {0};
	char buf_recvbytes[128] = {0};
	get_current_time_str(maxinfo->tm_sendmsgnum, buf_sendmsgnum, sizeof(buf_sendmsgnum));
	get_current_time_str(maxinfo->tm_sendbytes, buf_sendbytes, sizeof(buf_sendbytes));
	get_current_time_str(maxinfo->tm_recvmsgnum, buf_recvmsgnum, sizeof(buf_recvmsgnum));
	get_current_time_str(maxinfo->tm_recvbytes, buf_recvbytes, sizeof(buf_recvbytes));

	snprintf(infostr, sizeof(infostr) - 1, "total:\n\tsend msg num:%.6fM, send bytes:%.6fMB\n\trecv msg num:%.6fM, recv bytes:%.6fMB\nmax:\n\tsend msg num:%.0f, time:%s\n\tsend bytes:%.6fMB, time:%s\n\trecv msg num:%.0f, time:%s\n\trecv bytes:%.6fMB, time:%s\nnow:\n\tsend msg num:%.0f, send bytes:%.6fMB\n\trecv msg num:%.0f, recv bytes:%.6fMB\n", totalsendmsgnum, totalsendbytes, totalrecvmsgnum, totalrecvbytes, maxsendmsgnum, buf_sendmsgnum, maxsendbytes, buf_sendbytes, maxrecvmsgnum, buf_recvmsgnum, maxrecvbytes, buf_recvbytes, nowsendmsgnum, nowsendbytes, nowrecvmsgnum, nowrecvbytes);

	return infostr;
}

/* 获取网络库的指定类型的详情 */
struct datainfo *GetNetDataInfoByType(int type) {
	if (type < 0 || type >= (int)enum_netdata_end)
		return NULL;

	return &s_datamgr.datatable[type];
}

}

