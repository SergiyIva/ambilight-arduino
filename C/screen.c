#include "screen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>

static XImage *g_shmimage = NULL;       // XImage, созданное через XShmCreateImage
static XShmSegmentInfo g_shminfo;       // структура с инфой о сегменте
static int g_use_shm = 0;               // флаг: 0 - не используем, 1 - используем
static int g_shm_inited = 0;

/**
 * Generate a random integer in the specified range [min, max].
 */
int randint(int min, int max) {
  return min + (rand() % (max - min));
}

/**
 * Convert RGB (0..255) to HSV.
 * h, s, v are returned by pointers:
 *   - h in degrees [0..360]
 *   - s, v in the range [0..1].
 */
void rgb_to_hsv(unsigned char r, unsigned char g, unsigned char b,
                float *h, float *s, float *v)
{
    float R = r / 255.0f;
    float G = g / 255.0f;
    float B = b / 255.0f;

    float max = fmaxf(R, fmaxf(G, B));
    float min = fminf(R, fminf(G, B));
    float delta = max - min;

    // Value
    *v = max;

    // Saturation
    if (max > 0.0f)
        *s = delta / max;
    else
        *s = 0.0f;

    // Hue
    if (delta < 0.00001f) {
        *h = 0.0f;  
    } else {
        if (max == R) {
            *h = (G - B) / delta;
        } else if (max == G) {
            *h = 2.0f + (B - R) / delta;
        } else {
            *h = 4.0f + (R - G) / delta;
        }
        *h *= 60.0f;   // переводим в градусы
        if (*h < 0.0f) *h += 360.0f;
    }
}

/**
 * Convert HSV to RGB (each component in 0..255).
 * h in [0..360], s and v in [0..1].
 * If saturation is 0, all components are equal to v (gray shade).
 */
void hsv_to_rgb(float h, float s, float v,
                unsigned char *r, unsigned char *g, unsigned char *b)
{
    if (s <= 0.0f) {
        *r = (unsigned char)(v * 255.0f);
        *g = (unsigned char)(v * 255.0f);
        *b = (unsigned char)(v * 255.0f);
        return;
    }

    float hh = fmodf(h, 360.0f) / 60.0f;
    int i = (int)hh;
    float ff = hh - i;

    float p = v * (1.0f - s);
    float q = v * (1.0f - s * ff);
    float t = v * (1.0f - s * (1.0f - ff));

    float R, G, B;
    switch(i) {
        case 0: R = v;  G = t;  B = p;  break;
        case 1: R = q;  G = v;  B = p;  break;
        case 2: R = p;  G = v;  B = t;  break;
        case 3: R = p;  G = q;  B = v;  break;
        case 4: R = t;  G = p;  B = v;  break;
        default:
            R = v;  G = p;  B = q;  
            break;
    }

    // Convert back to [0..255]
    *r = (unsigned char)(R * 255.0f);
    *g = (unsigned char)(G * 255.0f);
    *b = (unsigned char)(B * 255.0f);
}

/**
 * Find the average color of a block
 * @param i index to write colour to in values
 * @param min_x minimum x value for a pixel
 * @param max_x maximum x value for a pixel
 * @param min_y minimum y value for a pixel
 * @param max_y maximum y value for a pixel
 * @param values array of bytes to write color to
 * @param d The X display to query
 * @param image The snapshot of the display to query
 * @param pixels_to_process The number of pixels to use for an average
 * @param brightness A percentage to scale the color by
 * @param saturation Saturation adjustment in percent (value added to the original saturation)
 */
