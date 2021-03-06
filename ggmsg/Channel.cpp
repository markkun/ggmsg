#include "stdafx.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include <boost/bind.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <random>
#include <boost/asio.hpp>

#include "3Des.h"
#include "Diagnosis.h"


std::atomic_uint Channel::m_nConnectIDSerial = 0;

Channel::Channel(ChannelMgr *pChannelMgr, tcp::socket socket, boost::asio::io_context *pIoContext, int nChannalType)
	: socket_(std::move(socket))
	, m_timerHeartBeat(*pIoContext)
{
	m_pIoContext = pIoContext;
	m_pChannelMgr = pChannelMgr;
	m_nLastActiveTime = std::time(0);
	m_tCreateTime = std::time(0);
	m_nSendTimes = 0;
	m_nRecvTimes = 0;
	m_nSendBytes = 0;
	m_nRecvBytes = 0;

	m_bSending = false;

	m_channalType = (ChannalType)nChannalType;
	m_nConnectID = ++m_nConnectIDSerial;
	m_nRecvBufLen = 1024;
	m_pRecvBuf = new char[m_nRecvBufLen];
	// 1. 创建随机数的生成器
	std::mt19937 randomGenerator;
	// 2. 创建随机数的分布函数
	std::uniform_int_distribution<> urd(0, 255);
	// 3. 装配生成器与分布函数，生成变量生成器
	/*std::variate_generator<mt19937, uniform_real_distribution<double> > vg(randomGenerator, urd);*/
	for (int i = 0; i < sizeof(m_chSelfEncryptKey); ++i) {
		m_chSelfEncryptKey[i] = urd(randomGenerator);
	}

	//std::wstring s;
	//for (int i=0;i < sizeof(m_chSelfEncryptKey);++i)
	//{
	//	wchar_t buf[16] = { 0 };
	//	_stprintf_s(buf, L"%0x", (unsigned char)m_chSelfEncryptKey[i]);
	//	s += buf;
	//}

	//DiagnosisTrace(L"self key:%s\n", s.c_str());
}

Channel::~Channel()
{
	delete[]m_pRecvBuf;

}


void Channel::do_close()
{
	m_pChannelMgr->DeleteService(shared_from_this());
	socket_.shutdown(tcp::socket::shutdown_both);
	socket_.close();

	if (m_channalType == ChannalType::positive) {
		if (m_pChannelMgr->m_fnOnPositiveDisConnect) {
			m_pChannelMgr->m_fnOnPositiveDisConnect(m_nServiceID, m_nConnectID);
		}

		if (m_pChannelMgr->working_) {
			m_pChannelMgr->InternalConnect(m_strRemoteIp, m_uRemotePort);
		}
	}
	else {
		if (m_pChannelMgr->m_fnOnPassiveDisConnect) {
			m_pChannelMgr->m_fnOnPassiveDisConnect(m_nServiceID, m_nConnectID);
		}
	}
}

void Channel::Start()
{
	m_uRemotePort = socket_.remote_endpoint().port();
	m_strRemoteIp = socket_.remote_endpoint().address().to_string();
	DoReadHead();

	
}

void Channel::PumpHeartBeat() {
	auto self(shared_from_this());
	m_timerHeartBeat.expires_from_now(30);
	m_timerHeartBeat.async_wait([self, this](const boost::system::error_code& ec) {
		if (ec) {
			return;
		}

		std::time_t tNow = std::time_t(0);
		if (tNow - m_nLastActiveTime > 30) {
			HeartBeat();
		}

		PumpHeartBeat();
	});
}

void Channel::HeartBeat()
{
	const int nPackageLen = sizeof(NetHead) + sizeof(HeartBeatReq);
	char data[nPackageLen] = { 0 };
	auto pHead = (NetHead*)data;
	pHead->nHeadSize = sizeof(NetHead);
	pHead->nBodySize = sizeof(ShakeHandReq);
	pHead->nMsgType = ggmtHeartBeat;

	auto pReq = (HeartBeatReq*)(data + pHead->nHeadSize);
	strcpy_s(pReq->chInfo, "alive");

	write(data, nPackageLen);
}

