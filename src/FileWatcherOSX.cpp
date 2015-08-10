#include "../includes/FileWatcherOSX.h"
#include <iostream>

namespace NSFW {

  FileWatcherOSX::FileWatcherOSX(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles)
    : mEventsQueue(eventsQueue), mPath(path), mWatchFiles(watchFiles) {}

  void FileWatcherOSX::callback(
      ConstFSEventStreamRef streamRef,
      void *clientCallBackInfo,
      size_t numEvents,
      void *eventPaths,
      const FSEventStreamEventFlags eventFlags[],
      const FSEventStreamEventId eventIds[])
  {
      FileWatcherOSX *fwOSX = (FileWatcherOSX *)clientCallBackInfo;
      fwOSX->processCallback();
  }

  bool FileWatcherOSX::checkTimeValEquality(struct timespec *x, struct timespec *y)
  {
    return x->tv_sec == y->tv_sec && x->tv_nsec == y->tv_nsec;
  }

  void FileWatcherOSX::deleteDirTree() {
    std::queue<Directory *> dirQueue;

    dirQueue.push(mDirTree);

    while (!dirQueue.empty()) {
      Directory *root = dirQueue.front();

      // delete all file entries
      for (std::map<ino_t, FileDescriptor>::iterator fileIter = root->fileMap.begin();
        fileIter != root->fileMap.end(); ++fileIter)
      {
        delete fileIter->second.entry;
      }

      // Add directories to the queue to continue deleting directories/files
      for (std::map<ino_t, Directory *>::iterator dirIter = root->childDirectories.begin();
        dirIter != root->childDirectories.end(); ++dirIter)
      {
        dirQueue.push(dirIter->second);
      }

      dirQueue.pop();

      delete root->entry;
      delete root;
    }
    mDirTree = NULL;
  }

  std::string FileWatcherOSX::getPath() {
    return mPath;
  }

  // traverses the tree under a directory and adds every item as an event of type action
  void FileWatcherOSX::handleTraversingDirectoryChange(std::string action, Directory *directory) {
    std::queue<Directory *> dirQueue;

    dirQueue.push(directory);

    while (!dirQueue.empty()) {
      Directory *root = dirQueue.front();

      // Events for all files in this 'root' directory
      for (std::map<ino_t, FileDescriptor>::iterator fileIter = root->fileMap.begin();
        fileIter != root->fileMap.end(); ++fileIter)
      {
        Event event;
        event.directory = root->path;
        event.file = new std::string(fileIter->second.entry->d_name);
        event.action = action;
        mEventsQueue.push(event);
      }

      // Add directories to the queue to continue listing events
      for (std::map<ino_t, Directory *>::iterator dirIter = root->childDirectories.begin();
        dirIter != root->childDirectories.end(); ++dirIter)
      {
        dirQueue.push(dirIter->second);
      }

      Event event;
      event.directory = root->path;
      event.file = new std::string(root->entry->d_name);
      event.action = action;
      mEventsQueue.push(event);

      dirQueue.pop();
    }
  }

