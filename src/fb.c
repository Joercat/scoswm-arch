/* Original SCos drawing primitives. Linux: the back buffer is presented
 * to the host window by platform_present() instead of a GOP scanout copy. */
#include "scos.h"
struct surface screen;
int screen_w,screen_h;
static int clip_on,clip_x,clip_y,clip_r,clip_b;
void fb_scene_clip(int x,int y,int w,int h){clip_on=1;clip_x=x;clip_y=y;clip_r=x+w;clip_b=y+h;}
void fb_scene_unclip(void){clip_on=0;}
u32 fb_bpp(void){return 32;}
void fb_init(int width,int height){
 screen_w=width;screen_h=height;
 screen=(struct surface){palloc_owned((size_t)screen_w*screen_h*4,HEAP_WM),screen_w,screen_h};
 if(!screen.px)panic("desktop framebuffer allocation failed");
}
/* On bare metal a solid output rectangle could be handed to a bound GPU
 * engine; no engine is ported to Linux, so the CPU copy in fb_flip_rect
 * below is the only path (gpu_engine_* all decline, see platform_linux.c). */

/* One text line of the console is 16 rows by columns*8 pixels of uncached device memory.  On a card
 * with a bit-block transfer engine that move belongs in the GPU's own video RAM, where it is a few
 * register writes, and not in a loop the boot CPU runs a pixel at a time.  A device without an engine,
 * or one that declines the rectangle, takes the loop below; both paths are counted so the boot report
 * says which one did the work. */
int fb_scroll_output(u32 x,u32 y,u32 w,u32 h,u32 step,u32 color){
 (void)x;(void)y;(void)w;(void)h;(void)step;(void)color;
 if(!gpu_engine_drives_output())return 0;
 return 0;   /* no GPU engine is bound in the Linux port */
}

/* The per-pixel loop fb_scroll_output replaces, kept here so the fallback and the first paint of a
 * cleared band share one implementation. */
void fb_scroll_cpu(u32 x,u32 y,u32 w,u32 h,u32 step,u32 color){
 /* Only the (not ported) kernel TTY used device-memory scrolling; the
  * Linux compositor scrolls inside its RAM surfaces instead. */
 (void)x;(void)y;(void)w;(void)h;(void)step;(void)color;
}
void fb_flip_rect(int x,int y,int w,int h){
 if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}
 if(w<=0||h<=0||x>=screen_w||y>=screen_h)return;
 if(w>screen_w-x)w=screen_w-x;
 if(h>screen_h-y)h=screen_h-y;
 platform_present(screen.px,screen_w,x,y,w,h);
 platform_note_present();
}
void fb_flip(void){fb_flip_rect(0,0,screen_w,screen_h);}
void fb_clear(u32 color)
{
    for (int i = 0; i < screen_w * screen_h; i++) screen.px[i] = color;
}

/* ------------------------------------------------------------ surface ---- */
void s_pixel(struct surface *s, int x, int y, u32 c)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    if(s==&screen&&clip_on&&(x<clip_x||y<clip_y||x>=clip_r||y>=clip_b))return;
    s->px[y * s->w + x] = c;
}

