# Türkçe terimler sözlüğü

Proje sitesinin (site/), Türkçe kurulum rehberinin (release/templates/README-INSTALL.tr.md) ve diğer Türkçe
belgelerin kullandığı terimler. Amaç, aynı kavramın her sayfada aynı sözcükle geçmesi ve çevirinin İngilizceden
sözcük sözcük aktarılmış gibi değil, doğal Türkçe gibi okunması.

## Genel kural

- **Ekranda İngilizce görünen ne varsa İngilizce kalır:** firmware (BIOS/UEFI) menü öğeleri, OpenCore'un ve
  config.plist'in anahtarları, komutlar, dosya ve klasör adları, ürün adları, OpenCore açılış menüsündeki öğeler.
  Kullanıcı bunları ekranda o hâliyle arayacak.
- **macOS'un kendi arayüz adları, macOS Türkçe kullanıldığında gösterdiği adla yazılır** (Sistem Ayarları,
  Bu Mac Hakkında...). Rehberi okuyan kişi büyük olasılıkla macOS'u Türkçe kullanıyor.
- **IntelXe Manager'ın (macOS ve Windows) düğme ve sayfa adları, uygulamanın Türkçe arayüzündeki adlarıyla yazılır**
  (app/Resources/tr.lproj/Localizable.strings, windows/app/IntelXe.App/Strings/tr-TR/Resources.resw). Ekran
  görüntüleri İngilizce arayüzden olduğu için gerekirse İngilizce adı ayraç içinde eklenir.
- **Kavramlar Türkçeleştirilir**, sitenin zaten en çok kullandığı karşılıkla. Türkçe hackintosh kullanıcılarının
  yerleşik olarak İngilizce söylediği birkaç terim (kext, quirk, boot-arg, firmware) İngilizce kalır.
- Okura her yerde **"siz"** diye seslenilir.

## Kesme işareti ve ekler

Ek, adın okunuşuna göre gelir; kod biçimindeki adlarda kesme işareti `</code>`'dan hemen sonra yazılır.

