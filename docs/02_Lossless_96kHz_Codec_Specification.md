# 2. Mathematical Specification & Protocol of the Real-Time Lossless Codec (`96 kHz / 24-bit 5.1`)

🌐 **Language:** **English** | [Bahasa Indonesia](./02_Lossless_96kHz_Codec_Specification.id.md)

[⬅️ Back to Main README](../README.md)

This document provides the mathematical foundation and packet layout defined in [`firmware/include/lossless_codec.h`](../firmware/include/lossless_codec.h), [`firmware/src/lossless_codec.c`](../firmware/src/lossless_codec.c), and [`firmware/src/usb_descriptors.c`](../firmware/src/usb_descriptors.c).

---

## 2.1. Why Real-Time Lossless Compression is Required

The native USB hardware on the `RP2350` / `RP2040` family is compliant with **USB 1.1 Full-Speed (`12 Mbps`)**, giving a practical throughput ceiling of roughly **`1,000 KB/s`**:
* **`48 kHz / 24-bit 5.1` (Raw):** `48,000 × 6 × 3 Bytes = 864 KB/s` *(Fits comfortably inside USB 1.1 bandwidth)*.
* **`96 kHz / 24-bit 5.1` (Raw):** `96,000 × 6 × 3 Bytes = 1,728 KB/s` *(Exceeds USB 1.1 bandwidth by 72%)*.

Using our **Adaptive Linear Predictor (`Order-4 / 1 / 2 / 0`) + Wasted-Bits Removal + Escaped Golomb-Rice Codec**, the uncompressed `1,728.0 KB/s` stream is reduced to an average of **`716.1 KB/s` (`41.4%`)** with **100% mathematical bit-exactness (`0 bit error`)** and an ultra-fast unpacking execution time of `< 0.03 ms` on Cortex-M33 `Core 1`. For sustained stability across host *USB 2.0 Transaction Translators* (TT), the PC streamer enforces a **Unified Safe USB 1.1 TT Bandwidth Guard** (`<= 680 KiB/s`: `<= 348 B/pkt @ 96 kHz`, `<= 696 B/pkt @ 48 kHz`) via controlled dynamic right-shifting.

---

## 2.2. Micro-Frame Architecture: `0.5 ms` (`48 Samples / Channel`)

Every USB packet packages exactly **48 time frames** (`0.5 milliseconds` at `96 kHz`, or `1.0 millisecond` at `48 kHz`), totaling `288 samples` (`6 channels × 48`).
* **Sample 0 (`Anchor Frame`):** Transmitted completely raw without compression (`6 channels × 3 bytes = 18 bytes` at Offsets `14..31`).
* This guarantees that every packet is **100% stateless across packets** — if a packet is lost due to USB cable interference, the very next packet resynchronizes instantaneously without audio glitches.

---

## 2.3. Per-Channel Adaptive Linear Predictor (`Order-4 / 1 / 2 / 0`) & Wasted-Bits

For each sample index $n \in \lbrace 1, \dots, 47 \rbrace$ across every channel $c \in \lbrace 0, \dots, 5 \rbrace$, the encoder evaluates 4 predictor candidates and selects the lowest bit-cost per channel (`pred_code` packed in bits `[6:5]` of `rice_k[c]`):
* `pred_code = 0` (**Order-4**, backward-compatible default):
  * Warm-up ($n = 1, 2, 3$): $x_c[0]$, $2x_c[1] - x_c[0]$, $3x_c[2] - 3x_c[1] + x_c[0]$
  * Steady-state ($n \ge 4$):

$$\hat{x}_c[n] = 4x_c[n-1] - 6x_c[n-2] + 4x_c[n-3] - x_c[n-4]$$

* `pred_code = 1` (**Order-1**): $\hat{x}_c[n] = x_c[n-1]$
* `pred_code = 2` (**Order-2**): $\hat{x}_c[n] = 2x_c[n-1] - x_c[n-2]$ ($x_c[0]$ at $n=1$)
* `pred_code = 3` (**Order-0**): $\hat{x}_c[n] = 0$ (optimal for dense high-frequency transients)

When all 24-bit samples in a subframe share 8 zero LSBs (*16-in-24* sources), the encoder automatically enables **`shift_code = 4`** (`shift_bits = 8`, 100% bit-exact), and uses compact 2-byte verbatim fallback (`<= 590 B`) when needed.

The residual difference is evaluated using 24-bit signed 2's complement arithmetic:

$$e_c[n] = \mathrm{sext}_{24}\left(x_c[n] - \hat{x}_c[n]\right)$$

---

