/*!
 * \file TraderYD.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "TraderYD.h"

#include "../Includes/WTSError.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../Share/decimal.h"
#include "../Share/ModuleHelper.hpp"

#include <boost/filesystem.hpp>

const char* ENTRUST_SECTION = "entrusts";
const char* ORDER_SECTION = "orders";

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(ITraderSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	fmt::format_to(buffer, format, args...);

	sink->handleTraderLog(ll, buffer);
}

inline WTSDirectionType wrapPosDirection(int dirType)
{
	if (YD_PD_Long == dirType)
		return WDT_LONG;
	else
		return WDT_SHORT;
}

inline WTSDirectionType wrapDirectionType(int dirType, int offsetType)
{
	if (YD_D_Buy == dirType)
		if (offsetType == YD_OF_Open)
			return WDT_LONG;
		else
			return WDT_SHORT;
	else
		if (offsetType == YD_OF_Open)
			return WDT_SHORT;
		else
			return WDT_LONG;
}

inline WTSPriceType wrapPriceType(int priceType)
{
	if (YD_ODT_Market == priceType)
		return WPT_ANYPRICE;
	else if (YD_ODT_Limit == priceType)
		return WPT_LIMITPRICE;
	else
		return WPT_LASTPRICE;
}

inline WTSOffsetType wrapOffsetType(int offType)
{
	if (YD_OF_Open == offType)
		return WOT_OPEN;
	else if (YD_OF_Close == offType)
		return WOT_CLOSE;
	else if (YD_OF_CloseToday == offType)
		return WOT_CLOSETODAY;
	else if (YD_OF_CloseYesterday == offType)
		return WOT_CLOSEYESTERDAY;
	else
		return WOT_FORCECLOSE;
}

inline WTSOrderState wrapOrderState(int orderState)
{
	switch (orderState)
	{
	case YD_OS_Accepted: return WOS_NotTraded_NotQueuing;
	case YD_OS_Queuing: return WOS_NotTraded_Queuing;
	case YD_OS_Canceled:
	case YD_OS_Rejected:
		return WOS_Canceled;
	case YD_OS_AllTraded: return WOS_AllTraded;
	default:
		return WOS_Submitting;
	}
}


extern "C"
{
	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderYD *instance = new TraderYD();
		return instance;
	}

	EXPORT_FLAG void deleteTrader(ITraderApi* &trader)
	{
		if (NULL != trader)
		{
			delete trader;
			trader = NULL;
		}
	}
}

TraderYD::TraderYD()
	: m_pUserAPI(NULL)
	, m_mapPosition(NULL)
	, m_mapOrders(NULL)
	, m_mapTrades(NULL)
	, m_wrapperState(WS_NOTLOGIN)
	, m_uLastQryTime(0)
	, m_iRequestID(0)
	, m_bCatchup(false)
	, m_bStopped(false)
{
}


TraderYD::~TraderYD()
{
}


void TraderYD::notifyReadyForLogin(bool hasLoginFailed)
{
	if (m_sink)
		m_sink->handleEvent(WTE_Connect, 0);
}


void TraderYD::notifyLogin(int errorNo, int maxOrderRef, bool isMonitor)
{
	if (errorNo == 0)
	{
		m_wrapperState = WS_LOGINED;

		// ����Ự����
		m_orderRef = maxOrderRef;
		///��ȡ��ǰ������
		m_lDate = m_pUserAPI->getTradingDay();

		write_log(m_sink, LL_INFO, "[TraderYD] {} Login succeed, AppID: {}, Trading Day: {}",
			m_strUser.c_str(), m_strAppID.c_str(), m_lDate);
	}
	else
	{
		write_log(m_sink, LL_ERROR, "[TraderYD] {} Login failed: {}", m_strUser.c_str(), errorNo);
		m_wrapperState = WS_LOGINFAILED;

		if (m_sink)
			m_sink->onLoginResult(false, "Login failed", 0);
	}
}

void TraderYD::notifyFailedOrder(const YDInputOrder *pFailedOrder, const YDInstrument *pInstrument, const YDAccount *pAccount)
{
	WTSEntrust* entrust = makeEntrust(pFailedOrder, pInstrument);
	if (entrust)
	{
		WTSError *err = makeError(pFailedOrder->ErrorNo, WEC_ORDERINSERT);
		//g_orderMgr.onRspEntrust(entrust, err);
		if (m_sink)
			m_sink->onRspEntrust(entrust, err);
		entrust->release();
		err->release();
	}
}

void TraderYD::notifyFailedCancelOrder(const YDFailedCancelOrder *pFailedCancelOrder, const YDExchange *pExchange, const YDAccount *pAccount)
{
	WTSError* error = makeError(pFailedCancelOrder->ErrorNo, WEC_ORDERCANCEL);
	if (m_sink)
		m_sink->onTraderError(error);
}


void TraderYD::notifyFinishInit()
{
	/*
	 *	����Ĵ����߼��Ƚϸ���
	 *	�ڵ�һ�ε�½�ɹ��Ժ󣬵ײ��ȥ��ѯ��������
	 *	�ײ�������Ժ󣬻ᴥ������ص�
	 *	������Ҫ������ȡ���ս����Ժ������
	 *	Ȼ�������ûر���������ƴ��
	 *	�����notifyCatchup��ʱ����֪ͨlistener
	 */

	//�Ȳ��ʽ�
	{
		const YDAccount* accInfo = m_pUserAPI->getMyAccount();

		WTSAccountInfo* accountInfo = WTSAccountInfo::create();
		accountInfo->setDescription(m_strUser.c_str());
		//accountInfo->setUsername(m_strUserName.c_str());
		accountInfo->setPreBalance(accInfo->PreBalance);
		accountInfo->setDeposit(accInfo->Deposit);
		accountInfo->setWithdraw(accInfo->Withdraw);
		accountInfo->setBalance(accountInfo->getPreBalance() + accountInfo->getDeposit() - accountInfo->getWithdraw());
		accountInfo->setCurrency("CNY");

		if (m_ayFunds == NULL)
			m_ayFunds = WTSArray::create();

		m_ayFunds->append(accountInfo, false);
	}

	//�ٲ�ֲ�
	{
		if (NULL == m_mapPosition)
			m_mapPosition = DataMap::create();

		int cnt = m_pUserAPI->getPrePositionCount();
		for (int i = 0; i < cnt; i++)
		{
			const YDPrePosition* pInfo = m_pUserAPI->getPrePosition(i);
			const YDInstrument* instInfo = pInfo->m_pInstrument;

			WTSContractInfo* contract = m_bdMgr->getContract(instInfo->InstrumentID, instInfo->m_pExchange->ExchangeID);
			if (contract)
			{
				WTSCommodityInfo* commInfo = contract->getCommInfo();
				std::string key = StrUtil::printf("{}-{}", contract->getCode(), wrapPosDirection(pInfo->PositionDirection));
				WTSPositionItem* pos = (WTSPositionItem*)m_mapPosition->get(key);
				if (pos == NULL)
				{
					pos = WTSPositionItem::create(contract->getCode(), commInfo->getCurrency(), commInfo->getExchg());
					pos->setContractInfo(contract);
					m_mapPosition->add(key, pos, false);
				}

				pos->setDirection(wrapPosDirection(pInfo->PositionDirection));
				pos->setPrePosition(pInfo->PrePosition);
				pos->setNewPosition(0);

				pos->setMargin(0);
				pos->setDynProfit(0);
				pos->setPositionCost(pInfo->PrePosition*commInfo->getVolScale()*pInfo->PreSettlementPrice);

				if (pos->getTotalPosition() != 0)
				{
					pos->setAvgPrice(pInfo->PreSettlementPrice);
				}
				else
				{
					pos->setAvgPrice(0);
				}

				pos->setAvailPrePos(pos->getPrePosition());
				pos->setAvailNewPos(0);
			}
		}
	}
}


