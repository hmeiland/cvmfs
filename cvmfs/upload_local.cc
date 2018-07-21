/**
 * This file is part of the CernVM File System.
 */

#include "upload_local.h"
#include "cvmfs_config.h"

#include <errno.h>

#include <string>

#include "compression.h"
#include "logging.h"
#include "platform.h"
#include "util/posix.h"

namespace upload {

LocalUploader::LocalUploader(const SpoolerDefinition &spooler_definition)
    : AbstractUploader(spooler_definition),
      backend_file_mode_(default_backend_file_mode_ ^ GetUmask()),
      upstream_path_(spooler_definition.spooler_configuration),
      temporary_path_(spooler_definition.temporary_path) {
  assert(spooler_definition.IsValid() &&
         spooler_definition.driver_type == SpoolerDefinition::Local);

  atomic_init32(&copy_errors_);
}

bool LocalUploader::WillHandle(const SpoolerDefinition &spooler_definition) {
  return spooler_definition.driver_type == SpoolerDefinition::Local;
}

unsigned int LocalUploader::GetNumberOfErrors() const {
  return atomic_read32(&copy_errors_);
}

void LocalUploader::FileUpload(const std::string &local_path,
                               const std::string &remote_path,
                               const CallbackTN *callback) {
  LogCvmfs(kLogSpooler, kLogVerboseMsg, "FileUpload call started.");

  // create destination in backend storage temporary directory
  std::string tmp_path = CreateTempPath(temporary_path_ + "/upload", 0666);
  if (tmp_path.empty()) {
    LogCvmfs(kLogSpooler, kLogVerboseMsg,
             "failed to create temp path for "
             "upload of file '%s' (errno: %d)",
             local_path.c_str(), errno);
    atomic_inc32(&copy_errors_);
    Respond(callback, UploaderResults(1, local_path));
    return;
  }

  // copy file into controlled temporary directory location
  int retval = CopyPath2Path(local_path, tmp_path);
  int retcode = retval ? 0 : 100;
  if (retcode != 0) {
    LogCvmfs(kLogSpooler, kLogVerboseMsg,
             "failed to copy file '%s' to staging "
             "area: '%s'",
             local_path.c_str(), tmp_path.c_str());
    atomic_inc32(&copy_errors_);
    Respond(callback, UploaderResults(retcode, local_path));
    return;
  }

  // move the file in place (atomic operation)
  retcode = Move(tmp_path, remote_path);
  if (retcode != 0) {
    LogCvmfs(kLogSpooler, kLogVerboseMsg,
             "failed to move file '%s' from the "
             "staging area to the final location: "
             "'%s'",
             tmp_path.c_str(), remote_path.c_str());
    atomic_inc32(&copy_errors_);
    Respond(callback, UploaderResults(retcode, local_path));
    return;
  }
  Respond(callback, UploaderResults(retcode, local_path));
}

UploadStreamHandle *LocalUploader::InitStreamedUpload(
    const CallbackTN *callback) {
  std::string tmp_path;
  const int tmp_fd = CreateAndOpenTemporaryChunkFile(&tmp_path);
  if (tmp_fd < 0) {
    atomic_inc32(&copy_errors_);
    return NULL;
  }

  return new LocalStreamHandle(callback, tmp_fd, tmp_path);
}

void LocalUploader::StreamedUpload(UploadStreamHandle *handle,
                                   UploadBuffer buffer,
                                   const CallbackTN *callback) {
  LocalStreamHandle *local_handle = static_cast<LocalStreamHandle *>(handle);

  const off_t offset = lseek(local_handle->file_descriptor, 0, SEEK_CUR);
  if (offset < 0) {
    int seek_err = errno;
    LogCvmfs(kLogSpooler, kLogVerboseMsg,
             "failed to seek in '%s' (errno: %d)",
             local_handle->temporary_path.c_str(), seek_err);
    atomic_inc32(&copy_errors_);
    Respond(callback,
            UploaderResults(UploaderResults::kBufferUpload, seek_err));
    return;
  }

  const size_t bytes_written =
      write(local_handle->file_descriptor, buffer.data, buffer.size);
  if (bytes_written != buffer.size) {
    const int cpy_errno = errno;
    LogCvmfs(kLogSpooler, kLogVerboseMsg,
             "failed to write %d bytes to '%s' "
             "(errno: %d)",
             buffer.size, local_handle->temporary_path.c_str(),
             cpy_errno);
    atomic_inc32(&copy_errors_);
    Respond(callback,
            UploaderResults(UploaderResults::kBufferUpload, cpy_errno));
    return;
  }

  // Tell kernel to evict written pages from the page cache.  We don't care if
  // it succeeds or not.
  (void)platform_invalidate_kcache(local_handle->file_descriptor, offset,
                                   bytes_written);

  Respond(callback, UploaderResults(UploaderResults::kBufferUpload, 0));
}

void LocalUploader::FinalizeStreamedUpload(UploadStreamHandle *handle,
                                           const shash::Any &content_hash) {
  int retval = 0;
  LocalStreamHandle *local_handle = static_cast<LocalStreamHandle *>(handle);

  retval = close(local_handle->file_descriptor);
  if (retval != 0) {
    const int cpy_errno = errno;
    LogCvmfs(kLogSpooler, kLogVerboseMsg,
             "failed to close temp file '%s' "
             "(errno: %d)",
             local_handle->temporary_path.c_str(), cpy_errno);
    atomic_inc32(&copy_errors_);
    Respond(handle->commit_callback,
            UploaderResults(UploaderResults::kChunkCommit, cpy_errno));
    return;
  }

  const std::string final_path = "data/" + content_hash.MakePath();
  if (!Peek(final_path)) {
    retval = Move(local_handle->temporary_path, final_path);
    if (retval != 0) {
      const int cpy_errno = errno;
      LogCvmfs(kLogSpooler, kLogVerboseMsg,
               "failed to move temp file '%s' to "
               "final location '%s' (errno: %d)",
               local_handle->temporary_path.c_str(), final_path.c_str(),
               cpy_errno);
      atomic_inc32(&copy_errors_);
      Respond(handle->commit_callback,
              UploaderResults(UploaderResults::kChunkCommit, cpy_errno));
      return;
    }
  } else {
    printf("Peek retval = %d\n", Peek(final_path));
    if (counters_.IsValid()) {
      perf::Inc(counters_->n_duplicated_files);
    }
    const int retval = unlink(local_handle->temporary_path.c_str());
    if (retval != 0) {
      LogCvmfs(kLogSpooler, kLogVerboseMsg,
               "failed to remove temporary file '%s' (errno: %d)",
               local_handle->temporary_path.c_str(), errno);
    }
  }

  const CallbackTN *callback = handle->commit_callback;
  delete local_handle;

  Respond(callback, UploaderResults(UploaderResults::kChunkCommit, 0));
}

/**
 * TODO(jblomer): investigate if parallelism increases the GC speed on local
 * disks.
 */
void LocalUploader::DoRemoveAsync(const std::string &file_to_delete) {
  const int retval = unlink((upstream_path_ + "/" + file_to_delete).c_str());
  if ((retval != 0) && (errno != ENOENT))
    atomic_inc32(&copy_errors_);
  Respond(NULL, UploaderResults());
}

bool LocalUploader::Peek(const std::string &path) const {
  return FileExists(upstream_path_ + "/" + path);
}

bool LocalUploader::PlaceBootstrappingShortcut(const shash::Any &object) const {
  const std::string src = "data/" + object.MakePath();
  const std::string dest = upstream_path_ + "/" + object.MakeAlternativePath();
  return SymlinkForced(src, dest);
}

int LocalUploader::Move(const std::string &local_path,
                        const std::string &remote_path) const {
  const std::string destination_path = upstream_path_ + "/" + remote_path;

  // make sure the file has the right permissions
  int retval = chmod(local_path.c_str(), backend_file_mode_);
  int retcode = (retval == 0) ? 0 : 101;
  if (retcode != 0) {
    LogCvmfs(kLogSpooler, kLogVerboseMsg,
             "failed to set file permission '%s' "
             "errno: %d",
             local_path.c_str(), errno);
    return retcode;
  }

  // move the file in place
  retval = rename(local_path.c_str(), destination_path.c_str());
  retcode = (retval == 0) ? 0 : errno;
  if (retcode != 0) {
    LogCvmfs(kLogSpooler, kLogVerboseMsg,
             "failed to move file '%s' to '%s' "
             "errno: %d",
             local_path.c_str(), remote_path.c_str(), errno);
  }

  return retcode;
}

}  // namespace upload
