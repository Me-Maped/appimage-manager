#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace aimgr {

/// Streaming SHA-256 (public-domain style implementation, no OpenSSL needed).
class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t len);
  std::string hex_final();
  static std::string hex_of(const void* data, size_t len);

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint64_t bits_ = 0;
  uint8_t buf_[64];
  size_t buf_len_ = 0;
};

/// Hashes a file. Returns lowercase hex, or empty string on I/O failure.
std::string sha256_file(const std::filesystem::path& path, std::string* err = nullptr);

}  // namespace aimgr
