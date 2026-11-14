# p3p-ios

Bản port native (dịch tĩnh, không giả lập) của *Persona 3 Portable* (ULUS-10512) cho iPhone,
dựng trên dự án mã nguồn mở [P3P3DS](https://github.com/endercodezz/P3P3DS) (MIT).

**Repo này phải để private**: `game/eboot.elf` là file thực thi đã giải mã từ bản game của bạn,
và IPA build ra chứa mã sinh từ file đó. Chỉ dùng cho cá nhân, không chia sẻ.

## Cấu trúc

| Đường dẫn | Nội dung |
|---|---|
| `runner/live_runner.*` | Lớp chạy thời gian thực (C++ thuần): luồng game, khung hình, âm thanh, nút bấm, hộp thoại save |
| `runner/host_test.cpp` | Chương trình thử không màn hình (Linux/macOS) |
| `ios/main.mm`, `ios/Info.plist` | Vỏ iOS: màn hình, nút cảm ứng, âm thanh, danh sách save |
| `scripts/build_ios.sh` | Build IPA chưa ký trên macOS (GitHub Actions) |
| `scripts/gen_make.py` | Sinh Makefile cho bản host và bản iOS |
| `upstream.ref` | Commit P3P3DS được dùng |
| `game/eboot.elf` | File thực thi đã giải mã (SHA-256 `be2abbd4…49db`) |

## Build

Đẩy lên nhánh `main` hoặc `claude/**` thì GitHub Actions tự build; IPA nằm trong artifact `P3P-ipa`.
Log rút gọn của lần build gần nhất được đẩy lên nhánh `ci-logs`.

## Cài và chơi

1. Sideload `P3P-unsigned.ipa` (Sideloadly/SideStore).
2. Mở app một lần, rồi vào Tệp (Files) → Trên iPhone → Persona 3 P, chép file ISO vào đó.
3. Mở lại app. Save nằm trong `ms0/`, `report.txt` ghi tốc độ và lý do dừng.

## Hạn chế hiện tại

- Chưa có video mở đầu (tự bỏ qua).
- Dựng hình bằng CPU ở độ phân giải gốc PSP 480×272.
- Upstream mới kiểm chứng vài ngày đầu của game và trận đầu tiên.
