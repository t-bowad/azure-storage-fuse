#include "blobfuse.h"
#include <sys/file.h>

gc_cache g_gc_cache;

int map_errno(int error)
{
    auto mapping = error_mapping.find(error);
    if (mapping == error_mapping.end())
    {
        syslog(LOG_INFO, "Failed to map storage error code %d to a proper errno.  Returning EIO = %d instead.\n", error, EIO);
        return EIO;
    }
    else
    {
        return mapping->second;
    }
}

std::string prepend_mnt_path_string(const std::string& path)
{
    std::string result;
    result.reserve(str_options.tmpPath.length() + 5 + path.length());
    return result.append(str_options.tmpPath).append("/root").append(path);
}

//Helper function to help calculate the disk space we have left for the cache location
//params: none
//return: Returns true if we've reached the threshold, false otherwise
bool gc_cache::check_disk_space()
{
    struct statvfs buf;
    if(statvfs(str_options.tmpPath.c_str(), &buf) != 0)
    {
        return false;
    }

    //calculating the percentage of the amount of used space on the cached disk
    //<used space in bytes> = <total size of disk in bytes> - <size of available disk space in bytes>
    //<used percent of cached disk >= <used space> / <total size>
    //f_frsize - the fundamental file system block size (in bytes) (used to convert file system blocks to bytes)
    //f_blocks - total number of blocks on the filesystem/disk in the units of f_frsize
    //f_bfree - total number of free blocks in units of f_frsize
    double total = buf.f_blocks * buf.f_frsize;
    double available = buf.f_bfree * buf.f_frsize;
    double used = total - available;
    double used_percent = (double)(used / total) * (double)100;

    if(used_percent >= high_threshold && !disk_threshold_reached)
    {
        return true;
    }
    else if(used_percent >= low_threshold && disk_threshold_reached)
    {
        return true;
    }
    return false;
}

void gc_cache::add_file(std::string path)
{
    file_to_delete file;
    file.path = path;
    file.closed_time = time(NULL); 
    
    // lock before updating deque
    std::lock_guard<std::mutex> lock(m_deque_lock);
    m_cleanup.push_back(file);
}

void gc_cache::run()
{
    std::thread t1(std::bind(&gc_cache::run_gc_cache,this));
    t1.detach();
}

// cleanup function to clean cached files that are too old
void gc_cache::run_gc_cache()
{

    while(true){

        // lock the deque
        file_to_delete file;
        bool is_empty;
        {
            std::lock_guard<std::mutex> lock(m_deque_lock);
            is_empty = m_cleanup.empty();
            if(!is_empty)
            {
                file = m_cleanup.front();
            }
        }

        //if deque is empty, skip
        if(is_empty)
        {
            //run it every 1 second
            usleep(1000);
            continue;
        }

        time_t now = time(NULL);
        //check if the closed time is old enough to delete
        if(((now - file.closed_time) > file_cache_timeout_in_seconds) || disk_threshold_reached)
        {
            AZS_DEBUGLOGV("File %s being considered for deletion by file cache GC.\n", file.path.c_str());

            // path in the temp location
            const char * mntPath;
            std::string mntPathString = prepend_mnt_path_string(file.path);
            mntPath = mntPathString.c_str();

            //check if the file on disk is still too old
            //mutex lock
            auto fmutex = file_lock_map::get_instance()->get_mutex(file.path.c_str());
            std::lock_guard<std::mutex> lock(*fmutex);

            struct stat buf;
            stat(mntPath, &buf);
            if ((((now - buf.st_mtime) > file_cache_timeout_in_seconds) && ((now - buf.st_ctime) > file_cache_timeout_in_seconds))
                || disk_threshold_reached)
            {
                //clean up the file from cache
                int fd = open(mntPath, O_WRONLY);
                if (fd > 0)
                {
                    int flockres = flock(fd, LOCK_EX|LOCK_NB);
                    if (flockres != 0)
                    {
                        if (errno == EWOULDBLOCK)
                        {
                            // Someone else holds the lock.  In this case, we will postpone updating the cache until the next time open() is called.
                            // TODO: examine the possibility that we can never acquire the lock and refresh the cache.
                            AZS_DEBUGLOGV("Did not clean up file %s from file cache because there's still an open file handle to it.", mntPath);
                        }
                        else
                        {
                            // Failed to acquire the lock for some other reason.  We close the open fd, and continue.
                            syslog(LOG_ERR, "Did not clean up file %s from file cache because we failed to acquire the flock for an unknown reason, errno = %d.\n", mntPath, errno);
                        }
                    }
                    else
                    {
                        unlink(mntPath);
                        flock(fd, LOCK_UN);

                        //update disk space
                        disk_threshold_reached = check_disk_space();
                    }

                    close(fd);
                }
                else
                {
                    //TODO:if we can't open the file consistently, should we just try to move onto the next file?
                    //or somehow timeout on a file we can't open?
                    AZS_DEBUGLOGV("Failed to open file %s from file cache in GC, skipping cleanup. errno from open = %d.", mntPath, errno);
                }
            }

            // lock to remove from front
            {
                std::lock_guard<std::mutex> lock(m_deque_lock);
                m_cleanup.pop_front();
            }

        }
        else
        {
            // no file was timed out - let's wait a second
            usleep(1000);
            //check disk space
            disk_threshold_reached = check_disk_space();
        }
    }

}