void Channel::DoReqShakeHand()
{
	const int nPackageLen = sizeof(NetHead) + sizeof(ShakeHandReq);
	char data[nPackageLen] = { 0 };
	auto pHead = (NetHead*)data;
	pHead->nHeadSize = sizeof(NetHead);
	pHead->nBodySize = sizeof(ShakeHandReq);
	pHead->nMsgType = ggmtShakeHand;
	
	auto pReq = (ShakeHandReq*)(data + pHead->nHeadSize);
	pReq->nServiceID = m_pChannelMgr->GetServiceID();
	memcpy(pReq->chEncryptKey, m_chSelfEncryptKey, sizeof(pReq->chEncryptKey));

	write(data, nPackageLen);
	DoReadHead();
}

void Channel::OnRecvShakeHandRsp(const void *pPacket, int nLength)
{
	DiagnosisTrace(L"OnRecvShakeHandRsp: %0x\n", this);
	auto pHead = (NetHead*)pPacket;
	auto pRsp = (ShakeHandRsp*)((char*)pPacket + pHead->nHeadSize);
	if (pRsp->nResult == 0) {
		m_nServiceID = pRsp->nServiceID;
		memcpy(m_chPeerEncryptKey, pRsp->chEncryptKey, sizeof(m_chPeerEncryptKey));
	}

	m_pChannelMgr->AddService(shared_from_this());

	if (m_pChannelMgr->m_fnOnPositiveConnect) {
		m_pChannelMgr->m_fnOnPositiveConnect(m_nServiceID, m_nConnectID);
	}
}

void Channel::OnRecvShakeHandReq(const void *pPacket, int nLength)
{
	DiagnosisTrace(L"OnRecvShakeHandReq:%0x\n", this);

	auto pReqHead = (NetHead*)pPacket;
	auto pReq = (ShakeHandReq*)((char*)pPacket + pReqHead->nHeadSize);
	m_nServiceID = pReq->nServiceID;
	memcpy(m_chPeerEncryptKey, pReq->chEncryptKey, sizeof(m_chPeerEncryptKey));

	const int nPackageLen = sizeof(NetHead) + sizeof(ShakeHandRsp);
	char buf[nPackageLen] = { 0 };
	auto pRspHead = (NetHead*)(buf);
	pRspHead->nHeadSize = sizeof(NetHead);
	pRspHead->nBodySize = sizeof(ShakeHandRsp);
	pRspHead->nMsgType = pReqHead->nMsgType;
	auto pRsp = (ShakeHandRsp*)(buf + pRspHead->nHeadSize);
	pRsp->nServiceID = m_pChannelMgr->GetServiceID();
	m_pChannelMgr->AddService(shared_from_this());
	memcpy(pRsp->chEncryptKey, m_chSelfEncryptKey, sizeof(pRsp->chEncryptKey));

	write(buf, nPackageLen);

	if (m_pChannelMgr->m_fnOnPassiveConnect) {
		m_pChannelMgr->m_fnOnPassiveConnect(m_nServiceID, m_nConnectID);
	}
}

void Channel::DoReadHead() 
{
	auto self(shared_from_this());
	boost::asio::async_read(socket_, boost::asio::buffer(m_pRecvBuf, sizeof(NetHead)), 
		[this, self](boost::system::error_code ec, std::size_t length)
	{
		if (ec) {
			do_close();
		}
		else {
			//std::cout << "read head completed \n";
			auto pNetHead = (NetHead*)m_pRecvBuf;
			DoReadBody(*pNetHead);
		}
	});
}

void Channel::DoReadBody(const NetHead & head)
{
	auto self(shared_from_this());
	int nHeadSize = head.nHeadSize;
	int nBodySize = head.nBodySize;
	if (m_nRecvBufLen < nHeadSize + nBodySize) {
		m_nRecvBufLen = (nHeadSize + nBodySize + 1024) / 1024 * 1024;
		char *pNewBuf = new char[m_nRecvBufLen];
		memcpy(pNewBuf, m_pRecvBuf, nHeadSize);
		delete[]m_pRecvBuf;
		m_pRecvBuf = pNewBuf;
	}

	boost::asio::async_read(socket_, boost::asio::buffer(m_pRecvBuf + nHeadSize, nBodySize),
		[this, self](boost::system::error_code ec, std::size_t length)
	{
		if (ec) {
			do_close();
		}
		else {
			auto pNetHead = (NetHead*)m_pRecvBuf;
			OnReceivePacket(m_pRecvBuf, pNetHead->nHeadSize + pNetHead->nBodySize);
			DoReadHead();
		}
	});
}

