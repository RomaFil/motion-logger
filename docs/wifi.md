# Wi-Fi у власному образі

Плата Pi 5 стоїть без Ethernet, тому образ сам підіймає `wlan0` і дає SSH (dropbear) по Wi-Fi.

## Що за що відповідає

| Частина | Де |
|---|---|
| Драйвер `brcmfmac` (модуль ядра) | `rootfs_overlay/etc/modules-load.d/motion_logger.conf` |
| Прошивка радіочипа CYW43455 | пакет `linux-firmware` (`BRCM_BCM43XXX`) у `defconfig` |
| NVRAM Pi 5 | симлінк `lib/firmware/brcm/brcmfmac43455-sdio.raspberrypi,5-model-b.txt` на файл Pi 4 (той самий чип; у `linux-firmware` файлу саме для Pi 5 немає) |
| Підключення до мережі | `wpa_supplicant` (nl80211), скрипт `etc/init.d/S41wifi` |
| Адреса | `udhcpc` з BusyBox, ім'я хоста для роутера `motion-logger` |
| Країна | `COUNTRY=UA` у `wifi.txt`, база `wireless-regdb` |

## Де лежать пароль і назва мережі

Не в git і не в кореневій файловій системі образу. Це файл `wifi.txt` у корені розділу **boot** на microSD
(FAT, у Windows це диск з `config.txt`). Зразок: `board/motion_logger/wifi.txt.example`. Робочий конфіг
`wpa_supplicant` скрипт щоразу генерує у `/run` (tmpfs).

- **Збірка:** `post-image.sh` кладе у boot-розділ образу файл `~/workspace/wifi.secret` з машини збірки
  (або шлях із змінної `WIFI_SECRET`). Нема файлу: образ збереться без `wifi.txt` і напише попередження.
- **Змінити мережу без перезбірки:** вставити картку в ПК і відредагувати `wifi.txt` на диску boot, або на
  платі `vi /boot/wifi.txt`, потім `/etc/init.d/S41wifi restart`.
- **Запасні мережі:** `SSID2`/`PSK2` (до `SSID4`), наприклад точка доступу телефону для показу.
- Образи, зібрані з `wifi.secret`, містять пароль у відкритому вигляді на boot-розділі. Не віддавати такі
  `.img` нікому; для передачі збирати без `wifi.secret` або стерти `wifi.txt` з образу.

## Перевірка на платі

```sh
/etc/init.d/S41wifi status        # PID wpa_supplicant і IPv4-адреса wlan0
wpa_cli -i wlan0 status           # wpa_state=COMPLETED, ssid, ip_address
wpa_cli -i wlan0 scan_results     # які мережі бачить плата
dmesg | grep -i brcmfmac          # завантаження прошивки (помилка -2 = нема файлу в /lib/firmware/brcm)
grep -E "wpa_supplicant|udhcpc" /var/log/messages
```

## Діагностика без консолі

Через 25 і 90 с після старту `S41wifi` дописує знімок стану (модулі, `dmesg` про brcmfmac, адреса, `wpa_cli status`,
список мереж, журнал) у `/boot/wifi-debug.log`. Файл видно з Windows, коли картку вставлено в ПК: консоль і Ethernet
не потрібні. Вимкнути: `DEBUG=0` у `wifi.txt`.

## Обмеження

- WPA3 у `wpa_supplicant` не ввімкнено (це тягне OpenSSL). Мережа WPA2 або змішана WPA2/WPA3 працює.
- У паролі й назві мережі не можна `"` і `\`.
- Якщо `/boot/wifi.txt` нема або `wlan0` не з'явився, завантаження не зупиняється, скрипт пише причину в консоль.