void TraderYD::notifyOrder(const YDOrder *pOrder, const YDInstrument *pInstrument, const YDAccount *pAccount)
{
	WTSOrderInfo *orderInfo = makeOrderInfo(pOrder, pInstrument);
	if (orderInfo)
	{
		//���������ﶪ
		if (NULL == m_mapOrders)
			m_mapOrders = DataMap::create();

		const char* oid = orderInfo->getOrderID();
		auto it = m_mapOrders->find(oid);
		if(it == m_mapOrders->end())
		{
			//����ö����ǵ�һ�α�����
			//�����Ƿ���ƽ��ί��
			//�����ƽ��ί�У���Ҫ������������
			if (orderInfo->getOffsetType() != WOT_OPEN)
			{
				std::string key = StrUtil::printf("{}-{}", orderInfo->getCode(), orderInfo->getDirection());
				WTSPositionItem* pos = (WTSPositionItem*)m_mapPosition->get(key);
				double preQty = pos->getPrePosition();
				double newQty = pos->getNewPosition();
				double availPre = pos->getAvailPrePos();
				double availNew = pos->getAvailNewPos();

				WTSCommodityInfo* commInfo = orderInfo->getContractInfo()->getCommInfo();
				if(commInfo->getCoverMode() == CM_CoverToday)
				{
					if (orderInfo->getOffsetType() == WOT_CLOSETODAY)
						availNew -= orderInfo->getVolume();
					else
						availPre -= orderInfo->getVolume();
				}
				else
				{
					//������ƽ��ƽ�����ȶ�����֣��ٶ�����
					double maxQty = min(availPre, orderInfo->getVolume());
					availPre -= maxQty;
					if(decimal::lt(orderInfo->getVolume(), maxQty))
					{
						availNew -= orderInfo->getVolume() - maxQty;
					}
				}

				pos->setAvailPrePos(availPre);
				pos->setAvailNewPos(availNew);
			}
		}
		else
		{
			WTSOrderInfo* preOrd = (WTSOrderInfo*)it->second;
			//����������ǵ�һ�α����ͣ����Ƿ��ǳ���
			//����ǳ���������֮�䶩��״̬������Ч�ģ����ƽ��ί��Ҫ�ͷŶ��������
			if(preOrd->isAlive() && orderInfo->getOrderState() == WOS_Canceled && orderInfo->getOffsetType() != WOT_OPEN)
			{
				std::string key = StrUtil::printf("{}-{}", orderInfo->getCode(), orderInfo->getDirection());
				WTSPositionItem* pos = (WTSPositionItem*)m_mapPosition->get(key);
				double preQty = pos->getPrePosition();
				double newQty = pos->getNewPosition();
				double availPre = pos->getAvailPrePos();
				double availNew = pos->getAvailNewPos();

				double untrade = orderInfo->getVolume() - orderInfo->getVolTraded();

				WTSCommodityInfo* commInfo = orderInfo->getContractInfo()->getCommInfo();
				if (commInfo->getCoverMode() == CM_CoverToday)
				{
					if (orderInfo->getOffsetType() == WOT_CLOSETODAY)
						availNew += untrade;
					else
						availPre += untrade;
				}
				else
				{
					//������ƽ��ƽ�������ͷŽ�֣����ͷ����
					double maxQty = min(newQty-availNew , untrade);
					availNew += maxQty;
					if (decimal::lt(untrade, maxQty))
					{
						availPre += orderInfo->getVolume() - maxQty;
					}
				}

				pos->setAvailPrePos(availPre);
				pos->setAvailNewPos(availNew);
			}
		}
		m_mapOrders->add(oid, orderInfo, false);

		//����Ѿ�׷���ˣ���ֱ�����Ƴ�ȥ
		if (m_sink && m_bCatchup)
		{
			m_sink->onPushOrder(orderInfo);
		}
	}
}


