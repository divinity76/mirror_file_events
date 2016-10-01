// license: unlicense.org
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <dirent.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>

#if !defined(likely) 
#if defined(__GNUC__) || defined(__INTEL_COMPILER) || defined(__clang__)
#define likely(x)       __builtin_expect((x),1)
#else
#define likely(x) (x)
#endif
#endif
#if !defined(unlikely)
#if defined(__GNUC__) || defined(__INTEL_COMPILER) || defined(__clang__)
#define unlikely(x)     __builtin_expect((x),0)
#else
#define unlikely(x) (x)
#endif
#endif
typ1edef struct {
	int watchfd;
	char* path;
} monitor;
static int inotify_fd = 0;
static monitor** monitors = NULL;
static size_t monitors_size = 0;
void* emalloc(size_t size) {
	void* ret = malloc(size);
	if (unlikely(size && !ret)) {
		fprintf(stderr, "malloc failed to allocate %zu bytes. terminating...",
				size);
#if defined(__GLIBC__)
		//add glibc backtrace() and stuff here?
#endif
		exit(EXIT_FAILURE);
	}
	return ret;
}
void* erealloc(void* ptr, size_t size) {
	void* ret = realloc(ptr, size);
	if (unlikely(size && !ret)) {
		fprintf(stderr, "realloc failed to allocate %zu bytes. terminating...",
				size);
#if defined(__GLIBC__)
		//add glibc backtrace() and stuff here?
#endif
		exit(EXIT_FAILURE);
	}
	return ret;
}
void* ecalloc(size_t num, size_t size) {
	//im not implementing arbitrary precision integer logic for the error message, feel free to fix it
	void* ret = calloc(num, size);
	if (unlikely(num > 0 && size > 0 && !ret)) {
		fprintf(stderr, "calloc failed to allocate %zu bytes. terminating...",
				num * size);
#if defined(__GLIBC__)
		//add glibc backtrace() and stuff here?
#endif
		exit(EXIT_FAILURE);

	}
	return ret;
}
char* safe_realpath(const char* dir, void* unused __attribute__((unused))) {
//seems that the buffer used by realpath(?,NULL) can be reused by something... make sure that doesn't happen
	char* ret = emalloc(PATH_MAX + 1);
	realpath(dir, ret);
	ret = erealloc(ret, strlen(ret) + 1);
	return ret;
}
void** remove_from_array(void** arr, size_t index, size_t* array_size) {
	const size_t size = *array_size;
	assert(index < size);
	free(arr[index]);
	for (size_t i = index + 1; i < size; ++i) {
		arr[i - 1] = arr[i];
	}
	--(*array_size);
	arr = erealloc(arr, sizeof(arr[0]) * (size - 1));
	return arr;
}
void** add_to_array(void** arr, void* ptr, size_t* array_size) {
	const size_t size = (*array_size) + 1;
	arr = erealloc(arr, sizeof(arr[0]) * size);
	arr[size - 1] = ptr;
	(*array_size) = size;
	return arr;
}
bool is_dir(const char *path) {
	struct stat s;
	if (lstat(path, &s) == -1) {
		return false;
	}
	return S_ISDIR(s.st_mode);
}
int get_index_of_monitored_dir(const char* dir) {
		for (size_t i = 0; i < monitors_size; ++i) {
			if (strcmp(monitors[i]->path, dir) == 0) {
				return i;
			}
		}
	return -1;
}
size_t* get_indexes_of_monitored_dirs(const char* dir, size_t* matches) {
	size_t* ret = ecalloc(0, sizeof(size_t));
	size_t rets = 0;
	int len = strlen(dir);
	assert(len > 1); //on most systems, >1...
	for (size_t i = 0; i < monitors_size; ++i) {
		if (strncmp(dir, monitors[i]->path, len) == 0) {
			++rets;
			ret = erealloc(ret, sizeof(size_t) * rets);
			ret[i - 1] = i;
		}
	}
	*matches = rets;
	return ret;
}
bool is_monitored(const char* dir) {
	if (get_index_of_monitored_dir(dir) == -1) {
		return false;
	}
	return true;
}

