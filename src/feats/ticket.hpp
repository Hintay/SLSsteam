#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

class CMsgClientGetAppOwnershipTicketResponse;
class CMsgClientRequestEncryptedAppTicketResponse;
class CProtoBufMsgBase;

namespace Ticket
{
	class SavedTicket
	{
public:
		uint32_t steamId;
		std::string ticket;
	};

	// Atomic: written by getTicketOwnershipExtendedData (one hook thread) and
	// read-and-cleared by hkClientUser_GetSteamId (another) — exchange() keeps
	// the one-time semantics without a torn/lost-update data race.
	extern std::atomic<uint32_t> oneTimeSteamIdSpoof;
	extern std::map<uint32_t, SavedTicket> ticketMap;
	extern std::map<uint32_t, SavedTicket> encryptedTicketMap;

	std::string getTicketDir();

	//TODO: Fill with error checks
	std::string getTicketPath(uint32_t appId);
	SavedTicket getCachedTicket(uint32_t appId);
	bool saveTicketToCache(CMsgClientGetAppOwnershipTicketResponse* resp);

	void launchApp(uint32_t appId);
	void getTicketOwnershipExtendedData(uint32_t appId);

	std::string getEncryptedTicketPath(uint32_t appId);
	SavedTicket getCachedEncryptedTicket(uint32_t appId);
	bool saveEncryptedTicketToCache(CMsgClientRequestEncryptedAppTicketResponse* resp);

	void recvEncryptedAppTicket(CMsgClientRequestEncryptedAppTicketResponse* msg);
	void recvAppTicket(CMsgClientGetAppOwnershipTicketResponse* msg);
	void recvMsg(CProtoBufMsgBase* msg);
}
