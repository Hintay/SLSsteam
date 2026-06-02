#pragma once

#include <cstdint>


namespace DepotKey
{
	// Tries to satisfy a depot decryption key request from the Lua layer.
	// On success, copies the 32-byte key into the loader's output buffer and
	// returns true; the caller then short-circuits the original loader.
	// outBuf is the original arg3: a pointer-to-pointer whose target buffer
	// receives the key at offset 0.
	bool provideKey(uint32_t depotId, void* outBuf);
}
