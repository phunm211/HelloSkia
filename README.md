# HelloSkia: So sánh và Đồng bộ Hiển thị Font giữa Android TextView và Skia

Dự án này thực hiện hiển thị so sánh trực quan theo thời gian thực (2 cột, 3 dòng) giữa **Android TextView (HWUI)** và **Skia (OpenGL Backend Textures)** trên thiết bị thật (Samsung Galaxy S20 FE 5G - Android 13 OneUI 5.1).

---

## 1. Cấu trúc Giao diện So sánh

* **Cột trái**: 3 `TextView` tiêu chuẩn của Android (Normal, Bold, Bold Italic).
* **Cột phải**: 3 OpenGL Backend Textures được Skia render độc lập offscreen (`WrapBackendTexture`), sau đó vẽ trực tiếp lên `SurfaceView` thông qua zero-copy `BorrowTextureFrom`.
* **Nội dung mỗi ô (4 dòng)**:
  1. **Latin (English)**: `Hello`
  2. **Korean (Tiếng Hàn)**: `안녕하세요`
  3. **Color Emoji**: `😀🎉🚀`
  4. **Hebrew (Tiếng Do Thái)**: `שלום`

---

## 2. Các vấn đề sai khác font ban đầu & Nguyên nhân cốt lõi

### 2.1. Vấn đề 1: Trọng số Bold và Font File không đồng nhất
* **Hiện tượng**: Ban đầu, khi hiển thị dòng Bold/Bold Italic, chữ bên TextView trông khác biệt rõ rệt so với bên Skia.
* **Nguyên nhân phát hiện qua Logcat & hệ thống**:
  * Khi `TextView` có `fontFamily="sans-serif"` và `textStyle="bold"`, trên Android/Samsung nó **không** nạp file font `Roboto_700wght.ttf` của nhà thiết kế font.
  * Thay vào đó, Android dùng font thường (`weight=400`, `RobotoStatic-Regular.ttf`) và áp dụng **Synthetic Bold** qua `TextPaint.setFakeBoldText(true)`.
  * Trong khi đó, Skia nếu yêu cầu style Bold (`SkFontStyle::Bold()`) sẽ cố resolve ra file `Roboto_700wght.ttf` hoặc `SEC-Bold.ttf`. Do đó đường nét (contours, curves, apertures) của 2 bên hoàn toàn khác nhau.
* **Cách khắc phục**:
  * Đưa Skia về cùng cơ chế với Android: luôn nạp typeface cơ sở là `sans-serif` Regular (`SkFontStyle::Normal()`), sau đó áp dụng giả lập bold tương thích với `setFakeBoldText(true)`.

---

### 2.2. Vấn đề 2: Sai khác độ nở nét của Fake Bold (chữ "e" bị hẹp lỗ)
* **Hiện tượng**: Dù cùng fake bold, chữ `e` của Skia bị nở dày hơn, khoảng trống bên trong (inner counter) bị bóp hẹp lại so với TextView.
* **Nguyên nhân**:
  * Skia dùng `font.setEmbolden(true)`, bên dưới gọi FreeType `FT_GlyphSlot_Embolden` với tỉ lệ nở outline cố định là $\frac{1}{24} \approx 4.16\%$.
  * Trong khi đó, Android HWUI thực hiện fake bold bằng kỹ thuật Stroke-and-Fill:
    $$\text{strokeWidth} = \frac{\text{textSize}}{30.0f} \approx 3.33\%$$
    với `kRound_Join` và `kRound_Cap`.
* **Cách khắc phục**:
  * Thay thế `font.setEmbolden(true)` trong Skia bằng:
    ```cpp
    if (isBold) {
        textPaint.setStyle(SkPaint::kStrokeAndFill_Style);
        textPaint.setStrokeWidth(globalTextSizePx / 30.0f);
        textPaint.setStrokeJoin(SkPaint::kRound_Join);
        textPaint.setStrokeCap(SkPaint::kRound_Cap);
    }
    ```

---

### 2.3. Vấn đề 3: Sai khác ở đuôi chữ "e" (Hinting / Grid-Fitting)
* **Hiện tượng**: Ở dòng Normal (không bold), đuôi chữ `e` của TextView đo được $93 \times 114$ px, trong khi Skia là $96 \times 117$ px (lệch ~3px), đuôi chữ TextView có cảm giác bị thẳng đứng hơn một chút.
* **Nguyên nhân**:
  * Hai file font `RobotoStatic-Regular.ttf` và `SEC-Regular.ttf` có tọa độ vector của 31 điểm glyph `e` **trùng khớp nhau 100%**.
  * Sự khác nhau đến từ **Hinting (Grid-Fitting)**: Skia mặc định bật `SkFontHinting::kNormal`, ép các điểm vector theo lưới pixel. Ngược lại, Android TextView khi vẽ text kích thước lớn sẽ tắt Hinting và bật `LinearMetrics`.
* **Cách khắc phục**:
  ```cpp
  font.setHinting(SkFontHinting::kNone);
  font.setLinearMetrics(true);
  ```

---

### 2.4. Vấn đề 4: Đồng bộ độ nghiêng (Slant / Italic)
* **Hiện tượng**: Khi dòng 3 áp dụng Italic, độ nghiêng cần khớp hoàn toàn.
* **Giải pháp**: Cả 2 bên đều áp dụng hệ số skew:
  * Android: `paint.getTextSkewX() == -0.25f`
  * Skia: `font.setSkewX(isItalic ? -0.25f : 0.0f)`

