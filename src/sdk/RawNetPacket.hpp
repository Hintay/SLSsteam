#pragma once

#include "CNetPacket.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace netpacket
{
	struct RawPacketView
	{
		uint32_t eMsg = 0;
		MsgHdr msgHdr{};
		const uint8_t* header = nullptr;
		uint32_t headerSize = 0;
		const uint8_t* body = nullptr;
		uint32_t bodySize = 0;
	};

	// Parse [MsgHdr][protobuf header][protobuf body]. Returns false unless the
	// packet is protobuf-flagged and the declared header fits inside the payload.
	bool UnpackRaw(const uint8_t* data, uint32_t size, RawPacketView& out);
	bool UnpackRaw(const uint8_t* data, uint32_t size,
	               uint32_t& eMsg, const uint8_t*& pHdr, uint32_t& cbHdr,
	               const uint8_t*& pBody, uint32_t& cbBody);
	bool UnpackRaw(const uint8_t* data, uint32_t size,
	               uint16_t& eMsg, const uint8_t*& pHdr, uint32_t& cbHdr,
	               const uint8_t*& pBody, uint32_t& cbBody);

	// Compatibility shim for older call sites; new code should prefer UnpackRaw.
	inline bool unpackRaw(const uint8_t* data, uint32_t size,
	                      uint16_t& eMsg, const uint8_t*& pHdr, uint32_t& cbHdr,
	                      const uint8_t*& pBody, uint32_t& cbBody)
	{
		return UnpackRaw(data, size, eMsg, pHdr, cbHdr, pBody, cbBody);
	}

	// Assemble a raw protobuf packet into a caller-owned buffer. rawEMsg must be the
	// full MsgHdr eMsg value (including kMsgHdrProtoFlag when needed).
	bool AssembleRaw(std::vector<uint8_t>& buffer, uint32_t rawEMsg,
	                 const void* newHdr, size_t cbNewHdr,
	                 const void* newBody, size_t cbNewBody,
	                 const uint8_t*& outData, uint32_t& outSize);
	bool AssembleRaw(std::vector<uint8_t>& buffer, const MsgHdr& original,
	                 const void* newHdr, size_t cbNewHdr,
	                 const void* newBody, size_t cbNewBody,
	                 const uint8_t*& outData, uint32_t& outSize);

	// Replace helpers keep the rewritten bytes alive in a small thread-local ring
	// until the immediate trampoline call consumes them.
	bool ReplaceRecvPacket(CNetPacket* packet,
	                       const void* newHdr, size_t cbNewHdr,
	                       const void* newBody, size_t cbNewBody);
	// Returns bytes backed by the recv replacement ring; valid until that slot is
	// reused by a later recv replacement on the same thread.
	bool BuildRecvPacketReplacement(const CNetPacket* packet,
	                                const void* newHdr, size_t cbNewHdr,
	                                const void* newBody, size_t cbNewBody,
	                                const uint8_t*& outData, uint32_t& outSize);
	bool ReplaceSendPacket(const uint8_t* pubData, uint32_t cubData,
	                       const void* newHdr, size_t cbNewHdr,
	                       const void* newBody, size_t cbNewBody,
	                       const uint8_t*& outData, uint32_t& outSize);
}
