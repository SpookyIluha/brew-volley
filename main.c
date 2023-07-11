#include "libdragon.h"
#include <malloc.h>
#include <math.h>

static sprite_t *brew_sprite;
static sprite_t *tiles_sprite;

static rspq_block_t *tiles_block;

typedef struct {
    float x;
    float y;
    float dx;
    float dy;
    float scale_factor;
} object_t;

#define NUM_BLOBS 2

static object_t blobs[NUM_BLOBS];

// Fair and fast random generation (using xorshift32, with explicit seed)
static uint32_t rand_state = 1;
static uint32_t rand(void) {
	uint32_t x = rand_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 5;
	return rand_state = x;
}

// RANDN(n): generate a random number from 0 to n-1
#define RANDN(n) ({ \
	__builtin_constant_p((n)) ? \
		(rand()%(n)) : \
		(uint32_t)(((uint64_t)rand() * (n)) >> 32); \
})

static int32_t obj_min_x;
static int32_t obj_max_x;
static int32_t obj_min_y;
static int32_t obj_max_y;
static int32_t cur_tick = 0;

#define FRICTION_FACTOR 0.9f
#define EPSILON 1e-1

void update(int ovfl)
{
    //fprintf(stderr, "update\n");
    for (uint32_t i = 0; i < NUM_BLOBS; i++)
    {
        object_t *obj = &blobs[i];
        //fprintf(stderr, "blob[%ld]: x=%ld y=%ld dx=%f dy=%f\n", i, obj->x, obj->y, obj->dx, obj->dy);

        float x = obj->x + obj->dx;
        float y = obj->y + obj->dy;

        if (x >= obj_max_x) {
            x = obj_max_x - (x - obj_max_x);
            obj->dx = -obj->dx;
        }
        if (x < obj_min_x) {
            x = obj_min_x + (obj_min_x - x);
            obj->dx = -obj->dx;
        }
        if (y >= obj_max_y) {
            y = obj_max_y - (y - obj_max_y);
            obj->dy = -obj->dy;
        }
        if (y < obj_min_x) {
            y = obj_min_x + (obj_min_x - y);
            obj->dy = -obj->dy;
        }
        
        obj->x = x;
        obj->y = y;

        //fprintf(stderr, "blob[%ld]: x=%ld y=%ld dx=%f dy=%f\n", i, obj->x, obj->y, obj->dx, obj->dy);
        //fprintf(stderr, "blob[%ld]: fabs(dx)=%f\n", i, fabs(obj->dx));

        // Apply gravity / friction
        if (obj->dx != 0) {
            if (fabs(obj->dx) < EPSILON) {
                fprintf(stderr, "dx < %f --> 0\n", EPSILON);
                obj->dx = 0;
            } else {
                //fprintf(stderr, "applying friction...\n");
                float next_dx = fabs(obj->dx) * FRICTION_FACTOR;
                //fprintf(stderr, "blob[%ld]: next_dx=%f obj->dx=%f/%f\n", i, next_dx, -1.0f * next_dx, next_dx);
                fprintf(stderr, "blob[%ld]: x=%f dx=%f fabs(dx)=%f next_dx=%f\n", i, obj->x, obj->dx, fabs(obj->dx), (obj->dx < 0) ? (-1.0f * next_dx) : next_dx);
                if (obj->dx < 0) {
                    obj->dx = -1.0f * next_dx;
                } else {
                    obj->dx = next_dx;
                }
            }
        }
    }
    cur_tick++;
}

void render(int cur_frame)
{
    // Attach and clear the screen
    surface_t *disp = display_get();
    rdpq_attach_clear(disp, NULL);

    // Draw the tile background, by playing back the compiled block.
    // This is using copy mode by default, but notice how it can switch
    // to standard mode (aka "1 cycle" in RDP terminology) in a completely
    // transparent way. Even if the block is compiled, the RSP commands within it
    // will adapt its commands to the current render mode, Try uncommenting
    // the line below to see.
    rdpq_debug_log_msg("tiles");
    rdpq_set_mode_copy(false);
    // rdpq_set_mode_standard();
    rspq_block_run(tiles_block);
    
    // Draw the brew sprites. Use standard mode because copy mode cannot handle
    // scaled sprites.
    rdpq_debug_log_msg("sprites");
    rdpq_set_mode_standard();
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_mode_alphacompare(1);                // colorkey (draw pixel with alpha >= 1)

    for (uint32_t i = 0; i < NUM_BLOBS; i++)
    {
        rdpq_sprite_blit(brew_sprite, (int32_t) blobs[i].x, (int32_t) blobs[i].y, &(rdpq_blitparms_t){
            .scale_x = blobs[i].scale_factor, .scale_y = blobs[i].scale_factor,
        });
    }

    rdpq_detach_show();
}

