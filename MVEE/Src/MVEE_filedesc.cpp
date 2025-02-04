/*
 * GHent University Multi-Variant Execution Environment (GHUMVEE)
 *
 * This source file is distributed under the terms and conditions 
 * found in GHUMVEELICENSE.txt.
 */

/*-----------------------------------------------------------------------------
    Includes
-----------------------------------------------------------------------------*/
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <set>
#include <string.h>
#include "MVEE.h"
#include "MVEE_filedesc.h"
#include "MVEE_macros.h"
#include "MVEE_private_arch.h"
#include "MVEE_logging.h"
#include "MVEE_memory.h"

/*-----------------------------------------------------------------------------
    fd_info class
-----------------------------------------------------------------------------*/
fd_info::fd_info()
    : access_flags(0)
    , master_file(false)
	, close_on_exec(false)
	, unsynced_reads(false)
	, unlinked(false)
	, original_file_size(0)
{
    fds.resize(mvee::numvariants);
}

fd_info::fd_info
(
	FileType                  type,
    std::vector<unsigned long>& fds,
    std::string               path,
    unsigned long             access_flags,
    bool                      close_on_exec,
    bool                      master_file,
    bool                      unsynced_reads,
	bool                      unlinked,
    ssize_t                   original_file_size
)
    : fds(fds)
	, path(path)
	, access_flags(access_flags)
	, master_file(master_file)
	, close_on_exec(close_on_exec)
	, unsynced_reads(unsynced_reads)
	, unlinked(unlinked)
	, original_file_size(original_file_size)
	, file_type(type)	
{
}

/*-----------------------------------------------------------------------------
    print_fd_info
-----------------------------------------------------------------------------*/
void fd_info::print_fd_info ()
{
    SERIALIZEVECTOR(fds, str);
    debugf("> fds          = %s\n",         str.c_str());
    debugf("> path         = %s\n",         path.c_str());
    debugf("> flags        = 0x%04X, %s\n", access_flags, getTextualFileFlags(access_flags).c_str());
    debugf("> cloexec      = %d\n",         close_on_exec);
    debugf("> master file  = %d\n",         master_file);
    debugf("> unsynced     = %d\n",         unsynced_reads);
	debugf("> unlinked     = %d\n",         unlinked);
	debugf("> file type    = %s\n",         getTextualFileType(file_type));
}

/*-----------------------------------------------------------------------------
    fd_table class
-----------------------------------------------------------------------------*/
void fd_table::init()
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock, &attr);
	file_map = NULL;
	temporary_files.resize(mvee::numvariants);
}

fd_table::fd_table()
{
    init();

    // clean table. Just add default fds
    std::vector<unsigned long> fds(mvee::numvariants);
    std::fill(fds.begin(), fds.end(), 0);
    create_fd_info(FT_SPECIAL, fds, "stdin", O_RDONLY, false, false, false, true, 0);
    std::fill(fds.begin(), fds.end(), 1);
    create_fd_info(FT_SPECIAL, fds, "stdout", O_WRONLY, false, false, false, true, 0);
    std::fill(fds.begin(), fds.end(), 2);
    create_fd_info(FT_SPECIAL, fds, "stderr", O_WRONLY, false, false, false, true, 0);

    char*                      cwd = getcwd(NULL, 0);
    fd_cwd = std::string(cwd);
    free(cwd);
}

fd_table::fd_table(const fd_table& parent)
{
    init();
    table     = parent.table;
    fd_cwd    = parent.fd_cwd;
    epoll_map = parent.epoll_map;
}

fd_table::~fd_table()
{
	if (file_map)
		delete file_map;
}

