#pragma once

#include <pthread.h>
#include <cstdint>
#include <string>
#include <unordered_map>

// path = full path of the changed entry (dir base + inotify name), mask = inotify event mask.
typedef void(*FileModifyEvent_t)(const std::string& path, uint32_t mask);

class CFileWatcher
{
	pthread_t watchThread;

public:
	int notifyFd;
	std::unordered_map<int, const char*> fileFdMap;

	FileModifyEvent_t onModify;

	CFileWatcher(FileModifyEvent_t onModify);
	~CFileWatcher();

	bool addFile(const char* path);
	bool addDirectory(const char* path);
	bool start();
	void stop();
};
