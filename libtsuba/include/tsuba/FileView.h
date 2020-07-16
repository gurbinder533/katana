#ifndef GALOIS_LIBTSUBA_TSUBA_FILE_VIEW_H_
#define GALOIS_LIBTSUBA_TSUBA_FILE_VIEW_H_

#include <string>
#include <cstdint>

#include <parquet/arrow/reader.h>

#include "galois/Result.h"
#include "galois/Logging.h"

namespace tsuba {

class FileView : public arrow::io::RandomAccessFile {
  uint8_t* map_start_;
  uint8_t* region_start_;
  uint64_t file_offset_;
  uint64_t map_size_;
  int64_t region_size_;
  int64_t cursor_;
  bool valid_ = false;

public:
  FileView()                = default;
  FileView(const FileView&) = delete;
  FileView& operator=(const FileView&) = delete;

  FileView(FileView&& other) noexcept
      : map_start_(other.map_start_), region_start_(other.region_start_),
        map_size_(other.map_size_), region_size_(other.region_size_),
        cursor_(other.cursor_), valid_(other.valid_) {
    other.valid_ = false;
  }

  FileView& operator=(FileView&& other) noexcept {
    if (&other != this) {
      if (auto res = Unbind(); !res) {
        GALOIS_LOG_ERROR("Unbind: {}", res.error());
      }
      map_start_    = other.map_start_;
      region_start_ = other.region_start_;
      file_offset_  = other.file_offset_;
      map_size_     = other.map_size_;
      region_size_  = other.region_size_;
      cursor_       = other.cursor_;
      valid_        = other.valid_;
      other.valid_  = false;
    }
    return *this;
  }

  ~FileView();

  bool Equals(const FileView& other) const;

  galois::Result<void> Bind(const std::string& filename, uint64_t begin,
                            uint64_t end);
  galois::Result<void> Bind(const std::string& filename, uint64_t stop) {
    return Bind(filename, 0, stop);
  }
  galois::Result<void> Bind(const std::string& filename);

  bool Valid() { return valid_; }

  galois::Result<void> Unbind();

  template <typename T>
  const T* ptr(uint64_t off) const {
    return reinterpret_cast<T*>(region_start_ + off); // NOLINT
  }
  template <typename T>
  const T* ptr() const {
    return ptr<T>(0);
  }

  uint64_t size() const { return region_size_; }
  uint64_t file_offset() const { return file_offset_; }

  // support iterating through characters
  const char* begin() { return ptr<char>(); }
  const char* end() { return ptr<char>() + size(); }

  ///// Begin arrow::io::RandomAccessFile methods ///////

  virtual arrow::Status Close();
  virtual arrow::Result<int64_t> Tell() const;
  virtual bool closed() const;
  virtual arrow::Status Seek(int64_t);
  virtual arrow::Result<int64_t> Read(int64_t, void*);
  virtual arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t);
  virtual arrow::Result<int64_t> GetSize();

  ///// End arrow::io::RandomAccessFile methods ///////
};

} // namespace tsuba

#endif