void Channel::write(char *data, std::size_t length) {
	auto self(shared_from_this());
	char *pData = new char[length];
	memcpy(pData, data, length);

	m_pIoContext->post([self, pData, length]() {
		DataEle de = { pData,length };
		self->m_dataQueue.push(de);
		self->do_write();
	});
}

void Channel::do_write() {
	if ( m_bSending || m_dataQueue.empty()) {
		return;
	}

	auto self(shared_from_this());

	m_bSending = true;
	auto d = m_dataQueue.front();
	m_dataQueue.pop();
	auto pData = d.pData;
	boost::asio::async_write(socket_, boost::asio::buffer(pData, d.nLen),
		[this, self, pData](boost::system::error_code ec, std::size_t length)
	{
		delete[]pData;
		m_bSending = false;
		if (ec)
		{
			//do_close();
			//std::string msg = ec.message();
			// 这里不用do_close()的，这里如果调用do_close，会触发do_read返回错误
			// 从而再次调用do_close引发异常
		}
		else {
			m_nSendBytes += length;
			m_nSendTimes++;

			do_write();
		}

	});
}
	
std::mutex m;
void Channel::SendMsg(const void *pMsg, std::size_t nDataLen)
{
	auto self(shared_from_this());
	int nEncryptLen = (nDataLen + 7) / 8 * 8;
	nEncryptLen = 128;
	char *pBuf = new char[nEncryptLen];
	memset(pBuf, 0, nEncryptLen);
	memcpy(pBuf, pMsg, nDataLen);
	
	int nPackageLen = sizeof(NetHead) + nEncryptLen;
	char *pData = new char[nPackageLen];
	memset(pData, 0, nPackageLen);

	auto pHead = (NetHead*)pData;
	pHead->nMagic = 0;
	pHead->nHeadSize = sizeof(NetHead);
	pHead->nBodySize = nEncryptLen;
	pHead->nBeforeCompressLen = 0;
	pHead->nBeforeEncryptLen = nDataLen;
	pHead->nCompressMethod = 0;
	pHead->nEncryptMethod = ggem3des;
	pHead->nMsgType = ggmtMsg;

	char key[32] = { 0 };
	strcpy_s(key, "01234567890123456789");
// 	std::lock_guard<std::mutex> lk(m);
// 	C3DES des;
// 	des.DoDES(pData + pHead->nHeadSize, pBuf, nEncryptLen, key, 32, ENCRYPT);

	memcpy(pData + pHead->nHeadSize, pBuf, nEncryptLen);
	delete[]pBuf;

	m_pIoContext->post([self, pData, nPackageLen]() {
		DataEle de = { pData,nPackageLen };
		self->m_dataQueue.push(de);
		self->do_write();
	});
}

void Channel::OnReceivePacket(const void *pPacket, int nLength)
{
	//std::cout << "OnReceivePacket\n";
	auto pHead = (NetHead*)(pPacket);
	switch (pHead->nMsgType)
	{
	case ggmtShakeHand: {
		if (m_channalType == ChannalType::positive) {
			OnRecvShakeHandRsp(pPacket, nLength);
		}
		else if(m_channalType == ChannalType::passive)
		{
			OnRecvShakeHandReq(pPacket, nLength);
		}
	}
	break;
	case ggmtMsg: {
		if (m_pChannelMgr->m_fnOnReceiveMsg) {
			char *pData = new char[pHead->nBodySize];

			/*std::wstring s;
			for (int i = 0; i < sizeof(m_chPeerEncryptKey); ++i)
			{
				wchar_t buf[16] = { 0 };
				_stprintf_s(buf, L"%0x", (unsigned char)m_chPeerEncryptKey[i]);
				s += buf;
			}

			DiagnosisTrace(L"peer key:%s\n", s.c_str());*/
			//std::lock_guard<std::mutex> lk(m);
			// 解密
			C3DES des;
			//des.DoDES(pData, (char *)pPacket + pHead->nHeadSize, pHead->nBodySize, m_chPeerEncryptKey, 32, DECRYPT);
			/*char key[32] = { 0 };
			strcpy_s(key, "01234567890123456789");
			des.DoDES(pData, (char *)pPacket + pHead->nHeadSize, pHead->nBodySize, key, 32, DECRYPT);*/
			memcpy(pData, (char *)pPacket + pHead->nHeadSize, pHead->nBodySize);
			m_pChannelMgr->m_fnOnReceiveMsg(m_nServiceID, m_nConnectID, pData, pHead->nBeforeEncryptLen);
			delete[]pData;
		}
	}break;
	default:
		break;
	}
}