void TraderYD::notifyTrade(const YDTrade *pTrade, const YDInstrument *pInstrument, const YDAccount *pAccount)
{
	WTSTradeInfo *trdInfo = makeTradeRecord(pTrade, pInstrument);
	if (trdInfo)
	{
		//���������ﶪ
		if (NULL == m_mapTrades)
			m_mapTrades = DataMap::create();

		const char* tid = trdInfo->getTradeID();
		WTSContractInfo* contract = trdInfo->getContractInfo();
		WTSCommodityInfo* commInfo = contract->getCommInfo();
		auto it = m_mapTrades->find(tid);
		if(it == m_mapTrades->end())
		{
			m_mapTrades->add(tid, trdInfo, false);

			//�ɽ��ر�����Ҫ���³ֲ�
			std::string key = StrUtil::printf("{}-{}", trdInfo->getCode(), trdInfo->getDirection());
			WTSPositionItem* pos = (WTSPositionItem*)m_mapPosition->get(key);
			if(pos == NULL)
			{
				pos = WTSPositionItem::create(contract->getCode(), commInfo->getCurrency(), commInfo->getExchg());
				pos->setContractInfo(contract);
				m_mapPosition->add(key, pos, false);
			}

			double preQty = pos->getPrePosition();
			double newQty = pos->getNewPosition();
			double availPre = pos->getAvailPrePos();
			double availNew = pos->getAvailNewPos();

			double qty = trdInfo->getVolume();

			if(trdInfo->getOffsetType() == WOT_OPEN)
			{
				newQty += qty;
				availNew += qty;

				//����һ���ǽ��
				pos->setNewPosition(newQty);
				pos->setAvailNewPos(availNew);
			}
			else
			{
				//ƽ��Ҫ����
				if (commInfo->getCoverMode() == CM_CoverToday)
				{
					//ƽ�ֲ��ø��¿��óֲ�
					//��Ϊ���óֲ��ڶ����ر��ĵط��Ѿ����¹���
					if (trdInfo->getOffsetType() == WOT_CLOSETODAY)
						newQty -= qty;
					else
						preQty -= qty;
				}
				else
				{
					//������ƽ��ƽ�����ȼ�����֣��ڵ������
					double maxQty = min(preQty, qty);
					preQty -= maxQty;
					if (decimal::lt(qty, maxQty))
					{
						newQty -= (qty - maxQty);
					}
				}

				pos->setNewPosition(newQty);
				pos->setPrePosition(preQty);
			}
		}

		if (m_sink && m_bCatchup)
			m_sink->onPushTrade(trdInfo);
	}
}