## 2.4. ZigZag Mapping & Escaped Golomb-Rice Coding

1. **ZigZag Mapping:** Maps signed residuals $e_c[n] \in [-2^{23}, 2^{23}-1]$ to unsigned integers $z \in [0, 2^{24}-1]$ so values centered near zero remain small positive numbers:

   $$z = (e \ll 1) \oplus (e \gg 31)$$

2. **Optimal Parameter $k$ Estimation via Hardware `CLZ`:**
   The Golomb-Rice parameter $k_c \in \lbrace 0, \dots, 22 \rbrace$ is derived from the mean steady-state residual ($n \ge 3$):

   $$k_c = \min\left(22,\ 31 - \mathrm{CLZ}(\bar{z}_c)\right)$$

   For warm-up samples $n \in \lbrace 1, 2 \rbrace$, $k_{\mathrm{warmup}} = k_c + 6$ is applied to absorb transient polynomial step deltas without inflating unary code lengths.

3. **Escaped Golomb-Rice Bit Packing:**
   The unsigned residual $z$ is split into quotient $q = \lfloor z / 2^k \rfloor$ (`q = z >> k`) and remainder $r = z \bmod 2^k$ (`r = z & ((1 << k) - 1)`):
   * **Normal Encoding** ($q < 15$): Emit $q$ ones, followed by a terminating `0` stop bit, followed by the $k$-bit remainder $r$.
   * **Transient / Explosion Escape** ($q \ge 15$): Emit `15` ones followed by a `0` stop bit, followed by the full raw value of $z$ as **25 raw bits**. This escape mechanism guarantees that even full-scale white noise or extreme movie explosions will never trigger buffer overflow.

---

## 2.5. Packet Structure (`HT306_LosslessHeader_t` — 14-Byte Header)

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       sync_word (0xA55A)      |          packet_len           |  Offset 0..3
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   sequence    |     flags     |  num_frames   |   rice_k[0]   |  Offset 4..7
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   rice_k[1]   |   rice_k[2]   |   rice_k[3]   |   rice_k[4]   |  Offset 8..11
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   rice_k[5]   | payload_crc8  |                               |  Offset 12..13
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               +
|                  Anchor Frame 0 (6 Ch x 3B = 18B)             |  Offset 14..31
+                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                               |  Packed Golomb-Rice Bitstream |  Offset 32..N
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Byte Offset | Field Name | Type | Description |
| :---: | :--- | :---: | :--- |
| **`0..1`** | `sync_word` | `uint16_t` | Synchronization preamble (`0xA55A`, Little-Endian: `0x5A, 0xA5`). |
| **`2..3`** | `packet_len` | `uint16_t` | Total packet length in bytes (`header + payload`) for 64-byte USB Bulk stream reassembly. |
| **`4`** | `sequence` | `uint8_t` | Packet sequence index (`0..255`) for drop detection. |
| **`5`** | `flags` | `uint8_t` | Bit `0`: `HT306_FLAG_RATE_96KHZ` (`1` = `96 kHz`, `0` = `48 kHz`), Bit `1`: `HT306_FLAG_BITDEPTH_24` (default `24-bit` PC transport, max `812 B` / verbatim `878 B`), Bit `2`: `HT306_FLAG_CHANNELS_8` (rejected by decoder), Bit `3`: `HT306_FLAG_BITDEPTH_32`, Bit `4`: `HT306_FLAG_VERBATIM_RAW` (3B/2B raw fallback), Bit `5..7`: `HT306_FLAG_SHIFT_MASK` (`shift_bits = shift_code × 2`, 16-in-24 wasted-bits / dynamic shift). |
| **`6`** | `num_frames` | `uint8_t` | Number of time frames per channel in the packet (default: `48`). |
| **`7..12`** | `rice_k[6]` | `uint8_t[6]` | Bits `0..4`: Golomb-Rice $k \in \lbrace 0, \dots, 22 \rbrace$ (`30` for 32-bit); Bits `5..6`: `pred_code` (`0`=O4, `1`=O1, `2`=O2, `3`=O0); Bit `7`: must be `0`. |
| **`13`** | `payload_crc8` | `uint8_t` | `CRC-8` checksum (polynomial `0x07`, `256 B` LUT) calculated across **Header `[2..12]` + all `payload` bytes**. |
| **`14..31`** | `anchor[6]` | `18 Bytes` | Uncompressed initial sample (`Frame 0`) in 24-bit signed PCM for all 6 channels. |
| **`32..N`** | `bitstream` | Variable | Compressed Golomb-Rice residual stream for `Frames 1..47`. |
