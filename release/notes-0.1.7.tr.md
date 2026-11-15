0.1.6 için bildirilen üç sorunu düzelten bir sürüm: **kapanışta artık çekirdek paniği yok** (issue #8), **uygulama
simgeleri katmanlarının hiçbirini kaybetmiyor** (issue #9: Safari pusulasının kırmızı ibresi, WeChat'in öndeki
baloncuğu) ve IntelXe Manager normal bir yeniden başlatmadan sonra artık çekirdek paniği bildirmiyor. Güncelledikten
sonra simge önbelleklerini bir kez temizleyin (aşağıda); yoksa 0.1.6'nın yanlış çizdiği simgeler bozuk kalır. Kurulum
rehberi de kısaldı: önce olağan yol, ayrıntılar katlanmış bölümlerde.

> [!WARNING]
> Deneysel. Tek bir makinede, tek bir kartla (Arc B580) test edildi; Arc B570, Pro B60 ve Pro B50 destekleniyor ama
> test edilmedi. Apple imzası yok; SIP'in ve AMFI'nin gevşetilmesi ve OpenCore gerekir. Kurmadan önce çalışan bir
> EFI'yi USB bellekte hazır tutun.

> [!NOTE]
> Yapay zekâ desteğiyle geliştirildi: kodu, her derlemeyi gerçek donanımda test eden proje sahibinin yönetiminde
> Anthropic'in Claude Opus 5.5 modeli (High çaba düzeyi) yazdı.

### Öne çıkanlar

- **Kapanışta çekirdek paniği yok (issue #8).** 0.1.6'daki scan-out'un 4. aşaması, WindowServer'ın birleştirdiği
  karenin en yenisini, bir şey yüzeyin sayfalarını okuduğunda scan-out arabelleğinden o sayfalara kopyalıyor (GPU'nun
  yaptığı bir "sayfalara alma"). Böyle bir kopya 2 saniye içinde bitmediğinde `IntelXe.kext`, GPU sayfalara hâlâ
  yazabileceği için onların cihaz eşlemesini bilerek tutuyordu, ama sayfaları kendisi kilitli (wired) tutmuyordu.
  NMXAccel kopyanın hemen ardından kilidi her zamanki gibi bırakınca sayfaların DMA eşlemesi kalıyor ama kilidi
  kalmıyordu; macOS bunu ölümcül bir hata sayıyor: `NMXResource::pageoff` içinde `complete() while dma active`.
  Kapanışta GPU durdurulurken böyle bir kopya zaman aşımına uğrayabiliyor. Artık `IntelXe.kext` tuttuğu eşlemeyle
  birlikte bir kilit de tutuyor; NMXAccel de bir yüzeyin sayfalarını yalnızca sayfalara alınacak bir kare olduğunda
  (sayfalarından daha yeni olarak işaretlenmiş, bağlı bir birleştirme) kilitliyor. Önceden VRAM'i olmayan her yüzeyin
  her işlemci kilidi, sayfalarını boşuna kilitleyip bırakıyordu. B580'de: paniksiz yeniden başlatmalar, sayfalara
  alma çalışmaya devam ediyor.
- **Uygulama simgeleri katmanlarının hiçbirini kaybetmiyor (issue #9).** Katmanlı bazı uygulama simgeleri bir katmanı
  eksik çiziliyordu, üstelik her çizimde farklı: Safari pusulasının kırmızı ibresinin yarısı, WeChat'in öndeki
  baloncuğu. Neden Mesa'nın Vulkan sürücüsünde (ANV), descriptor buffer'larla ilgili: bağlaması olmayan bir küme
  düzeniyle birlikte verilen küme konumu hiçbir shader aşamasını işaretlemiyordu, bu yüzden ANV daha önce gönderdiği
  konumu koruyordu; aynı konumda aşamaları olan sonraki bir düzen "değişiklik yok" gibi görünüyor, aşamaları da kümeyi
  daha eski bir bölgenin konumundan okuyordu. Metal sürücüsü bu tür boş aşama planlarına bir sonraki gerçek bölgenin
  başladığı konumu veriyordu; sorun tam da buydu (RenderBox'ta Safari simgesi: 168 çizimin 3'ü). İki yerde düzeltildi:
  - Metal sürücüsü boş aşama planlarına 0 konumunu veriyor; her descriptor sayfasının ilk 64 baytı hiçbir zaman bir
    bölge olmuyor (`nullmoth/plugin/nvmtl_dbuf.c`). B580'de Safari simgesi yalnızca bununla bile doğru çiziliyor;
  - Mesa yaması `0109`, ANV'nin kendisini bu sürücüdeki her Vulkan programı için düzeltiyor (Zink OpenGL ve Vulkan
    paketleri kurulu ANV'yi kullandığı için onlar da bundan yararlanıyor). Sorunu yeniden üreten
    `user/tests/vulkan/vk_dbuf_empty_set` B580'de 0.1.7'nin ANV'siyle geçiyor, 0.1.6'nınkiyle kalıyor.

  macOS çizilmiş simgeleri simge önbelleklerinde saklar; bu yüzden güncellemeden önce yanlış çizilen simgeler
  önbellekler temizlenene kadar yanlış kalır: *0.1.6'dan yükseltme* bölümünün 5. adımına bakın.
- **IntelXe Manager: normal bir yeniden başlatmadan sonra "macOS bir çekirdek paniğinden sonra yeniden başladı" yok.**
  Genel bakış sayfası son 7 günün herhangi bir panik raporunu bildiriyordu; bu yüzden tek bir panikten sonra o hafta
  her yeniden başlatmada aynı uyarı çıkıyordu. Artık yalnızca bu açılış sırasında yazılmış bir panik raporunu
  bildiriyor; macOS bir önceki açılıştaki paniğin raporunu tam da o sırada yazar.
- **Bulanıklık ve saydam pencereler GPU'yu çok daha az yoruyor.** Arkasını bulanıklaştıran (backdrop blur) ya da
  başka biçimde saydam olan pencereler ve paneller, çizilirken ekranda zaten bulunan pikselleri okuyor. Metal sürücüsü
  bu okumalar sırasını şaşırmasın diye böyle bir çizimde GPU'yu neredeyse her iki üçgenin arasında bekletiyordu; artık
  yalnızca iki üçgenin gerçekten üst üste gelebildiği yerde bekletiyor. En çok kazananlar çok sayıda `backdrop-filter`
  paneli olan web sayfaları: B580'de ekranda bulanık on iki panel varken GPU'nun beklemeleri saniyede yaklaşık
  300.000'den 10.000'in altına indi, masaüstü de her kare için daha az GPU zamanı harcıyor. Saydam bir pencere bir gün
  yanlış görünürse `/Library/GPUBundles/nvmtl/nvrm610.conf` dosyasına eklenen `NVMTL_FETCH_OVERLAP=0` satırı, yeniden
  başlattıktan sonra eski yönteme döndürür; böyle bir durumu lütfen bildirin.
- **Daha sade bir kurulum rehberi.** `README-INSTALL.tr.md` (İngilizcesi `README-INSTALL.md`) önce olağan yolu kısa
  adımlarla veriyor; arka plan bilgisi ve seyrek durumlar aşağıda katlanmış bölümlerde. Sitedeki rehber de aynı
  şekilde sadeleşti.

### Paketler

| Dosya | Ne için | Nerede açılmalı |
|---|---|---|
| `IntelXe-Manager-0.1.7.zip` | IntelXe Manager uygulaması (macOS 15 ve sonrası) | macOS |
| `IntelXeManager-0.1.7-win-x64.zip` | Windows için IntelXe Manager: Windows'tan 2. ve 4. aşamalar, bilgisayarın denetimi, kurulum adımları, raporlar (sürüm numarası dışında değişmedi) | Windows 10 ya da 11 (x64) |
| `intel-gpu-macos-driver-0.1.7-tahoe.zip` | macOS 26 Tahoe | macOS |
| `intel-gpu-macos-driver-0.1.7-sequoia.zip` | macOS 15 Sequoia (aynı kaynaklar; Sequoia'da yeniden denenmedi) | macOS |
| `intel-gpu-macos-driver-0.1.7-efi.zip` | OpenCore parçaları: `IntelXe.kext`, `SSDT-ARCOFF`, `HybridPM.kext` (isteğe bağlı), `IntelXe-EFI.ps1` ve `efi-rules.json` | herhangi bir sistem |
| `intel-gpu-macos-driver-0.1.7-zink-gl.zip` | isteğe bağlı: Mesa'nın Zink'i üzerinden OpenGL (0.1.5'in ikili dosyaları; kurulu ANV'yi kullandığı için issue #9 düzeltmesi onda da geçerli) | macOS |
| `intel-gpu-macos-driver-0.1.7-vulkan.zip` | isteğe bağlı, deneysel: Wine için Vulkan (0.1.5'in ikili dosyaları; aynı şekilde) | macOS |
| `xe-probe-stick-0.1.0.img.zip` | isteğe bağlı, deneysel: sürücünün henüz desteklemediği Intel GPU'ları için Xe tarama belleği görüntüsü (0.1.5 ve 0.1.6'dakinin aynısı) | bir USB belleğe yazılır |

0.1.7 için yeniden derlenenler: `IntelXe.kext`, `NMXAccel.kext`, Metal sürücüsü (iki kopyası da), Mesa'nın Vulkan
sürücüsü `libvulkan_intel.dylib` (iki kopyası da: 0.1.6'nın kaynağı ve `0109` yaması), IntelXe Manager.
`HybridPM.kext` 0.1.5'in kodunu yeni sürüm numarasıyla taşıyor; `IntelXeFB.kext`, `NMXAGDC.kext`, shader çeviricisi,
video eklentileri ve FFmpeg 0.1.6'dakilerle aynı dosyalar (her paketteki `SOURCES.md` bütün ikili dosyaları listeliyor).

Sağlama toplamları: `intel-gpu-macos-driver-0.1.7.sha256`, projenin anahtarıyla
`intel-gpu-macos-driver-0.1.7.sha256.sig` içinde imzalı; Windows uygulamasının zip'ini ve tarama belleği görüntüsünü
de listeliyor. Kurulum rehberi: her macOS paketinde `README-INSTALL.tr.md` ve
[depoda](https://github.com/yusufklncc/intel-gpu-macos-driver/blob/v0.1.7/release/README-INSTALL.tr.md) (İngilizcesi
`README-INSTALL.md`).

### 0.1.6'dan yükseltme

1. Paketi kurun (uygulamada *v0.1.7 sürümünü indir*, sonra *Bu paketi kur*; ya da `sudo ./install.sh --apply`).
2. Yeniden başlatmadan önce **EFI'deki `IntelXe.kext`'i değiştirin**: macOS'ta uygulamanın EFI sayfası ya da
   `sudo ./efi.sh --apply`; Windows'ta Windows için IntelXe Manager (EFI sayfası, *IntelXe.kext ve SSDT-ARCOFF
   (4. aşama)*) ya da `IntelXe-EFI.ps1`; ya da elle. `config.plist` kaydı olduğu gibi kalır. Kullanılıyorsa
   `HybridPM.kext`'i de aynı adım değiştirir. Kapanış paniği düzeltmesinin yarısı `IntelXe.kext`'in içinde: EFI'de
   0.1.6'nınki kalırsa panik yine olabilir.
3. Safari için sistem diski kopyasını kurduysanız **bu paketinkini de kurun** (uygulamanın Sürücü sayfası, *Safari ve
   WebKit*; ya da `sudo ./system-volume/ssv-install.sh --apply`). 0.1.7 iki kopyadaki Metal sürücüsünü ve Mesa'yı
   değiştiriyor; siz kurana kadar `install.sh`, her süreç yeni `/Library` kopyasını kullansın diye
   `NVMTLDriver.use-library` dosyasını oluşturur, `ssv-install.sh --apply` de bu dosyayı yeniden siler.
4. Yeniden başlatın.
5. **Yeniden başlattıktan sonra simge önbelleklerini bir kez temizleyin**, Terminal'de:

   ```bash
   rm -rf "$(getconf DARWIN_USER_CACHE_DIR)com.apple.iconservices" "$(getconf DARWIN_USER_CACHE_DIR)com.apple.dock.iconcache"
   sudo rm -rf /Library/Caches/com.apple.iconservices.store
   killall Dock
   ```

   Dock ve Finder simgeleri bundan sonra yeni sürücüyle yeniden çizer. Bunu yeniden başlatmadan önce değil, sonra
   yapın: yeniden başlatana kadar simgeleri yine eski sürücü çizer.

0.1.5'ten geliyorsanız da bu adımları yapın, sonra 0.1.6'nın neleri değiştirdiğini onun sürüm notlarında okuyun
(0.1.5'teki davranışa dönme anahtarları hâlâ geçerli). 0.1.4 ya da öncesinden geliyorsanız önce rehberin yükseltme
bölümünü izleyin.

### Düzeltilenler

- `IntelXe.kext`, `NMXAccel.kext`: bir scan-out sayfalara alma kopyası zaman aşımına uğradıktan sonra kapanışta
  çekirdek paniği, `NMXResource::pageoff` içinde `complete() while dma active` (issue #8, yukarıda).
- Metal sürücüsü ve Mesa'nın ANV'si: bir katmanı eksik çizilen katmanlı uygulama simgeleri (issue #9, yukarıda).
- IntelXe Manager: bir panikten sonra bir hafta boyunca her normal yeniden başlatmada gösterilen "macOS bir çekirdek
  paniğinden sonra yeniden başladı" uyarısı (yukarıda).

### Geliştiriciler için

- `mesa/patches/0109` (ANV, descriptor buffer'lar: her küme için, son konum değişikliğinde işaretlenmeyen aşamalar,
  sonraki bir düzende varsa işaretleniyor; platformdan bağımsız, upstream `f01e9b07a91` ve `intelxe.diff` üzerine
  uygulanıyor) ve `mesa/README.md`'deki satırı. 0.1.7'nin ANV'si 0.1.6'nın kaynağı ve `0109`'dan ibaret: `0109`
  olmadan aynı derleme bit bit 0.1.6'nın dosyası.
- `user/tests/vulkan/vk_dbuf_empty_set.c`: issue #9'un yalnızca Vulkan'la yeniden üretilmesi (komut satırında verilen
  ANV'yi yüklüyor, böylece iki derleme yan yana karşılaştırılabiliyor); `MESA=<bir Mesa ağacı>
  user/tests/vulkan/build.sh` ile derlenir.
- Metal sürücüsünün descriptor sayfaları: `nullmoth/plugin/nvmtl_dbuf.c`'de `NVMTL_DB_EMPTY_OFF` ve ayrılan ilk 64
  bayt; `dbuf_model` (descriptor buffer bağlamalarının ana makine modeli) bunlarla geçiyor.
- NMXAccel'in 4. aşaması: `nmxScanNewer`, bir işlemci kilidinin ya da kopyalanan bir karenin sayfalara alınacak bir
  şeyi olup olmadığına, hiçbir sayfa kilitlenmeden önce karar veriyor; `IntelXe.kext`'in `svCopyBo`'su zaman aşımına
  uğrayan bir kopyanın IOVA eşlemesiyle birlikte bir kilit de tutuyor.

### Bilinen sorunlar

- **Bu sürüm paket olarak B580'de:** 0.1.6'nın üstüne kendi paketiyle kuruldu (EFI, `/Library` ve sistem diski
  kopyası). Sürümler beklendiği gibi, Metal sistem diskinden yükleniyor; simge önbellekleri temizlendikten sonra
  Safari'nin simgesi üç denemede de doğru çizildi; `vk_dbuf_empty_set` iki ANV kopyasıyla da geçiyor; üç yeniden
  başlatmada panik yok; 4. aşama ve GPU saat takviyesi sayaçları beklendiği gibi; framebuffer fetch sıra testi
  `mtl_fetch_order` 18'de 18; regress 19'da 18 (aşağıdaki GPU hataları yüzünden; Safari açık olduğu için WebGPU
  atlandı); Geekbench 7 Metal 96.824 ile 99.496, OpenCL 87.144 ile 91.609 arası; dört dakikalık uyku ve uyanmanın
  ardından regress 13'te 13.
- **4K bir panelde ölçeklenmiş 2560x1440 modu görüntüyü bozuyor** (issue #10, 0.1.4'ten beri): ekran görüntüleri
  temiz, panel ise anlamsız bir görüntü gösteriyor; yani ölçeklenmiş kare ekrana giderken bozuluyor. Test makinesinin
  paneli 2560x1440 olduğu için sorun yeniden üretilemiyor; bildiren kişinin tanılama raporu bekleniyor. O zamana kadar
  panelin kendi çözünürlüğünü ya da başka bir ölçeklenmiş modu kullanın.
- **Oturum açıldıktan sonraki ilk dakikalarda GPU saat takviyesi (display boost)**, 0.1.6'daki gibi: boşta kalmanın
  ardından açılan bir pencereden hemen önce başka bir ekran güncellemesi (bir saat, bir bildirim) gelirse 400 ms'lik
  süreyi o alır; pencerenin kendi kareleri GuC saati yükselmeden başlayabilir.
- **3440x1440 bir panelde scan-out'un 4. aşaması** (issue #7) ana makine testinde denetlendi ama henüz böyle bir
  panelde çalışmadı. Bir ekran görüntüsü eski bir kare gösterirse `/Library/GPUBundles/nvmtl/nvrm610.conf`'ta
  `NVMTL_SCANOUT_READBACK=1` tam geri okumayı geri getirir (WindowServer bir sonraki yeniden başlatmada alır); lütfen
  bildirin.
- **Safari açıkken GPU hataları (nedeni açık)**, 0.1.6'da ve öncesinde de var: Safari çalışırken, penceresi olmadan
  arka planda olsa bile, ağır bir OpenCL programı GPU'nun eşlenmemiş bir adrese erişmesine yol açabiliyor
  (`MEMORY_CAT_ERROR`). GuC o programın işini sıfırlıyor; program işini kaybedebilir ya da kapanabilir (B580'de
  Geekbench 7 OpenCL: Safari açıkken 6 koşunun 5'inde hata, Safari olmadan 18 koşuda hiç; tamamlanan koşuların puanı
  normal). Hatalı iş OpenCL programının kendi işi; Safari'nin GPU sürecinin yalnızca açık olması yetiyor. Bu yüzden
  neden, sürücünün bu iki istemciyi birlikte nasıl yönettiğinde aranıyor. Düzelene kadar uzun OpenCL işlerinden önce
  Safari'yi kapatın; bir `MEMORY_CAT_ERROR` görürseniz lütfen rapor gönderin.
- Gerisi 0.1.6'daki gibi: OpenCL, Geekbench 6'da 0.1.4'ün çekirdek sürücüsüne göre yaklaşık %3,5 daha yavaş;
  `ocvalidate`, `ResizeAppleGpuBars = 10` için uyarıyor (10'da bırakın); açılış korumasının hızlandırmayı durdurduğu
  bir açılış firmware'in framebuffer'ıyla açılmalı (henüz donanımda denenmedi); Windows için IntelXe Manager henüz
  gerçek bir EFI bölümünü değiştirmedi ve henüz hiçbir bilgisayar onun yaptığı bir EFI, USB kurulum belleği ya da
  tarama belleğiyle açılmadı; Windows uygulamasının kod imzası yok; B570, Pro B60 ve Pro B50 test edilmedi;
  DisplayPort'ta tek monitör; Safari, Intel GPU'larda WebGPU `timestamp-query` sunmuyor.

### Teşekkürler

- Issue #8, #9 ve #10 için `@mydogbro-beep`'e: geri izlemesiyle birlikte kapanış paniği, bir katmanı eksik simgeler
  ve onları yeniden ortaya çıkaran adımlar, 4K bir paneldeki ölçeklenmiş mod.
- Her sürümde olduğu gibi: nullmoth (NullMoth Systems), metal2vulkan (Anees Iqbal), Linux `xe` ve i915, Mesa, FFmpeg,
  Emil J. Tywoniak'ın Xe2 VDEnc çalışması ve Lilu (acidanthera); README'ye bakın.