void s_fill(struct surface *s, int x, int y, int w, int h, u32 c)
{
    if(s==&screen&&clip_on){if(x<clip_x){w-=clip_x-x;x=clip_x;}if(y<clip_y){h-=clip_y-y;y=clip_y;}if(w>clip_r-x)w=clip_r-x;if(h>clip_b-y)h=clip_b-y;}
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if(w<=0||h<=0)return;
    for (int j = 0; j < h; j++) {
        u32 *row = s->px + (y + j) * s->w + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

void s_frame_rect(struct surface *s, int x, int y, int w, int h, u32 c)
{
    s_fill(s, x, y, w, 1, c);
    s_fill(s, x, y + h - 1, w, 1, c);
    s_fill(s, x, y, 1, h, c);
    s_fill(s, x + w - 1, y, 1, h, c);
}

void s_line(struct surface *s, int x0, int y0, int x1, int y1, u32 c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        s_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void s_circle(struct surface *s, int cx, int cy, int r, u32 c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        s_pixel(s, cx + x, cy + y, c); s_pixel(s, cx - x, cy + y, c);
        s_pixel(s, cx + x, cy - y, c); s_pixel(s, cx - x, cy - y, c);
        s_pixel(s, cx + y, cy + x, c); s_pixel(s, cx - y, cy + x, c);
        s_pixel(s, cx + y, cy - x, c); s_pixel(s, cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void s_disc(struct surface *s, int cx, int cy, int r, u32 c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) s_pixel(s, cx + x, cy + y, c);
}

static u32 blend(u32 a, u32 b, int t)      /* t 0..255 */
{
    u32 ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    u32 br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    u32 r = (ar * (255 - t) + br * t) / 255;
    u32 g = (ag * (255 - t) + bg * t) / 255;
    u32 bl = (ab * (255 - t) + bb * t) / 255;
    return (r << 16) | (g << 8) | bl;
}

void s_vgrad(struct surface *s, int x, int y, int w, int h, u32 top, u32 bot)
{
    for (int j = 0; j < h; j++) {
        u32 c = blend(top, bot, h > 1 ? (j * 255) / (h - 1) : 0);
        s_fill(s, x, y + j, w, 1, c);
    }
}

/* --------------------------------------------------------------- text ---- */
void s_char_bg(struct surface *s, int x, int y, char ch, u32 fg, u32 bg)
{
    if (!bg) { s_char(s, x, y, ch, fg); return; }
    const u8 *glyph = font8x16[(u8)ch];
    for (int row = 0; row < FONT_H; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < FONT_W; col++)
            s_pixel(s, x + col, y + row, (bits & (0x80 >> col)) ? fg : bg);
    }
}

void s_char(struct surface *s, int x, int y, char ch, u32 fg)
{
    const u8 *glyph = font8x16[(u8)ch];
    for (int row = 0; row < FONT_H; row++) {
        u8 bits = glyph[row];
        if (!bits) continue;
        for (int col = 0; col < FONT_W; col++)
            if (bits & (0x80 >> col)) s_pixel(s, x + col, y + row, fg);
    }
}

void s_text(struct surface *s, int x, int y, const char *str, u32 fg)
{
    int x0 = x;
    while (*str) {
        if (*str == '\n') { x = x0; y += FONT_H + 2; str++; continue; }
        s_char(s, x, y, *str, fg);
        x += FONT_W;
        str++;
    }
}

void s_text_bg(struct surface *s, int x, int y, const char *str, u32 fg, u32 bg)
{
    int x0 = x;
    while (*str) {
        if (*str == '\n') { x = x0; y += FONT_H + 2; str++; continue; }
        s_char_bg(s, x, y, *str, fg, bg);
        x += FONT_W;
        str++;
    }
}

void s_text_scaled(struct surface *s, int x, int y, const char *str, u32 fg, int scale)
{
    while (*str) {
        const u8 *glyph = font8x16[(u8)*str];
        for (int row = 0; row < FONT_H; row++) {
            u8 bits = glyph[row];
            if (!bits) continue;
            for (int col = 0; col < FONT_W; col++) {
                if (bits & (0x80 >> col))
                    s_fill(s, x + col * scale, y + row * scale, scale, scale, fg);
            }
        }
        x += FONT_W * scale;
        str++;
    }
}

int s_text_width(const char *str)
{
    return (int)strlen(str) * FONT_W;
}

void s_clip_text(struct surface *s, int x, int y, const char *str, u32 fg, int max_w)
{
    int n=max_w/FONT_W;if(n<=0)return;if(n>127)n=127;
    char tmp[128];int len=0;
    while(str[len]&&str[len]!='\n'&&str[len]!='\r'&&len<n){tmp[len]=str[len];len++;}
    if(len==n&&str[len]&&str[len]!='\n'&&str[len]!='\r'){
        int dots=n<3?n:3;for(int i=0;i<dots;i++)tmp[n-1-i]='.';
    }
    tmp[len]=0;s_text(s,x,y,tmp,fg);
}

/* Draw flowing text as wrapped rows, and report how many were painted.  `max_rows' bounds the work for a
 * caller whose space is finite (0 means no bound).  A row that no longer fits the surface stops the loop:
 * writing below a surface is not clipping, it is corruption, and the count returned says so by being short.
 */
/* Draw flowing text as wrapped rows and report how many were painted.  The row pitch is the text cell
 * itself, and the surface is the only bound: a caller that wants the text to stop at the bottom of its box
 * hands over a surface whose height *is* the box, which is also how a disagreement between the two becomes
 * visible instead of silent.  A row that would fall outside the surface is not painted, and a surface
 * painted short of the string is the caller's cue that it reserved too little. */
int s_text_wrap(struct surface *s, int x, int y, int width, const char *text, u32 fg)
{
    char row[256];
    int cols = s_text_cols(width), rows = 0;

    if (cols > (int)sizeof(row) - 1) cols = (int)sizeof(row) - 1;
    const char *p = text;
    while (p && y + rows * FONT_H + FONT_H <= s->h) {
        p = s_wrap_row(p, cols, row, sizeof row);
        if (row[0]) s_text(s, x, y + rows * FONT_H, row, fg);
        rows++;
    }
    return rows;
}

/* -------------------------------------------------------------- icons ---- */
void s_icon(struct surface *s, int id, int x, int y, u32 c)
{
    /* 24x24 procedural icons */
    switch (id) {
    case ICON_FOLDER:
        s_fill(s, x + 2, y + 6, 8, 3, c);
        s_frame_rect(s, x + 2, y + 8, 20, 12, c);
        s_fill(s, x + 3, y + 9, 18, 10, (c & 0xFEFEFE) >> 1);
        s_frame_rect(s, x + 2, y + 8, 20, 12, c);
        break;
    case ICON_TERMINAL:
        s_frame_rect(s, x + 1, y + 3, 22, 18, c);
        s_line(s, x + 4, y + 8, x + 8, y + 11, c);
        s_line(s, x + 8, y + 11, x + 4, y + 14, c);
        s_fill(s, x + 10, y + 14, 8, 2, c);
        break;
    case ICON_NOTEPAD:
        s_frame_rect(s, x + 4, y + 2, 16, 20, c);
        for (int i = 0; i < 4; i++)
            s_fill(s, x + 7, y + 6 + i * 4, 10, 1, c);
        break;
    case ICON_BROWSER:
        s_circle(s, x + 12, y + 12, 10, c);
        s_line(s, x + 2, y + 12, x + 22, y + 12, c);
        s_line(s, x + 12, y + 2, x + 12, y + 22, c);
        s_circle(s, x + 12, y + 12, 5, c);
        break;
    case ICON_CALENDAR:
        s_frame_rect(s, x + 2, y + 4, 20, 17, c);
        s_fill(s, x + 3, y + 5, 18, 4, c);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++)
                s_fill(s, x + 5 + j * 4, y + 12 + i * 3, 2, 1, c);
        break;
    case ICON_SETTINGS:
        s_circle(s, x + 12, y + 12, 5, c);
        for (int i = 0; i < 8; i++) {
            int dxs[8] = { 0, 3, 4, 3, 0, -3, -4, -3 };
            int dys[8] = { -4, -3, 0, 3, 4, 3, 0, -3 };
            s_fill(s, x + 12 + dxs[i] - 1, y + 12 + dys[i] * 2 - 1, 3, 3, c);
        }
        s_circle(s, x + 12, y + 12, 8, c);
        break;
    case ICON_INFO:
        s_circle(s, x + 12, y + 12, 10, c);
        s_fill(s, x + 11, y + 10, 2, 8, c);
        s_fill(s, x + 11, y + 6, 2, 2, c);
        break;
    case ICON_SOL:
        s_fill(s, x + 3, y + 6, 12, 16, (c & 0xFEFEFE) >> 1);
        s_frame_rect(s, x + 3, y + 6, 12, 16, c);
        s_fill(s, x + 8, y + 4, 12, 16, (c & 0xFEFEFE) >> 1);
        s_frame_rect(s, x + 8, y + 4, 12, 16, c);
        s_fill(s, x + 13, y + 2, 12, 16, (c & 0xFEFEFE) >> 1);
        s_frame_rect(s, x + 13, y + 2, 12, 16, c);
        break;
    case ICON_CHART:
        s_frame_rect(s, x + 2, y + 3, 20, 18, c);
        s_fill(s, x + 5, y + 12, 3, 6, c);
        s_fill(s, x + 10, y + 8, 3, 10, c);
        s_fill(s, x + 15, y + 5, 3, 13, c);
        break;
    case ICON_CARDS:
        s_fill(s, x + 8, y + 2, 14, 18, c);
        s_frame_rect(s, x + 8, y + 2, 14, 18, c);
        s_fill(s, x + 9, y + 3, 12, 16, (c & 0xFEFEFE) >> 1);
        s_frame_rect(s, x + 4, y + 5, 14, 18, c);
        s_fill(s, x + 5, y + 6, 12, 16, (c & 0xFEFEFE) >> 1);
        s_disc(s, x + 11, y + 14, 2, c);
        break;
    }
}

void s_blit(struct surface *d, struct surface *s, int dx, int dy)
{
    int sx = 0, sy = 0, w = s->w, h = s->h;
    if(d==&screen&&clip_on){if(dx<clip_x){sx=clip_x-dx;w-=sx;dx=clip_x;}if(dy<clip_y){sy=clip_y-dy;h-=sy;dy=clip_y;}if(w>clip_r-dx)w=clip_r-dx;if(h>clip_b-dy)h=clip_b-dy;}
    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > d->w) w = d->w - dx;
    if (dy + h > d->h) h = d->h - dy;
    if(w<=0||h<=0)return;
    for (int y = 0; y < h; y++)
        memcpy(d->px + (dy + y) * d->w + dx,
               s->px + (sy + y) * s->w + sx, w * 4);
}

/* The SCos logo: "SC" + spinning 'o' ring + "s", tightly kerned. */
void s_scos_logo(struct surface *s, int x, int y, u32 color, int scale, int phase)
{
    int cw = 8 * scale;
    s_text_scaled(s, x, y, "SC", color, scale);
    int r = 4 * scale + 2;
    int ox = x + 2 * cw + r + 2, oy = y + 6 * scale + r;
    static const int dxs[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    static const int dys[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
    for (int k = 0; k < 8; k++) {
        int rel = (k - phase + 16) % 8;
        int on = rel < 3;
        int rr = (on ? 3 : 2) * scale / 2 + (on ? 1 : 0);
        int px = ox + dxs[k] * r / 10, py = oy + dys[k] * r / 10;
        s_disc(s, px, py, rr, on ? color : ((color >> 2) & 0x3F3F3F));
    }
    s_text_scaled(s, x + 2 * cw + 2 * r + 6, y, "s", color, scale);
}

u32 color_blend(u32 a, u32 b, int t)
{
    return blend(a, b, t);
}