// Acquire shared lock utility function
int shared_lock_file(int flags, int fd)
{
    if((flags&O_NONBLOCK) == O_NONBLOCK)
    {
        if(0 != flock(fd, LOCK_SH|LOCK_NB))
        {
            int flockerrno = errno;
            if (flockerrno == EWOULDBLOCK)
            {
               AZS_DEBUGLOGV("Failure to acquire flock due to EWOULDBLOCK.  fd = %d.", fd);
            }
            else
            {
               syslog(LOG_ERR, "Failure to acquire flock for fd = %d.  errno = %d", fd, flockerrno);
            }
            close(fd);
            return 0 - flockerrno;
        }
    }
    else
    {
        if (0 != flock(fd, LOCK_SH))
        {
            int flockerrno = errno;
            syslog(LOG_ERR, "Failure to acquire flock for fd = %d.  errno = %d", fd, flockerrno);
            close(fd);
            return 0 - flockerrno;
        }
    }

    return 0;
}

bool is_directory_blob(unsigned long long size, std::vector<std::pair<std::string, std::string>> metadata)
{
    if (size == 0)
    {
        for (auto iter = metadata.begin(); iter != metadata.end(); ++iter)
        {
            if ((iter->first.compare("hdi_isfolder") == 0) && (iter->second.compare("true") == 0))
            {
                return true;
            }
        }
    }
    return false;
}

int ensure_files_directory_exists_in_cache(const std::string& file_path)
{
    char *pp;
    char *slash;
    int status;
    char *copypath = strdup(file_path.c_str());

    status = 0;
    errno = 0;
    pp = copypath;
    while (status == 0 && (slash = strchr(pp, '/')) != 0)
    {
        if (slash != pp)
        {
            *slash = '\0';
            AZS_DEBUGLOGV("Making cache directory %s.\n", copypath);
            struct stat st;
            if (stat(copypath, &st) != 0)
            {
                status = mkdir(copypath, default_permission);
            }

            // Ignore if some other thread was successful creating the path
            if(errno == EEXIST)
            {
                status = 0;
                errno = 0;
            }

            *slash = '/';
        }
        pp = slash + 1;
    }
    free(copypath);
    return status;
}

