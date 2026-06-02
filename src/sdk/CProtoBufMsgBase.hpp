#pragma once

#include "protobufs/steammessages_base.pb.h"
#include "protobufs/encrypted_app_ticket.pb.h"
#include "protobufs/steammessages_clientserver.pb.h"
#include "protobufs/steammessages_clientserver_2.pb.h"
#include "protobufs/steammessages_clientserver_appinfo.pb.h"
#include "protobufs/steammessages_clientserver_friends.pb.h"
#include "protobufs/steammessages_clientserver_userstats.pb.h"

#include <cstdint>

enum EMsgType : uint16_t
{
	EMSG_SERVICE_METHOD_RESPONSE = 147,            //k_EMsgServiceMethodResponse (incoming ServiceMethod reply)
	EMSG_SERVICE_METHOD_CALL_FROM_CLIENT = 151,    //k_EMsgServiceMethodCallFromClient (outgoing ServiceMethod call)
	EMSG_GAMESPLAYED_NO_DATABLOB = 715,
	EMSG_GAMESPLAYED = 742,
	EMSG_REQUEST_USERSTATS_RESPONSE = 819,
	EMSG_APPOWNERSHIPTICKET_RESPONSE = 858,
	EMSG_ENCRYPTED_APPTICKET_RESPONSE = 5527,
	EMSG_WALLET_INFO_UPDATE = 5528,
	EMSG_GAMESPLAYED_WITH_DATABLOB = 5410,
	EMSG_EMAIL_ADDRESS_INFO = 5456,
	EMSG_PICS_PRODUCTINFO_REQUEST = 8903,
	EMSG_PICS_ACCESSTOKEN_RESPONSE = 8906,
};

enum EGameFlags
{
	//1 << 0 is set for spacewar, not other mp games. idk
	EGAMEFLAG_JOINABLE = 1 << 1, //Or in Server, etc
	EGAMEFLAG_MULTIPLAYER = 1 << 13,
};

class CProtoBufMsgBase
{
public:
	char __pad_0x0[0x14];		//0x0
	uint16_t type;				//0x14
	char __pad_0x16[0x6];		//0x16
	CMsgProtoBufHeader* header; //0x1C
	void* __pBody;				//0x20
	char __pad_0x24[0x8];		//0x24
	
	uint32_t send();
	template<typename T> constexpr T* getBody() const
	{
		return reinterpret_cast<T*>(__pBody);
	}
}; //0x2C


//Replaced by actual dumped & protoc generated classes
//TODO: Replace ticket.cpp implementation with proper class too
class CMsgAppOwnershipTicketResponse
{
public:
	char __pad_0x0[0x10];	//0x0
	//Ticket gets accessed like this: pTicket = *(int *)(*(uint *)(local_48.pBody_likely + 0x10) & 0xfffffffe);
	void** ppTicket;			//0x10
	uint32_t appId;			//0x14
	uint32_t result;		//0x18
}; //0x1C ?
