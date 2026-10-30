# UV-K5 Özel Firmware — Türkçe Kod Kılavuzu ve İnceleme Raporu

> Kaynak: [egzumer/uv-k5-firmware-custom](https://github.com/egzumer/uv-k5-firmware-custom), commit `7607f0a`
> Kapsam: mimari, çalışma zamanı modeli, sürücüler, kalıcı veri, seri protokol, derleme seçenekleri ve kod inceleme bulguları.
> Not: Bu rapor statik kod okumasına dayanır; firmware derlenip donanımda koşturulmamıştır.

---

## 1. Genel Bakış

Bu proje, Quansheng UV-K5/K6/5R el telsizinin orijinal v2.1.27 firmware'inin açık kaynak yeniden yazımıdır (DualTachyon). Üzerine OneOfEleven, fagci (spektrum analizörü) ve egzumer'in modları eklenmiştir.

| Özellik | Değer | Kaynak |
|---|---|---|
| MCU | DP32G030, ARM Cortex-M0, 48 MHz | `driver/systick.c` → `SysTick_Config(480000)` = 10 ms |
| Flash (kullanılabilir) | 60 KB | `firmware.ld` |
| RAM | 16 KB, yığın en az 0x80 bayt, heap 0 | `firmware.ld` |
| RF alıcı-verici | Beken BK4819 (yazılımsal 3 telli seri arayüz) | `driver/bk4819.c` |
| FM yayın alıcısı | Beken BK1080 | `driver/bk1080.c` |
| Ekran | ST7565, 128 px genişlik, SPI0 üzerinden | `driver/st7565.c` |
| EEPROM | 8 KB (adres < 0x2000), yazılımsal I²C, 8 baytlık bloklar | `driver/eeprom.c` |
| Kod büyüklüğü | ~30 bin satır C (external hariç) | |

Dinamik bellek (heap) yok; tüm durum global değişkenlerde ve derleme anında boyutlandırılmış dizilerde tutulur. Bu, 16 KB RAM'li bir Cortex-M0 için doğru tercih.

---

## 2. Dizin Yapısı

```
.
├── start.S, init.c        Vektör tablosu, reset, .data/.bss başlatma
├── main.c                 Main(): donanım init + sonsuz ana döngü
├── scheduler.c            SysTick kesmesi (10 ms): bayraklar ve geri sayaçlar
├── functions.c            Telsiz durum makinesi (FUNCTION_*)
├── radio.c                VFO/kanal yapılandırma, BK4819'a yansıtma, TX/RX hazırlık
├── settings.c             EEPROM ↔ gEeprom senkronizasyonu
├── frequencies.c          Bant tabloları, adım değerleri, TX izin kontrolü
├── audio.c, dcs.c         Bip/ses, CTCSS/DCS kod tabloları
├── am_fix.c               AM demodülatör doyma düzeltmesi (AGC kazanç tablosu)
├── board.c                Pin/periferik init, ADC, kalibrasyon
├── misc.c/.h              Paylaşılan global değişkenlerin çoğu
├── app/                   Uygulama mantığı (tuş işleme, menü, tarama, spektrum, UART)
├── ui/                    Ekran çizimi (her ekran için ayrı dosya)
├── driver/                Donanım sürücüleri (bk4819, st7565, eeprom, i2c, spi, uart...)
├── bsp/dp32g030/          Register tanımları (hardware/*.def dosyalarından üretilir)
├── helper/                Batarya ve önyükleme modu yardımcıları
├── external/              CMSIS_5 ve printf (git submodule)
├── fw-pack.py             .bin → şifrelenmiş .packed.bin (resmi güncelleyici formatı)
└── Makefile               ENABLE_* bayrakları ile özellik seçimi
```

Katman sınırları genel olarak temiz: `driver/` donanıma, `app/` iş mantığına, `ui/` çizime ayrılmış. Ancak `misc.h` üzerinden paylaşılan yüzlerce global değişken bu ayrımı pratikte zayıflatıyor (bkz. Bölüm 10).

---

## 3. Önyükleme Akışı

1. **`start.S`**: Vektör tablosu. `HandlerReset` → `init.c` içindeki `BSS_Init()` ve `DATA_Init()` → `Main()`. `SystickHandler` zayıf (weak) tanımlı, gerçek gövdesi `scheduler.c`'de.
2. **`Main()` (`main.c`)**, sırasıyla:
   - Periferik saat kapılarını açar (GPIO A/B/C, UART1, SPI0, SARADC, CRC, AES, PWM+0).
   - `SYSTICK_Init()` → 10 ms kesme. `BOARD_Init()` → pinler, ekran, ADC.
   - `UART_Init()` ve versiyon gönderimi (ENABLE_UART ise).
   - `BK4819_Init()` → RF çipini başlatır.
   - `SETTINGS_InitEEPROM()`, `SETTINGS_LoadCalibration()` → kalıcı ayarlar RAM'e.
   - `RADIO_ConfigureChannel(0/1)`, `RADIO_SelectVfos()`, `RADIO_SetupRegisters(true)` → iki VFO hazırlanır, aktif olan BK4819'a yazılır.
   - Batarya ölçümleri, `AM_fix_init()`.
   - `BOOT_GetMode()`: açılışta basılı tuşlara göre gizli menü (F-LOCK) veya AIRCOPY modu.
   - Batarya boşsa doğrudan `FUNCTION_POWER_SAVE`; değilse karşılama ekranı (~2,5 s), opsiyonel şifre ekranı.
3. **Ana döngü**:

```c
while (true) {
    APP_Update();                 // her turda, olay odaklı işler
    if (gNextTimeslice) {         // ISR her 10 ms'de set eder
        APP_TimeSlice10ms();      // tuş tarama, ekran, RX işleri
        if (gNextTimeslice_500ms)
            APP_TimeSlice500ms(); // batarya, zaman aşımları, yavaş işler
    }
}
```

---

## 4. Zamanlama Modeli (Süper Döngü + Tick Bayrakları)

RTOS yok. Klasik **kooperatif süper döngü** kullanılıyor:

- **`SystickHandler()`** (`scheduler.c`, her 10 ms) iş yapmaz; sadece:
  - `gNextTimeslice` (10 ms), `gNextTimeslice40ms`, `gNextTimeslice_500ms` bayraklarını kaldırır,
  - `*_10ms` / `*_500ms` geri sayaçlarını azaltır, sıfıra inen sayaç ilgili `gSchedule*` bayrağını kaldırır (`DECREMENT_AND_TRIGGER` makrosu).
- Ana döngü bayrakları tüketir ve indirir (`app/app.c:1090`, `app/app.c:1261`).

Örnek sayaçlar: TX zaman aşımı (`gTxTimerCountdown_500ms` → `gTxTimeoutReached`), güç tasarrufu (`gBatterySaveCountdown_10ms` → `gSchedulePowerSave`), çift izleme (`gDualWatchCountdown_10ms` → `gScheduleDualWatch`), tarama bekleme, kuyruk tonu eleme, VOX.

**Önemli kural:** Ana döngüde bloklayan uzun işlemler (ör. `SYSTEM_DelayMs`, EEPROM yazımı başına 8 ms) tüm sistemi durdurur. ISR sayaçları ilerlemeye devam eder ama bayraklar ancak döngü döndüğünde işlenir. Yeni kod eklerken uzun beklemeleri durum makinesine bölmek gerekir.

**Spektrum analizörü istisnası:** `APP_RunSpectrum()` (`app/spectrum.c`) kendi `while (isInitialized)` döngüsüne girer ve tick bayraklarını kendisi tüketir. Bu modda ana `APP_Update()` çalışmaz; çıkışta register yedeği geri yüklenir.

---

## 5. Telsiz Durum Makinesi

`functions.h` içindeki `FUNCTION_Type_t`:

| Durum | Anlamı |
|---|---|
| `FUNCTION_FOREGROUND` | Boşta, kullanıcı etkileşimi |
| `FUNCTION_TRANSMIT` | Gönderim |
| `FUNCTION_MONITOR` | Squelch zorla açık dinleme |
| `FUNCTION_INCOMING` | Sinyal geldi, squelch açıldı |
| `FUNCTION_RECEIVE` | Alım (ses çıkışı aktif) |
| `FUNCTION_POWER_SAVE` | Uyku; periyodik uyanıp sinyal kontrolü |
| `FUNCTION_BAND_SCOPE` | Tanımlı ama kullanılmıyor |

Geçişler `FUNCTION_Select()` ile yapılır. `APP_Update()` → `HandleFunction()` duruma göre `CheckForIncoming` / `HandleIncoming` / `HandleReceive` / `HandlePowerSave` çağırır. BK4819 kesmeleri (squelch açıldı/kapandı, CTCSS/DCS bulundu, DTMF karakteri vb.) `CheckRadioInterrupts()` içinde **yoklama (polling)** ile okunur; donanım kesmesi kullanılmıyor.

TX akışı: PTT → `RADIO_PrepareTX()` (`radio.c`) → `TX_freq_check()` → BK4819 TX moduna. Bitiş: `APP_EndTransmission()` (kuyruk tonu, RX'e dönüş).

---

## 6. Uygulama ve Kullanıcı Arayüzü Katmanı

**Tuş işleme:** `CheckKeys()` klavye matrisini tarar, debounce ve uzun basmayı ayırır, sonra `ProcessKey()` çağırır. `ProcessKey()`:
- Kilit, düşük batarya uyarısı, DTMF, FM gibi özel durumları önce ele alır,
- Sonra aktif ekrana göre işlev tablosundan çağırır:

```c
ProcessKeysFunctions[gScreenToDisplay](Key, bKeyPressed, bKeyHeld);
// DISPLAY_MAIN → MAIN_ProcessKeys, DISPLAY_MENU → MENU_ProcessKeys, ...
```

- Yan tuşlar (SIDE1/SIDE2) ve atanabilir eylemler `ACTION_Handle()` (`app/action.c`) üzerinden.

**Ekran çizimi:** `ui/ui.c` içinde paralel bir tablo var: `UI_DisplayFunctions[gScreenToDisplay]()`. Çizim `gFrameBuffer[FRAME_LINES][128]` ve `gStatusLine[128]` tamponlarına yapılır, sonra ST7565'e gönderilir. Tablonun eleman sayısı `static_assert` ile kontrol ediliyor (iyi uygulama).

**Yeni bir ekran eklemek için:** `GUI_DisplayType_t`'ye değer ekle → `ui/` altında `UI_DisplayXxx()` yaz → `UI_DisplayFunctions` ve `ProcessKeysFunctions` tablolarına ekle → Makefile'a nesne dosyası.

**Menü:** `app/menu.c` (1739 satır) ve `ui/menu.c`. `MenuList[]` sonlandırıcılı bir dizi; `FIRST_HIDDEN_MENU_ITEM` sonrası öğeler sadece F-LOCK önyükleme modunda görünür.

---

## 7. Donanım Sürücüleri

**BK4819 (`driver/bk4819.c`, 1819 satır):** 3 telli seri arayüz GPIOC üzerinden yazılımla sürülüyor (SCN/SCL/SDA, her yarım periyotta `SYSTICK_DelayUs(1)`). Bir register erişimi ~50 µs mertebesinde. Sürücü düşük seviye register okuma/yazmanın yanında yüksek seviye işlemleri de içeriyor: filtre bant genişliği, AGC, PA bias, CTCSS/DCS üretimi ve algılama, DTMF, VOX, scrambler, frekans tarama. Register anlamları `bk4819-regs.h` içinde; çoğu Beken veri sayfası olmadan tersine mühendislikle bulunmuş.

**EEPROM (`driver/eeprom.c`):** Okuma serbest uzunlukta. Yazma her zaman **8 bayt** ve önce oku-karşılaştır yapıyor; aynıysa yazmıyor (aşınmayı azaltır). Her fiili yazım 8 ms bloklar.

**AM Fix (`am_fix.c`):** BK4819'un AM modunda güçlü sinyalde doyması sorununa karşı RSSI'ye bakarak REG_10 kazanç tablosunda (LNA/mixer/PGA kombinasyonları) yazılımsal AGC uyguluyor. Güç elektroniği/kontrol bakışıyla: basit bir histerezisli kazanç planlama (gain scheduling) döngüsü.

---

## 8. Kalıcı Veri (EEPROM Haritası, özet)

`settings.c` incelemesinden çıkarılan başlıca bölgeler:

| Adres | İçerik |
|---|---|
| `0x0000–0x0C7F` | Bellek kanalları (16 bayt/kanal) |
| `0x0C80+` | Frekans (VFO) kanalları, bant başına 2×16 bayt |
| `0x0D60` | Kanal öznitelikleri (`gMR_ChannelAttributes`) |
| `0x0E40` | FM kanalları |
| `0x0E70–0x0EA8` | Genel ayarlar (squelch, VOX, ekran, anahtar atamaları...) |
| `0x0E98` | Açılış şifresi |
| `0x0EE0–0x0F18` | DTMF kimlik/kodları |
| `0x0F30–0x0F3F` | Özel AES anahtarı |
| `0x0F40` | F-LOCK ve TX bant izinleri |
| `0x0F50+` | Kanal isimleri (16 bayt/kanal) |
| `0x0F18` | Tarama listesi ayarları |
| `0x1E00–0x1F8F` | Kalibrasyon: squelch eşikleri, RSSI, TX gücü, batarya, VOX, çeşitli |

`gEeprom` yapısı RAM'deki ayna; ayar değişince ilgili `SETTINGS_Save*()` 8 baytlık blok yazar.

---

## 9. Seri (UART) Programlama Protokolü

`app/uart.c`, CHIRP gibi programlama yazılımlarıyla uyumlu protokolü uygular.

- Çerçeve: `AB CD | len(2) | payload | CRC(2) | DC BA`. Payload ve CRC, 16 baytlık sabit bir XOR dizisiyle gizlenir (şifreleme değil, obfuscation).
- DMA ile dolan 256 baytlık dairesel tampondan `UART_IsCommandAvailable()` çerçeveyi ayıklar.
- Başlıca komutlar: `0x0514` el sıkışma/versiyon, `0x051B` EEPROM oku, `0x051D` EEPROM yaz, `0x0527` RSSI, `0x0529` batarya, `0x052D` AES challenge, `0x05DD` reset.
- Oturum `Timestamp` alanıyla eşleştirilir. Özel AES anahtarı yoksa kilit fiilen kapalıdır.
- `ENABLE_UART_RW_BK_REGS=1` ile `0x0601/0x0602` doğrudan BK4819 register okuma/yazma (kimlik doğrulamasız; sadece hata ayıklama için).

---

## 10. Derleme Seçenekleri

Tüm özellikler `Makefile` başındaki `ENABLE_*` değişkenleriyle seçilir ve `-D` makrolarına dönüşür. Derleyici bayrakları: `-Os -Wall -Werror -Wextra -mcpu=cortex-m0 -std=c2x -flto` (LTO açıkken OVERLAY kapanır). `-Werror` sayesinde uyarı içeren değişiklik derlenmez.

Öne çıkanlar (varsayılan değer parantez içinde):

| Bayrak | İşlev |
|---|---|
| `ENABLE_SPECTRUM` (1) | fagci spektrum analizörü (F+5) |
| `ENABLE_AM_FIX` (1) | AM doyma düzeltmesi |
| `ENABLE_FMRADIO` (1) | BK1080 FM radyo |
| `ENABLE_UART` (1) | Seri programlama |
| `ENABLE_WIDE_RX` (1) | Alt ve üst bantları BK4819'un çip sınırlarına kadar genişletir (`BX4819_band1/2`) |
| `ENABLE_DTMF_CALLING` (1) | DTMF çağrı sistemi |
| `ENABLE_SCAN_RANGES` (1) | Frekans aralığı tarama |
| `ENABLE_VOICE` (0) | Sesli bildirim |
| `ENABLE_AIRCOPY` (0) | Telsizden telsize kablosuz klonlama |
| `ENABLE_TX_WHEN_AM` (0) | AM modunda TX'e izin |
| `ENABLE_SWD` (0) | SWD hata ayıklama pinlerini serbest bırakır |
| `ENABLE_UART_RW_BK_REGS` (0) | UART üzerinden ham BK4819 register erişimi |

**Flash bütçesi en büyük kısıt:** 60 KB dolmaya yakın olduğundan bir özelliği açmak için genelde başka birini kapatmak gerekir. Derleme sonunda `arm-none-eabi-size` çıktısı kontrol edilmeli.

Çıktılar: `firmware.bin` (ham) ve `firmware.packed.bin` (`fw-pack.py` ile resmi güncelleyiciye uygun şifrelenmiş paket).

---

## 11. Kod İnceleme Bulguları

Önem sırası: **Yüksek** = bellek güvenliği / yanlış davranış riski, **Orta** = sağlamlık, **Düşük** = bakım/okunabilirlik.

### Y1 — UART EEPROM okumada yığın taşması (`app/uart.c`, `CMD_051B`) — Yüksek

```c
REPLY_051B_t Reply;                 // Data[128]
...
EEPROM_ReadBuffer(pCmd->Offset, Reply.Data.Data, pCmd->Size);   // Size: uint8_t, 0–255
SendReply(&Reply, pCmd->Size + 8);
```

`pCmd->Size` gelen paketten alınıyor ve 128'e karşı kontrol edilmiyor. 129–255 arası bir değer, yığındaki `Reply` yapısının ötesine EEPROM verisi yazar (dönüş adresi dahil), ardından `SendReply` yığın içeriğini dışarı gönderir. Tetikleyici seri kablo erişimi gerektirir; CHIRP normalde ≤128 ister, bu yüzden pratikte görülmemiş olabilir. Düzeltme tek satır:

```c
if (pCmd->Size > sizeof(Reply.Data.Data))
    return;
```

### Y2 — UART EEPROM yazmada tampon ötesi okuma (`CMD_051D`) — Orta

Döngü `pCmd->Size / 8` kez çalışıyor ve `pCmd->Data[i*8]`'den okuyor. Veri, 256 baytlık `UART_Command.Buffer` içinde 12. bayttan başlıyor; `Size` en fazla 255 olabildiğinden son blok tamponun 4 bayt ötesine taşabilir (statik bellekten okuma; çökme olmaz ama EEPROM'a çöp yazılır). Ayrıca `Size`, çerçevenin gerçek uzunluğuyla (`Header.Size`) karşılaştırılmıyor. `Offset`'in 8 bayt hizalı olduğu da kontrol edilmiyor; hizasız bir adres EEPROM sayfa sınırını aşıp sayfa başına sarabilir. Öneri: `Size <= Header.Size - 8` ve `(Offset & 7) == 0` kontrolleri.

### O1 — ISR'de okunan ama `volatile` olmayan değişkenler — Düşük/Orta

`SystickHandler` içinde `gCurrentFunction`, `gScanStateDir`, `gCssBackgroundScan`, `gEeprom.DUAL_WATCH` okunuyor; bunlar `volatile` değil. ISR ayrı bir fonksiyon olduğu ve her girişte bellekten okuduğu için şu an doğru çalışıyor. LTO ile satır içi optimizasyonlar değişirse kırılgan hale gelebilir. Geri sayaçların ve `gSchedule*` bayraklarının tamamı doğru şekilde `volatile`.

### O2 — Ana döngü ile ISR arasında oku-değiştir-yaz yarışı — Düşük

Ana döngü bazı 16 bitlik sayaçları yeniden yüklerken ISR aynı anda azaltabilir. Cortex-M0'da 16 bit hizalı yazma atomik olduğundan değer bozulmaz; en kötü durumda bir tick kaybolur. Kritik sayaçlar (TX zaman aşımı) için sorun değil, bilgi amaçlı.

### O3 — `TX_freq_check()` dönüş değeri tutarsız — Düşük

Yorum "izinliyse 0, değilse -1" diyor ama aralık dışı durum `1` döndürüyor. Çağıranlar `!= 0` / doğruluk kontrolü yaptığı için davranış doğru; yine de sözleşme belirsiz. `bool` döndürmek daha temiz olur.

### O4 — Spektrum modunun ayrı ana döngüsü — Orta (mimari)

`APP_RunSpectrum()` kendi sonsuz döngüsünü çalıştırıyor. Bu modda `APP_Update()` içindeki TX zaman aşımı, güç tasarrufu ve UART işleme devre dışı kalır. Spektrumda TX yapılmadığı için güvenlik açısından kritik değil ama yeni özellik eklenirken bu davranış akılda tutulmalı.

### D1 — Global durum yoğunluğu — Düşük (bakım)

`misc.h` yaklaşık 300 global değişken dışa açıyor. Herhangi bir dosya herhangi bir durumu değiştirebiliyor; değişikliğin etkisini izlemek zor. Bu, 60 KB'lık kısıtta bilinçli bir tercih ama yeni kodda modül içi `static` durum ve küçük erişim fonksiyonları tercih edilmeli.

### D2 — Okunabilirlik notları — Düşük

- `bIsLocked = bHasCustomAesKey ? gIsLocked : bHasCustomAesKey;` aslında `bHasCustomAesKey && gIsLocked`.
- `gIsLocked` `uint8_t` olarak `0xFF` ile başlatılıp bool gibi kullanılıyor.
- `FUNCTION_BAND_SCOPE` tanımlı ama kullanılmıyor; `FUNCTION_FOREGROUND` yanında `// ???` yorumu duruyor.
- BK4819 register değerlerinin önemli kısmı sihirli sayı; `bk4819-regs.h` kısmen belgeliyor.

### Olumlu yönler

- `-Wall -Wextra -Werror` ile derleme; uyarısız kod tabanı.
- ISR minimal; iş ana döngüde. Gömülü sistemler için doğru desen.
- EEPROM yazmadan önce oku-karşılaştır; gereksiz aşınma yok.
- Ekran/tuş işlev tablolarında `static_assert` ile boyut kontrolü.
- Özellik bayraklarıyla tutarlı koşullu derleme; kapalı özelliğin kodu Makefile'dan hiç bağlanmıyor.

---

## 12. Değişiklik Yaparken Pratik Rehber

1. **Önce flash bütçesine bak.** Derle, `arm-none-eabi-size firmware` çıktısındaki `text + data` değerini 61440'a karşı kontrol et.
2. **Yeni özelliği bayrağın arkasına koy.** Makefile'a `ENABLE_XXX ?= 0`, `CFLAGS += -DENABLE_XXX`, gerekiyorsa `OBJS += ...`; kodda `#ifdef ENABLE_XXX`.
3. **Ana döngüyü bloklama.** 10 ms'den uzun işi geri sayaç + `gSchedule*` bayrağı desenine böl (`scheduler.c`'ye sayaç ekle, `APP_Update()`'te bayrağı tüket).
4. **EEPROM'a sık yazma.** Ayar değişince değil, menüden çıkarken veya zaman aşımıyla toplu yaz. Yeni alan için `settings.c`'deki tüm `EEPROM_ReadBuffer` adreslerini tarayıp kullanılmayan bir 8 baytlık blok bul; harita tam belgelenmiş değil.
5. **BK4819 register'larını değiştirirken** önce `BK4819_ReadRegister` ile yedekle, işin sonunda geri yükle (spektrum modu bu deseni kullanıyor: `BackupRegisters()`).
6. **Test:** Donanımsız birim testi altyapısı yok. Mantık kısımlarını (frekans hesapları, tablo aramaları) saf fonksiyon olarak yazıp host tarafında `gcc` ile ayrıca test etmek mümkün.
7. **Flash'lama:** `firmware.packed.bin` resmi Quansheng güncelleyicisi veya web tabanlı flasher'larla yüklenir. Bootloader ayrı bir bölgede durduğu için hatalı firmware genellikle bootloader moduna girilip yeniden yüklenerek kurtarılabilir.

---

## 13. Önerilen Sonraki Adımlar

1. Y1 ve Y2 düzeltmelerini ayrı bir dalda yap, derle, CHIRP ile okuma/yazma testinden geçir.
2. Varsayılan bayraklarla derleyip flash/RAM kullanımını ölç; hangi özelliklerin ne kadar yer kapladığını bayrak bayrak çıkar.
3. İlgi alanına göre geliştirme:
   - **Kontrol/kestirim:** AM fix'in yazılımsal AGC'si, histerezisli ve sabit eşikli. RSSI üzerinde basit bir filtre/gözlemci ile daha yumuşak kazanç geçişi denenebilir.
   - **Sinyal işleme:** Spektrum analizöründe RSSI ortalama/tepe tutma modları.
   - **Gömülü altyapı:** Host tarafında birim testi iskeleti (frekans/adım hesapları için).