std::vector<std::pair<std::vector<list_blobs_hierarchical_item>, bool>> list_all_blobs_hierarchical(const std::string& container, const std::string& delimiter, const std::string& prefix, const std::size_t maxresults)
{
    static const int maxFailCount = 20;
    std::vector<std::pair<std::vector<list_blobs_hierarchical_item>, bool>>  results;

    std::string continuation;

    std::string prior;
    bool success = false;
    int failcount = 0;
    do
    {
        AZS_DEBUGLOGV("About to call list_blobs_hierarchial.  Container = %s, delimiter = %s, continuation = %s, prefix = %s\n", container.c_str(), delimiter.c_str(), continuation.c_str(), prefix.c_str());

        errno = 0;
        list_blobs_hierarchical_response response;
        if (maxresults <= 0)
        {
            response = azure_blob_client_wrapper->list_blobs_hierarchical(container, delimiter, continuation, prefix);
        }
        else
        {
            response = azure_blob_client_wrapper->list_blobs_hierarchical(container, delimiter, continuation, prefix, maxresults);
        }
        if (errno == 0)
        {
            success = true;
            failcount = 0;
            AZS_DEBUGLOGV("Successful call to list_blobs_hierarchical.  results count = %s, next_marker = %s.\n", to_str(response.blobs.size()).c_str(), response.next_marker.c_str());
            continuation = response.next_marker;
            if(response.blobs.size() > 0)
            {
                bool skip_first = false;
                if(response.blobs[0].name == prior)
                {
                    skip_first = true;
                }
                prior = response.blobs.back().name;
                results.push_back(std::make_pair(std::move(response.blobs), skip_first));
            }
        }
        else
        {
            failcount++;
            success = false;
            AZS_DEBUGLOGV("list_blobs_hierarchical failed for the %d time with errno = %d.\n", failcount, errno);

        }
    } while ((!continuation.empty() || !success) && (failcount < maxFailCount) );

    // errno will be set by list_blobs_hierarchial if the last call failed and we're out of retries.
    return results;
}

/*
 * Check if the directory is empty or not by checking if there is any blob with prefix exists in the specified container.
 *
 * return
 *   - D_NOTEXIST if there's nothing there (the directory does not exist)
 *   - D_EMPTY is there's exactly one blob, and it's the ".directory" blob
 *   - D_NOTEMPTY otherwise (the directory exists and is not empty.)
 */
int is_directory_empty(const std::string& container, const std::string& dir_name)
{
    std::string delimiter = "/";
    bool dir_blob_exists = false;
    errno = 0;
    blob_property props = azure_blob_client_wrapper->get_blob_property(container, dir_name);
    if ((errno == 0) && (props.valid()))
    {
        dir_blob_exists = is_directory_blob(props.size, props.metadata);
    }
    if (errno != 0)
    {
        if ((errno != 404) && (errno != ENOENT))
        {
            return -1; // Failure in fetching properties - errno set by blob_exists
        }
    }

    std::string prefix_with_slash = dir_name;
    prefix_with_slash.append(delimiter);
    std::string continuation;
    bool success = false;
    int failcount = 0;
    bool old_dir_blob_found = false;
    do
    {
        errno = 0;
        list_blobs_hierarchical_response response = azure_blob_client_wrapper->list_blobs_hierarchical(container, delimiter, continuation, prefix_with_slash, 2);
        if (errno == 0)
        {
            success = true;
            failcount = 0;
            continuation = response.next_marker;
            if (response.blobs.size() > 1)
            {
                return D_NOTEMPTY;
            }
            if (response.blobs.size() > 0)
            {
                if ((!old_dir_blob_found) &&
                    (!response.blobs[0].is_directory) &&
                    (response.blobs[0].name.size() > former_directory_signifier.size()) &&
                    (0 == response.blobs[0].name.compare(response.blobs[0].name.size() - former_directory_signifier.size(), former_directory_signifier.size(), former_directory_signifier)))
                {
                    old_dir_blob_found = true;
                }
                else
                {
                    return D_NOTEMPTY;
                }
            }
        }
        else
        {
            success = false;
            failcount++; //TODO: use to set errno.
        }
    } while ((continuation.size() > 0 || !success) && failcount < 20);

    if (!success)
    {
    // errno will be set by list_blobs_hierarchial if the last call failed and we're out of retries.
        return -1;
    }

    return old_dir_blob_found || dir_blob_exists ? D_EMPTY : D_NOTEXIST;
}


