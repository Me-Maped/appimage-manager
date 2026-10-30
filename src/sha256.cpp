#include "aimgr/sha256.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace aimgr {
namespace {
constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

Sha256::Sha256() {
  h_[0] = 0x6a09e667;
  h_[1] = 0xbb67ae85;
  h_[2] = 0x3c6ef372;
  h_[3] = 0xa54ff53a;
  h_[4] = 0x510e527f;
  h_[5] = 0x9b05688c;
  h_[6] = 0x1f83d9ab;
  h_[7] = 0x5be0cd19;
}

void Sha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (size_t i = 0; i < 16; ++i)
    w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
           (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
  uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
  h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  bits_ += static_cast<uint64_t>(len) * 8;
  if (buf_len_) {
    size_t take = std::min(len, size_t(64) - buf_len_);
    std::memcpy(buf_ + buf_len_, p, take);
    buf_len_ += take;
    p += take;
    len -= take;
    if (buf_len_ == 64) {
      block(buf_);
      buf_len_ = 0;
    }
  }
  while (len >= 64) {
    block(p);
    p += 64;
    len -= 64;
  }
  if (len) {
    std::memcpy(buf_, p, len);
    buf_len_ = len;
  }
}

std::string Sha256::hex_final() {
  uint64_t total_bits = bits_;
  buf_[buf_len_++] = 0x80;
  if (buf_len_ > 56) {
    std::memset(buf_ + buf_len_, 0, 64 - buf_len_);
    block(buf_);
    buf_len_ = 0;
  }
  std::memset(buf_ + buf_len_, 0, 56 - buf_len_);
  for (int i = 0; i < 8; ++i) buf_[56 + i] = uint8_t((total_bits >> (56 - i * 8)) & 0xff);
  block(buf_);
  static const char* hexd = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (int i = 0; i < 8; ++i)
    for (int b = 3; b >= 0; --b) {
      uint8_t byte = uint8_t((h_[i] >> (b * 8)) & 0xff);
      out += hexd[byte >> 4];
      out += hexd[byte & 0xf];
    }
  return out;
}

std::string Sha256::hex_of(const void* data, size_t len) {
  Sha256 s;
  s.update(data, len);
  return s.hex_final();
}

std::string sha256_file(const std::filesystem::path& path, std::string* err) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (err) *err = "cannot open " + path.string() + ": " + std::strerror(errno);
    return "";
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    const int saved = errno;
    ::close(fd);
    if (err) *err = path.string() + " is not a regular file" +
                    (saved ? std::string(": ") + std::strerror(saved) : "");
    return "";
  }
  Sha256 s;
  std::vector<char> buf(size_t{256} * 1024);
  for (;;) {
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      const int saved = errno;
      ::close(fd);
      if (err) *err = "read error while hashing " + path.string() + ": " + std::strerror(saved);
      return "";
    }
    s.update(buf.data(), static_cast<size_t>(n));
  }
  ::close(fd);
  return s.hex_final();
}

}  // namespace aimgr
