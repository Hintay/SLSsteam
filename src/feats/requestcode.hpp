#pragma once

class CProtoBufMsgBase;

namespace RequestCode
{
	// Outgoing hook: when an outbound ServiceMethod call is
	// ContentServerDirectory.GetManifestRequestCode#1, kick off an async
	// HTTP/Lua fetch of the manifest request code keyed by the header's
	// jobid_source. The original request still goes to the server.
	void sendMsg(CProtoBufMsgBase* msg);

	// Incoming hook: when the matching ServiceMethod response comes back,
	// wait (bounded) for the pending fetch and, on success, inject the code
	// into the body and force the header eresult to OK.
	void recvMsg(CProtoBufMsgBase* msg);
}