| Ad | Okunuş | Örnekler |
|---|---|---|
| macOS | mek-o-es | macOS'u, macOS'ta, macOS'tan, macOS'un |
| EFI | efi | EFI'yi, EFI'de, EFI'den, EFI'nin, EFI'si |
| install.sh, efi.sh, hidpi.sh | ...eş | install.sh'i, install.sh'in, install.sh'e, install.sh'te (install.sh'nin değil) |
| .kext, .plist, .ps1 | kekst, pi-list, pe-es-bir | IntelXe.kext'i, config.plist'te, IntelXe-EFI.ps1'i |
| .aml | a-em-el | SSDT-ARCOFF.aml'i, SSDT-ARCOFF.aml'in |
| .bundle | bandıl | NVMTLDriver.bundle'ı, NVMTLDriver.bundle'da |
| /Library | laybrıri | /Library'ye, /Library'de, /Library'deki |
| SIP, AMFI | sip, amfi | SIP'i, SIP'in; AMFI'yi, AMFI'nin |
| GPU | ge-pe-u | GPU'yu, GPU'nun, GPU'da |

## Terimler

| İngilizce | Sitede kullanılan | Neden |
|---|---|---|
| stage | aşama ("2. aşama", "2. ve 4. aşamalar") | Sitede ve iki uygulamada yerleşik. |
| step | adım | Windows uygulamasının beş adımı; aşamayla karıştırılmaz. |
| release | sürüm | "0.1.6 sürümü". Yayın işinin kendisi: yayımlamak. |
| version | sürüm | |
| package | paket | |
| driver | sürücü | |
| the Metal driver | Metal sürücüsü | |
| boot (the process) | açılış; "açılışta", "açılmazsa" | Sitede yerleşik; "boot" ve "önyükleme" süreç için kullanılmaz. |
| boot from the USB stick | USB bellekten açmak / başlatmak | |
| boot menu (firmware's key, F8/F12) | (anakartın) önyükleme menüsü | Anakart kılavuzlarının kullandığı ad. |
| OpenCore picker | OpenCore'un açılış menüsü | Firmware'in önyükleme menüsüyle karışmasın. |
| boot guard | açılış koruması | Sitede yerleşik (40 yer) ve macOS uygulamasının arayüzünde böyle. |
| held boot | hızlandırması durdurulmuş açılış; cümle içinde "açılış korumasının hızlandırmayı durdurduğu açılış" | "Durdurulmuş açılış" açılışın kendisi durmuş gibi okunuyordu; açılış sürüyor, yalnızca hızlandırma kapalı. |
| boot-arg | boot-arg (boot-arg'ı) | OpenCore anahtarı; ekranda böyle. |
| quirk | quirk | OpenCore terimi; Türkçe karşılığı yok. |
| kext | kext (kext'i, kext'ler) | |
| kernel | çekirdek | Kavram. "Kernel > Add" gibi config.plist yolları ve "Yardımcı Kernel Koleksiyonu" (uygulamanın kullandığı ad) olduğu gibi kalır. |
| Auxiliary Kernel Collection | Yardımcı Kernel Koleksiyonu | macOS uygulamasının ve sitenin kullandığı ad. |
| kernel panic | çekirdek paniği; ikinci anışta: panik | Sitede ikisi de var, Türkçesi seçildi. |
| firmware | firmware (firmware'i, firmware'de) | Sitede (39 yer) ve iki uygulamada yerleşik; "ürün yazılımı" ya da "bellenim" hackintosh okurunun aradığı sözcük değil. |
| firmware settings | BIOS ayarları / firmware ayarları | Kullanıcının menüye girdiği yerde "BIOS ayarları" (Windows uygulamasının adımı da bu); genel anlamda firmware. |
| UEFI, BIOS | UEFI, BIOS | |
| the card's firmware output (GOP) | kartın UEFI çıktısı | Sitede yerleşik. |
| Above 4G Decoding, Resizable BAR, CSM, Secure Boot, VT-d | İngilizce kalır, tam bu yazımla | Anakart menüsünde böyle görünür. ("Above 4G decoding" değil, "Above 4G Decoding".) |
| config.plist keys (SecureBootModel, DisableIoMapper...), Reset NVRAM | İngilizce kalır | Ekranda/dosyada böyle. |
| relaxed SIP / AMFI | SIP'i (ve AMFI'yi) gevşetmek; "SIP'in gevşetilmesi" | "Gevşetilmiş güvenlik istiyor" İngilizcenin kalıbıydı; Türkçede eylemle söylenir. |
| unsigned / signed / ad hoc signed | imzasız / imzalı / ad hoc imzalı | |
| acceleration / accelerated | hızlandırma / hızlandırmalı | |
| framebuffer; firmware framebuffer | framebuffer; firmware'in framebuffer'ı | Teknik ad; IntelXeFB'nin "FB"si. (macOS uygulamasının Türkçe arayüzü bir yerde "ürün yazılımının ekran arabelleği" diyor; site bu sözlüğe uyar.) |
| scan-out | scan-out ("scan-out'un 4. aşaması", "scan-out geri okuması") | Projenin iş kalemlerinin adı; "ekrana verme" anlaşılmıyordu. |
| display (the output) / monitor | ekran / monitör | |
| refresh rate | yenileme hızı | macOS Türkçe arayüzündeki ad ("tazeleme hızı" değil). |
| resolution | çözünürlük | |
| installer (macOS) | kurulum programı | |
| USB installer, installer stick | USB kurulum belleği, kurulum belleği | Windows uygulamasının adım adı "Yükleyici USB bellek" alıntılanırken olduğu gibi kalır. |
| USB stick | USB bellek | |
| plain OpenCore EFI | sade bir OpenCore EFI'si (sürücüsüz) | "Düz bir EFI" İngilizceden kalıptı. |
| EFI partition | EFI bölümü | |
| system volume | sistem diski | Sitede (58 yer) ve macOS uygulamasında yerleşik; "sistem birimi" kullanılmaz. |
| Recovery image (macrecovery) | recovery görüntüsü | Windows uygulamasının arayüzündeki ad; Dortania da böyle anar. |
| macOS Recovery (the environment) | macOS Kurtarma | macOS Türkçe arayüzündeki ad. |
| safe mode | güvenli mod | Apple'ın Türkçe belgelerindeki ad; "güvenli kip" kullanılmaz. |
| System Settings, About This Mac, Privacy & Security, Remote Login, Disk Utility, Activity Monitor | Sistem Ayarları, Bu Mac Hakkında, Gizlilik ve Güvenlik, Uzaktan Giriş, Disk İzlencesi, Etkinlik Monitörü | macOS Türkçe arayüzündeki adlar. |
| Terminal, Finder, Safari, WindowServer, VideoToolbox | olduğu gibi | Ürün ve program adları. |
| motherboard | anakart | |
| CPU / hybrid CPU | işlemci / hibrit işlemci | |
| GPU, card | GPU, kart | |
| PC | bilgisayar | Metin içinde "PC" yerine. |
| test machine | test makinesi | |
| desktop (macOS) / desktop PC | masaüstü / masaüstü bilgisayar | |
| per application | uygulama bazında | "Uygulama uygulama" anlaşılmıyordu. |
| dual-boot | aynı bilgisayarda Windows ile macOS'u birlikte kullanmak | Türkçede yerleşik tek sözcüğü yok. |
| backup / undo / repair / uninstall | yedek / geri almak / onarmak / kaldırmak | |
| check / verify | denetlemek / doğrulamak | İmza ve SHA-256 için "doğrulamak", genel kontrol için "denetlemek". |
| log | günlük ("açılış günlüğü", "çekirdek günlüğü") | macOS uygulamasının arayüzünde "günlük". |
| diagnostic report | tanılama raporu | |
| issue (GitHub) | issue, issue formu | GitHub'ın arayüzü İngilizce. |
| allow-list | izin listesi | |
| build (n.) | derleme ("RELEASE derlemesi") | |
| script | betik | Sitede yerleşik. |
| probe (XeProbe) | yoklama ("aşamalı yoklama") | macOS uygulamasının sayfa adı. |
| probe stick | tarama belleği | Windows uygulamasının arayüzündeki ad. |
| engine (GPU, media) | motor (medya motoru) | |
| hang / hung | takılma / takılan | |
| sleep / wake | uyku / uyanma | |
| encode / decode | kodlama / kod çözme | |
| noncommercial | ticari olmayan | |
| credits | emeği geçenler | |
| GPU process (Safari) | GPU süreci | |
| display boost | GPU saat takviyesi (display boost) | Ne yaptığını söylüyor (boştan çıkan pencerede GPU saatini kısa süre yükseltir); "hızlandırma" GPU hızlandırması (acceleration) için ayrıldı. İlk anışta İngilizcesiyle. |
| descriptor buffer, descriptor set | İngilizce kalır ("descriptor buffer modeli") | Metal/Vulkan API terimi; "tanımlayıcı arabelleği" okura bir şey söylemiyordu. |
| watchdog, link training (DP), page flip | İngilizce kalır; gerekirse Türkçe açıklamayla | Çekirdek ve ekran terimleri; günlüklerde böyle geçer. |
| upload (a report) | göndermek; "hiçbir şey internete yüklenmez" | "Yüklemek" kurulum anlamına da geldiği için tek başına kullanılmaz. |
| setup assistant (macOS, first start) | Kurulum Yardımcısı | macOS Türkçe arayüzündeki ad. |
| the app's install assistant | kurulum yardımcısı | macOS uygulamasının Türkçe arayüzündeki ad. |
| Cancel (Windows app dialogs) | Vazgeç | Windows uygulamasının Türkçe arayüzü "İptal" değil "Vazgeç" diyor. |
| screenshot (English UI) | ekran görüntüsü (İngilizce arayüz) | Açıklamalarda hep bu biçim. |
