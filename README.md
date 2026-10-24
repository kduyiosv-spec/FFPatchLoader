# FFPatchLoader

Loader dylib cho Unity game dùng engine hot-patch **IFix** (Tencent) — chạy patch `assembly-csharp-patch.bytes` (UnityFS AssetBundle chứa IL patch) **hoàn toàn bên trong VM của game**.

> Không hook native. Không patch `__TEXT`. Không vướng PPL / codesign trên iOS 26–27.
> Toàn bộ code trong patch (menu, ESP, aimbot...) do chính IFix VM của game thông dịch —
> loader chỉ có nhiệm vụ **đưa đúng file patch đến đúng chỗ**.

## Nguyên lý

Game có sẵn luồng hot-update: lúc khởi động nó mở file codepatch (AssetBundle
`assembly-csharp-patch.bytes`), lấy TextAsset và nạp bytes vào IFix VM. Trên iOS file
này nằm trong app bundle (đã seal) → không thể thay.

FFPatchLoader giải quyết bằng 2 cơ chế **PPL-safe**:

1. **dyld interposition** (compile-time `__DATA,__interpose`): mọi lời gọi
   `open/openat/fopen/access/faccessat/stat/lstat` trong app đều đi qua loader.
   Khi path chứa `assembly-csharp-patch` hoặc `remote_patch` ở chế độ **đọc** →
   tự động trỏ tới file patch trong `Documents`.
2. **GOT rebinding** (`__DATA_CONST`, đổi protection bằng `vm_protect` rồi trả về):
   fallback nếu interpose không chặn được các bind trước main — chỉ ghi đè các slot
   còn trỏ thẳng vào libSystem.

Ngoài ra loader còn **copy file patch** vào 3 vị trí trong `Documents` để game tìm thấy
bằng cả đường dẫn "hot-update" của nó:

```
Documents/assembly-csharp-patch.bytes          (canonical)
Documents/remote_patch.bytes
Documents/codepatch/assembly-csharp-patch.bytes
```

Chế độ **ghi** không bao giờ bị redirect (game download update bình thường).

## Cài đặt

1. **Inject dylib**: chép `FFPatchLoader.dylib` vào `<App.app/Frameworks/` và thêm
   load command `@executable_path/Frameworks/FFPatchLoader.dylib` vào binary chính
   (insert_dylib / IPT / tool bạn đang dùng). Vị trí trong danh sách load command
   không quan trọng — interpose + GOT rebind đều hoạt động.
   Cài lại app bằng **TrollStore** (TrollStore sẽ ký lại toàn bộ).
2. **Đặt patch file**: copy `assembly-csharp-patch.bytes` vào
   `Documents` của app (Filza: `/var/mobile/Containers/Data/Application/<APP>/Documents/`).
   - File có tên bất kỳ bắt đầu bằng `assembly-csharp-patch` hoặc `remote_patch`
     đều được nhận (VD cả tên có đuôi hash dài khi tải về) — loader tự chọn file **mới nhất**.
3. **Mở game**. Xong.

## Kiểm tra

Mỗi lần khởi động, loader ghi log vào `Documents/ffpatch_loader.log`:

```
[00:12:03] === FFPatchLoader v1.0.0 — PPL-safe IFix codepatch loader ===
[00:12:03] patch source: .../Documents/assembly-csharp-patch.bytes (171008 bytes, magic UnityFS ✓)
[00:12:03] canonical copy OK -> ...
[00:12:09] open(".../codepatch/assembly-csharp-patch.bytes", 0x0) -> REDIRECT ...
[00:12:09] ---- 5s summary: opens(patch)=3 stats(patch)=5 redirects=8 ----
```

- Có dòng `REDIRECT` → game đã đọc patch của bạn.
- Log báo `game never touched any patch path` → game nạp codepatch bằng
  `Resources.Load` (data pack trong `resources.assets`) — cần phương án v2,
  gửi lại file log này.

## Cập nhật patch

Thả file patch mới vào `Documents` rồi **restart app** (IFix chỉ nạp lúc khởi động).
Loader so sánh mtime, tự lấy bản mới nhất.

## Build

```
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=14.0 \
  -dynamiclib -O2 -Wno-deprecated-declarations \
  -Wl,-install_name,@executable_path/Frameworks/FFPatchLoader.dylib \
  src/ffpatch.c src/gotbind.c -o FFPatchLoader.dylib
ldid -S FFPatchLoader.dylib
```

CI (GitHub Actions) tự build khi push vào `main`, phát hành dylib ở phần Releases.

## Ghi chú kỹ thuật

- `custom_mach_vm_protect` = raw SVC trap -14 (không include `mach/mach_vm.h` —
  header này `#error` trên iOS SDK).
- GOT chỉ bị ghi khi slot **vẫn trỏ tới symbol thật của libSystem** (tức interpose
  đã hụt). Slot đã trỏ tới interposer thì giữ nguyên.
- Ảnh hệ thống trong dyld shared cache bị bỏ qua (đã seal, và không phải nguồn
  file-access của game).
- Không có bất kỳ write nào vào trang thực thi → an toàn trên thiết bị iOS 27
  có PPL (kịch bản v78/v80 của PTG đã chứng minh patch `__TEXT` = SIGKILL
  `KERN_PROTECTION_FAILURE`).