/*-----------------------------------------------------------------------------
    add_missing_fds - Firefox 51 does something weird that I can't quite figure
    out right now. At some point, there is a thread B that opens a shared memory
    backing file, unlinks it from the file system, maps it into the address
    space as a shared mapping, and closes the fd.

	At a later point, there is a thread A that maps that same file using a file
	descriptor whose creation we never see... Since GHUMVEE doesn't know the fd,
	it denies thread A's attempt to create the shared mapping. A and B are in
	different processes, which makes this even more mysterious.

	Until we find out where this missing fd is created, we use this function
	which reads missing fd info from /proc/pid/fd
-----------------------------------------------------------------------------*/
bool fd_table::add_missing_fds(std::vector<pid_t> variant_pids)
{
	std::map<std::string, std::vector<unsigned long>> missing_fds;

	int i = 0;
	for (auto pid : variant_pids)
	{
		debugf("adding missing fds for variant %d (pid %d)\n", i, pid);
		char cmd   [500];
		char perms [15];
		char file  [1024];
		int fd;
		int prot;

		sprintf(cmd, "ls -al /proc/%d/fd | grep \"\\->\" | sed 's/\\([lrwx-]*\\).*:...\\([0-9]*\\) -> \\(.*\\)/\\1 \\2 \\3/'", pid);
		std::string line, fd_list = mvee::log_read_from_proc_pipe(cmd, NULL);
		std::stringstream ss(fd_list);
		
		while(std::getline(ss, line))
		{
			if (sscanf(line.c_str(), "%s %d %s", perms, &fd, file) != 3)
			{
				warnf("Malformed line in refresh_fd_table: %s\n", line.c_str());
				continue;
			}

			if (perms[1] == 'r')
			{
				if (perms[2] == 'w')
					prot = O_RDWR;
				else
					prot = O_RDONLY;
			}
			else if (perms[2] == 'w')
			{
				prot = O_WRONLY;
			}
			else
			{
				prot = 0;
			}

			fd_info* info = get_fd_info(fd, i);
			if (!info)
			{
				debugf("variant %d (pid %d) has missing file: %d -> %s (perms: %s)\n",
				  i, pid, fd, file, perms);

				auto missing = missing_fds.find(std::string(file));
				if (missing == missing_fds.end())
				{
					std::vector<unsigned long> fds(mvee::numvariants);
					std::fill(fds.begin(), fds.end(), (unsigned long)-1);
					fds[i] = (unsigned long)fd | ((unsigned long)prot << 32);
					missing_fds.insert(std::make_pair(std::string(file), fds));
				}
				else
				{					
					if (missing->second[i] != (unsigned long)-1)
					{
						warnf("Found missing file using multiple fds. We can't handle this case :(\n");
						return false;
					}
					else
					{
						missing->second[i] = (unsigned long)fd | ((unsigned long)prot << 32);
					}
				}
			}
		}

		i++;
	}

	for (auto missing : missing_fds)
	{
		bool master_has_file = false;
		int num_fds = 0;
//		int prot = 0;
		i = 0;
		
		for (auto fd : missing.second)
		{
			if (fd != (unsigned long)-1)
			{
			    if (i == 0)
					master_has_file = true;
				num_fds++;
			}

			if (!master_has_file)
			{
				warnf("Found missing file that is not mapped by the master: %s - this shouldn't happen!\n", 
					  missing.first.c_str());
				return false;
			}

			if (num_fds != 1 && num_fds != mvee::numvariants)
			{
				warnf("Found missing file that is mapped by some, but not all slaves: %s - this shouldn't happen!\n",
					  missing.first.c_str());
				return false;
			}

//			prot = fd >> 32;
			missing.second[i++] = fd & 0xFFFFFFFF;
		}

		create_fd_info(FT_UNKNOWN, // TODO: use sys_stat to get extra info?
					   missing.second,
					   missing.first,
					   false,
					   num_fds == 1,
					   false,
					   false, // TODO: Check using stat?
					   0 // TODO: use sys_stat to get extra info?
			);
	}

	return true;
}

/*-----------------------------------------------------------------------------
    refresh_fd_table - Try to wipe and repopulate the fd table. We do this if
	we see an execve in an IP-MON process because GHUMVEE might lose track
	of which fds are opened in which variants...
-----------------------------------------------------------------------------*/
void fd_table::refresh_fd_table(std::vector<pid_t> variant_pids)
{
	table.clear();
	epoll_map.clear();
	temporary_files.clear();
	fd_cwd = "";

    // I'm not sure if it's really a good idea to repopulate the table
    // as we generally can't figure out the mapping between master and slave
    // fds if we haven't seen the original sys_open(at)
#if 0
	add_missing_fds(variant_pids);
#endif
}

/*-----------------------------------------------------------------------------
    File Map Management
-----------------------------------------------------------------------------*/
bool fd_table::file_map_exists()
{
	return file_map ? true : false;
}

_shm_info* fd_table::file_map_get()
{
	return file_map;
}

