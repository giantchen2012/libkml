// Copyright 2009, Google Inc. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//  1. Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//  2. Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//  3. Neither the name of Google Inc. nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
// EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// This file contains the implementation of the ZipFile class.

#include "kml/base/zip_file.h"
#include "kml/base/file.h"
#include "minizip/unzip.h"
#include "minizip/zip.h"

namespace kmlbase {

// This class hides the use of minizip from the interface.
class MinizipFile {
 public:
  MinizipFile(zipFile zipfile) : zipfile_(zipfile) {}
  ~MinizipFile() {
    if (zipfile_) {
      zipClose(zipfile_, 0);
    }
  }
  zipFile get_zipfile() { return zipfile_; }
 private:
  zipFile zipfile_;
  LIBKML_DISALLOW_EVIL_CONSTRUCTORS(MinizipFile);
};

// Static.
ZipFile* ZipFile::OpenFromString(const std::string& zip_data) {
  return IsZipData(zip_data) ? new ZipFile(zip_data) : NULL;
}

// Static.
ZipFile* ZipFile::OpenFromFile(const char* file_path) {
  if (!File::Exists(file_path)) {
    return NULL;
  }
  std::string data;
  if (!File::ReadFileToString(file_path, &data)) {
    return NULL;
  }
  return OpenFromString(data);
}

// Static.
ZipFile* ZipFile::Create(const char* file_path) {
  zipFile zipfile = zipOpen(file_path, 0);
  if (!zipfile) {
    return NULL;
  }
  MinizipFile* minizipfile = new MinizipFile(zipfile);
  if (!minizipfile) {
    return NULL;
  }
  return new ZipFile(minizipfile);
}

// Private. Class constructed with static methods.
ZipFile::ZipFile(const std::string& data)
  : minizip_file_(NULL), data_(data) {
  // Fill the table of contents for this zipfile.
  zlib_filefunc_def api;
  if (voidpf mem_stream = mem_simple_create_file(
      &api, const_cast<void*>(static_cast<const void*>(data.data())),
      data.size())) {
    unzFile zfile = unzAttach(mem_stream, &api);
    if (zfile) {
      unz_file_info finfo;
      do {
        static char buf[1024];
        if (unzGetCurrentFileInfo(zfile, &finfo, buf, sizeof(buf),
              0, 0, 0, 0) == UNZ_OK) {
          zipfile_toc_.push_back(buf);
        }
      } while (unzGoToNextFile(zfile) == UNZ_OK);
      unzClose(zfile);
    }
  }
}

// Private. Class constructed with static methods.
ZipFile::ZipFile(MinizipFile* minizip_file) : minizip_file_(minizip_file) {}

ZipFile::~ZipFile() {
  // Scoped ptr takes care of minizip_file_.
}

// Static.
bool ZipFile::IsZipData(const std::string& zip_data) {
  return zip_data.substr(0, 4) == "PK\003\004" ? true : false;
}

bool ZipFile::FindFirstOf(const std::string& file_extension,
                          std::string* path_in_zip) const {
  if (!path_in_zip) {
    return false;
  }
  kmlbase::StringVector::const_iterator itr = zipfile_toc_.begin();
  for(; itr != zipfile_toc_.end(); ++itr) {
    if (kmlbase::StringEndsWith(*itr, file_extension)) {
      *path_in_zip = *itr;
      return true;
    }
  }
  return false;
}

bool ZipFile::GetToc(kmlbase::StringVector* subfiles) const {
  if (!subfiles) {
    return false;
  }
  *subfiles = zipfile_toc_;
  return true;
}

// Is the requested path in the Zip file's table of contents?
bool ZipFile::IsInToc(const std::string& path_in_zip) const {
  kmlbase::StringVector::const_iterator itr = zipfile_toc_.begin();
  for(; itr != zipfile_toc_.end(); ++itr) {
    if (*itr == path_in_zip) {
      return true;
    }
  }
  return false;
}

// This helper class owns the closing of the unzFile handle used in the
// GetEntry method.
class UnzFileHelper {
 public:
  UnzFileHelper(unzFile unzfile) : unzfile_(unzfile) {}
  ~UnzFileHelper() { unzClose(unzfile_); }
  unzFile get_unzfile() { return unzfile_; }
 private:
  unzFile unzfile_;
};

bool ZipFile::GetEntry(const std::string& path_in_zip,
                       std::string* output) const {
  // Check the TOC first.
  if (!IsInToc(path_in_zip)) {
    return false;
  }
  // We permit output to be NULL.
  if (!output) {
    return true;
  }
  zlib_filefunc_def api;
  voidpf mem_stream = mem_simple_create_file(
      &api, const_cast<void*>(static_cast<const void*>(data_.data())),
      data_.size());
  if (!mem_stream) {
    return false;
  }
  unzFile unzfile = unzAttach(mem_stream, &api);
  if (!unzfile) {
    return false;
  }

  boost::scoped_ptr<UnzFileHelper> unzfilehelper(new UnzFileHelper(unzfile));
  unz_file_info finfo;
  if (unzLocateFile(unzfilehelper->get_unzfile(),
                    path_in_zip.c_str(), 0) != UNZ_OK ||
      unzOpenCurrentFile(unzfilehelper->get_unzfile()) != UNZ_OK ||
      unzGetCurrentFileInfo(unzfilehelper->get_unzfile(), &finfo, 0, 0, 0, 0,
                            0, 0) != UNZ_OK) {
    return false;
  }
  int nbytes = finfo.uncompressed_size;
  char* filedata = new char[nbytes];
  if (unzReadCurrentFile(unzfilehelper->get_unzfile(), filedata,
                         nbytes) == nbytes) {
    output->assign(filedata, nbytes);
    delete [] filedata;
    return true;
  }
  delete [] filedata;
  return false;
}

bool ZipFile::AddEntry(const std::string& data,
                       const std::string& path_in_zip) {
  // The path must be relative to and below the archive.
  if (path_in_zip.substr(0, 1).find_first_of("/\\") != std::string::npos ||
      path_in_zip.substr(0, 2) == "..") {
    return false;
  }
  if (!minizip_file_) {
    return false;
  }
  zipFile zipfile = minizip_file_->get_zipfile();
  if (!zipfile) {
    return false;
  }
  zipOpenNewFileInZip(zipfile, path_in_zip.c_str(), 0, 0, 0, 0, 0, 0,
                      Z_DEFLATED, Z_DEFAULT_COMPRESSION);
  zipWriteInFileInZip(zipfile, static_cast<const void*>(data.data()),
                      static_cast<unsigned int>(data.size()));
  return zipCloseFileInZip(zipfile) == ZIP_OK;
}

}  // end namespace kmlbase
