#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace SLSAPI
{
	extern const char* path;
	extern std::fstream fstream;

	bool isEnabled();
	void onFileChange(const std::string& path, uint32_t mask);
	void runPendingInstallsOnAppManagerFrame();
	void init();
	void shutdown();
}
