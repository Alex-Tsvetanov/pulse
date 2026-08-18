#include "pulse/websocket.hpp"

#include <cstring>

namespace pulse::ws {
namespace {

std::uint32_t rotate_left(std::uint32_t value, int bits) {
  return (value << bits) | (value >> (32 - bits));
}

}  // namespace

// SHA-1, FIPS 180-4. Present only because RFC 6455 requires it for the handshake, and
// pulling in a crypto library for one hash would make the project depend on a package
// manager. It is not used for anything security bearing.
std::string sha1(std::string_view data) {
  std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

  std::string message(data);
  const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8;
  message.push_back(static_cast<char>(0x80));
  while (message.size() % 64 != 56) message.push_back('\0');
  for (int i = 7; i >= 0; --i) {
    message.push_back(static_cast<char>((bit_length >> (i * 8)) & 0xFF));
  }

  for (std::size_t offset = 0; offset < message.size(); offset += 64) {
    std::uint32_t w[80] = {};
    for (int i = 0; i < 16; ++i) {
      const unsigned char* p =
          reinterpret_cast<const unsigned char*>(message.data() + offset + i * 4);
      w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
             (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rotate_left(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f = 0;
      std::uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const std::uint32_t temp = rotate_left(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotate_left(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  std::string digest;
  digest.reserve(20);
  for (std::uint32_t word : h) {
    for (int i = 3; i >= 0; --i) {
      digest.push_back(static_cast<char>((word >> (i * 8)) & 0xFF));
    }
  }
  return digest;
}

std::string base64_encode(std::string_view data) {
  static const char* const alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  std::size_t i = 0;
  while (i + 2 < data.size()) {
    const auto b0 = static_cast<unsigned char>(data[i]);
    const auto b1 = static_cast<unsigned char>(data[i + 1]);
    const auto b2 = static_cast<unsigned char>(data[i + 2]);
    out.push_back(alphabet[b0 >> 2]);
    out.push_back(alphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
    out.push_back(alphabet[((b1 & 0x0F) << 2) | (b2 >> 6)]);
    out.push_back(alphabet[b2 & 0x3F]);
    i += 3;
  }
  const std::size_t remaining = data.size() - i;
  if (remaining == 1) {
    const auto b0 = static_cast<unsigned char>(data[i]);
    out.push_back(alphabet[b0 >> 2]);
    out.push_back(alphabet[(b0 & 0x03) << 4]);
    out += "==";
  } else if (remaining == 2) {
    const auto b0 = static_cast<unsigned char>(data[i]);
    const auto b1 = static_cast<unsigned char>(data[i + 1]);
    out.push_back(alphabet[b0 >> 2]);
    out.push_back(alphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
    out.push_back(alphabet[(b1 & 0x0F) << 2]);
    out.push_back('=');
  }
  return out;
}

std::string accept_key(std::string_view client_key) {
  std::string combined(client_key);
  combined.append(kGuid);
  return base64_encode(sha1(combined));
}

bool is_control(Opcode opcode) { return (static_cast<std::uint8_t>(opcode) & 0x08) != 0; }

Decode decode_frame(std::string_view buffer, Frame& out, std::size_t& consumed,
                    std::size_t max_payload) {
  if (buffer.size() < 2) return Decode::Incomplete;
  const auto* bytes = reinterpret_cast<const unsigned char*>(buffer.data());

  const bool fin = (bytes[0] & 0x80) != 0;
  const unsigned reserved = bytes[0] & 0x70;
  const auto opcode = static_cast<Opcode>(bytes[0] & 0x0F);
  const bool masked = (bytes[1] & 0x80) != 0;
  std::uint64_t length = bytes[1] & 0x7F;

  // No extension was negotiated, so a reserved bit set means the peer is speaking a
  // protocol this server did not agree to.
  if (reserved != 0) return Decode::Error;

  switch (opcode) {
    case Opcode::Continuation:
    case Opcode::Text:
    case Opcode::Binary:
    case Opcode::Close:
    case Opcode::Ping:
    case Opcode::Pong:
      break;
    default:
      return Decode::Error;
  }
  // Control frames carry at most 125 bytes and are never fragmented, section 5.5.
  if (is_control(opcode) && (length > 125 || !fin)) return Decode::Error;

  std::size_t cursor = 2;
  if (length == 126) {
    if (buffer.size() < cursor + 2) return Decode::Incomplete;
    length = (static_cast<std::uint64_t>(bytes[2]) << 8) | bytes[3];
    cursor += 2;
    if (length < 126) return Decode::Error;  // non-minimal length encoding
  } else if (length == 127) {
    if (buffer.size() < cursor + 8) return Decode::Incomplete;
    length = 0;
    for (int i = 0; i < 8; ++i) {
      length = (length << 8) | bytes[2 + i];
    }
    cursor += 8;
    if (length <= 0xFFFF) return Decode::Error;
    if ((length >> 63) != 0) return Decode::Error;  // the high bit must be zero
  }

  // The declared length comes from the network. Refusing it before reserving memory is
  // the difference between a rejected frame and an allocation the peer chose.
  if (length > max_payload) return Decode::Error;

  std::array<std::uint8_t, 4> mask{};
  if (masked) {
    if (buffer.size() < cursor + 4) return Decode::Incomplete;
    for (int i = 0; i < 4; ++i) mask[i] = bytes[cursor + i];
    cursor += 4;
  }

  const auto payload_size = static_cast<std::size_t>(length);
  if (buffer.size() < cursor + payload_size) return Decode::Incomplete;

  out.fin = fin;
  out.opcode = opcode;
  out.masked = masked;
  out.payload.assign(buffer.substr(cursor, payload_size));
  if (masked) {
    for (std::size_t i = 0; i < payload_size; ++i) {
      out.payload[i] = static_cast<char>(static_cast<unsigned char>(out.payload[i]) ^ mask[i % 4]);
    }
  }
  consumed = cursor + payload_size;
  return Decode::Ok;
}

std::string encode_frame(Opcode opcode, std::string_view payload, bool fin,
                         const std::array<std::uint8_t, 4>* mask) {
  std::string out;
  out.reserve(payload.size() + 14);
  out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(opcode)));

  const std::uint8_t mask_bit = mask != nullptr ? 0x80 : 0x00;
  const std::size_t size = payload.size();
  if (size < 126) {
    out.push_back(static_cast<char>(mask_bit | static_cast<std::uint8_t>(size)));
  } else if (size <= 0xFFFF) {
    out.push_back(static_cast<char>(mask_bit | 126));
    out.push_back(static_cast<char>((size >> 8) & 0xFF));
    out.push_back(static_cast<char>(size & 0xFF));
  } else {
    out.push_back(static_cast<char>(mask_bit | 127));
    for (int i = 7; i >= 0; --i) {
      out.push_back(static_cast<char>((static_cast<std::uint64_t>(size) >> (i * 8)) & 0xFF));
    }
  }

  if (mask != nullptr) {
    for (std::uint8_t byte : *mask) out.push_back(static_cast<char>(byte));
    const std::size_t start = out.size();
    out.append(payload);
    for (std::size_t i = 0; i < size; ++i) {
      out[start + i] =
          static_cast<char>(static_cast<unsigned char>(out[start + i]) ^ (*mask)[i % 4]);
    }
  } else {
    out.append(payload);
  }
  return out;
}

std::string encode_close(std::uint16_t code, std::string_view reason) {
  std::string payload;
  payload.push_back(static_cast<char>((code >> 8) & 0xFF));
  payload.push_back(static_cast<char>(code & 0xFF));
  payload.append(reason);
  return encode_frame(Opcode::Close, payload);
}

Assembler::Event Assembler::feed(const Frame& frame) {
  if (is_control(frame.opcode)) {
    opcode_ = frame.opcode;
    payload_ = frame.payload;
    return Event::Control;
  }

  if (frame.opcode == Opcode::Continuation) {
    if (!in_message_) return Event::Protocol;  // continuation without a start frame
  } else {
    if (in_message_) return Event::Protocol;  // a new data frame inside a message
    in_message_ = true;
    pending_.clear();
    pending_opcode_ = frame.opcode;
  }

  if (pending_.size() + frame.payload.size() > max_message_) {
    in_message_ = false;
    pending_.clear();
    return Event::Protocol;
  }
  pending_ += frame.payload;

  if (!frame.fin) return Event::NeedMore;

  payload_.swap(pending_);
  pending_.clear();
  opcode_ = pending_opcode_;
  in_message_ = false;
  return Event::Message;
}

}  // namespace pulse::ws