void TraderYD::notifyCaughtUp()
{
	m_bCatchup = true;

	if(!m_bApiInited)
	{
		//ȫ����ʼ�������Ժ���֪ͨ��¼�ɹ�
		m_wrapperState = WS_ALLREADY;

		if (m_sink)
			m_sink->onLoginResult(true, "", m_lDate);

		m_bApiInited = true;
	}
}

void TraderYD::notifyAccount(const YDAccount *pAccount)
{
	
}

bool TraderYD::init(WTSVariant* config)
{
	m_strCfgFile = config->getCString("config");
	m_strUser = config->getCString("user");
	m_strPass = config->getCString("pass");

	m_strAppID = config->getCString("appid");
	m_strAuthCode = config->getCString("authcode");

	std::string module = config->getCString("ydmodule");
	if (module.empty())
		module = "yd";

	m_strModule = getBinDir() + DLLHelper::wrap_module(module.c_str(), "");

	m_hInstYD = DLLHelper::load_library(m_strModule.c_str());
	m_funcCreator = (YDCreator)DLLHelper::get_symbol(m_hInstYD, "makeYDApi");

	return true;
}

void TraderYD::release()
{
	m_bStopped = true;

	if (m_pUserAPI)
	{
		m_pUserAPI->startDestroy();
		m_pUserAPI = NULL;
	}

	if (m_mapOrders)
		m_mapOrders->clear();

	if (m_mapPosition)
		m_mapPosition->clear();

	if (m_mapTrades)
		m_mapTrades->clear();
}

void TraderYD::connect()
{
	m_pUserAPI = m_funcCreator(m_strCfgFile.c_str());

	if (m_pUserAPI)
	{
		m_pUserAPI->start(this);
	}
}

void TraderYD::disconnect()
{
	if (m_pUserAPI == NULL)
		return;

	m_pUserAPI->disconnect();
}

bool TraderYD::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length == 0)
		return false;

	try
	{
		memset(buffer, 0, length);
		uint32_t orderref = m_orderRef.fetch_add(1) + 1;
		sprintf(buffer, "%s#%010u", m_strUser.c_str(), orderref);
		return true;
	}
	catch (...)
	{

	}

	return false;
}

void TraderYD::registerSpi(ITraderSpi *listener)
{
	m_sink = listener;
	if (m_sink)
	{
		m_bdMgr = listener->getBaseDataMgr();
	}
}

uint32_t TraderYD::genRequestID()
{
	return m_iRequestID.fetch_add(1) + 1;
}

