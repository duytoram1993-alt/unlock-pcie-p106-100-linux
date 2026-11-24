[English](README.md) | [Tiếng Việt](README.vi.md)

# unlock-pcie-p106-100-linux

Mở khóa **PCIe Gen2 (`5.0 GT/s`)** cho card đào **NVIDIA P106-100** (`10DE:1C07` / `10DE:1C06`) và các card mining Pascal khác (**P106-090 `10DE:1C09`**, **P104-100 `10DE:1B87`**) trên **Linux** với driver NVIDIA độc quyền.

> **Nếu bạn gặp lỗi tụt FPS nặng trong game (~15–45 FPS):** Lỗi đó do bảng Restricted SKU trong driver NVIDIA — xử lý độc lập bằng repo đi kèm [remove-black-list-nvidia-linux](https://github.com/duytoram1993-alt/remove-black-list-nvidia-linux). Hai repo tách biệt hoàn toàn, có thể dùng riêng hoặc kết hợp cùng nhau.

---

## Kết quả kiểm chứng thực tế

Đã xác nhận thực tế trên **Fedora 45 KDE Plasma** (kernel `7.3.3-300.fc45.x86_64`, driver `akmod-nvidia-580xx 580.196.07`, Gigabyte **P106-100 `10DE:1C07`**, Intel Core i7-3770K + main H61):

| Thanh ghi / Chỉ số | Trước (Khởi động mặc định) | Sau (`p106-gen2.service`) |
|---|---|---|
| **GPU `0000:01:00.0` `LNKSTA`** | `0x1081` (`2.5 GT/s x8`) | **`0x1082` (`5.0 GT/s x8` — Gen2)** |
| **Root Port `0000:00:01.0` `LNKSTA`** | `0x1081` (`2.5 GT/s x8`) | **`0x1082` (`5.0 GT/s x8` — Gen2)** |
| **`LNKCAP2` / `LNKCTL2`** | `0x00000002` / `TLS=1` | **`0x00000006` / `TLS=2`** |
| **`0x2241C` (`PUNITS.PCI`)** | `0x00000000` | **`0x00000081`** (mở `CAP2=0x06` trong ~13 µs) |
| **Độ ổn định / Lỗi** | — | **0 lỗi Xid, 0 lần tụt Gen1** (tự động lên Gen2 lúc boot) |

> **Lưu ý về độ rộng làn (`x4` / `x8` / `x16`):** Công cụ này mở khóa **tốc độ liên kết** (`2.5 GT/s → 5.0 GT/s`, gấp đôi băng thông trên mỗi làn). Số làn vật lý (`x4`/`x8`/`x16`) do mạch card và khe mainboard quyết định, không đổi bằng phần mềm được.

---

## Cơ chế hoạt động (Điểm mấu chốt `0x2241C` trên P106-100)

Để chạy được PCIe Gen2 ổn định trên Linux cần giải quyết **2 lớp khóa**:

1. **Mở khai báo khả năng Gen2 qua BAR0 (`gen2kr_p106.ko` gắn `kretprobe` lên `nv_pci_probe`):**
   - Khi mới bật máy, card đào Pascal chỉ khai báo `2.5 GT/s` (`LNKCAP2 = 0x00000002`, `EXP_HEAD.VER = 1`).
   - Đặc biệt trên các lô card có `0x8841C` mặc định là `0xC0B46500` (như Gigabyte P106-100 `10DE:1C07`), để `LNKCAP2` lật từ `0x02 → 0x06` cần ghi đủ bộ:
     - XP3G override (`0x8E110=1`, `0x8E11C=4`, `0x8E120=0`, `0x8E12C=0x200000`)
     - `0x88610 = (v & ~(1<<12)) | 1` và `0x8860C |= 1` (chuyển `EXP_HEAD.VER` từ `1 → 2`)
     - **`0x2241C` (`PUNITS.PCI`) `|= 0x81`** (chọn tốc độ Gen2 trực tiếp, có kiểm tra an toàn chống poison/bit cao bất thường)
     - `0x8841C` (`bit15=1`, `bit11=1`, xóa cổng hạ tốc `[14:13]=0`, `bit12=0`; có fallback `bit13=1`)
     - `0x8C040` `SPEED[19:18] = 1` (`5.0 GT/s` theo mã Pascal — **TUYỆT ĐỐI KHÔNG** ghi bit0 `COMMIT`)
     - `0x8872C = 6` → `LNKCAP2` chuyển `0x00000006` → đặt `LNKCTL2.TLS = 2` cả 2 đầu → phát lệnh Retrain từ Root Port.
2. **Chặn driver NVIDIA tự hạ tốc về Gen1 (`01-patch-driver-gen2.py`):**
   - Mỗi lần ứng dụng khởi tạo CUDA/Vulkan/GL (`rm_init_adapter`), NVIDIA RM gọi `set_link_speed(mode=1)` để ép `0x8C040.SPEED=2` (Gen1).
   - Patch **2 byte** dịch kiểm tra biên đầu hàm (`lea -1` → `lea -2`, `cmp $2` → `cmp $1`) để mọi lời gọi `mode==1` thoát êm với `NV_OK`, giữ link ổn định ở Gen2 mà không cần vòng lặp đè liên tục.

---

## Yêu cầu chuẩn bị

- **Driver NVIDIA độc quyền (`proprietary/closed`)** (hỗ trợ `.ko`, `.ko.xz` CRC32, `.ko.zst`, `.ko.gz`).
- **Kernel headers & bộ công cụ biên dịch** cho `gen2kr_p106.ko`:
  - Fedora / RHEL: `sudo dnf install kernel-devel gcc make psmisc`
  - Debian / Ubuntu: `sudo apt install linux-headers-$(uname -r) build-essential psmisc`
  - Arch Linux: `sudo pacman -S linux-headers base-devel psmisc`
- **Tắt Secure Boot** và kernel hỗ trợ `CONFIG_KRETPROBES=y` (mặc định bật trên các distro phổ biến).
- **Xuất hình qua iGPU hoặc card phụ** (khuyên dùng, vì P106-100 không có cổng xuất hình).

---

## Hướng dẫn từng bước

```bash
git clone https://github.com/duytoram1993-alt/unlock-pcie-p106-100-linux.git
cd unlock-pcie-p106-100-linux
```

### Bước 1: Biên dịch module & kiểm tra dấu vân tay BAR0 (chỉ đọc)

```bash
# Biên dịch gen2kr_p106.ko (chạy user thường, không cần sudo)
bash 02-push-gen2.sh build

# Thăm dò chỉ đọc: kiểm tra BAR0_FINGERPRINT=MATCH (KHÔNG ghi thanh ghi nào)
sudo python3 00-probe-bar0.py
```

**DỪNG LẠI NGAY nếu kết quả báo `BAR0_FINGERPRINT=MISMATCH`!** Chỉ tiếp tục khi thấy `BAR0_FINGERPRINT=MATCH`.

### Bước 2: Áp patch 2 byte chống hạ tốc vào `nvidia.ko`

```bash
bash 01-patch-driver-gen2.sh check
sudo bash 01-patch-driver-gen2.sh apply
```

*(Nếu distro của bạn nhúng `nvidia.ko` trong initramfs — ví dụ Ubuntu/Debian/Arch bật early KMS — hãy chạy lại `update-initramfs -u` hoặc `mkinitcpio -P`.)*

### Bước 3: Cài systemd service tự động lúc khởi động & reboot

Chạy qua boot service giúp đẩy Gen2 **trước khi** `nvidia-powerd` hoặc Display Manager mở `/dev/nvidia*` (tránh treo kernel khi unbind):

```bash
sudo bash 03-install-boot-service.sh install
sudo reboot
```

### Bước 4: Kiểm tra tốc độ PCIe thực tế

> **KHÔNG dùng `nvidia-smi` để kiểm tra PCIe Gen!** `nvidia-smi` hiển thị giá trị cache của driver và vẫn có thể báo Gen1 khi phần cứng đã chạy Gen2. Luôn kiểm tra bằng thanh ghi hoặc sysfs:

```bash
sudo bash 02-push-gen2.sh status
# Hoặc đọc trực tiếp từ sysfs:
cat /sys/bus/pci/devices/0000:01:00.0/current_link_speed
# Kết quả chuẩn: 5.0 GT/s PCIe
```

---

## Các rào chắn an toàn được tích hợp

1. **Tuyệt đối không ghi bit0 `COMMIT` của `0x8C040`:** Ghi `COMMIT` từ bên ngoài làm card rơi khỏi bus PCIe (trạng thái `ZOMBIE`, phải tắt bật máy). Mọi đường ghi `SPEED` đều kiểm tra kép `lk != 0xFFFFFFFF && (lk & 1) == 0`.
2. **Không bao giờ unbind khi GPU đang bận:** `02-push-gen2.sh` tự động chặn nếu có tiến trình đang giữ `/dev/nvidia*`, tránh treo tiến trình ở trạng thái D trong kernel.
3. **Cổng kiểm tra dấu vân tay BAR0:** `gen2kr_p106.ko` kiểm tra `BAR0 0x88084 == LNKCAP` và `0x880A4 == LNKCAP2` trước khi làm bất kỳ thao tác ghi nào.
4. **Không spam Retrain:** `guard_fn` tự dừng ngay nếu `LNKCAP2 != 0x06` hoặc Retrain thất bại 5 lần liên tiếp; `02-push-gen2.sh` tự gỡ module nếu không lên được Gen2.
5. **Tương thích SELinux:** Tự động cài `gen2kr_p106.ko` vào `/lib/modules/<kver>/extra/p106-gen2/` (`modules_object_t`) và script vào `/usr/local/sbin/` (`bin_t`).

---

## Bảo trì & Gỡ bỏ hoàn toàn

### Sau khi cập nhật kernel hoặc driver NVIDIA mới
```bash
sudo bash 01-patch-driver-gen2.sh apply
bash 02-push-gen2.sh build
sudo bash 03-install-boot-service.sh install
sudo reboot
```

### Gỡ bỏ / Hoàn tác toàn bộ
```bash
sudo bash 02-push-gen2.sh stop
sudo bash 03-install-boot-service.sh uninstall
sudo bash 01-patch-driver-gen2.sh revert
sudo reboot
```

---

## Nguồn tham khảo & Giấy phép

Dự án tham khảo và phát triển từ các nghiên cứu mở khóa Pascal trên Linux/Windows:
- [WJNKAC/nvidia-p104-unlock-gen2-for-linux](https://github.com/WJNKAC/nvidia-p104-unlock-gen2-for-linux) — Kiến trúc `kretprobe` trên Linux (`gen2kr.c`), patch 2 byte `set_link_speed`, và kỷ luật an toàn
- [DaZuiZZZZ/P106-Unlock-Gen2](https://github.com/DaZuiZZZZ/P106-Unlock-Gen2) — Kiểm tra dấu vân tay BAR0 mirror cho P106-100 / P106-090
- `skbskhud/p104-gen2-unlock` — Ma trận trạng thái phần cứng `0x2241C` (`PUNITS.PCI`), `0x8841C[14:13]` và `0x8872C`

Phát hành theo giấy phép **GNU General Public License v2.0** ([LICENSE](LICENSE)). Người dùng tự chịu trách nhiệm khi thao tác trực tiếp trên thanh ghi PCIe/BAR0.
