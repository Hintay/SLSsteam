#pragma once
#include <cstdint>

struct CNetPacket;

namespace RequestCode
{
	// hkBBuildAndAsyncSendFrame (outgoing WebSocket binary frame). If the frame is
	// the ContentServerDirectory.GetManifestRequestCode#1 ServiceMethod call
	// (EMsg 151), capture it, launch an async fetch of the manifest request code
	// keyed by the header's jobid_source, and return TRUE to tell the caller to DROP
	// the frame (it is never sent to the CM — the server never sees the request).
	// A fabricated ServiceMethodResponse is injected later (see nextInjection).
	// Returns false for every other frame (caller sends it normally).
	bool onSendFrame(const uint8_t* pubData, uint32_t cubData);

	// hkRecvPkt, called once per incoming packet. If a blocked request's code fetch
	// has completed, build the fabricated ServiceMethodResponse (derived from the
	// request's own header) and hand its bytes back so the caller can deliver it by
	// borrowing the current packet as a carrier. Returns true (and fills outData/
	// outSize) for at most one fabricated response per call; cheap fast-path when no
	// request is outstanding. The returned buffer is valid until the next call.
	bool nextInjection(const uint8_t*& outData, uint32_t& outSize);
}