int TraderYD::login(const char* user, const char* pass, const char* productInfo)
{
	m_strUser = user;
	m_strPass = pass;

	if (m_pUserAPI == NULL)
	{
		return -1;
	}

	m_wrapperState = WS_LOGINING;
	doLogin();

	return 0;
}

int TraderYD::doLogin()
{
	if (m_pUserAPI == NULL)
	{
		return 0;
	}

	if (!m_pUserAPI->login(m_strUser.c_str(), m_strPass.c_str(), m_strAppID.c_str(), m_strAuthCode.c_str()))
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[TraderYD] Sending login request failed");
	}

	return 0;
}

int TraderYD::logout()
{
	if (m_pUserAPI == NULL)
	{
		return -1;
	}

	return 0;
}

int TraderYD::orderInsert(WTSEntrust* entrust)
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	/*
	CThostFtdcInputOrderField req;
	memset(&req, 0, sizeof(req));
	///���͹�˾����
	strcpy(req.BrokerID, m_strBroker.c_str());
	///Ͷ���ߴ���
	strcpy(req.InvestorID, m_strUser.c_str());
	///��Լ����
	strcpy(req.InstrumentID, entrust->getCode());

	strcpy(req.ExchangeID, entrust->getExchg());

	if (strlen(entrust->getUserTag()) == 0)
	{
		///��������
		sprintf(req.OrderRef, "%u", m_orderRef.fetch_add(0));

		//���ɱ���ί�е���
		//entrust->setEntrustID(generateEntrustID(m_frontID, m_sessionID, m_orderRef++).c_str());	
	}
	else
	{
		uint32_t fid, sid, orderref;
		extractEntrustID(entrust->getEntrustID(), fid, sid, orderref);
		//entrust->setEntrustID(entrust->getUserTag());
		///��������
		sprintf(req.OrderRef, "%u", orderref);
	}

	if (strlen(entrust->getUserTag()) > 0)
	{
		//m_mapEntrustTag[entrust->getEntrustID()] = entrust->getUserTag();
		m_iniHelper.writeString(ENTRUST_SECTION, entrust->getEntrustID(), entrust->getUserTag());
		m_iniHelper.save();
	}

	WTSContractInfo* ct = m_bdMgr->getContract(entrust->getCode(), entrust->getExchg());
	if (ct == NULL)
		return -1;

	///�û�����
	//	TThostFtdcUserIDType	UserID;
	///�����۸�����: �޼�
	req.OrderPriceType = wrapPriceType(entrust->getPriceType(), strcmp(entrust->getExchg(), "CFFEX") == 0);
	///��������: 
	req.Direction = wrapDirectionType(entrust->getDirection(), entrust->getOffsetType());
	///��Ͽ�ƽ��־: ����
	req.CombOffsetFlag[0] = wrapOffsetType(entrust->getOffsetType());
	///���Ͷ���ױ���־
	req.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
	///�۸�
	req.LimitPrice = entrust->getPrice();
	///����: 1
	req.VolumeTotalOriginal = (int)entrust->getVolume();
	///��Ч������: ������Ч
	req.TimeCondition = wrapTimeCondition(entrust->getTimeCondition());
	///GTD����
	//	TThostFtdcDateType	GTDDate;
	///�ɽ�������: �κ�����
	req.VolumeCondition = THOST_FTDC_VC_AV;
	///��С�ɽ���: 1
	req.MinVolume = 1;
	///��������: ����
	req.ContingentCondition = THOST_FTDC_CC_Immediately;
	///ֹ���
	//	TThostFtdcPriceType	StopPrice;
	///ǿƽԭ��: ��ǿƽ
	req.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
	///�Զ������־: ��
	req.IsAutoSuspend = 0;
	///ҵ��Ԫ
	//	TThostFtdcBusinessUnitType	BusinessUnit;
	///������
	//	TThostFtdcRequestIDType	RequestID;
	///�û�ǿ����־: ��
	req.UserForceClose = 0;

	int iResult = m_pUserAPI->ReqOrderInsert(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_sink, LL_ERROR, "[TraderYD] Order inserting failed: {}", iResult);
	}
	*/

	return 0;
}