int azs_getattr(const char *path, struct stat *stbuf)
{
    AZS_DEBUGLOGV("azs_getattr called with path = %s\n", path);
    // If we're at the root, we know it's a directory
    if (strlen(path) == 1)
    {
        stbuf->st_mode = S_IFDIR | default_permission; // TODO: proper access control.
        stbuf->st_uid = fuse_get_context()->uid;
        stbuf->st_gid = fuse_get_context()->gid;
        stbuf->st_nlink = 2; // Directories should have a hard-link count of 2 + (# child directories).  We don't have that count, though, so we just use 2 for now.  TODO: Evaluate if we could keep this accurate or not.
        stbuf->st_size = 4096;
        stbuf->st_mtime = time(NULL);
        return 0;
    }

    // Ensure that we don't get attributes while the file is in an intermediate state.
    auto fmutex = file_lock_map::get_instance()->get_mutex(path);
    std::lock_guard<std::mutex> lock(*fmutex);

    // Check and see if the file/directory exists locally (because it's being buffered.)  If so, skip the call to Storage.
    std::string pathString(path);
    std::string mntPathString = prepend_mnt_path_string(pathString);

    int res;
    int acc = access(mntPathString.c_str(), F_OK);
    if (acc != -1 )
    {
        AZS_DEBUGLOGV("Accessing mntPath = %s for get_attr succeeded; object is in the local cache.\n", mntPathString.c_str());
        
        res = lstat(mntPathString.c_str(), stbuf);
        if (res == -1)
        {
            int lstaterrno = errno;
            syslog(LOG_ERR, "lstat on file %s in local cache during get_attr failed with errno = %d.\n", mntPathString.c_str(), lstaterrno);
            return -lstaterrno;
        }
        else
        {
            AZS_DEBUGLOGV("lstat on file %s in local cache succeeded.\n", mntPathString.c_str());
            return 0;
        }
    }
    else
    {
        AZS_DEBUGLOGV("Object %s is not in the local cache during get_attr.\n", mntPathString.c_str());
    }
    //It's not in the local cache. Check to see if it's a directory using list 
    std::string blobNameStr(&(path[1]));
    errno = 0;
    size_t resultCount = 2;
    std::vector<std::pair<std::vector<list_blobs_hierarchical_item>, bool>> listResponse =  list_all_blobs_hierarchical(str_options.containerName, "/", blobNameStr, resultCount) ;
    
    if (errno == 0 && listResponse.size() > 0 )
    {
        list_blobs_hierarchical_item blobItem;

        unsigned int batchNum = 0;

        unsigned int resultStart =0;

        // this variable will be incremented below if it is a directory, otherwise it will not be used. 
        unsigned int dirSize = 0;

        for (batchNum=0; batchNum < listResponse.size(); batchNum++)
        {
            // if skip_first start the listResults at 1
            if (listResponse[batchNum].second)
            {
                resultStart=1;
            }
            else
            {
                resultStart=0;
            }
            std::vector<list_blobs_hierarchical_item> listResults = listResponse[batchNum].first;
        
            for (unsigned int i=resultStart; i < listResults.size(); i++)
            {
                AZS_DEBUGLOGV("In azs_getattr list_blobs_hierarchical_item %d file %s\n", i, listResults[i].name.c_str() );

                // if the path for exact name is found the dirSize will be 1 here so check to see if it has files or subdirectories inside
                // match dir name or longer paths to determine dirSize
                if ( listResults[i].name.compare(blobNameStr + '/') < 0)
                {
                    dirSize++;
                    // listing is hierarchical so no need of the 2nd is blobitem.name empty condition but just in case for service errors
                    if (dirSize > 2 && !blobItem.name.empty())
                    {
                        break;
                    }
                }

                // the below will be skipped blobItem has been found already because we only need the exact match
                // find the element with the exact prefix
                // this could lead to a bug when there is a file with the same name as the directory in the parent directory. In short, that won't work.
                if (blobItem.name.empty() && (listResults[i].name == blobNameStr || listResults[i].name == (blobNameStr + '/')))
                {
                    blobItem = listResults[i];
                    AZS_DEBUGLOGV("In azs_getattr found blob in list hierarchical file %s\n", blobItem.name.c_str() );
                    // leave 'i' at the value it is, it will be used in the remaining batches and loops to check for directory empty check.
                    if (dirSize==0 && (is_directory_blob(0, blobItem.metadata) || blobItem.is_directory || blobItem.name == (blobNameStr + '/')))
                    {
                        dirSize = 1; // root directory exists so 1
                    }
                }
            }

        }
        
        if (!blobItem.name.empty() && (is_directory_blob(0, blobItem.metadata) || blobItem.is_directory || blobItem.name == (blobNameStr + '/')))
        {
            AZS_DEBUGLOGV( "%s is a directory, blob name is %s\n", mntPathString.c_str(), blobItem.name.c_str() ); 
            AZS_DEBUGLOGV("Blob %s, representing a directory, found during get_attr.\n", path);
            stbuf->st_mode = S_IFDIR | default_permission;
            // If st_nlink = 2, means directory is empty.
            // Directory size will affect behaviour for mv, rmdir, cp etc.
            stbuf->st_uid = fuse_get_context()->uid;
            stbuf->st_gid = fuse_get_context()->gid;   
            // assign directory status as empty or non-empty based on the value from above  
            stbuf->st_nlink = dirSize > 1 ? 3 : 2;
            stbuf->st_size = 4096;
            return 0;
        }
        else if (!blobItem.name.empty() )
        {
            AZS_DEBUGLOGV("%s is a file, blob name is %s\n", mntPathString.c_str(), blobItem.name.c_str() ); 
            AZS_DEBUGLOGV("Blob %s, representing a file, found during get_attr.\n", path);
            stbuf->st_mode = S_IFREG | default_permission; // Regular file (not a directory)
            stbuf->st_uid = fuse_get_context()->uid;
            stbuf->st_gid = fuse_get_context()->gid;
            auto blob_property = azure_blob_client_wrapper->get_blob_property(str_options.containerName, blobNameStr);
            stbuf->st_mtime = blob_property.last_modified;
            AZS_DEBUGLOGV("The last modified time is %s, the size is %llu ", blobItem.last_modified.c_str(), blob_property.size);
            stbuf->st_nlink = 1;
            stbuf->st_size = blob_property.size;
            return 0;
        }
        else // none of the blobs match exactly so blob not found
        { 
            AZS_DEBUGLOGV("%s does not match the exact name in the top 2 return from list_hierarchial_blobs. It will be treated as a new blob", blobNameStr.c_str() );
            return -(ENOENT);
        }     
    }
    else if (errno > 0)
    {
        int storage_errno = errno;
        AZS_DEBUGLOGV("Failure when attempting to determine if %s exists on the service.  errno = %d.\n", blobNameStr.c_str(), storage_errno);
        syslog(LOG_ERR, "Failure when attempting to determine if %s exists on the service.  errno = %d.\n", blobNameStr.c_str(), storage_errno);
        return 0 - map_errno(storage_errno);
    }
    else // it is a new blob
    { 
        AZS_DEBUGLOGV("%s not returned in list_hierarchial_blobs. It is a new blob", blobNameStr.c_str() );
        return -(ENOENT);
    }      
   
}