  void *FileWatcherOSX::mainLoop(void *params) {
    // load initial dir tree
    ((FileWatcherOSX *)params)->mDirTree = ((FileWatcherOSX *)params)->snapshotDir();

    FileWatcherOSX *fwOSX = (FileWatcherOSX *)params;
    CFStringRef mypath = CFStringCreateWithCString(
      NULL,
      (char *)(fwOSX->getPath().c_str()), // the path that the file watcher should watch
      kCFStringEncodingUTF8);
    CFArrayRef pathsToWatch = CFArrayCreate(NULL, (const void **)&mypath, 1, NULL);
    FSEventStreamRef stream;
    CFAbsoluteTime latency = 1.0;

    FSEventStreamContext callbackInfo;
    callbackInfo.version = 0;
    callbackInfo.info = params; // pass the calling filewatcherosx object into the callback

    /* Create the stream, passing in a callback */
    stream = FSEventStreamCreate(NULL,
        &FileWatcherOSX::callback,
        &callbackInfo,
        pathsToWatch,
        kFSEventStreamEventIdSinceNow,
        latency,
        kFSEventStreamCreateFlagNone
    );

    FSEventStreamScheduleWithRunLoop(stream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    FSEventStreamStart(stream);
    CFRunLoopRun();
  }

  void FileWatcherOSX::processCallback() {
    Directory *currentTree = snapshotDir();
    std::queue<DirectoryPair> dirPairQueue;

    if (currentTree == NULL) {
      // handle errors
      return;
    }

    DirectoryPair snapshot;
    snapshot.prev = mDirTree;
    snapshot.current = currentTree;

    dirPairQueue.push(snapshot);

    while(!dirPairQueue.empty()) {
      // get a DirectoryPair
      snapshot = dirPairQueue.front();

      // compare files in the directory ----------------------------------------
      // -----------------------------------------------------------------------

      std::map<ino_t, FileDescriptor> currentFileMapCopy(snapshot.current->fileMap);

      // iterate through old snapshot, compare to new snapshot
      for (std::map<ino_t, FileDescriptor>::iterator fileIter = snapshot.prev->fileMap.begin();
        fileIter != snapshot.prev->fileMap.end(); ++fileIter)
      {
        std::map<ino_t, FileDescriptor>::iterator currentComparableFilePtr = currentFileMapCopy.find(fileIter->first);

        // deleted event
        if (currentComparableFilePtr == currentFileMapCopy.end()) {
          Event event;
          event.directory = snapshot.current->path;
          event.file = new std::string(fileIter->second.entry->d_name);
          event.action = "DELETED";
          mEventsQueue.push(event);
          continue;
        }

        // renamed event
        if (strcmp(fileIter->second.entry->d_name, currentComparableFilePtr->second.entry->d_name)) {
          Event event;
          event.directory = snapshot.current->path;
          event.file = new std::string[2];
          event.file[0] = fileIter->second.entry->d_name;
          event.file[1] = currentComparableFilePtr->second.entry->d_name;
          event.action = "RENAMED";
          mEventsQueue.push(event);
        }

        // changed event
        if (!checkTimeValEquality(&fileIter->second.meta.st_mtimespec, &currentComparableFilePtr->second.meta.st_mtimespec)) {
          Event event;
          event.directory = snapshot.current->path;
          event.file = new std::string(fileIter->second.entry->d_name);
          event.action = "CHANGED";
          mEventsQueue.push(event);
        }

        currentFileMapCopy.erase(currentComparableFilePtr);
      }

      // find all new files
      for (std::map<ino_t, FileDescriptor>::iterator fileIter = currentFileMapCopy.begin();
        fileIter != currentFileMapCopy.end(); ++fileIter)
      {
        // created event
        Event event;
        event.directory = snapshot.current->path;
        event.file = new std::string(fileIter->second.entry->d_name);
        event.action = "CREATED";
        mEventsQueue.push(event);
      }

      // compare directory structure -------------------------------------------
      // -----------------------------------------------------------------------

      std::map<ino_t, Directory *> currentChildDirectories(snapshot.current->childDirectories);

      // iterate through old snapshot, compare to new snapshot
      for(std::map<ino_t, Directory *>::iterator dirIter = snapshot.prev->childDirectories.begin();
        dirIter != snapshot.prev->childDirectories.end(); ++dirIter)
      {
        std::map<ino_t, Directory *>::iterator currentComparableDirPtr = currentChildDirectories.find(dirIter->first);

        // deleted event
        if (currentComparableDirPtr == currentChildDirectories.end()) {
          // add all associated delete events for this directory deletion
          handleTraversingDirectoryChange("DELETED", dirIter->second);

          continue;
        }

        // renamed event
        if (strcmp(dirIter->second->entry->d_name, currentComparableDirPtr->second->entry->d_name)) {
          Event event;
          event.directory = snapshot.current->path;
          event.file = new std::string[2];
          event.file[0] = dirIter->second->entry->d_name;
          event.file[1] = currentComparableDirPtr->second->entry->d_name;
          event.action = "RENAMED";
          mEventsQueue.push(event);
        }

        // create a new pair for comparison
        DirectoryPair enqueueSnapshot;
        enqueueSnapshot.prev = dirIter->second;
        enqueueSnapshot.current = currentComparableDirPtr->second;

        // push the new pair to the queue
        dirPairQueue.push(enqueueSnapshot);

        // remove the directory so that we can discover added directories faster
        currentChildDirectories.erase(currentComparableDirPtr);
      }

      for (std::map<ino_t, Directory *>::iterator dirIter = currentChildDirectories.begin();
        dirIter != currentChildDirectories.end(); ++dirIter)
      {
        // add all associated created events for this directory deletion
        handleTraversingDirectoryChange("CREATED", dirIter->second);
      }

      // remove the snapshot from the queue
      dirPairQueue.pop();
    }

    // delete mDirTree
    deleteDirTree();

    // assign currentTree to mDirTree
    mDirTree = currentTree;
  }

  Directory *FileWatcherOSX::snapshotDir() {
    std::queue<Directory *> dirQueue;
    Directory *topRoot = new Directory;

    // create root of snapshot
    topRoot->entry = NULL;
    topRoot->path = mPath;

    dirQueue.push(topRoot);

    while (!dirQueue.empty()) {
      Directory *root = dirQueue.front();
      dirent ** directoryContents = NULL;
      int n = scandir(root->path.c_str(), &directoryContents, NULL, alphasort);

      if (n < 0) {
        return NULL; // add error handling
      }

      // find all the directories within this directory
      // this breaks the alphabetical sorting of directories
      std::queue<int> childLocation;
      for (int i = 0; i < n; ++i) {
        if (!strcmp(directoryContents[i]->d_name, ".") || !strcmp(directoryContents[i]->d_name, ".."))
          continue; // skip navigation folder

        ino_t inode = directoryContents[i]->d_ino;

        if (directoryContents[i]->d_type == DT_DIR)
        {
          // create the directory struct for this directory and add a reference of this directory to its root
          Directory *dir = new Directory;
          dir->entry = directoryContents[i];
          dir->path = root->path + "/" + dir->entry->d_name;
          root->childDirectories[inode] = dir;
          dirQueue.push(dir);
        } else {
          // store the file information in a quick data structure for later
          FileDescriptor fd;
          fd.entry = directoryContents[i];
          fd.path = root->path + "/" + fd.entry->d_name;
          int error = stat(fd.path.c_str(), &fd.meta);

          if (error < 0) {
            return NULL; // add error handling
          }

          root->fileMap[inode] = fd;
        }
      }

      delete[] directoryContents;
      dirQueue.pop();
    }

    return topRoot;
  }

  bool FileWatcherOSX::start() {
    if (pthread_create(&mThread, 0, &FileWatcherOSX::mainLoop, (void *)this)) {
      return true;
    } else {
      return false;
    }
  }

}