int TraderYD::orderAction(WTSEntrustAction* action)
{
	if (m_wrapperState != WS_ALLREADY)
		return -1;

	uint32_t orderref;
	if (!extractEntrustID(action->getEntrustID(), orderref))
		return -1;

	/*
	CThostFtdcInputOrderActionField req;
	memset(&req, 0, sizeof(req));
	///���͹�˾����
	strcpy(req.BrokerID, m_strBroker.c_str());
	///Ͷ���ߴ���
	strcpy(req.InvestorID, m_strUser.c_str());
	///��������
	sprintf(req.OrderRef, "%u", orderref);
	///������
	///ǰ�ñ��
	req.FrontID = frontid;
	///�Ự���
	req.SessionID = sessionid;
	///������־
	req.ActionFlag = wrapActionFlag(action->getActionFlag());
	///��Լ����
	strcpy(req.InstrumentID, action->getCode());

	req.LimitPrice = action->getPrice();

	req.VolumeChange = (int32_t)action->getVolume();

	strcpy(req.OrderSysID, action->getOrderID());
	strcpy(req.ExchangeID, action->getExchg());

	int iResult = m_pUserAPI->ReqOrderAction(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_sink, LL_ERROR, "[TraderYD] Sending cancel request failed: {}", iResult);
	}
	*/
	return 0;
}

int TraderYD::queryAccount()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			if (m_sink) m_sink->onRspAccount(m_ayFunds);
		});
	}

	return 0;
}

int TraderYD::queryPositions()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			WTSArray* ayPos = WTSArray::create();

			if (m_mapPosition && m_mapPosition->size() > 0)
			{
				for (auto it = m_mapPosition->begin(); it != m_mapPosition->end(); it++)
				{
					ayPos->append(it->second, true);
				}
			}

			if (m_sink)
				m_sink->onRspPosition(ayPos);

			ayPos->release();
		});
	}

	return 0;
}

int TraderYD::queryOrders()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			WTSArray* ayOrders = WTSArray::create();
			if(m_mapOrders)
			{
				for(auto it = m_mapOrders->begin(); it != m_mapOrders->end(); it++)
				{
					ayOrders->append(it->second, true);
				}
			}
			if (m_sink) m_sink->onRspOrders(ayOrders);
		});
	}

	return 0;
}

int TraderYD::queryTrades()
{
	if (m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this]() {
			WTSArray* ayTrades = WTSArray::create();
			if (m_mapTrades)
			{
				for (auto it = m_mapTrades->begin(); it != m_mapTrades->end(); it++)
				{
					ayTrades->append(it->second, true);
				}
			}
			if (m_sink) m_sink->onRspTrades(ayTrades);
		});
	}

	return 0;
}

WTSOrderInfo* TraderYD::makeOrderInfo(const YDOrder* orderField, const YDInstrument* instInfo)
{
	const YDExchange* exchgInfo = instInfo->m_pExchange;

	WTSContractInfo* contract = m_bdMgr->getContract(instInfo->InstrumentID, exchgInfo->ExchangeID);
	if (contract == NULL)
		return NULL;

	WTSOrderInfo* pRet = WTSOrderInfo::create();
	pRet->setPrice(orderField->Price);
	pRet->setVolume(orderField->OrderVolume);
	pRet->setDirection(wrapDirectionType(orderField->Direction, orderField->OffsetFlag));
	pRet->setPriceType(wrapPriceType(orderField->OrderType));
	pRet->setTimeCondition(WTC_GFD);
	pRet->setOffsetType(wrapOffsetType(orderField->OffsetFlag));

	pRet->setVolTraded(orderField->TradeVolume);
	pRet->setVolLeft(orderField->OrderVolume - orderField->TradeVolume);

	pRet->setCode(contract->getCode());
	pRet->setExchange(contract->getExchg());

	uint32_t uDate = m_lDate;
	uint32_t uTime = orderField->InsertTime;
	pRet->setOrderDate(uDate);
	pRet->setOrderTime(TimeUtils::makeTime(uDate, uTime * 1000));

	pRet->setOrderState(wrapOrderState(orderField->OrderStatus));
	if (orderField->OrderStatus == YD_OS_Rejected)
		pRet->setError(true);

	pRet->setEntrustID(generateEntrustID(orderField->OrderRef).c_str());
	pRet->setOrderID(fmt::format("{}",orderField->OrderSysID).c_str());

	pRet->setStateMsg("");

	std::string usertag = m_iniHelper.readString(ENTRUST_SECTION, pRet->getEntrustID(), "");
	if(usertag.empty())
	{
		pRet->setUserTag(pRet->getEntrustID());
	}
	else
	{
		pRet->setUserTag(usertag.c_str());

		if (strlen(pRet->getOrderID()) > 0)
		{
			m_iniHelper.writeString(ORDER_SECTION, StrUtil::trim(pRet->getOrderID()).c_str(), usertag.c_str());
			m_iniHelper.save();
		}
	}

	return pRet;
}

