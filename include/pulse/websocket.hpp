// WebSocket, RFC 6455. Handshake key derivation, frame decoding and frame encoding,
// plus the assembler that turns a run of fragments into one application message.
//
// Everything here works on byte buffers, never on sockets, so the whole protocol layer
// is exercised by the unit tests without opening a connection.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::ws {

// The magic value from RFC 6455 section 1.3.
inline constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string sha1(std::string_view data);          // 20 raw bytes
std::string base64_encode(std::string_view data);

// Sec-WebSocket-Accept for a given Sec-WebSocket-Key.
std::string accept_key(std::string_view client_key);

enum class Opcode : std::uint8_t {
  Continuation = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

bool is_control(Opcode opcode);

struct Frame {
  bool fin = true;
  Opcode opcode = Opcode::Text;
  bool masked = false;
  std::string payload;  // already unmasked when decoded
};

enum class Decode {
  Incomplete,
  Ok,
  Error,
};

// Decodes one frame from the front of buffer. max_payload bounds the allocation, since
// the declared length arrives from the network and cannot be trusted.
Decode decode_frame(std::string_view buffer, Frame& out, std::size_t& consumed,
                    std::size_t max_payload = 1024 * 1024);

// Encodes one frame. A server frame is never masked, a client frame always is, so the
// mask argument is what distinguishes the two directions.
std::string encode_frame(Opcode opcode, std::string_view payload, bool fin = true,
                         const std::array<std::uint8_t, 4>* mask = nullptr);

std::string encode_close(std::uint16_t code, std::string_view reason = {});

// Reassembles fragmented messages. Control frames may be interleaved between the
// fragments of a data message, so they are reported separately and do not disturb the
// message being accumulated.
class Assembler {
 public:
  enum class Event {
    NeedMore,   // no complete unit available yet
    Message,    // a full data message is ready in payload()
    Control,    // a control frame arrived, see opcode() and payload()
    Protocol,   // the peer broke the protocol, the connection must be closed
  };

  // Feeds the frame and reports what became available.
  Event feed(const Frame& frame);

  Opcode opcode() const { return opcode_; }
  const std::string& payload() const { return payload_; }
  std::size_t max_message() const { return max_message_; }
  void set_max_message(std::size_t bytes) { max_message_ = bytes; }

 private:
  std::string payload_;
  std::string pending_;
  Opcode opcode_ = Opcode::Text;
  Opcode pending_opcode_ = Opcode::Text;
  bool in_message_ = false;
  std::size_t max_message_ = 1024 * 1024;
};

}  // namespace pulse::ws
