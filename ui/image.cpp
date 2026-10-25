#include "ui.h"
#include "fb.h"
#include "fs.h"
#include "string.h"

#define UI_IMAGE_CAPACITY (1024ULL * 1024ULL)
#define UI_PATH_CAPACITY 192

static uint8_t image_data[UI_IMAGE_CAPACITY];
static uint64_t image_size = 0;
static uint64_t image_mtime = 0;
static char image_path[UI_PATH_CAPACITY];
static int image_valid = 0;

struct BmpInfo
{
    uint32_t width;
    uint32_t height;
    uint32_t row_stride;
    uint32_t pixel_offset;
    uint16_t bits_per_pixel;
    int top_down;
};

static uint16_t get_u16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t get_i32(const uint8_t* p)
{
    return (int32_t)get_u32(p);
}

static int path_copy(char* dst, int capacity, const char* src)
{
    if (dst == 0 || src == 0 || capacity < 2) return -1;
    int i = 0;
    while (src[i] != '\0' && i < capacity - 1)
    {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
    return src[i] == '\0' ? 0 : -1;
}

static int bmp_info(BmpInfo* out)
{
    if (out == 0 || image_size < 54ULL) return -1;
    if (image_data[0] != 'B' || image_data[1] != 'M') return -1;

    uint32_t offset = get_u32(image_data + 10);
    uint32_t header_size = get_u32(image_data + 14);
    int32_t width = get_i32(image_data + 18);
    int32_t signed_height = get_i32(image_data + 22);
    uint16_t planes = get_u16(image_data + 26);
    uint16_t bpp = get_u16(image_data + 28);
    uint32_t compression = get_u32(image_data + 30);

    if (header_size < 40U || width <= 0 || signed_height == 0 ||
        signed_height == (-2147483647 - 1) || planes != 1U ||
        (bpp != 24U && bpp != 32U) || compression != 0U)
        return -1;

    uint32_t height = (uint32_t)(signed_height < 0 ? -signed_height : signed_height);
    uint64_t bits_per_row = (uint64_t)(uint32_t)width * (uint64_t)bpp;
    uint64_t stride64 = ((bits_per_row + 31ULL) / 32ULL) * 4ULL;
    uint64_t end = (uint64_t)offset + stride64 * (uint64_t)height;
    if ((uint32_t)width > 8192U || height > 8192U ||
        offset < 54U || stride64 > 0xFFFFFFFFULL || end > image_size ||
        end < offset || stride64 == 0)
        return -1;

    out->width = (uint32_t)width;
    out->height = height;
    out->row_stride = (uint32_t)stride64;
    out->pixel_offset = offset;
    out->bits_per_pixel = bpp;
    out->top_down = signed_height < 0;
    return 0;
}

static int load_image(const char* path, BmpInfo* info)
{
    if (path == 0 || path[0] != '/') return -1;
    FsStat stat;
    if (fs_stat_path(path, fs_get_root(), &stat) != 0 || stat.type != FS_FILE ||
        stat.size < 54ULL || stat.size > UI_IMAGE_CAPACITY)
        return -1;
    if (image_valid && str_equal(image_path, path) &&
        image_size == stat.size && image_mtime == stat.mtime)
        return bmp_info(info);

    uint64_t loaded = 0;
    if (fs_read_file(path, fs_get_root(), image_data, sizeof(image_data), &loaded) != 0 ||
        loaded != stat.size)
    {
        image_valid = 0;
        image_path[0] = '\0';
        image_size = 0;
        return -1;
    }
    image_size = loaded;
    image_mtime = stat.mtime;
    image_valid = 1;
    if (path_copy(image_path, sizeof(image_path), path) != 0 || bmp_info(info) != 0)
    {
        image_valid = 0;
        image_path[0] = '\0';
        image_size = 0;
        return -1;
    }
    return 0;
}

static uint32_t bmp_pixel(const BmpInfo* info, uint32_t x, uint32_t y)
{
    uint32_t source_y = info->top_down ? y : info->height - 1U - y;
    uint64_t offset = (uint64_t)info->pixel_offset +
                      (uint64_t)source_y * info->row_stride +
                      (uint64_t)x * (uint64_t)(info->bits_per_pixel / 8U);
    const uint8_t* p = image_data + offset;
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static int in_rounded_corner(int x, int y, int width, int height, int radius)
{
    if (radius <= 0) return 1;
    if (radius * 2 > width) radius = width / 2;
    if (radius * 2 > height) radius = height / 2;
    int cx = x < radius ? radius - 1 : (x >= width - radius ? width - radius : x);
    int cy = y < radius ? radius - 1 : (y >= height - radius ? height - radius : y);
    if (x >= radius && x < width - radius) return 1;
    if (y >= radius && y < height - radius) return 1;
    int dx = x - cx;
    int dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

int ui_image_draw_bmp(const char* path, int x, int y, int width, int height, int radius)
{
    if (width <= 0 || height <= 0) return -1;
    BmpInfo info;
    if (load_image(path, &info) != 0) return -1;
    int screen_w = fb_get_width(), screen_h = fb_get_height();
    if (x >= screen_w || y >= screen_h || x + width <= 0 || y + height <= 0) return 0;
    for (int dy = 0; dy < height; ++dy)
    {
        int py = y + dy;
        if (py < 0 || py >= screen_h) continue;
        uint32_t sy = (uint32_t)(((uint64_t)(uint32_t)dy * info.height) / (uint32_t)height);
        if (sy >= info.height) sy = info.height - 1U;
        for (int dx = 0; dx < width; ++dx)
        {
            if (!in_rounded_corner(dx, dy, width, height, radius)) continue;
            int px = x + dx;
            if (px < 0 || px >= screen_w) continue;
            uint32_t sx = (uint32_t)(((uint64_t)(uint32_t)dx * info.width) / (uint32_t)width);
            if (sx >= info.width) sx = info.width - 1U;
            fb_draw_pixel(px, py, bmp_pixel(&info, sx, sy));
        }
    }
    return 0;
}

static int has_bmp_extension(const char* name)
{
    if (name == 0) return 0;
    int n = 0;
    while (name[n]) ++n;
    if (n < 4) return 0;
    char a = name[n - 4], b = name[n - 3], c = name[n - 2], d = name[n - 1];
    if (a != '.') return 0;
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (d >= 'A' && d <= 'Z') d = (char)(d - 'A' + 'a');
    return b == 'b' && c == 'm' && d == 'p';
}

int ui_wallpaper_count()
{
    int dir = fs_resolve("/wallpapers", fs_get_root());
    if (dir < 0 || fs_get_type(dir) != FS_DIR) return 0;
    int all = fs_get_child_count(dir), count = 0;
    for (int i = 0; i < all; ++i)
    {
        int child = fs_get_child(dir, i);
        if (child >= 0 && fs_get_type(child) == FS_FILE && has_bmp_extension(fs_get_name(child))) ++count;
    }
    return count;
}

int ui_wallpaper_get_entry(int index, char* path, int path_capacity, char* name, int name_capacity)
{
    if (index < 0) return -1;
    int dir = fs_resolve("/wallpapers", fs_get_root());
    if (dir < 0 || fs_get_type(dir) != FS_DIR) return -1;
    int all = fs_get_child_count(dir), match = 0;
    for (int i = 0; i < all; ++i)
    {
        int child = fs_get_child(dir, i);
        if (child < 0 || fs_get_type(child) != FS_FILE || !has_bmp_extension(fs_get_name(child))) continue;
        if (match++ != index) continue;
        char full[UI_PATH_CAPACITY];
        int p = 0;
        const char* prefix = "/wallpapers/";
        while (prefix[p] && p < (int)sizeof(full) - 1) { full[p] = prefix[p]; ++p; }
        const char* child_name = fs_get_name(child);
        for (int n = 0; child_name[n] && p < (int)sizeof(full) - 1; ++n) full[p++] = child_name[n];
        full[p] = '\0';
        if ((path && path_copy(path, path_capacity, full) != 0) ||
            (name && path_copy(name, name_capacity, child_name) != 0)) return -1;
        return 0;
    }
    return -1;
}

void ui_wallpaper_current_path(char* out, int capacity)
{
    if (out == 0 || capacity < 2) return;
    uint8_t config[UI_PATH_CAPACITY];
    uint64_t size = 0;
    if (fs_read_file("/settings/wallpaper.cfg", fs_get_root(), config,
                     sizeof(config) - 1ULL, &size) == 0 && size > 0 && size < sizeof(config))
    {
        config[size] = 0;
        int n = 0;
        while (config[n] && config[n] != '\r' && config[n] != '\n' && n < capacity - 1)
        {
            out[n] = (char)config[n];
            ++n;
        }
        out[n] = '\0';
        if (n > 0) return;
    }
    (void)path_copy(out, capacity, "/wallpapers/rioos-default.bmp");
}

int ui_wallpaper_set_active(const char* path)
{
    if (path == 0 || path[0] == '\0') return -1;
    const char prefix[] = "/wallpapers/";
    for (int i = 0; i < (int)sizeof(prefix) - 1; ++i)
        if (path[i] != prefix[i]) return -1;
    /* Only direct children of /wallpapers are valid selections. */
    for (int i = (int)sizeof(prefix) - 1; path[i] != '\0'; ++i)
        if (path[i] == '/' || path[i] == '\\') return -1;
    BmpInfo info;
    if (load_image(path, &info) != 0) return -1;
    uint64_t len = 0;
    while (path[len] && len < UI_PATH_CAPACITY - 1) ++len;
    if (path[len] != '\0' || fs_write_data("/settings/wallpaper.cfg", fs_get_root(),
                                            (const uint8_t*)path, len) != 0)
        return -1;
    return 0;
}

int ui_wallpaper_draw_background()
{
    char path[UI_PATH_CAPACITY];
    ui_wallpaper_current_path(path, sizeof(path));
    BmpInfo info;
    if (load_image(path, &info) != 0)
    {
        if (load_image("/wallpapers/rioos-default.bmp", &info) != 0) return 0;
    }
    int target_w = fb_get_width(), target_h = fb_get_height();
    if (target_w <= 0 || target_h <= 0) return 0;
    uint32_t crop_x = 0, crop_y = 0, crop_w = info.width, crop_h = info.height;
    uint64_t img_aspect = (uint64_t)info.width * (uint64_t)target_h;
    uint64_t screen_aspect = (uint64_t)info.height * (uint64_t)target_w;
    if (img_aspect > screen_aspect)
    {
        crop_w = (uint32_t)(((uint64_t)info.height * (uint64_t)target_w) / (uint64_t)target_h);
        if (crop_w == 0) crop_w = 1;
        crop_x = (info.width - crop_w) / 2U;
    }
    else if (img_aspect < screen_aspect)
    {
        crop_h = (uint32_t)(((uint64_t)info.width * (uint64_t)target_h) / (uint64_t)target_w);
        if (crop_h == 0) crop_h = 1;
        crop_y = (info.height - crop_h) / 2U;
    }
    for (int y = 0; y < target_h; ++y)
    {
        uint32_t sy = crop_y + (uint32_t)(((uint64_t)(uint32_t)y * crop_h) / (uint32_t)target_h);
        if (sy >= info.height) sy = info.height - 1U;
        for (int x = 0; x < target_w; ++x)
        {
            uint32_t sx = crop_x + (uint32_t)(((uint64_t)(uint32_t)x * crop_w) / (uint32_t)target_w);
            if (sx >= info.width) sx = info.width - 1U;
            fb_draw_pixel(x, y, bmp_pixel(&info, sx, sy));
        }
    }
    return 1;
}