// Helper method for FTW to remove an entire directory & it's contents.
int rm(const char *fpath, const struct stat * /*sb*/, int tflag, struct FTW * /*ftwbuf*/)
{
    if (tflag == FTW_DP)
    {
        errno = 0;
        int ret = rmdir(fpath);
        return ret;
    }
    else
    {
        errno = 0;
        int ret = unlink(fpath);
        return ret;
    }
}

// Delete the entire contents of tmpPath.
void azs_destroy(void * /*private_data*/)
{
    AZS_DEBUGLOG("azs_destroy called.\n");
    std::string rootPath(str_options.tmpPath + "/root");

    errno = 0;
    // FTW_DEPTH instructs FTW to do a post-order traversal (children of a directory before the actual directory.)
    nftw(rootPath.c_str(), rm, 20, FTW_DEPTH); 
}


// Not yet implemented section:
int azs_access(const char * /*path*/, int /*mask*/)
{
    return 0;  // permit all access
}

int azs_readlink(const char * /*path*/, char * /*buf*/, size_t /*size*/)
{
    return -EINVAL; // not a symlink
}

int azs_fsync(const char * /*path*/, int /*isdatasync*/, struct fuse_file_info * /*fi*/)
{
    return 0; // Skip for now
}

int azs_chown(const char * /*path*/, uid_t /*uid*/, gid_t /*gid*/)
{
    //TODO: Implement
//    return -ENOSYS;
    return 0;
}

