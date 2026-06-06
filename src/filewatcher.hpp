#pragma once

#include <pthread.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

// path = full path of the changed entry (dir base + inotify name), mask = inotify event mask.
typedef void(*FileModifyEvent_t)(const std::string& path, uint32_t mask);

class CFileWatcher
{
	friend void* watchLoop(void* args);

	pthread_t watchThread {};
	std::atomic_bool running {false};
	bool started = false;

public:
	int notifyFd = -1;
	std::unordered_map<int, std::string> fileFdMap;

	FileModifyEvent_t onModify;

	CFileWatcher(FileModifyEvent_t onModify);
	~CFileWatcher();

	bool addFile(const char* path);
	bool addDirectory(const char* path);
	bool start();
	void stop();
};
