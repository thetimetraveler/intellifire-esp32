// Host test for ecm_framing.h — the frame-delimiting layer.
//
// No board, no UART, no ESPHome: a synthetic clock drives the assembler with
// byte streams, including the mis-framing cases this layer exists to survive.
//
// Build:  clang++ -std=c++20 -Wall -Wextra -O1 -o /tmp/ecm_framing_test \
//                   test/ecm_framing_test.cpp && /tmp/ecm_framing_test

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ecm_framing.h"

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const char *label, const char *detail = "") {
  if (ok) {
    ++g_pass;
    std::printf("  PASS  %s\n", label);
  } else {
    ++g_fail;
    std::printf("  FAIL  %s%s%s\n", label, *detail ? "\n          " : "", detail);
  }
}

// A REAL frame taken from a capture: a 31-byte DC status frame, CRC-valid.
// Using recorded bytes rather than synthetic ones means the test cannot pass
// on a stream that would never occur on the wire.
static const uint8_t kDcFrame[] = {
    0x4B, 0x01, 0x5D, 0xDC, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xFF};
static const size_t kDcLen = sizeof(kDcFrame);

// A real 8-byte BC poll frame.
static const uint8_t kBcFrame[] = {0x4B, 0x02, 0x5E, 0xBC, 0x02, 0x04, 0xA4, 0xCF};
static const size_t kBcLen = sizeof(kBcFrame);

static void feed(ecm::FrameAssembler &a, const uint8_t *bytes, size_t n,
                 uint32_t start_ms, uint32_t per_byte_ms = 1) {
  for (size_t i = 0; i < n; ++i) {
    a.push(bytes[i], start_ms + static_cast<uint32_t>(i) * per_byte_ms);
  }
}