int fd_table::file_map_id()
{
	if (!file_map)
	{
		file_map = new _shm_info();
		if (!mvee::os_alloc_sysv_sharedmem(4096, 
										   &file_map->id,
										   &file_map->sz,
										   &file_map->ptr))
		{
			warnf("ERROR: Couldn't create file map\n");
			return -1;
		}		

		// populate the map
		for (auto it = table.begin(); it != table.end(); ++it)
			file_map_set(it->first, it->second.file_type);
	}

	return file_map->id;
}

void fd_table::file_map_set(int fd, FileType type)
{
	if (file_map)
	{
		char* map = (char*)file_map->ptr;
		map[fd] = type;
	}
}

void fd_table::set_blocking(int fd)
{
	fd_info* info = get_fd_info(fd);
	if (info)
	{
		info->file_type = (FileType)(info->file_type | MVEE_BLOCKING_FD);
		file_map_set(fd, info->file_type);
	}
}

void fd_table::set_non_blocking(int fd)
{
	fd_info* info = get_fd_info(fd);
	if (info)
	{
		info->file_type = (FileType)(info->file_type & (~MVEE_BLOCKING_FD));
		file_map_set(fd, info->file_type);
	}
}

/*-----------------------------------------------------------------------------
    create_fd_info -
-----------------------------------------------------------------------------*/
void fd_table::create_fd_info
(
	FileType                  type,
    std::vector<unsigned long>& fds,
    std::string               path,
    unsigned long             access_flags,
    bool                      close_on_exec,
    bool                      master_file,
    bool                      unsynced_reads,
	bool                      unlinked,
    ssize_t                   original_file_size
)
{
    fd_info info(type, fds, path, access_flags, close_on_exec, master_file, unsynced_reads, unlinked, original_file_size);

    auto it = table.find(fds[0]);
    if (it != table.end())
    {
		if (!(*mvee::config_variant_global)["use_ipmon"].asBool())
			warnf("fd override!!! FIXME (unless IP-MON is managing fds, in which case you can safely ignore this warning)\n");
        it->second.print_fd_info();
        free_fd_info(it->second.fds[0]);
    }

#ifndef MVEE_BENCHMARK
    debugf("created new fd\n");
    info.print_fd_info();
#endif

    table.insert(std::make_pair(fds[0], info));
	file_map_set(fds[0], type);
}

/*-----------------------------------------------------------------------------
    free_fd_info - We cannot simply erase the file descriptors from the fd table
    since they might also be in the epoll map
-----------------------------------------------------------------------------*/
std::map<unsigned long, fd_info>::iterator fd_table::free_fd_info (unsigned long fd)
{
    auto it = table.find(fd);
    if (it != table.end())
    {
        debugf("removed fd: %d (%s)\n", fd, it->second.path.c_str());
        it = table.erase(it);
    }

    // check if it's an epoll fd
    auto epoll_it = epoll_map.find(fd);
    if (epoll_it != epoll_map.end())
    {
        debugf("removed fd from epoll map: %d\n", fd);
        epoll_map.erase(epoll_it);
    }

    // check if the fd is registered with any epoll instance
    for (epoll_it = epoll_map.begin(); epoll_it != epoll_map.end(); ++epoll_it)
    {
        if (epoll_it->second.find(fd) != epoll_it->second.end())
        {
            debugf("fd: %d was registered with epoll fd: %d\n", fd, epoll_it->first);
            epoll_it->second.erase(fd);
        }
    }

	file_map_set(fd, FT_UNKNOWN);
	return it;
}

/*-----------------------------------------------------------------------------
    free_cloexec_fds
-----------------------------------------------------------------------------*/
void fd_table::free_cloexec_fds ()
{
    for (auto it = table.begin(); it != table.end(); )
    {
        /*
         * POSIX.1-2001 says that if file
         * descriptors 0, 1, and 2 would otherwise be closed after a successful
         * execve(), and the process would gain privilege because the set-user_ID or
         * set-group_ID permission bit was set on the executed file, then the system
         * may open an unspecified file for each of these file descriptors.  As a
         * general principle, no portable program, whether privileged or not, can
         * assume that these three file descriptors will remain closed across an
         * execve().
         *
         * I have NO idea what the fuck is going on here but apparently, std[in|out|err]
         * don't get closed after an execve even if they're marked as cloexec???
         */
        if (it->second.close_on_exec)
        {
            debugf("removing cloexec fd: %d (%s)\n", it->second.fds[0], it->second.path.c_str());
            it = free_fd_info(it->second.fds[0]);
        }
		else
		{
			it++;
		}
    }
}