bool monitor_dir(const char* fullpath) {
	if (is_monitored(fullpath)) {
		return true;
	}
	monitor* mon = emalloc(sizeof(monitor));
	mon->path = emalloc(strlen(fullpath) + 1);
	strcpy(mon->path, fullpath);
	//TODO: IN_ATTRIB ?
	mon->watchfd = inotify_add_watch(inotify_fd, mon->path,
			0 | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY
					| IN_MOVE_SELF | IN_MOVED_TO | IN_EXCL_UNLINK
					| IN_MOVED_FROM /* | ~0*/);
	if (mon->watchfd == -1) {
		fprintf(stderr, "Warning, failed to monitor \"%s\"...\n", mon->path);
		free(mon->path);
		free(mon);
		return false;
	}
	monitors = (typeof(monitors)) add_to_array((void**) monitors, mon,
			&monitors_size);
	return true;
}
bool monitor_dir_r(const char* path) {
	if (!is_dir(path)) {
		return false;
	}
	char* fullpath = safe_realpath(path, NULL);
	if (is_monitored(fullpath)) {
		free(fullpath);
		return true;
	}
	if (!monitor_dir(fullpath)) {
		free(fullpath);
		return false;
	}
	DIR* dirh = opendir(fullpath);
	if (!dirh) {
		fprintf(stderr,
				"warning, failed to open dir \"%s\". ignoring dir. error: %s\n",
				fullpath, strerror(errno));
		free(fullpath);
		return false;
	}
	struct dirent de = { 0 };
	struct dirent* notreadall = (void*) 1;
	size_t read = 0;
	int err;
	while ((err = readdir_r(dirh, &de, &notreadall)) == 0 && notreadall != NULL) {
		++read;
		if (strcmp(de.d_name, ".") == 0 || strcmp(de.d_name, "..") == 0) {
			//printf("continuing on %s\n", de.d_name);
			continue;
		}
		int fullpathlen = snprintf(NULL, 0, "%s/%s", fullpath, de.d_name) + 1;
		char* fullpath2 = emalloc(fullpathlen);
		if (fullpathlen - 1
				!= snprintf(fullpath2, fullpathlen, "%s/%s", fullpath,
						de.d_name)) {
			fprintf(stderr,
					"some kind of super weird error... screw this. get your CPU chip checked or something.\n");
			exit(EXIT_FAILURE);
		}
		if (is_dir(fullpath2)) {
			if (!monitor_dir_r(fullpath2)) {
				fprintf(stderr, "warning, failed to monitor \"%s\"...\n",
						fullpath2);
			}
		}
		free(fullpath2);
	}

	printf("monitoring \"%s\"\n", fullpath);
	closedir(dirh);
	free(fullpath);
	return true;
}
bool unmonitor_dir(char* fullpath, bool recursive) {
	bool ret = false;
	if (unlikely(!recursive)) {
		int index = get_index_of_monitored_dir(fullpath);
		if (index == -1) {
			return false;
		}
		if (inotify_rm_watch(inotify_fd, monitors[index]->watchfd) != 0) {
			fprintf(stderr,
					"Warning, could not remove monitor of \"%s\", this will likely result in memory leaks..",
					monitors[index]->path);
		}
		free(monitors[index]->path);
		monitors = (typeof(monitors)) remove_from_array((void**) monitors,
				index,
				&monitors_size);
		return true;
	} else {
		size_t matches;
		size_t* indexes = get_indexes_of_monitored_dirs(fullpath, &matches);
		if (matches == 0) {
			free(indexes);
			return false;
		}
		ret = true;
		for (size_t i = 0; i < matches; ++i) {
			size_t index = indexes[i];
			if (inotify_rm_watch(inotify_fd, monitors[index]->watchfd) != 0) {
				fprintf(stderr,
						"Warning, could not remove monitor of \"%s\", this will likely result in memory leaks..",
						monitors[index]->path);
			}
			free(monitors[index]->path);
			monitors = (typeof(monitors)) remove_from_array((void**) monitors,
					index,
					&monitors_size);
		}
		free(indexes);
	}
	return ret;
}

void shutdown_cleanup(void) {
	printf("shutting down\n");
	for (size_t i = 0; i < monitors_size; ++i) {
		if (inotify_rm_watch(inotify_fd, monitors[i]->watchfd) != 0) {
			fprintf(stderr, "Warning, failed to remove watcher for \"%s\"\n",
					monitors[i]->path);
		}
		free(monitors[i]->path);
		free(monitors[i]);
	}
	free(monitors);
	close(inotify_fd);
}

int main(int argc, char* argv[]) {
	atexit(shutdown_cleanup);
	//signal(SIGTERM, shutdown_cleanup);
	//signal(SIGINT,shutdown_cleanup);
	inotify_fd = inotify_init1(0);
	if (inotify_fd == -1) {
		fprintf(stderr, "Error, inotify_init1 failed. exiting.. error: %s\n",
				strerror(errno));
		exit(EXIT_FAILURE);
	}
	monitors = ecalloc(0, sizeof(monitors[0]));
	if (argc < 3) {
		fprintf(stderr, "usage: %s \"dir\" \"dir\" \"dir\"... \"targetdir\"\n",
				argv[0]);
		exit(EXIT_FAILURE);
	}
	for (int i = 1; i < argc - 1; ++i
			) {
		if (!monitor_dir_r(argv[i])) {
			fprintf(stderr, "Error, could not monitor dir \"%s\", exiting..\n",
					argv[i]);
			exit(EXIT_FAILURE);
		}
	}
	while (monitors_size > 0) {
		break;
	}
	return EXIT_SUCCESS;
}
