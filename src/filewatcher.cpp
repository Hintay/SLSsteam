#include "filewatcher.hpp"

#include "log.hpp"

#include <sys/inotify.h>
#include <unistd.h>

#include <string>


//TODO: Investigate why gcc complains when put into CFileWatcher itself
void* watchLoop(void* args)
{
	auto watcher = reinterpret_cast<CFileWatcher*>(args);
	g_pLog->debug("Started FileWatcher %u\n", watcher->notifyFd);

	// Buffer must hold at least one event + its name; loop over all events the
	// kernel returns per read (directory watches can coalesce several).
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	for(;;)
	{
		ssize_t len = read(watcher->notifyFd, buf, sizeof(buf));
		if (len <= 0)
		{
			continue;
		}

		for (char* p = buf; p < buf + len; )
		{
			auto* ev = reinterpret_cast<struct inotify_event*>(p);
			const char* base = watcher->fileFdMap[ev->wd];
			std::string path = base ? base : "";
			// For a directory watch, ev->name is the entry within the dir.
			if (ev->len > 0 && !path.empty())
			{
				path += '/';
				path += ev->name;
			}
			g_pLog->debug("inotify %u(%s) -> %u\n", ev->wd, path.c_str(), ev->mask);
			watcher->onModify(path, ev->mask);
			p += sizeof(struct inotify_event) + ev->len;
		}
	}

	return nullptr;
}

CFileWatcher::CFileWatcher(FileModifyEvent_t onModify)
{
	this->onModify = onModify;

	notifyFd = inotify_init();
	g_pLog->debug("Created notify fd %i\n", notifyFd);
}

CFileWatcher::~CFileWatcher()
{
	if (watchThread)
	{
		stop();
	}

	if (notifyFd != -1)
	{
		close(notifyFd);

		for(const auto& fd : fileFdMap)
		{
			if (fd.first == -1)
			{
				continue;
			}

			close(fd.first);
		}
	}
}

bool CFileWatcher::addFile(const char* path)
{
	int fd = inotify_add_watch(notifyFd, path, IN_MODIFY);
	if (fd == -1)
	{
		return false;
	}

	fileFdMap[fd] = path;
	g_pLog->debug("Added %s to FileWatcher %i\n", path, notifyFd);
	return fd != -1;
}

bool CFileWatcher::addDirectory(const char* path)
{
	int fd = inotify_add_watch(notifyFd, path,
	    IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
	if (fd == -1)
	{
		return false;
	}

	fileFdMap[fd] = path;
	g_pLog->debug("Added dir %s to FileWatcher %i\n", path, notifyFd);
	return true;
}

bool CFileWatcher::start()
{
	int code = pthread_create(&watchThread, nullptr, &watchLoop, this);
	return code == 0;
}

void CFileWatcher::stop()
{
	pthread_cancel(watchThread);
}