int azs_chmod(const char * /*path*/, mode_t /*mode*/)
{
    //TODO: Implement
//    return -ENOSYS;
    return 0;

}

//#ifdef HAVE_UTIMENSAT
int azs_utimens(const char * /*path*/, const struct timespec [2] /*ts[2]*/)
{
    //TODO: Implement
//    return -ENOSYS;
    return 0;
}
//  #endif

int azs_rename_directory(const char *src, const char *dst)
{
    AZS_DEBUGLOGV("azs_rename_directory called with src = %s, dst = %s.\n", src, dst);
    std::string srcPathStr(src);
    std::string dstPathStr(dst);

    // Rename the directory blob, if it exists.
    errno = 0;
    blob_property props = azure_blob_client_wrapper->get_blob_property(str_options.containerName, srcPathStr.substr(1));
    if ((errno == 0) && (props.valid()))
    {
        if (is_directory_blob(props.size, props.metadata))
        {
            azs_rename_single_file(src, dst);
        }
    }
    if (errno != 0)
    {
        if ((errno != 404) && (errno != ENOENT))
        {
            return 0 - map_errno(errno); // Failure in fetching properties - errno set by blob_exists
        }
    }

    if (srcPathStr.size() > 1)
    {
        srcPathStr.push_back('/');
    }
    if (dstPathStr.size() > 1)
    {
        dstPathStr.push_back('/');
    }
    std::vector<std::string> local_list_results;

    // Rename all files and directories that exist in the local cache.
    ensure_files_directory_exists_in_cache(prepend_mnt_path_string(dstPathStr + "placeholder"));
    std::string mntPathString = prepend_mnt_path_string(srcPathStr);
    DIR *dir_stream = opendir(mntPathString.c_str());
    if (dir_stream != NULL)
    {
        struct dirent* dir_ent = readdir(dir_stream);
        while (dir_ent != NULL)
        {
            if (dir_ent->d_name[0] != '.')
            {
                int nameLen = strlen(dir_ent->d_name);
                char *newSrc = (char *)malloc(sizeof(char) * (srcPathStr.size() + nameLen + 1));
                memcpy(newSrc, srcPathStr.c_str(), srcPathStr.size());
                memcpy(&(newSrc[srcPathStr.size()]), dir_ent->d_name, nameLen);
                newSrc[srcPathStr.size() + nameLen] = '\0';

                char *newDst = (char *)malloc(sizeof(char) * (dstPathStr.size() + nameLen + 1));
                memcpy(newDst, dstPathStr.c_str(), dstPathStr.size());
                memcpy(&(newDst[dstPathStr.size()]), dir_ent->d_name, nameLen);
                newDst[dstPathStr.size() + nameLen] = '\0';

                AZS_DEBUGLOGV("Local object found - about to rename %s to %s.\n", newSrc, newDst);
                if (dir_ent->d_type == DT_DIR)
                {
                    azs_rename_directory(newSrc, newDst);
                }
                else
                {
                    azs_rename_single_file(newSrc, newDst);
                }

                free(newSrc);
                free(newDst);

                std::string dir_str(dir_ent->d_name);
                local_list_results.push_back(dir_str);
            }

            dir_ent = readdir(dir_stream);
        }

        closedir(dir_stream);
    }

    // Rename all files & directories that don't exist in the local cache.
    errno = 0;
    std::vector<std::pair<std::vector<list_blobs_hierarchical_item>, bool>> listResults = list_all_blobs_hierarchical(str_options.containerName, "/", srcPathStr.substr(1));
    if (errno != 0)
    {
        int storage_errno = errno;
        syslog(LOG_ERR, "list blobs operation failed during attempt to rename directory %s to %s.  errno = %d.\n", src, dst, storage_errno);
        return 0 - map_errno(storage_errno);
    }

    AZS_DEBUGLOGV("Total of %s result lists found from list_blobs call during rename operation\n.", to_str(listResults.size()).c_str());
    for (size_t result_lists_index = 0; result_lists_index < listResults.size(); result_lists_index++)
    {
        int start = listResults[result_lists_index].second ? 1 : 0;
        for (size_t i = start; i < listResults[result_lists_index].first.size(); i++)
        {
            // We need to parse out just the trailing part of the path name.
            int len = listResults[result_lists_index].first[i].name.size();
            if (len > 0)
            {
                std::string prev_token_str;
                if (listResults[result_lists_index].first[i].name.back() == '/')
                {
                    prev_token_str = listResults[result_lists_index].first[i].name.substr(srcPathStr.size() - 1, listResults[result_lists_index].first[i].name.size() - srcPathStr.size());
                }
                else
                {
                    prev_token_str = listResults[result_lists_index].first[i].name.substr(srcPathStr.size() - 1);
                }

                // TODO: order or hash the list to improve perf
                if ((prev_token_str.size() > 0) && (std::find(local_list_results.begin(), local_list_results.end(), prev_token_str) == local_list_results.end()))
                {
                    int nameLen = prev_token_str.size();
                    char *newSrc = (char *)malloc(sizeof(char) * (srcPathStr.size() + nameLen + 1));
                    memcpy(newSrc, srcPathStr.c_str(), srcPathStr.size());
                    memcpy(&(newSrc[srcPathStr.size()]), prev_token_str.c_str(), nameLen);
                    newSrc[srcPathStr.size() + nameLen] = '\0';

                    char *newDst = (char *)malloc(sizeof(char) * (dstPathStr.size() + nameLen + 1));
                    memcpy(newDst, dstPathStr.c_str(), dstPathStr.size());
                    memcpy(&(newDst[dstPathStr.size()]), prev_token_str.c_str(), nameLen);
                    newDst[dstPathStr.size() + nameLen] = '\0';

                    AZS_DEBUGLOGV("Object found on the service - about to rename %s to %s.\n", newSrc, newDst);
                    if (listResults[result_lists_index].first[i].is_directory)
                    {
                        azs_rename_directory(newSrc, newDst);
                    }
                    else
                    {
                        azs_rename_single_file(newSrc, newDst);
                    }

                    free(newSrc);
                    free(newDst);
                }
            }
        }
    }
    azs_rmdir(src);
    return 0;
}