void fillRGB(int i, int min_x, int max_x, int min_y, int max_y,
             unsigned char values[], Display* d, XImage* image,
             int pixels_to_process, int brightness, int saturation) {
	int total[3] = {0, 0, 0};
	int x, y;
	XColor c;

	for (int j = 0; j < pixels_to_process; j++) {
		x = randint(min_x, max_x);
		y = randint(min_y, max_y);

		c.pixel = XGetPixel(image, x, y);
		XQueryColor(d, DefaultColormap(d, DefaultScreen(d)), &c);

		total[0] += c.red/256;
		total[1] += c.green/256;
		total[2] += c.blue/256;
	}

    // Average color (0..255)
    int r = total[0] / pixels_to_process;
    int g = total[1] / pixels_to_process;
    int b = total[2] / pixels_to_process;

    // Apply brightness (100% = no change)
    r = (r * brightness) / 100;
    g = (g * brightness) / 100;
    b = (b * brightness) / 100;

    // Clamp to [0..255]
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
		
    // Convert to HSV
    float h, s, v;
    rgb_to_hsv(r, g, b, &h, &s, &v);

    // Adjust saturation: add (saturation)% to current saturation
    s = (s * (saturation + 100)) / 100;
    if (s > 1.0f) s = 1.0f; // max limit

    // Convert to RGB
	hsv_to_rgb(h, s, v, (unsigned char *)&r, (unsigned char *)&g, (unsigned char *)&b);
    
    // Write the final color into the array (3 bytes per LED)
    values[i + 0] = (unsigned char)r;
    values[i + 1] = (unsigned char)g;
    values[i + 2] = (unsigned char)b;
}

/**
 * Инициализация XShm (однократно) при первом вызове. Если не удалось, будет use_shm=0.
 */
static void init_xshm(Display *d, int width, int height)
{
    // Если уже инициализировались раньше, не делаем повторно
    if (g_shm_inited) {
        return;
    }
    g_shm_inited = 1;

    // Проверяем, доступно ли расширение
    if (!XShmQueryExtension(d)) {
        fprintf(stderr, "XShm extension not available. Falling back to XGetImage.\n");
        g_use_shm = 0;
        return;
    }

    // Пытаемся создать XImage через XShm
    int screen_n = DefaultScreen(d);
    Visual *vis = DefaultVisual(d, screen_n);
    int depth = DefaultDepth(d, screen_n);

    g_shmimage = XShmCreateImage(d, vis, depth, ZPixmap, NULL, &g_shminfo, width, height);
    if (!g_shmimage) {
        fprintf(stderr, "XShmCreateImage failed. Fallback to XGetImage.\n");
        g_use_shm = 0;
        return;
    }

    // Выделяем общий сегмент памяти
    size_t image_size = g_shmimage->bytes_per_line * g_shmimage->height;
    g_shminfo.shmid = shmget(IPC_PRIVATE, image_size, IPC_CREAT | 0777);
    if (g_shminfo.shmid < 0) {
        fprintf(stderr, "shmget failed. Fallback to XGetImage.\n");
        XDestroyImage(g_shmimage);
        g_shmimage = NULL;
        g_use_shm = 0;
        return;
    }

    g_shminfo.shmaddr = (char *)shmat(g_shminfo.shmid, NULL, 0);
    if (g_shminfo.shmaddr == (char *)-1) {
        fprintf(stderr, "shmat failed. Fallback to XGetImage.\n");
        XDestroyImage(g_shmimage);
        g_shmimage = NULL;
        g_use_shm = 0;
        return;
    }

    g_shmimage->data = g_shminfo.shmaddr;
    g_shminfo.readOnly = False;

    // Подключаем сегмент к X-серверу
    if (!XShmAttach(d, &g_shminfo)) {
        fprintf(stderr, "XShmAttach failed. Fallback to XGetImage.\n");
        shmdt(g_shminfo.shmaddr);
        XDestroyImage(g_shmimage);
        g_shmimage = NULL;
        g_use_shm = 0;
        return;
    }

    // Если дошли сюда, всё хорошо
    g_use_shm = 1;
    fprintf(stderr, "XShm successfully initialized.\n");
}

/**
 * Get the RGB colors for the display
 * @param d the X display to get the colors from
 * @param values Array to store the result colors (3 bytes per LED)
 * @param t a random seed
 * @param cnf config
 */
