#include "RawNetPacket.hpp"

#include <array>
#include <cstring>
#include <limits>

namespace netpacket
{
	namespace
	{
		constexpr size_t kReplacementRingSize = 8;
		thread_local std::array<std::vector<uint8_t>, kReplacementRingSize> t_recvPackets;
		thread_local std::array<std::vector<uint8_t>, kReplacementRingSize> t_sendPackets;
		thread_local size_t t_recvPacketIndex = 0;
		thread_local size_t t_sendPacketIndex = 0;

		std::vector<uint8_t>& NextRecvPacketBuffer()
		{
			auto& buffer = t_recvPackets[t_recvPacketIndex];
			t_recvPacketIndex = (t_recvPacketIndex + 1) % t_recvPackets.size();
			return buffer;
		}

		std::vector<uint8_t>& NextSendPacketBuffer()
		{
			auto& buffer = t_sendPackets[t_sendPacketIndex];
			t_sendPacketIndex = (t_sendPacketIndex + 1) % t_sendPackets.size();
			return buffer;
		}

		bool ReadMsgHdr(const uint8_t* data, uint32_t size, MsgHdr& out)
		{
			if (!data || size < sizeof(MsgHdr)) return false;
			memcpy(&out, data, sizeof(out));
			return true;
		}

		bool AssembleRawImpl(std::vector<uint8_t>& buffer, uint32_t rawEMsg,
		                     const void* newHdr, size_t cbNewHdr,
		                     const void* newBody, size_t cbNewBody,
		                     const uint8_t*& outData, uint32_t& outSize)
		{
			outData = nullptr;
			outSize = 0;

			if ((cbNewHdr && !newHdr) || (cbNewBody && !newBody)) return false;
			if (cbNewHdr > std::numeric_limits<uint32_t>::max()) return false;

			const size_t prefixSize = sizeof(MsgHdr) + cbNewHdr;
			if (prefixSize < cbNewHdr) return false;
			const size_t totalSize = prefixSize + cbNewBody;
			if (totalSize < prefixSize || totalSize > std::numeric_limits<uint32_t>::max()) return false;

			try
			{
				buffer.resize(totalSize);
			}
			catch (...)
			{
				return false;
			}
			const MsgHdr out{rawEMsg, static_cast<uint32_t>(cbNewHdr)};
			memcpy(buffer.data(), &out, sizeof(out));

			if (cbNewHdr)
				memcpy(buffer.data() + sizeof(MsgHdr), newHdr, cbNewHdr);
			if (cbNewBody)
				memcpy(buffer.data() + sizeof(MsgHdr) + cbNewHdr, newBody, cbNewBody);

			outData = buffer.data();
			outSize = static_cast<uint32_t>(totalSize);
			return true;
		}
	}

	bool UnpackRaw(const uint8_t* data, uint32_t size, RawPacketView& out)
	{
		out = RawPacketView{};
		MsgHdr msgHdr{};
		if (!ReadMsgHdr(data, size, msgHdr)) return false;

		if (!(msgHdr.eMsg & kMsgHdrProtoFlag)) return false;

		const uint32_t cbHdr = msgHdr.headerLength;
		if (cbHdr > size - sizeof(MsgHdr)) return false;

		const uint32_t bodyOffset = sizeof(MsgHdr) + cbHdr;
		out.eMsg = msgHdr.eMsg & ~kMsgHdrProtoFlag;
		out.msgHdr = msgHdr;
		out.header = data + sizeof(MsgHdr);
		out.headerSize = cbHdr;
		out.body = data + bodyOffset;
		out.bodySize = size - bodyOffset;
		return true;
	}

	bool UnpackRaw(const uint8_t* data, uint32_t size,
	               uint32_t& eMsg, const uint8_t*& pHdr, uint32_t& cbHdr,
	               const uint8_t*& pBody, uint32_t& cbBody)
	{
		RawPacketView view;
		if (!UnpackRaw(data, size, view))
		{
			eMsg = 0;
			pHdr = nullptr;
			cbHdr = 0;
			pBody = nullptr;
			cbBody = 0;
			return false;
		}

		eMsg = view.eMsg;
		pHdr = view.header;
		cbHdr = view.headerSize;
		pBody = view.body;
		cbBody = view.bodySize;
		return true;
	}

	bool UnpackRaw(const uint8_t* data, uint32_t size,
	               uint16_t& eMsg, const uint8_t*& pHdr, uint32_t& cbHdr,
	               const uint8_t*& pBody, uint32_t& cbBody)
	{
		uint32_t wideEMsg = 0;
		if (!UnpackRaw(data, size, wideEMsg, pHdr, cbHdr, pBody, cbBody))
		{
			eMsg = 0;
			return false;
		}

		eMsg = static_cast<uint16_t>(wideEMsg);
		return true;
	}

	bool AssembleRaw(std::vector<uint8_t>& buffer, uint32_t rawEMsg,
	                 const void* newHdr, size_t cbNewHdr,
	                 const void* newBody, size_t cbNewBody,
	                 const uint8_t*& outData, uint32_t& outSize)
	{
		return AssembleRawImpl(buffer, rawEMsg, newHdr, cbNewHdr, newBody, cbNewBody,
		                       outData, outSize);
	}

	bool AssembleRaw(std::vector<uint8_t>& buffer, const MsgHdr& original,
	                 const void* newHdr, size_t cbNewHdr,
	                 const void* newBody, size_t cbNewBody,
	                 const uint8_t*& outData, uint32_t& outSize)
	{
		return AssembleRawImpl(buffer, original.eMsg, newHdr, cbNewHdr, newBody, cbNewBody,
		                       outData, outSize);
	}

	bool ReplaceRecvPacket(CNetPacket* packet,
	                       const void* newHdr, size_t cbNewHdr,
	                       const void* newBody, size_t cbNewBody)
	{
		const uint8_t* outData = nullptr;
		uint32_t outSize = 0;
		if (!BuildRecvPacketReplacement(packet, newHdr, cbNewHdr, newBody, cbNewBody,
		                                outData, outSize))
		{
			return false;
		}

		packet->m_pubData = const_cast<uint8_t*>(outData);
		packet->m_cubData = outSize;
		return true;
	}

	bool BuildRecvPacketReplacement(const CNetPacket* packet,
	                                const void* newHdr, size_t cbNewHdr,
	                                const void* newBody, size_t cbNewBody,
	                                const uint8_t*& outData, uint32_t& outSize)
	{
		outData = nullptr;
		outSize = 0;
		if (!packet) return false;

		RawPacketView view;
		if (!UnpackRaw(packet->m_pubData, packet->m_cubData, view)) return false;
		return AssembleRaw(NextRecvPacketBuffer(), view.msgHdr, newHdr, cbNewHdr, newBody, cbNewBody,
		                   outData, outSize);
	}

	bool ReplaceSendPacket(const uint8_t* pubData, uint32_t cubData,
	                       const void* newHdr, size_t cbNewHdr,
	                       const void* newBody, size_t cbNewBody,
	                       const uint8_t*& outData, uint32_t& outSize)
	{
		outData = nullptr;
		outSize = 0;

		RawPacketView view;
		if (!UnpackRaw(pubData, cubData, view)) return false;
		return AssembleRaw(NextSendPacketBuffer(), view.msgHdr, newHdr, cbNewHdr, newBody, cbNewBody,
		                   outData, outSize);
	}
}