int main()
{
    debug_init_isviewer();
    debug_init_usblog();

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, ANTIALIAS_RESAMPLE);

    controller_init();
    timer_init();

    uint32_t display_width = display_get_width();
    uint32_t display_height = display_get_height();
    
    dfs_init(DFS_DEFAULT_LOCATION);

    rdpq_init();
    rdpq_debug_start();

    fprintf(stderr, "Starting\n");

    brew_sprite = sprite_load("rom:/n64brew.sprite");

    obj_min_x = 0;
    obj_max_x = display_width - brew_sprite->width;
    obj_min_y = 0;
    obj_max_y = display_height - brew_sprite->height;

    for (uint32_t i = 0; i < NUM_BLOBS; i++)
    {
        fprintf(stderr, "init blob[%ld]\n", i);
        object_t *obj = &blobs[i];

        obj->x = 40 + i*160;
        obj->y = 200;
        obj->dx = 0;
        obj->dy = 0;
        fprintf(stderr, "blob[%ld]: x=%f y=%f dx=%f dy=%f\n", i, obj->x, obj->y, obj->dx, obj->dy);
    }

    tiles_sprite = sprite_load("rom:/tiles.sprite");

    surface_t tiles_surf = sprite_get_pixels(tiles_sprite);

    // Create a block for the background, so that we can replay it later.
    rspq_block_begin();

    // Check if the sprite was compiled with a paletted format. Normally
    // we should know this beforehand, but for this demo we pretend we don't
    // know. This also shows how rdpq can transparently work in both modes.
    bool tlut = false;
    tex_format_t tiles_format = sprite_get_format(tiles_sprite);
    if (tiles_format == FMT_CI4 || tiles_format == FMT_CI8) {
        // If the sprite is paletted, turn on palette mode and load the
        // palette in TMEM. We use the mode stack for demonstration,
        // so that we show how a block can temporarily change the current
        // render mode, and then restore it at the end.
        rdpq_mode_push();
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_tex_upload_tlut(sprite_get_palette(tiles_sprite), 0, 16);
        tlut = true;
    }
    uint32_t tile_width = tiles_sprite->width / tiles_sprite->hslices;
    uint32_t tile_height = tiles_sprite->height / tiles_sprite->vslices;
 
    for (uint32_t ty = 0; ty < display_height; ty += tile_height)
    {
        for (uint32_t tx = 0; tx < display_width; tx += tile_width)
        {
            // Load a random tile among the 4 available in the texture,
            // and draw it as a rectangle.
            // Notice that this code is agnostic to both the texture format
            // and the render mode (standard vs copy), it will work either way.
            int s = RANDN(2)*32, t = RANDN(2)*32;
            rdpq_tex_upload_sub(TILE0, &tiles_surf, NULL, s, t, s+32, t+32);
            rdpq_texture_rectangle(TILE0, tx, ty, tx+32, ty+32, s, t);
        }
    }
    
    // Pop the mode stack if we pushed it before
    if (tlut) rdpq_mode_pop();
    tiles_block = rspq_block_end();

    update(0);
    new_timer(TIMER_TICKS(1000000 / 60), TF_CONTINUOUS, update);

    fprintf(stderr, "Entering main loop\n");

    int cur_frame = 0;
    while (1)
    {
        render(cur_frame);

        controller_scan();
        //struct controller_data ckeys = get_keys_down();
        struct controller_data pressed = get_keys_pressed();

        for (uint32_t i = 0; i < NUM_BLOBS; i++)
        {
            object_t *obj = &blobs[i];
            if (pressed.c[i].up) {  // TODO Jump if not mid-air
                obj->dy = -2;
            }

            if (pressed.c[i].left) {
                obj->dx = -3;
            }

            if (pressed.c[i].right) {
                obj->dx = 3;
            }
        }

        cur_frame++;
    }
}