/*-----------------------------------------------------------------------------
    create_temporary_fd_info
-----------------------------------------------------------------------------*/
void fd_table::create_temporary_fd_info
(
	int variantnum,
	unsigned long fd,
	std::string path,
	unsigned long access_flags,
	bool close_on_exec,
	ssize_t original_file_size
)
{
	std::vector<unsigned long> fds(mvee::numvariants);
	std::fill(fds.begin(), fds.end(), MVEE_UNKNOWN_FD);
	fds[variantnum] = fd;

    fd_info info(FT_REGULAR, fds, path, access_flags, close_on_exec, false, true, original_file_size);

	auto it = temporary_files[variantnum].find(fd);
    if (it != temporary_files[variantnum].end())
    {
        warnf("temporary fd override!!! FIXME\n");
        it->second.print_fd_info();
        free_temporary_fd_info(variantnum, fd);
    }

#ifndef MVEE_BENCHMARK
    debugf("created new temporary fd\n");
    info.print_fd_info();
#endif

	temporary_files[variantnum].insert(std::make_pair(fd, info));
}

/*-----------------------------------------------------------------------------
    free_temporary_fd_info
-----------------------------------------------------------------------------*/
void fd_table::free_temporary_fd_info (int variantnum, unsigned long fd)
{
	auto it = temporary_files[variantnum].find(fd);
    if (it != temporary_files[variantnum].end())
    {
        debugf("removed fd: %d (%s)\n", fd, it->second.path.c_str());
        temporary_files[variantnum].erase(it);
    }
}

/*-----------------------------------------------------------------------------
    flush_temporary_files
-----------------------------------------------------------------------------*/
void fd_table::flush_temporary_files (int variantnum)
{
	temporary_files[variantnum].clear();
}

/*-----------------------------------------------------------------------------
    print_fd_table
-----------------------------------------------------------------------------*/
void fd_table::print_fd_table ()
{
	debugf("Normal FD table dump:\n");
    for (auto it = table.begin(); it != table.end(); it++)
        it->second.print_fd_info();

	for (int i = 0; i < mvee::numvariants; ++i)
	{
		debugf("Temporary FD table dump for variant %d:\n", i);
		for (auto it = temporary_files[i].begin(); it != temporary_files[i].end(); ++it)
			it->second.print_fd_info();
	}
}

/*-----------------------------------------------------------------------------
  print_fd_table_proc
-----------------------------------------------------------------------------*/
void fd_table::print_fd_table_proc(pid_t pid)
{
    char        cmd[500];
    sprintf(cmd, "ls -al /proc/%d/fd", pid);

    debugf("fd list for variant %d: \n", pid);
    std::string str = mvee::log_read_from_proc_pipe(cmd, NULL);
    debugf("%s\n",                     str.c_str());
}

/*-----------------------------------------------------------------------------
    verify_fd_table
-----------------------------------------------------------------------------*/
bool fd_table::verify_path(std::string& mvee_path, const char* proc_path)
{
    if (mvee_path == proc_path)
        return true;

    if ((strstr(proc_path, "/dev/pts/") == proc_path)
        && mvee_path.find("std") == 0)
        return true;

    if (strstr(proc_path, "pipe:") == proc_path
        && (mvee_path.find("pipe:") == 0
            || mvee_path.find("pipe2:") == 0))
        return true;

    if (strstr(proc_path, "socket:") == proc_path
        && (mvee_path.find("sock:") == 0 || mvee_path.find("clientsock:") == 0 || mvee_path.find("srvsock:") == 0))
        return true;

    if (strcmp(proc_path, "anon_inode:[eventfd]") == 0
        && mvee_path == "eventfd")
        return true;

    if (strcmp(proc_path, "anon_inode:[eventpoll]") == 0
        && mvee_path == "epoll_sock")
        return true;

    if (strcmp(proc_path, "anon_inode:inotify") == 0
        && mvee_path.find("inotify_init") == 0)
        return true;

	if (strstr(proc_path, "/proc/") == proc_path && 
		mvee_path.find("/proc/") == 0)
		return true;

	if (strstr(proc_path, ".mozilla/firefox/Crash"))
		return true;

    return false;
}

