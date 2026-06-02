#pragma once

#include <cstdint>


namespace DepotKey
{
	// Tries to satisfy a depot decryption key request from the Lua layer.
	// LoadDepotDecryptionKey passes Key as a direct output buffer of outSize
	// bytes and expects the number of bytes written as the return value. On a
	// Lua hit this copies the 32-byte key into outBuf and returns 32; otherwise
	// it returns 0 so the caller falls through to the real loader.
	int provideKey(uint32_t depotId, void* outBuf, uint32_t outSize);
}
