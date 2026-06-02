#pragma once

#include <cstdint>
#include <fstream>
#include <string>


class CFileWatcher;

namespace SLSAPI
{
	extern const char* path;
	extern std::fstream fstream;
	extern CFileWatcher* watcher;

	bool isEnabled();
	void onFileChange(const std::string& path, uint32_t mask);
	void init();
}
