# Sorun giderme

[English](TROUBLESHOOTING.md) · [README'ye dön](../README.tr.md)

Sorunların çoğu MTPort'tan değil, telefondan, kablodan ya da telefonu tutan başka bir uygulamadan gelir. Önce
[kontrol listesini](#önce-kontrol-listesi) uygula; aşağıdaki bölümler genelde karşılaşılma sırasıyla dizilmiştir.

**Hızlı teşhis** (komutlar `build/native/cli/mtport` içinde ya da
`~/Applications/MTPort.app/Contents/Resources/bin/mtport` konumunda):

```bash
mtport doctor                  # macOS, libmtp, çakışan uygulamalar, USB adayları, boş alan
mtport devices                 # telefon USB'de görünüyor mu?
mtport status                  # etkin bağlantılar ve durumları
mount | grep webdav            # telefon hiç bağlı mı?
ls ~/Library/Application\ Support/MTPort/runtime/    # işçi günlükleri: mount-<cihaz>.log (içinde dosya adı yok)
```

## Önce kontrol listesi

1. Kablo **veri** taşıyor (bazı kablolar yalnızca şarj eder). Başka bir kablo dene ve doğrudan Mac'teki bir portu kullan,
   hub kullanma.
2. Telefon ekranı **açık ve kilitsiz**.
3. Telefondaki USB bildirimi **Dosya aktarımı (MTP)** diyor (*Yalnızca şarj* ya da *PTP* değil).
4. Telefon *verilere erişime izin* ya da *bu USB aksesuarına izin* sorduysa onayladın. Apple Silicon Mac'lerde macOS ilk
   seferde ayrıca *Aksesuarın bağlanmasına izin verilsin mi?* diye sorabilir; izin ver (*Sistem Ayarları → Gizlilik ve
   Güvenlik*).
5. Telefonu başka bir uygulama kullanmıyor: Android File Transfer, OpenMTP, MacDroid, Görüntü Yakalama, Fotoğraflar,
   Smart Switch.
6. **Kabloyu çıkar, 5 saniye bekle, ekran açıkken yeniden tak.** Bu adım diğer her şeyden fazla sorunu çözer.

## Telefon görünüyor ama boş ya da "Operation not permitted" diyor

**Belirti.** Bir birim görünüyor (ya da MTPort *Bağlanıyor…* / *Telefon algılandı: Dosya aktarımını seçin* diyor) ama
klasörler boş, Finder *Operation not permitted* veriyor ya da işçi günlüğünde
`MtpNotReady: the phone did not allow access to its storage` yazıyor.

**Neden.** USB bağlantısı kurulmuş ama telefon depolamasını henüz açmamış: ekran kilitli, *verilere erişime izin ver*
uyarısı bekliyor ya da USB modu Dosya aktarımı değil. Samsung dahil bazı telefonlar takıldıktan sonra 10–60 saniye
bekler ve her yeniden takışta tekrar sorar.

**Çözüm.** Kilidi aç, **Dosya aktarımı (MTP)** seç, uyarıyı onayla, sonra ekran açıkken kabloyu çıkarıp tak. MTPort
depolama için 90 saniyeye kadar bekler ve otomatik yeniden dener (10 saniyede bir, en fazla 6 deneme). Hepsi bittiyse
MTPort'u kapatıp yeniden aç. MTPort okunamayan bir telefon için asla boş bir birim göstermez; gösterirse lütfen hata
bildir.

## Finder'da hiçbir şey görünmüyor

* **MTPort çalışıyor mu?** Telefon yalnızca uygulama (ve agent'ı) çalışırken bağlıdır. Dock'tan aç.
* **Bağlı mı?** `mount | grep webdav` çıktısında `/Volumes/MTPort-<model>` ile biten bir satır olmalı. Varsa ama kenar
  çubuğu boşsa *Finder → Ayarlar → Kenar Çubuğu → Konumlar → **Bağlı sunucular***'ı aç. Sonra `killall Finder` dene.
* **Çıkardın.** *Çıkar*'dan sonra telefon, kabloyu çıkarıp yeniden takana kadar (ya da MTPort'ta *Bağla*'ya basana kadar)
  bilerek yeniden bağlanmaz; böylece kabloyu çekerken geri gelmez.
* **Uygulama telefonu bekliyor.** Bir önceki bölüme bak.
* Hâlâ yoksa: `mtport doctor`, `mtport devices` ve `mtport status` çalıştır.

## Kenar çubuğunda telefon adı yerine `127.0.0.1` yazıyor

İlk sürümler birimi düz bir `127.0.0.1` adresinden bağlıyordu ve Finder o adresin *sunucu adını* gösterir. Güncel
sürümler telefonun kendi adını yerel adres için (macOS mDNS servisiyle) yayınlar ve onu kullanır; kenar çubuğunda
ör. *Galaxy S21 FE* yazar. Son sürüme güncelle. Ad yayınlanamazsa (örneğin ağındaki başka bir cihazla çakışırsa) MTPort
`<ad>.localhost` sunucu adına geçer ve yine bağlar.

## "DeviceBusy: could not open an MTP session"

Telefonu başka bir program tutuyor: Android File Transfer, OpenMTP, MacDroid, Görüntü Yakalama, Fotoğraflar, MTPort'un
ikinci bir kopyası ya da eski `AndroidFinder`. `mtport doctor` bilinen çakışan süreçleri listeler. Hepsini kapat (menü
çubuğundakileri de), sonra kabloyu çıkarıp tak. macOS'un `ptpcamerad` yardımcısı PTP/kamera modundaki bir telefonu
kapabilir; bunun yerine Dosya aktarımı (MTP) seç.

## "libusb_open() failed … Operation timed out" ya da telefon USB'den kayboluyor

Telefonun USB tarafı cevap vermeyi bıraktı. Çok yoğun bir aktarımdan sonra, kablo kötüyse ya da telefonla konuşan bir
program aktarım ortasında öldürülürse olabilir. Kabloyu çıkarıp tak (başka kablo ve port dene). MTPort, takılmış bir USB
yığınını yazılımdan düzeltemez.

Önlemek için: kabloyu çekmeden önce çıkar; birimi bayt bayt okuyan araçlar çalıştırma (ör. `dd bs=1`; telefona gigabaytlar
okutur); `lsof +D /Volumes/MTPort-…` çalıştırma (tüm telefonu gezer).

## Büyük videolar geç açılıyor

macOS ağ biriminden bir dosyayı, onu arka planda indirerek ve aynı anda sunucudan bayt aralıkları isteyerek okur.
Dosyanın sonunu aşan bir aralıkta macOS istemcisi sunucunun (doğru) kısa cevabını yanlış işler ve programına fazladan
sıfır bayt verir; bu kopyaları bozar. MTPort bu yüzden böyle istekleri reddeder ve macOS, dosyanın o kısmına kendi
indirmesi ulaşana kadar bekler. Sonuç: her dosyanın *başı* ve dosya içinde kalan her okuma anında gelir, ama çok büyük
bir dosyanın **sonu** indirme oraya ulaşınca hazır olur. USB 2.0 bir telefonda ölçüldü: 62 MB video ≈ 2,5 sn,
1,6 GB video ≈ 50 sn (Quick Look önizlemesi ya da MP4 dizinini dosya sonundan okuyan bir oynatıcı o kadar bekler).
Kopyalama etkilenmez. Daha hızlı kablo/port (iki tarafta USB 3) yardımcı olur; başka çare yok, çünkü sınır macOS
istemcisindedir.

## Büyük bir klasörün ilk açılışı birkaç saniye sürüyor

Telefon dosyaları tek tek tarif eder (yaklaşık 5 ms/dosya). Yaklaşık 1800 fotoğraflı `DCIM/Camera` ilk seferde 4–8 saniye
sürer; MTPort bağlandıktan hemen sonra `DCIM/Camera`, `Download` ve `Pictures` klasörlerini arka planda okur ve
öğrendiklerini hatırlar; sonraki ziyaretler anında olur, yalnızca yeni dosyalar zaman alır. Telefonda çekilen ya da
silinen fotoğraflar birkaç saniye içinde görünür; var olan bir dosyanın *boyut/tarih* değişikliği beş dakikaya kadar
gecikebilir (dosyayı açmak ya da kopyalamak her zaman telefondan gerçek boyutu ister).

## Kaydederken, değiştirirken ya da silerken "Operation not supported" / "Operation not permitted"

Bunlar hata değil, güvenlik kurallarıdır ([docs/DATA_SAFETY.md](DATA_SAFETY.md)):

* **Üzerine yazma / üstüne kaydetme** → reddedilir. Yeni bir adla kopyala ya da eski dosyayı önce sil.
* **Yerinde düzenleme** → reddedilir. Kopyayı Mac'te düzenle, sonra yeni bir adla geri kopyala.
* **Klasörler arası taşıma** → reddedilir (MTP bunu atomik yapamaz). Kopyala, doğrula, sonra sil.
* **Dolu klasörü tek hamlede silme** → reddedilir. Önce içindekileri sil.
* **Spotlight, `chmod`, `chown`, genişletilmiş öznitelikler** → telefonda yok.

*Yeni* bir dosyayı kopyalarken "Operation not permitted" alırsan depolamanın yazılabilir olduğunu (SD kart kilitli olabilir)
ve telefonun kilidinin açık olduğunu kontrol et.

## Kopyalar orijinalden biraz büyük (fazladan sıfır baytlar)

Bu, geliştirme sürümlerinde gerçek bir hataydı: macOS'un sonunu aralık isteğiyle çektiği dosyalar sıfırlarla sonraki
1 MiB sınırına yuvarlanıyordu. Güncel sürümde düzeltildi (doğrulandı: 123 soğuk kopyanın 123'ü, aynı dosyaların düz HTTP
indirmesiyle bayt bayt aynı). Hâlâ görüyorsan eski bir sürüm çalıştırıyorsun: yeniden kur ve bir kopyanın
`shasum -a 256` değerini orijinalinkiyle karşılaştır.

## Birim adında `-1` eki var (`/Volumes/MTPort-<model>-1`)

`/Volumes` içinde aynı adlı eski, boş bir bağlantı klasörü kalmışsa macOS `-1` ekler (klasör root'a ait olduğu için yönetici
hakkı olmadan silinemez). Zararsızdır; yeniden başlatınca kaybolur.

## Çıkaramıyorum ya da bir bağlantı takılı kaldı

```bash
mtport unmount --force
diskutil unmount force /Volumes/MTPort-<model>
```

Sonra MTPort'u kapatıp yeniden başlat. Hiçbir şeyin (bir Terminal sekmesi, `lsof`, bir yedekleme programı) birimin
içinde çalışmadığından emin ol.

## Finder'da kapasite ya da boş alan yanlış görünüyor

Telefon bir WebDAV ağ birimi olarak sunulur ve Finder'ın bu birimler için kapasite gösterimi sınırlıdır. MTPort penceresi
telefonun bildirdiği kapasiteyi gösterir.

## macOS uygulamanın açılamadığını söylüyor ("tanımlanmamış geliştirici")

MTPort imzalı ya da notarize değildir. Kendi derlediğin uygulama karantinaya alınmaz ve normal açılır. Kopyalanmış bir
derleme için: sağ tık → *Aç* ya da `xattr -dr com.apple.quarantine /yol/MTPort.app`.

## Günlükler ve hata bildirimi

* İşçi günlükleri: `~/Library/Application Support/MTPort/runtime/mount-<cihaz>.log`. Sayaçlar ve durum içerir, dosya adı
  ya da seri numarası içermez; yine de yapıştırmadan önce oku.
* HTTP düzeyinde iz (içinde dosya adları geçer, körü körüne paylaşma):
  `MTPORT_WEBDAV_TRACE=1 mtport mount --read-only`, sonra işçi günlüğünü oku.
* Lütfen `mtport doctor` ve `mtport status` çıktısını, telefon modelini ve Android sürümünü, macOS sürümünü ve
  kablo/portu ekle. [Hata bildirimi şablonu](../.github/ISSUE_TEMPLATE/bug_report.md) ihtiyacımız olan her şeyi listeler.

## Yalnızca deneysel `--transport fskit` (macFUSE) kullanıyorsan

* macFUSE birimleri **Finder kenar çubuğunda listelenmez** (bilinen bir macFUSE sınırı); `/Volumes` içinden aç.
* macFUSE ≥ 5.4.0 kur ve dosya sistemi uzantısını etkinleştir (*Sistem Ayarları → Genel → Oturum Açma Öğeleri ve
  Uzantılar → Dosya Sistemi Uzantıları*). Eski sürümler küçük yazmaları bozabilir.
* macFUSE'un `local` bağlama seçeneğini asla ekleme: yeniden başlatmaya kadar ayrılamayan bağlantılar üretti.
* Çökme sonrası takılı kalan bir bağlantı: `diskutil unmount force /Volumes/<ad>`, olmazsa Mac'i yeniden başlat.
