// Minimal RFC 6455 server-side framing + Sec-WebSocket-Accept hash.

#include "websocket.h"

#include <cstring>
#include <string>

namespace napi_v8 {
namespace inspector {

// ---- SHA-1 (RFC 3174) -----------------------------------------------------

namespace {

struct Sha1 {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  uint64_t len_bits = 0;
  uint8_t  buf[64]  = {};
  size_t   buf_len  = 0;

  static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

  void process_block(const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(block[4*i])   << 24) |
             (uint32_t(block[4*i+1]) << 16) |
             (uint32_t(block[4*i+2]) <<  8) |
             (uint32_t(block[4*i+3]));
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20)      { f = (b & c) | ((~b) & d);    k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d;               k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else             { f = b ^ c ^ d;               k = 0xCA62C1D6u; }
      uint32_t tmp = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }

  void Update(const uint8_t* data, size_t len) {
    len_bits += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
      size_t take = std::min<size_t>(64 - buf_len, len);
      std::memcpy(buf + buf_len, data, take);
      buf_len += take;
      data    += take;
      len     -= take;
      if (buf_len == 64) {
        process_block(buf);
        buf_len = 0;
      }
    }
  }

  void Final(uint8_t out[20]) {
    buf[buf_len++] = 0x80;
    if (buf_len > 56) {
      while (buf_len < 64) buf[buf_len++] = 0;
      process_block(buf);
      buf_len = 0;
    }
    while (buf_len < 56) buf[buf_len++] = 0;
    for (int i = 7; i >= 0; --i) buf[buf_len++] = (len_bits >> (8 * i)) & 0xFF;
    process_block(buf);
    for (int i = 0; i < 5; ++i) {
      out[4*i]   = (h[i] >> 24) & 0xFF;
      out[4*i+1] = (h[i] >> 16) & 0xFF;
      out[4*i+2] = (h[i] >>  8) & 0xFF;
      out[4*i+3] =  h[i]        & 0xFF;
    }
  }
};

// ---- Base64 ---------------------------------------------------------------

std::string Base64(const uint8_t* data, size_t len) {
  static const char* T =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = uint32_t(data[i]) << 16;
    if (i + 1 < len) n |= uint32_t(data[i+1]) << 8;
    if (i + 2 < len) n |= uint32_t(data[i+2]);
    out.push_back(T[(n >> 18) & 0x3F]);
    out.push_back(T[(n >> 12) & 0x3F]);
    out.push_back(i + 1 < len ? T[(n >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < len ? T[ n       & 0x3F] : '=');
  }
  return out;
}

}  // namespace

std::string AcceptKey(const std::string& client_key) {
  static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::string s = client_key + kGuid;
  Sha1 sha;
  sha.Update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  uint8_t digest[20];
  sha.Final(digest);
  return Base64(digest, 20);
}

// ---- Frame decode ---------------------------------------------------------

void WsDecoder::Feed(const char* data, size_t len) {
  buf_.insert(buf_.end(), data, data + len);
}

int WsDecoder::TryDecode(WsFrame* out) {
  if (buf_.size() < 2) return 0;
  size_t i = 0;
  uint8_t b1 = static_cast<uint8_t>(buf_[i++]);
  uint8_t b2 = static_cast<uint8_t>(buf_[i++]);
  bool fin = (b1 & 0x80) != 0;
  uint8_t opcode = b1 & 0x0F;
  bool masked = (b2 & 0x80) != 0;
  uint64_t plen = b2 & 0x7F;

  if (plen == 126) {
    if (buf_.size() < i + 2) return 0;
    plen = (uint64_t(uint8_t(buf_[i])) << 8) | uint8_t(buf_[i+1]);
    i += 2;
  } else if (plen == 127) {
    if (buf_.size() < i + 8) return 0;
    plen = 0;
    for (int k = 0; k < 8; ++k) plen = (plen << 8) | uint8_t(buf_[i+k]);
    i += 8;
  }
  uint8_t mask[4] = {0,0,0,0};
  if (masked) {
    if (buf_.size() < i + 4) return 0;
    for (int k = 0; k < 4; ++k) mask[k] = uint8_t(buf_[i+k]);
    i += 4;
  }
  if (buf_.size() < i + plen) return 0;

  out->fin = fin;
  out->op = static_cast<WsOp>(opcode);
  out->payload.resize(plen);
  for (uint64_t k = 0; k < plen; ++k) {
    uint8_t c = uint8_t(buf_[i + k]);
    if (masked) c ^= mask[k & 3];
    out->payload[k] = static_cast<char>(c);
  }
  buf_.erase(buf_.begin(), buf_.begin() + i + plen);
  return 1;
}

// ---- Frame encode (server -> client, no mask) -----------------------------

void EncodeFrame(const WsFrame& f, std::string* out) {
  uint8_t b1 = (f.fin ? 0x80 : 0x00) | static_cast<uint8_t>(f.op);
  out->push_back(static_cast<char>(b1));
  size_t n = f.payload.size();
  if (n < 126) {
    out->push_back(static_cast<char>(n));
  } else if (n <= 0xFFFF) {
    out->push_back(static_cast<char>(126));
    out->push_back(static_cast<char>((n >> 8) & 0xFF));
    out->push_back(static_cast<char>( n       & 0xFF));
  } else {
    out->push_back(static_cast<char>(127));
    for (int k = 7; k >= 0; --k)
      out->push_back(static_cast<char>((n >> (8 * k)) & 0xFF));
  }
  out->append(f.payload);
}

}  // namespace inspector
}  // namespace napi_v8