void fd_table::verify_fd_table(std::vector<pid_t> pids)
{
#ifdef MVEE_FD_DEBUG
    for (int i = 0; i < mvee::numvariants; ++i)
    {
        char                                 cmd[500];
        sprintf(cmd, "ls -al /proc/%d/fd | sed 's/.*[0-9][0-9]:[0-9][0-9] //' | grep \"\\->\" | sed 's/ -> /:/'", pids[i]);

        std::stringstream                    ss(mvee::log_read_from_proc_pipe(cmd, NULL));
        std::string                          line;
        std::map<unsigned long, std::string> fds;
        unsigned long                        fd;
        char                                 path[500];

        while (std::getline(ss, line, '\n'))
        {
            if (sscanf(line.c_str(), "%lu:%s", &fd, path) == 2)
            {
                fds.insert(std::make_pair(fd, std::string(path)));
            }
        }

        // Check if /proc maps onto our internal state
        for (auto it = fds.begin(); it != fds.end(); ++it)
        {
            fd = it->first;
            strcpy(path, it->second.c_str());

            fd_info* info = get_fd_info(fd, i);

			if (!info && strstr(path, "socket:") != path)
			{
				warnf("FD TABLE VERIFICATION FAILED - A wild FD appeared! - variant: %d (PID: %d)\n",
					  i, pids[i]);
				warnf("> fd seen in /proc: %d - %s\n", fd, path);
				print_fd_table();
				print_fd_table_proc(pids[i]);
				return;
			}

            if (info && !verify_path(info->path, path))
            {
                warnf("FD TABLE VERIFICATION FAILED - /PROC => INTERNAL - variant: %d (PID: %d)\n", i,  pids[i]);
                warnf("> fd read from proc: %d - %s\n",                                           fd, path);
                warnf("> fd in internal set_fd_table: %d - %s\n",                                 info ? info->fds[i] : 0,
                            info ? info->path.c_str() : "<not found>");
                print_fd_table();
                print_fd_table_proc(pids[i]);
                return;
            }
        }

        // check if our internal state maps onto /proc
        for (auto it = table.begin(); it != table.end(); ++it)
        {
            if (i == 0 || !it->second.master_file)
            {
                auto proc = fds.find(it->second.fds[i]);

                if (proc == fds.end() || !verify_path(it->second.path, proc->second.c_str()))
                {
                    warnf("FD TABLE VERIFICATION FAILED - INTERNAL => /PROC - variant: %d (PID: %d)\n", i, pids[i]);
                    warnf("> fd read from proc: %d - %s\n",
                                proc == fds.end() ? 0 : proc->first,
                                proc == fds.end() ? "<not found>" : proc->second.c_str());
                    warnf("> fd in internal set_fd_table: %d - %s\n",
                                it->second.fds[i],
                                it->second.path.c_str());
                    print_fd_table();
                    print_fd_table_proc(pids[i]);
                    return;
                }
            }
        }
    }
#endif
}

/*-----------------------------------------------------------------------------
    get_fd_info -
-----------------------------------------------------------------------------*/
fd_info* fd_table::get_fd_info (unsigned long fd, int variantnum)
{
    if (variantnum == 0)
    {
        auto it = table.find(fd);
        if (it != table.end())
            return &it->second;
    }
    else if (variantnum < mvee::numvariants && variantnum > 0)
    {
        for (auto it = table.begin(); it != table.end(); it++)
        {
            if (it->second.fds[variantnum] == fd && !it->second.master_file)
                return &it->second;
        }
    }

	auto it = temporary_files[variantnum].find(fd);
	if (it != temporary_files[variantnum].end())
		return &it->second;

//    warnf("WARNING: couldn't find fd %d\n", fd);
    return NULL;
}

