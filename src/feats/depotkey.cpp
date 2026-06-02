#include "depotkey.hpp"

#include "../log.hpp"
#include "../lua/LuaLoader.hpp"

#include <cstring>
#include <vector>


// Size of a depot decryption key (AES-256). The local key loader always
// writes exactly this many bytes into the output buffer.
static constexpr size_t kDepotKeySize = 32;

int DepotKey::provideKey(uint32_t depotId, void* outBuf, uint32_t outSize)
{
	const std::vector<uint8_t>* key = LuaLoader::getKey(depotId);
	if (!key)
	{
		return 0;
	}

	// Defensive: only inject a correctly-sized key into a big-enough buffer.
	// parseHex64 already enforces exactly 32 bytes, but guard here so this
	// memory-writing path does not rely on an invariant enforced in another
	// layer; fall through to the original loader on any mismatch.
	if (key->size() != kDepotKeySize || !outBuf || outSize < kDepotKeySize)
	{
		return 0;
	}

	// Key is a direct output buffer (no pointer-to-pointer indirection): write
	// the 32-byte key straight in and report how many bytes we wrote, matching
	// the original loader's contract.
	memcpy(outBuf, key->data(), kDepotKeySize);

	g_pLog->once("Provided depot decryption key for %u\n", depotId);
	return static_cast<int>(kDepotKeySize);
}
