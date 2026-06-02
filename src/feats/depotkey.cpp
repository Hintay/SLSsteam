#include "depotkey.hpp"

#include "../log.hpp"
#include "../lua/LuaLoader.hpp"

#include <cstring>
#include <vector>


// Size of a depot decryption key (AES-256). The local key loader always
// writes exactly this many bytes into the output buffer.
static constexpr size_t kDepotKeySize = 32;

bool DepotKey::provideKey(uint32_t depotId, void* outBuf)
{
	const std::vector<uint8_t>* key = LuaLoader::getKey(depotId);
	if (!key)
	{
		return false;
	}

	// Defensive: only inject a correctly-sized key. parseHex64 already enforces
	// exactly 32 bytes, but guard here so this memory-writing path does not rely
	// on an invariant enforced in another layer; fall through to the original
	// loader on any mismatch.
	if (key->size() != kDepotKeySize)
	{
		return false;
	}

	// arg3 is a pointer-to-pointer: dereference once to reach the buffer the
	// loader expects the key to land in, then write the key at offset 0.
	void* dst = *reinterpret_cast<void**>(outBuf);
	memcpy(dst, key->data(), kDepotKeySize);

	g_pLog->once("Provided depot decryption key for %u\n", depotId);
	return true;
}