/*-----------------------------------------------------------------------------
    get_full_path - this function also supports the [syscall]at family
    but it can resolve normal paths as well (if master_dirfd == AT_FDCWD)
-----------------------------------------------------------------------------*/
std::string fd_table::get_full_path (int variantnum, pid_t variantpid, unsigned long dirfd, void* path_ptr)
{
    std::stringstream ss;

    // fetch the path and check if it's absolute...
	std::string tmp_path = rw::read_string(variantpid, path_ptr, 0);

    if (tmp_path.length() > 0 &&
		tmp_path.find("/proc/self/") == 0)
    {
        ss << "/proc/" << variantpid << "/" << tmp_path.substr(strlen("/proc/self/"));
    }
    else if (tmp_path.length() > 0 &&
			 tmp_path[0] == '/')
    {
        // it's absolute so we can ignore the dirfd...
        ss << tmp_path;
    }
    else
    {
        // relative path... fetch the base path
        if (dirfd == (unsigned long)AT_FDCWD)
        {
			if (fd_cwd == "")
			{
				char proc_path[100];
				char cwd_path[2048];

				memset(cwd_path, 0, 2048);
				sprintf(proc_path, "/proc/%d/cwd", variantpid);
				if (readlink(proc_path, cwd_path, 2048) != -1)
					ss << cwd_path;
			}
			else
			{
				ss << fd_cwd;
			}
        }
        else
        {
            fd_info* fd_info = get_fd_info(dirfd, variantnum);
            if (fd_info)
                ss << fd_info->path;
        }

		if (tmp_path.length() > 0)
		{
			if (ss.str()[ss.str().length()-1] != '/')
				ss << '/';
			ss << tmp_path;
		}
    }

	return mvee::os_normalize_path_name(ss.str());
}

/*-----------------------------------------------------------------------------
    grab_lock
-----------------------------------------------------------------------------*/
void fd_table::grab_lock()
{
    pthread_mutex_lock(&lock);
}

/*-----------------------------------------------------------------------------
    release_lock
-----------------------------------------------------------------------------*/
void fd_table::release_lock()
{
    pthread_mutex_unlock(&lock);
}

/*-----------------------------------------------------------------------------
    full_release_lock
-----------------------------------------------------------------------------*/
void fd_table::full_release_lock()
{
    while (lock.__data.__owner == syscall(__NR_gettid))
        release_lock();
}

/*-----------------------------------------------------------------------------
    have_unlocked
-----------------------------------------------------------------------------*/
bool fd_table::have_unlocked()
{
	if (lock.__data.__owner == syscall(__NR_gettid))
		return false;
	return true;
}

/*-----------------------------------------------------------------------------
    epoll_id_register
-----------------------------------------------------------------------------*/
void fd_table::epoll_id_register(unsigned long epfd, unsigned long fd, std::vector<unsigned long> ids)
{
    // check if we've already registered ids with this epfd
    auto it = epoll_map.find(epfd);
    if (it == epoll_map.end())
    {
        std::map<unsigned long, std::vector<unsigned long> > new_map;
        new_map.insert(std::make_pair(fd, ids));
        epoll_map.insert(std::make_pair(epfd, new_map));
        return;
    }

    it->second.erase(fd);
    it->second.insert(std::make_pair(fd, ids));
}

/*-----------------------------------------------------------------------------
    epoll_id_remove
-----------------------------------------------------------------------------*/
void fd_table::epoll_id_remove(unsigned long epfd, unsigned long fd)
{
    auto it = epoll_map.find(epfd);
    if (it != epoll_map.end())
        it->second.erase(fd);
}

/*-----------------------------------------------------------------------------
    epoll_id_map
-----------------------------------------------------------------------------*/
std::vector<unsigned long> fd_table::epoll_id_map(unsigned long epfd, unsigned long master_id)
{
    auto it = epoll_map.find(epfd);
    if (it != epoll_map.end())
    {
        for (auto fd_it = it->second.begin(); fd_it != it->second.end(); ++fd_it)
        {
            if (fd_it->second[0] == master_id)
                return fd_it->second;
        }
    }

    warnf("couldn't map master id 0x" PTRSTR " to slave ids for epoll fd: %d\n", master_id, epfd);

    std::vector<unsigned long> result(mvee::numvariants);
    for (int i = 0; i < mvee::numvariants; ++i)
        result[i] = 0;
    return result;
}

