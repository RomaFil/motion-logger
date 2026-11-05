# Запис образу на картку і перше ввімкнення

Образ `sdcard.img` (збирається за `README.md` або `scripts/build.sh build`) пишуть на **окрему** microSD-картку, не на ту, де лежить Raspberry Pi OS. Образ займає 152 МБ (boot 32 МБ + rootfs 120 МБ), тож підійде картка більшого розміру; залишок картки образ не використовує.

## 1. Перевірити образ

    sha256sum sdcard.img          # Linux/WSL
    certutil -hashfile sdcard.img SHA256     # Windows (cmd/PowerShell)

Сума має збігатися з тією, що записана в `CHANGELOG.md` для цього образу.

## 2. Записати на картку

Образ уже містить MBR з двома розділами (boot FAT і rootfs ext4), тому пишеться **цілком на пристрій**, а не в розділ.

- **Linux/WSL:** `sudo dd if=sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress`. Перевірте `X` командою `lsblk`: помилка зітре чужий диск.
- **Windows:** будь-яка програма, що пише «сирий» образ (Raspberry Pi Imager: *Choose OS → Use custom*; balenaEtcher). Обираєте картку, а не системний диск. Налаштування «Customisation» в Imager вимкніть: вони змінюють файли Raspberry Pi OS, які в нашому образі інші (нашу картку цим не перевірено, загальне знання).

Після запису Windows бачить диск `boot` (FAT): там `config.txt`, `cmdline.txt`, `Image`, `overlays/`, а в образах із U-Boot ще `u-boot.bin` і `boot.scr`. Розділ rootfs (ext4) Windows не показує, це нормально.

## 3. Wi-Fi

Образ шукає мережі у файлі `wifi.txt` у корені диска `boot`. Збірка кладе його з `~/workspace/wifi.secret` на машині збірки; якщо його не було, створіть вручну за зразком `board/motion_logger/wifi.txt.example`:

    SSID=назва_мережі
    PSK=пароль_мережі

Мережу можна змінити пізніше: вставити картку в ПК і відредагувати `wifi.txt`. Такий образ містить пароль у відкритому вигляді, тож `.img` з паролем нікому не передавайте (деталі в `docs/wifi.md`).

## 4. Перше ввімкнення

1. Вставте картку в Pi 5, подайте живлення.
2. Wi-Fi піднімається приблизно за 40 с, застосунок `motion_logger` стартує приблизно через 15 с після старту ядра (вимірювання 02.11.2026). Плату шукайте в роутері за ім'ям `motion-logger` або за MAC; `ping` до її адреси має відповідати.
3. **SSH.** Чистий образ ключа не має. Користувач `root`, навчальний пароль задано в `configs/motion_logger_defconfig` (`BR2_TARGET_GENERIC_ROOT_PASSWD`):

       ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL root@<адреса>     # Windows
       ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@<адреса>   # Linux

   Нова картка дає плату з тією ж адресою, але новим ключем хоста, тому звичайний `known_hosts` скаржитиметься `REMOTE HOST IDENTIFICATION HAS CHANGED`. Опції вище це обходять, не чіпаючи вашого `known_hosts`. Щоб заходити без пароля, додайте свій публічний ключ у `/root/.ssh/authorized_keys` на платі.
4. Перевірка, що все працює:

       /etc/init.d/S99motion_logger status      # 0 = запущено
       cat /sys/bus/iio/devices/iio:device*/name   # mpu6050, adxl345
       tail -n 3 /data/motion.jsonl            # рядок log, далі accel, accel2, gps, лог росте

   Лог лежить на tmpfs і зникає після вимкнення живлення.

## 5. Консоль на UART (кабель JST-SH до `DBG1`)

USB-UART 3,3 В, 115200 8N1 (`picocom -b 115200 /dev/ttyUSB0` або PuTTY), TX адаптера на RX плати і навпаки, спільна земля. Повідомлення ядра йдуть на роз'єм JST (`ttyAMA10`), на `DBG1` лише вхід `getty` і вивід автозапуску. У образі з U-Boot (`kernel=u-boot.bin` у `config.txt`) на цій консолі має з'явитись банер U-Boot, а клавіша під час затримки зупиняє автозавантаження й дає `U-Boot>` (перевірка банера з кабелем ще чекає, див. `docs/uboot.md`).

## 6. Якщо не стартує

| Симптом | Що перевірити |
|---|---|
| Плата не з'являється в мережі за 2 хв | `wifi.txt` на диску `boot` (назва, пароль, WPA2); діагностика без консолі: файл `wifi-debug.log` на тому ж диску (його пише `S41wifi` через 25 і 90 с після старту) |
| Нічого не відбувається, діод Pi не блимає | картка записана не як «сирий» образ, а як файл на розділ; запишіть заново |
| З U-Boot не завантажується | на диску `boot` мають бути `u-boot.bin`, `boot.scr`, `Image`, у `config.txt` `kernel=u-boot.bin`; для відкату на запуск без U-Boot змініть цей рядок на `kernel=Image` |
| `Permission denied` по SSH | пароль із `configs/motion_logger_defconfig`; ключ додається вручну (п. 3) |
| Застосунок не пише лог | `/etc/init.d/S99motion_logger status`; параметри в `/etc/default/motion_logger` |

## 7. Золота копія

Робочий образ зберігають у двох місцях під ім'ям з датою, комітом і SHA-256 у `CHANGELOG.md`; старі образи не перезаписують. Перед будь-яким експериментом залишають картку чи файл, до якого можна повернутись за хвилини.