---

## 3. Cơ chế Dynamic Font Fallback (Korean, Emoji, Hebrew)

Do một file font Latin cơ sở (`sans-serif` / `RobotoStatic-Regular`) không chứa ký tự tiếng Hàn, Emoji hay tiếng Do Thái, việc gọi `drawString` trực tiếp sẽ dẫn đến ký tự trống/tofu (`\uFFFD`).

### Cách triển khai trong Skia:
1. **UTF-8 Streaming Parser**:
   * Hàm `nextUtf8Char` giải mã từng codepoint Unicode (`SkUnichar`, 1 đến 4 byte).
2. **Dynamic Fallback tự động qua `SkFontMgr_Android` (Không cần tự đọc XML)**:
   * **Cơ chế ngầm**: Khi khởi tạo `SkFontMgr_New_Android(nullptr)`, Skia tự động tìm và đọc cấu hình font hệ thống Android (`/system/etc/fonts.xml`, `/data/fonts/`, v.v.) và tự xây dựng đồ thị Fallback Fonts theo thứ tự ưu tiên của chính máy đó.
   * **Không cần hardcode hay tự parse XML**: Nhà phát triển không phải tự viết parser XML hay lo sợ format file XML bị thay đổi qua các bản Android. Skia tự động tương thích với mọi hãng sản xuất (Samsung, Pixel, Xiaomi, Oppo...):
     * Khi gọi `androidFontMgr->matchFamilyStyleCharacter("sans-serif", style, nullptr, 0, u)`:
       * Skia duyệt chuỗi fallback của Android, kiểm tra bảng `cmap` của các file font hệ thống.
       * Tự động tìm thấy và nạp đúng font file hỗ trợ codepoint `u` tại runtime:
         * Tiếng Hàn $\rightarrow$ `SamsungKorean-Regular.ttf` / `SECCJK-Regular.ttc`
         * Emoji $\rightarrow$ `NotoColorEmoji.ttf`
         * Tiếng Do Thái $\rightarrow$ `NotoSansHebrew-Regular.ttf`
3. **Phân nhóm Run trong `SkTextBlob`**:
   * Gom các ký tự liên tiếp có cùng `SkTypeface` vào từng run của `SkTextBlobBuilder`.
   * Tính toán khoảng cách tọa độ `pos` chính xác dựa trên `font.getWidths`.
4. **Xử lý tránh hiện tượng Double Bold (Chữ Hàn & Hebrew bị quá đậm)**:
   * Khi gọi `matchFamilyStyleCharacter` với `style = Bold`, Android Font Manager sẽ tìm và trả về file font **chính chủ Bold** (ví dụ: `NotoSansHebrew-Bold.ttf`, `SamsungKorean-Bold.ttf`).
   * Nếu Skia tiếp tục áp dụng thêm stroke giả lập bold (`textPaint.setStrokeWidth`) lên một font vốn đã là file Bold thật, glyph sẽ bị cộng dồn 2 lần độ dày (**Double Bold**), khiến chữ tiếng Hàn và Hebrew đậm gấp đôi so với TextView.
   * **Cách xử lý**:
     * Luôn truy vấn fallback font với `SkFontStyle::Normal()`.
     * Kiểm tra `tf->isBold()`: Nếu font trả về đã là bold tự thân thì không stroke thêm, nếu là font regular thì mới áp dụng stroke fake bold của TextView.
5. **Hỗ trợ riêng cho Color Emoji**:
   * Nhận diện typeface thuộc họ `Emoji`.
   * Emoji là dạng bitmap đa màu (CBDT/CBLC), cần vẽ bằng `SkPaint` không chứa color filter hay màu đơn sắc đè lên để giữ trọn màu sắc gốc.
6. **Căn giữa khối Multiline**:
   * Đo đạc chiều rộng từng dòng qua `measureLineWidth`.
   * Tính `lineHeight = metrics.fDescent - metrics.fAscent + metrics.fLeading` để căn giữa hoàn toàn theo cả 2 trục giống như `gravity="center"` của `TextView`.

---

## 4. Danh sách các file được cập nhật

* [`app/src/main/res/values/strings.xml`](file:///home/phu/Repo/HelloSkia/app/src/main/res/values/strings.xml): Thêm chuỗi mẫu 4 dòng đa ngôn ngữ `sample_cell_text`.
* [`app/src/main/res/layout/activity_main.xml`](file:///home/phu/Repo/HelloSkia/app/src/main/res/layout/activity_main.xml): Gán `@string/sample_cell_text`, điều chỉnh kích thước về `28sp` để hiển thị vừa vặn 4 dòng.
* [`app/src/main/cpp/native_renderer.cpp`](file:///home/phu/Repo/HelloSkia/app/src/main/cpp/native_renderer.cpp):
  * Cấu hình `SkFontHinting::kNone` và `setLinearMetrics(true)`.
  * Áp dụng công thức stroke fake bold `strokeWidth = textSize / 30.0f`.
  * Bộ giải mã UTF-8 và thuật toán Fallback Font + Run clustering qua `SkTextBlobBuilder`.
  * Bộ căn chỉnh đa dòng theo `SkFontMetrics`.