WTSEntrust* TraderYD::makeEntrust(const YDInputOrder *entrustField, const YDInstrument* instInfo)
{
	WTSContractInfo* ct = m_bdMgr->getContract(instInfo->InstrumentID, instInfo->m_pExchange->ExchangeID);
	if (ct == NULL)
		return NULL;

	WTSEntrust* pRet = WTSEntrust::create(
		ct->getCode(),
		entrustField->OrderVolume,
		entrustField->Price,
		ct->getExchg());

	pRet->setDirection(wrapDirectionType(entrustField->Direction, entrustField->OffsetFlag));
	pRet->setPriceType(wrapPriceType(entrustField->OrderType));
	pRet->setOffsetType(wrapOffsetType(entrustField->OffsetFlag));
	pRet->setTimeCondition(WTC_GFD);

	pRet->setEntrustID(generateEntrustID(entrustField->OrderRef).c_str());

	std::string usertag = m_iniHelper.readString(ENTRUST_SECTION, pRet->getEntrustID());
	if (!usertag.empty())
		pRet->setUserTag(usertag.c_str());

	return pRet;
}

WTSError* TraderYD::makeError(int errorno, WTSErroCode ec)
{
	WTSError* pRet = WTSError::create(ec, StrUtil::printf("ErrorNo: %d", errorno).c_str());
	return pRet;
}

WTSTradeInfo* TraderYD::makeTradeRecord(const YDTrade *tradeField, const YDInstrument* instInfo)
{
	WTSContractInfo* contract = m_bdMgr->getContract(instInfo->InstrumentID, instInfo->m_pExchange->ExchangeID);
	if (contract == NULL)
		return NULL;

	WTSCommodityInfo* commInfo = contract->getCommInfo();
	WTSSessionInfo* sInfo = commInfo->getSessionInfo();

	WTSTradeInfo *pRet = WTSTradeInfo::create(contract->getCode(), commInfo->getExchg());
	pRet->setVolume(tradeField->Volume);
	pRet->setPrice(tradeField->Price);
	pRet->setTradeID(fmt::format("{}",tradeField->TradeID).c_str());

	uint32_t uDate = m_lDate;
	uint32_t uTime = tradeField->TradeTime;

	pRet->setTradeDate(uDate);
	pRet->setTradeTime(TimeUtils::makeTime(uDate, uTime * 1000));

	WTSDirectionType dType = wrapDirectionType(tradeField->Direction, tradeField->OffsetFlag);

	pRet->setDirection(dType);
	pRet->setOffsetType(wrapOffsetType(tradeField->OffsetFlag));
	pRet->setRefOrder(fmt::format("{}", tradeField->OrderSysID).c_str());
	pRet->setTradeType(WTT_Common);

	double amount = commInfo->getVolScale()*tradeField->Volume*pRet->getPrice();
	pRet->setAmount(amount);

	std::string usertag = m_iniHelper.readString(ORDER_SECTION, StrUtil::trim(pRet->getRefOrder()).c_str());
	if (!usertag.empty())
		pRet->setUserTag(usertag.c_str());

	return pRet;
}

std::string TraderYD::generateEntrustID(uint32_t orderRef)
{
	return StrUtil::printf("%s#%010u", m_strUser, orderRef);
}

bool TraderYD::extractEntrustID(const char* entrustid, uint32_t &orderRef)
{
	//Market.FrontID.SessionID.OrderRef
	const StringVector &vecString = StrUtil::split(entrustid, "#");
	if (vecString.size() != 2)
		return false;

	orderRef = strtoul(vecString[1].c_str(), NULL, 10);

	return true;
}

bool TraderYD::isConnected()
{
	return (m_wrapperState == WS_ALLREADY);
}