int main() {
  std::printf("\n=== ecm framing: host test ===\n");
  std::printf("    synthetic clock, no board, no hardware.\n\n");

  // -- 1. a clean frame is delivered only AFTER the gap --------------------
  std::printf("== 1. completion is by idle gap, not by byte count ==\n");
  {
    ecm::FrameAssembler a;
    feed(a, kDcFrame, kDcLen, 0);
    // feed() pushes byte i at t = i, so the LAST byte lands at t = kDcLen - 1.
    const uint32_t last = static_cast<uint32_t>(kDcLen - 1);
    check(!a.tick(last + 1), "1 ms after the last byte: not complete yet");
    check(!a.tick(last + ecm::FrameAssembler::GAP_MS - 1),
          "one millisecond short of the gap: still not complete");
    check(a.tick(last + ecm::FrameAssembler::GAP_MS),
          "exactly at the gap: the frame completes (boundary is inclusive)");
    check(a.length() == kDcLen, "and its length is right");
    check(std::memcmp(a.frame(), kDcFrame, kDcLen) == 0,
          "and its bytes are byte-identical to the capture");
    check(a.completed() == 1, "one frame completed");
    check(a.resyncs() == 0 && a.overflows() == 0, "with no resync and no overflow");

    // A completed frame is handed out ONCE. Re-delivering it on every loop
    // iteration re-counts it and re-answers the same poll ~10-20 times.
    check(!a.tick(last + ecm::FrameAssembler::GAP_MS + 16),
          "16 ms later: the frame is NOT delivered again");
    check(!a.tick(last + ecm::FrameAssembler::GAP_MS + 320),
          "320 ms later, still no re-delivery");
    check(a.completed() == 1, "the completed counter did not advance on either tick");

    // The next byte begins a new frame and re-arms delivery.
    a.push(kBcFrame[0], last + 400);
    check(!a.tick(last + 401), "a new frame still needs its own gap");
    check(a.tick(last + 400 + ecm::FrameAssembler::GAP_MS),
          "and completes once its gap elapses");
  }

  // -- 2. two frames back to back -----------------------------------------
  std::printf("\n== 2. consecutive frames ==\n");
  {
    ecm::FrameAssembler a;
    feed(a, kBcFrame, kBcLen, 0);                 // t = 0..7
    check(a.tick(7 + ecm::FrameAssembler::GAP_MS), "the first completes");
    check(a.length() == kBcLen, "first frame length");
    // The next exchange arrives ~158 ms later in reality; use 200 ms.
    feed(a, kDcFrame, kDcLen, 200);
    check(a.tick(200 + kDcLen + ecm::FrameAssembler::GAP_MS),
          "the second completes");
    check(a.length() == kDcLen, "second frame length");
    check(std::memcmp(a.frame(), kDcFrame, kDcLen) == 0, "second frame bytes");
    check(a.completed() == 2, "two frames completed");
  }

  // -- 3. a START byte INSIDE a payload is data, not a new frame ----------
  //     This is the case a "scan for 0x4B" parser gets wrong, and the reason
  //     the gap rule exists.
  std::printf("\n== 3. a 0x4B inside a payload is DATA, not a frame start ==\n");
  {
    ecm::FrameAssembler a;
    std::vector<uint8_t> f(kBcFrame, kBcFrame + kBcLen);
    f[5] = 0x4B;                                   // payload byte set to START
    feed(a, f.data(), f.size(), 0);
    check(a.tick(100), "the frame still completes as ONE frame");
    check(a.length() == kBcLen,
          "and it is the full length — the payload 0x4B did not start a new frame");
    check(a.frame()[5] == 0x4B, "with the payload byte preserved");
    check(a.completed() == 1 && a.resyncs() == 0, "exactly one frame, no resync");
  }

  // -- 4. idle noise before a frame ---------------------------------------
  std::printf("\n== 4. bytes before a start byte are discarded ==\n");
  {
    ecm::FrameAssembler a;
    const uint8_t noise[] = {0x00, 0xFF, 0x13, 0x80};
    feed(a, noise, sizeof(noise), 0);
    check(a.length() == 0, "noise alone assembles nothing");
    feed(a, kBcFrame, kBcLen, 10);
    check(a.tick(100), "the following real frame still completes");
    check(a.length() == kBcLen && std::memcmp(a.frame(), kBcFrame, kBcLen) == 0,
          "and it is correct");
    check(a.resyncs() == 4, "each noise byte counted as a resync");
  }

  // -- 5. a partial frame, then a gap -------------------------------------
  std::printf("\n== 5. a truncated frame completes but is SHORT ==\n");
  {
    ecm::FrameAssembler a;
    feed(a, kBcFrame, 4, 0);                       // only half the frame
    check(a.tick(100), "the partial frame still 'completes' on the gap");
    check(a.length() == 4,
          "and reports its true (short) length — rejecting it is the protocol "
          "core's job, not this layer's");
    check(a.completed() == 1, "counted as completed");
  }

  // -- 6. an unbroken garbage stream overflows and resyncs ----------------
  std::printf("\n== 6. a stream with no gap overflows rather than growing forever ==\n");
  {
    ecm::FrameAssembler a;
    uint8_t junk[200];
    std::memset(junk, 0xAA, sizeof(junk));
    junk[0] = ecm::FrameAssembler::START;          // so it starts assembling
    feed(a, junk, sizeof(junk), 0);
    check(a.overflows() == 1, "exactly one overflow was counted");
    check(a.length() <= ecm::FrameAssembler::MAX_LEN, "the buffer stayed bounded");
    // ...and a real frame immediately afterwards is still recovered.
    feed(a, kBcFrame, kBcLen, 300);
    check(a.tick(400), "a real frame after the garbage still completes");
    check(a.length() == kBcLen, "with the right length");
  }

  // -- 7. a byte arriving exactly at the gap boundary must NOT complete ----
  std::printf("\n== 7. a byte at the gap boundary keeps the frame open ==\n");
  {
    ecm::FrameAssembler a;
    a.push(ecm::FrameAssembler::START, 0);
    // NOTE: this documents real behaviour rather than desirable behaviour. The
    // assembler is a DELIMITER and cannot know a 1-byte frame is impossible, so
    // a lone start byte followed by silence IS delivered. The protocol core
    // rejects it (too short, then CRC) — that separation of concerns is
    // intended, and this check exists so it fails loudly if it ever changes.
    check(a.tick(ecm::FrameAssembler::GAP_MS),
          "a lone start byte followed by a gap IS delivered (the protocol core "
          "rejects it)");
    check(a.length() == 1, "and it is reported at its true 1-byte length");
  }
  {
    ecm::FrameAssembler a;
    feed(a, kBcFrame, kBcLen - 1, 0);              // all but the last byte
    uint32_t last = static_cast<uint32_t>(kBcLen - 1);   // t of the last push
    // A byte arriving exactly GAP_MS after the previous one must RESET the
    // timer, not complete the frame.
    a.push(kBcFrame[kBcLen - 1], last + ecm::FrameAssembler::GAP_MS);
    check(!a.tick(last + ecm::FrameAssembler::GAP_MS),
          "the late byte did not complete the frame");
    check(a.tick(last + 2 * ecm::FrameAssembler::GAP_MS),
          "but the frame completes a full gap after THAT byte");
    check(a.length() == kBcLen && std::memcmp(a.frame(), kBcFrame, kBcLen) == 0,
          "and it is the complete, correct frame");
  }

  // -- 8. tick() with nothing buffered is harmless ------------------------
  std::printf("\n== 8. ticking an empty assembler is harmless ==\n");
  {
    ecm::FrameAssembler a;
    bool any_completed = false;
    for (int i = 0; i < 50; ++i) {
      if (a.tick(static_cast<uint32_t>(i) * 100)) {
        any_completed = true;
      }
    }
    check(!any_completed, "50 idle ticks over 5 s completed nothing");
    check(a.completed() == 0, "and the completed counter is still zero");
  }

  std::printf("\n============================================================\n");
  if (g_fail) {
    std::printf("RESULT: %d FAILED, %d passed\n", g_fail, g_pass);
  } else {
    std::printf("RESULT: all %d checks passed\n", g_pass);
  }
  std::printf("============================================================\n");
  return g_fail ? 1 : 0;
}