/*-----------------------------------------------------------------------------
    mvee_fd_get_free_fd - get an available fd for this variant

    This function won't be used very often. At this moment it is only used
    for DUP2 and DUP3 in case the master replica tries to create an fd
    that is not in use yet.
-----------------------------------------------------------------------------*/
unsigned long fd_table::get_free_fd (int variantnum, unsigned long bias)
{
    unsigned long                              lowest_available = 0;
    std::set<unsigned long>                    variant_fds;

    if (bias != (unsigned long)-1 && !get_fd_info(bias, variantnum))
        return bias;

    for (auto it = table.begin(); it != table.end(); ++it)
        variant_fds.insert(it->second.fds[variantnum]);

    // now find the first element that's not in the set
    for (auto it2 = variant_fds.begin(); it2 != variant_fds.end(); ++it2)
    {
        if (*it2 > lowest_available)
            break;
        else
            lowest_available++;
    }

    return lowest_available;
}

/*-----------------------------------------------------------------------------
    is_fd_unsynced
-----------------------------------------------------------------------------*/
bool fd_table::is_fd_unsynced(unsigned long fd, int variantnum)
{
    fd_info* fd_info = get_fd_info(fd, variantnum);

    if (fd_info && fd_info->unsynced_reads)
        return true;
    return false;
}

/*-----------------------------------------------------------------------------
    is_fd_master_file
-----------------------------------------------------------------------------*/
bool fd_table::is_fd_master_file(unsigned long fd, int variantnum)
{
    fd_info* fd_info = get_fd_info(fd, variantnum);

    if (fd_info && fd_info->master_file)
        return true;
    return false;
}

/*-----------------------------------------------------------------------------
    master_fd_set_to_non_master_fd_sets - Creates non-master file descriptor sets
    given a master file descriptor set, using the mapping.

    @param master_fd_set    The file descriptor set for the master variant
    @param nfds Highest-numbered file descriptor in master_fd_set, plus one
    @param variant_fd_set Array of file descriptor sets for non-master variants, to be filled in
-----------------------------------------------------------------------------*/
void fd_table::master_fd_set_to_non_master_fd_sets(fd_set *master_fd_set, int nfds,
                                                   std::vector<fd_set>& variant_fd_sets)
{
    // initialize non-master file descriptor sets
    for (int i = 0; i < mvee::numvariants - 1; ++i)
        FD_ZERO(&variant_fd_sets[i]);

    fd_info* found_info;

    for (int fd = 0; fd < nfds; ++fd)
    {
        if (FD_ISSET(fd, master_fd_set))
        {
            // look up the mapping for the master file descriptor
            found_info = get_fd_info(fd, 0);
            if (found_info)
            {
                // put the corresponding non-master file descriptors in the sets
                for (int i = 1; i < mvee::numvariants; ++i)
                    FD_SET(found_info->fds[i], &variant_fd_sets[i-1]);
            }
        }
    }
}

/*-----------------------------------------------------------------------------
    chdir
-----------------------------------------------------------------------------*/
void fd_table::chdir(const char* path)
{
    if (path && path[0] != '/')
    {
        std::string tmp = fd_cwd;
        tmp   += "/";
        tmp   += path;
        fd_cwd = tmp;
//		warnf("trying to chdir to: %s\n", tmp.c_str());
//		return ::chdir(tmp.c_str());
    }
    else
    {
        fd_cwd = path;
//		return ::chdir(path);
    }
}

/*-----------------------------------------------------------------------------
    set_fd_unlinked
-----------------------------------------------------------------------------*/
void fd_table::set_fd_unlinked(unsigned long fd, int variantnum)
{
    fd_info* fd_info = get_fd_info(fd, variantnum);

    if (fd_info)
		set_file_unlinked(fd_info->path.c_str());
}

/*-----------------------------------------------------------------------------
    set_file_unlinked
-----------------------------------------------------------------------------*/
void fd_table::set_file_unlinked(const char* path)
{
	char* resolved_path = realpath(path, NULL);

	if (!resolved_path)
		resolved_path = strdup(path);

	for (auto it = table.begin(); it != table.end(); ++it)
	{
		if (!strcmp(it->second.path.c_str(), resolved_path))
			it->second.unlinked = true;
	}

	free(resolved_path);
}

/*-----------------------------------------------------------------------------
    is_fd_unlinked
-----------------------------------------------------------------------------*/
bool fd_table::is_fd_unlinked(unsigned long fd, int variantnum)
{
    fd_info* fd_info = get_fd_info(fd, variantnum);

    if (fd_info && (fd_info->unlinked || (fd_info->file_type != FT_REGULAR && fd_info->file_type != FT_UNKNOWN && fd_info->file_type != FT_SPECIAL)))
		return true;
	return false;
}