void get_colors(Display *d, unsigned char *values, unsigned t, struct config *cnf) {
	srand(t); // Initialising random

    XImage *image = NULL;

    if (g_use_shm && g_shmimage) {
        // Считываем в g_shmimage
        Status st = XShmGetImage(
            d,
            RootWindow(d, DefaultScreen(d)),
            g_shmimage,
            0,
            0,
            AllPlanes
        );
        if (st == 0) {
            // На всякий случай fallback, если XShmGetImage не смог
            fprintf(stderr, "XShmGetImage failed unexpectedly. Using fallback.\n");
            image = XGetImage(
                d,
                RootWindow(d, DefaultScreen(d)),
                0,
                0,
                cnf->horizontal_pixel_count,
                cnf->vertical_pixel_count,
                AllPlanes,
                ZPixmap
            );
        } else {
            image = g_shmimage; // Успешно считали кадр
        }
    }
    else {
        // Fallback
        image = XGetImage(
            d,
            RootWindow(d, DefaultScreen(d)),
            0,
            0,
            cnf->horizontal_pixel_count,
            cnf->vertical_pixel_count,
            AllPlanes,
            ZPixmap
        );
    }

    int ledsSide = cnf->leds_on_side;
    int ledsTop  = cnf->leds_on_top;
    int totalLeds = 2 * ledsSide + 2 * ledsTop;

    //  BOTTOM side (indices 0..ledsTop-1), left to right
    for (int i = 0; i < ledsTop; i++) {
        int ledIndex = i;

        // Compute X offset so that i=0 is the right edge, i=(ledsTop-1) is the left edge
        int offset = (ledsTop - i - 1);
        int xStart = cnf->horizontal_pixel_gap + offset * cnf->pixels_per_led_top;
        int xEnd   = xStart + cnf->pixels_per_led_top - 1;

        // For the bottom side, this is the lower strip in vertical:
        int yStart = cnf->vertical_pixel_count - cnf->pixels_per_led_top;
        int yEnd   = cnf->vertical_pixel_count - 1;

        fillRGB(
            ledIndex * 3,
            xStart, xEnd,
            yStart, yEnd,
            values, d, image,
            cnf->pixels_to_process,
            cnf->brightness,
            cnf->saturation
        );
    }

    // LEFT side (indices ledsTop..(ledsTop + ledsSide - 1))
    for (int k = 0; k < ledsSide; k++) {
        int j = ledsSide - k - 1;
        int i = ledsTop + k;

        fillRGB(
            i * 3,
            0,
            cnf->pixels_per_led_side,
            cnf->vertical_pixel_gap + j * cnf->pixels_per_led_side,
            cnf->vertical_pixel_gap + (j + 1) * cnf->pixels_per_led_side,
            values, d, image,
            cnf->pixels_to_process,
            cnf->brightness,
            cnf->saturation
        );
    }

    // TOP side (indices start at ledsTop + ledsSide)
    int topOffset = ledsTop + ledsSide;
    for (int i = 0; i < ledsTop; i++) {
        fillRGB(
            (topOffset + i) * 3,
            cnf->horizontal_pixel_gap + i * cnf->pixels_per_led_top,
            cnf->horizontal_pixel_gap + (i + 1) * cnf->pixels_per_led_top,
            0,
            cnf->pixels_per_led_top,
            values, d, image, cnf->pixels_to_process, cnf->brightness, cnf->saturation
        );
    }

    // RIGHT side
    int rightOffset = 2*ledsTop + ledsSide;
    for (int i = 0; i < ledsSide; i++) {
        fillRGB(
            (rightOffset + i) * 3,
            cnf->horizontal_pixel_count - cnf->pixels_per_led_side,
            cnf->horizontal_pixel_count - 1,
            cnf->vertical_pixel_gap + i * cnf->pixels_per_led_side,
            cnf->vertical_pixel_gap + (i + 1) * cnf->pixels_per_led_side,
            values, d, image,
            cnf->pixels_to_process,
            cnf->brightness,
            cnf->saturation
        );
    }

    if (image != NULL && image != g_shmimage) {
        XDestroyImage(image);
    }
}
