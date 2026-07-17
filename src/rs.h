// Reed–Solomon over GF(256), systematic, primitive polynomial 0x11D.
//
// Why this exists: measured end-to-end success was 18/24 = 75%, and identical at transmit
// gains 0.14 / 0.20 / 0.28 — so the loss is NOT link budget, it is symbol errors. With no
// FEC a single bad symbol fails the CRC and drops the whole message. Working backwards from
// 75% over ~46 symbols gives a per-symbol error rate near 0.8%, i.e. roughly half a byte
// error per frame. RS_PARITY = 8 corrects up to 4 byte errors, which is ~8x that mean.
//
// RS operates on bytes while the modem operates on 3-bit symbols, so one bad symbol can
// straddle two bytes. That is fine — RS corrects byte errors regardless of what caused them
// — but it is why the parity budget is set against BYTE errors, not symbol errors.
#pragma once
#include <cstdint>
#include <cstddef>

namespace uchat {

constexpr int RS_PARITY = 8;   // corrects up to RS_PARITY/2 = 4 byte errors anywhere

// Append RS_PARITY parity bytes for `data[0..k)` into `parity[0..RS_PARITY)`.
void rs_encode(const uint8_t* data, int k, uint8_t* parity);

// Correct `buf[0..n)` in place, where buf is data followed by RS_PARITY parity bytes.
// Returns false if the codeword has more errors than RS can correct — in which case buf
// may be left partially modified, so the CRC downstream is still the final authority.
bool rs_decode(uint8_t* buf, int n);

} // namespace uchat
