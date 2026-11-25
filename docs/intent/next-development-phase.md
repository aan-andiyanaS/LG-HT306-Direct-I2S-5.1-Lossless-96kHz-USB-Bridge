# Intent Pengembangan Berikutnya — LG HT306 USB-to-I2S Bridge

**Tanggal Konfirmasi:** 2026-11-19  
**Branch Aktif:** `feat/bulk-audio-bridge`  
**Status Prasyarat:** Firmware `ht306_rp2350_zero.uf2` sudah di-*flash* ke RP2350, aplikasi `ht306_desktop_app.exe` terhubung ke USB, indikator LED/log berjalan normal.

---

## Statement of Intent

- **Outcome:** Membuktikan jalur audio 5.1 PC $\rightarrow$ RP2350 $\rightarrow$ `Pulsus PS9850` bekerja di unit HT306 fisik terlebih dahulu, lalu dilanjutkan ke tahap pengamanan hardware/firmware, penyederhanaan UI/UX halaman setelah Dashboard, dan perbaikan bug yang muncul.
- **User:** Pengguna sekaligus pengembang utama sistem Home Theater LG HT306 di PC.
- **Why now:** Firmware RP2350 dan aplikasi PC sudah ter-*flash* dan lolos uji koneksi lokal, sehingga langkah berikutnya adalah verifikasi bertahap di hardware HT306 nyata (dimulai dari memutus 5 resistor jalur I2S).
- **Success:**
  1. HT306 tetap *boot* normal setelah 5 jalur I2S (`BCK`, `LRCK`, `ADATA0`, `ADATA1`, `ADATA2`) diputus, dan mengeluarkan suara 5.1 jernih saat disambung ke RP2350 melalui resistor seri `33 Ω`.
  2. Sistem memiliki mekanisme pengamanan (*power sequencing* / proteksi bus) yang aman untuk pemakaian harian.
  3. Halaman setelah Dashboard di `pc_stream_app` menjadi lebih ringkas/minimalis dan bebas bug kritis.
- **Constraint:** Harus berjalan bertahap—tidak mengubah UI atau menambah fitur baru sebelum hipotesis audio di HT306 fisik terbukti bekerja, serta menjaga keselamatan hardware HT306 selama uji meja kerja.
- **Out of scope:** Penambahan fitur baru di luar kestabilan audio, pengamanan, penyederhanaan halaman setelah Dashboard, dan perbaikan bug; serta perombakan besar pada tampilan Dashboard utama yang sudah berjalan.

---

## Urutan Gerbang (*Gate*) Pengembangan

1. **Fase 1 — Validasi Hipotesis Hardware (Meja Kerja / *Bench Test*):**
   - Angkat 5 resistor SMD jalur I2S (`R648`, `R647`, `R631`, `R632`, `R633`) dan pastikan `I2C (SDA/SCL)`, `RESET`, serta `MCLK` tetap utuh.
   - Uji nyala HT306 terlebih dahulu (*sanity check* display, mode `AUX`, dan relay normal tanpa *protect*).
   - Sambungkan `GP2..GP6` RP2350 ke `Pad B` (`IC602`) melalui resistor `33 Ω` + `DGND`, ikuti urutan Power ON/OFF manual, dan verifikasi ke-6 kanal audio 5.1 berbunyi.
2. **Fase 2 — Pengamanan Sistem:**
   - Terapkan proteksi permanen (`SN74LVC244A` dan/atau sensor firmware `GP10 VCC33 Sense`) setelah Fase 1 terbukti berhasil.
3. **Fase 3 — Pembenahan UI/UX & Perbaikan Bug:**
   - Sederhanakan tampilan halaman setelah Dashboard (tab Settings / Routing / Diagnostics) agar lebih ringkas dan minimalis.
   - Perbaiki bug yang ditemukan selama pengujian hardware maupun pengembangan lanjutan.