// TODO: Fix bug where the files and directories in the source in the file cache are not deleted.
// TODO: Fix bugs where the a file has been created but not yet uploaded.
// TODO: Fix the bug where this fails for multi-level dirrectories.
// TODO: If/when we upgrade to FUSE 3.0, we will need to worry about the additional possible flags (RENAME_EXCHANGE and RENAME_NOREPLACE)
int azs_rename(const char *src, const char *dst)
{
    AZS_DEBUGLOGV("azs_rename called with src = %s, dst = %s.\n", src, dst);

    struct stat statbuf;
    errno = 0;
    int getattrret = azs_getattr(src, &statbuf);
    if (getattrret != 0)
    {
        return getattrret;
    }
    if ((statbuf.st_mode & S_IFDIR) == S_IFDIR)
    {
        azs_rename_directory(src, dst);
    }
    else
    {
        azs_rename_single_file(src, dst);
    }

    return 0;
}


int azs_setxattr(const char * /*path*/, const char * /*name*/, const char * /*value*/, size_t /*size*/, int /*flags*/)
{
    return -ENOSYS;
}
int azs_getxattr(const char * /*path*/, const char * /*name*/, char * /*value*/, size_t /*size*/)
{
    return -ENOSYS;
}
int azs_listxattr(const char * /*path*/, char * /*list*/, size_t /*size*/)
{
    return -ENOSYS;
}
int azs_removexattr(const char * /*path*/, const char * /*name*/)
{
    return -ENOSYS;
}
