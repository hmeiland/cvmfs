#include "upload_file_processor.h"

#include "upload_file_chunker.h"
#include "compression.h"

using namespace upload;

FileProcessor::FileProcessor(const worker_context *context) :
  temporary_path_(context->temporary_path) {}


void FileProcessor::operator()(const FileProcessor::Parameters &data) {
  // get data references to the provided job structure for convenience
  const std::string &local_path     = data.local_path;
  const bool         allow_chunking = data.allow_chunking;

  Results result(local_path);

  // map the file to process into memory
  MemoryMappedFile mmf(local_path);
  if (!mmf.Map()) {
    result.return_code = 1;
    goto fail;
  }

  // chunk the file if needed
  if (allow_chunking && ! GenerateFileChunks(mmf, result)) {
    result.return_code = 2;
    goto fail;
  }

  // check if we only produced one chunk...
  if (result.file_chunks.size() == 1) {
    // ... and simply use that as bulk file as well
    result.bulk_file = result.file_chunks.front();

    // otherwise generate an additional bulk version of the file
  } else if (! GenerateBulkFile(mmf, result)) {
    result.return_code = 3;
    goto fail;
  }

  // all done
  result.return_code = 0;
  master()->JobSuccessful(result);
  return;

fail:
  master()->JobFailed(result);
}


bool FileProcessor::GenerateFileChunks(const MemoryMappedFile  &mmf,
                                       Results                 &data) const {
  assert (mmf.IsMapped());

  LogCvmfs(kLogSpooler, kLogVerboseMsg, "generating file chunks for %s",
             mmf.file_path().c_str());

  ChunkGenerator chunk_generator(mmf);

  while (chunk_generator.HasMoreData()) {
    // find the next file chunk boundary
    Chunk chunk_boundary = chunk_generator.Next();
    TemporaryFileChunk file_chunk;
    file_chunk.offset = chunk_boundary.offset();
    file_chunk.size   = chunk_boundary.size();

    // do what you need to do with the data
    if (!ProcessFileChunk(mmf, file_chunk)) {
      return false;
    }

    // all done... save the chunk information
    data.file_chunks.push_back(file_chunk);
  }

  return true;
}


bool FileProcessor::GenerateBulkFile(const MemoryMappedFile &mmf,
                                     Results                &data) const {
  assert (mmf.IsMapped());

  LogCvmfs(kLogSpooler, kLogVerboseMsg, "generating bulk file for %s",
             mmf.file_path().c_str());

  // create a chunk that contains the whole file
  data.bulk_file.offset = 0;
  data.bulk_file.size   = mmf.size();

  // process the whole file in bulk
  return ProcessFileChunk(mmf, data.bulk_file);
}


bool FileProcessor::ProcessFileChunk(const MemoryMappedFile &mmf,
                                     TemporaryFileChunk     &chunk) const {
  // create a temporary to store the compression result of this chunk
  FILE *fcas = CreateTempFile(temporary_path_ + "/chunk", 0666, "w",
                              &chunk.temporary_path);
  if (fcas == NULL) {
    LogCvmfs(kLogSpooler, kLogStderr, "failed to create temporary file for "
                                      "chunk: %d %d of file %s",
             chunk.offset,
             chunk.size,
             mmf.file_path().c_str());
    return false;
  }

  // compress the chunk and compute the content hash simultaneously
  if (! zlib::CompressMem2File(mmf.buffer() + chunk.offset,
                              chunk.size,
                              fcas,
                              &chunk.content_hash)) {
    LogCvmfs(kLogSpooler, kLogStderr, "failed to compress chunk: %d %d of "
                                      "file %s",
             chunk.offset,
             chunk.size,
             mmf.file_path().c_str());
    return false;
  }

  // all done... thanks
  LogCvmfs(kLogSpooler, kLogVerboseMsg, "compressed chunk: %d %d | hash: %s",
           chunk.offset,
           chunk.size,
           chunk.content_hash.ToString().c_str());
  return true;
}
