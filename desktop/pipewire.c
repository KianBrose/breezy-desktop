// based on example https://docs.pipewire.org/video-play_8c-example.html

#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include <SDL3/SDL.h>
#include <spa/utils/result.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/tag-utils.h>
#include <spa/param/props.h>
#include <spa/debug/format.h>
#include <spa/debug/pod.h>
 
#include <pipewire/pipewire.h>
#include <pthread.h>
 
#define WIDTH   1920
#define HEIGHT  1080
 
#define MAX_BUFFERS     64

#define FRAME_TICKS_NS 1000000000.0 / 60.0

void log_video_info(struct spa_video_info *info) {
    const char *media_type = spa_debug_type_find_name(spa_type_media_type, info->media_type);
    const char *media_subtype = spa_debug_type_find_name(spa_type_media_subtype, info->media_subtype);

    printf("Media Type: %s\n", media_type);
    printf("Media Subtype: %s\n", media_subtype);

    if (info->media_subtype == SPA_MEDIA_SUBTYPE_raw) {
        printf("Video Format: %s\n", spa_debug_type_find_name(spa_type_video_format, info->info.raw.format));
        printf("Size: %dx%d\n", info->info.raw.size.width, info->info.raw.size.height);
        printf("Framerate: %d/%d\n", info->info.raw.framerate.num, info->info.raw.framerate.denom);
    }
}
 
struct pixel {
        float r, g, b, a;
};
 
struct data {
        bool ready;
        const char *path;
 
        SDL_Renderer *renderer;
        SDL_Window *window;
        SDL_Texture *texture;
        SDL_Texture *cursor;
 
        struct pw_main_loop *loop;
 
        struct pw_stream *stream;
        struct spa_hook stream_listener;
 
        struct spa_io_position *position;
 
        struct spa_video_info format;
        int32_t stride;
        struct spa_rectangle size;
 
        int counter;
        SDL_FRect rect;
        SDL_FRect cursor_rect;
        bool is_yuv;

        pthread_mutex_t lock;
};
 
static void handle_events(struct data *data)
{
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                        pw_main_loop_quit(data->loop);
                        break;
                }
        }
}
 
/* our data processing function is in general:
 *
 *  struct pw_buffer *b;
 *  b = pw_stream_dequeue_buffer(stream);
 *
 *  .. do stuff with buffer ...
 *
 *  pw_stream_queue_buffer(stream, b);
 */
static void on_process(void *_data)
{
        struct data *data = _data;
        pthread_mutex_lock(&data->lock);
        struct pw_stream *stream = data->stream;
        struct pw_buffer *b;
        struct spa_buffer *buf;
        void *sdata, *ddata;
        int sstride, dstride, ostride;
        struct spa_meta_region *mc;
        struct spa_meta_cursor *mcs;
        uint32_t i, j;
        uint8_t *src, *dst;
        bool render_cursor = false;
 
        b = NULL;
        while (true) {
                struct pw_buffer *t;
                if ((t = pw_stream_dequeue_buffer(stream)) == NULL)
                        break;
                if (b)
                        pw_stream_queue_buffer(stream, b);
                b = t;
        }
        if (b == NULL) {
                pw_log_warn("out of buffers: %m");
                pthread_mutex_unlock(&data->lock);
                return;
        }
 
        buf = b->buffer;
 
        pw_log_trace("new buffer %p", buf);
 
        handle_events(data);
 
        if ((sdata = buf->datas[0].data) == NULL)
                goto done;
 
        /* get the videocrop metadata if any */
        if ((mc = spa_buffer_find_meta_data(buf, SPA_META_VideoCrop, sizeof(*mc))) &&
            spa_meta_region_is_valid(mc)) {
                data->rect.x = mc->region.position.x;
                data->rect.y = mc->region.position.y;
                data->rect.w = mc->region.size.width;
                data->rect.h = mc->region.size.height;
        }
        /* get cursor metadata */
        // if ((mcs = spa_buffer_find_meta_data(buf, SPA_META_Cursor, sizeof(*mcs))) &&
        //     spa_meta_cursor_is_valid(mcs)) {
        //         struct spa_meta_bitmap *mb;
        //         void *cdata;
        //         int cstride;
 
        //         data->cursor_rect.x = mcs->position.x;
        //         data->cursor_rect.y = mcs->position.y;
 
        //         mb = SPA_PTROFF(mcs, mcs->bitmap_offset, struct spa_meta_bitmap);
        //         data->cursor_rect.w = mb->size.width;
        //         data->cursor_rect.h = mb->size.height;
 
        //         if (data->cursor == NULL) {
        //                 data->cursor = SDL_CreateTexture(data->renderer,
        //                                          id_to_sdl_format(mb->format),
        //                                          SDL_TEXTUREACCESS_STREAMING,
        //                                          mb->size.width, mb->size.height);
        //                 SDL_SetTextureBlendMode(data->cursor, SDL_BLENDMODE_BLEND);
        //         }
 
 
        //         if (SDL_LockTexture(data->cursor, NULL, &cdata, &cstride) < 0) {
        //                 fprintf(stderr, "Couldn't lock cursor texture: %s\n", SDL_GetError());
        //                 goto done;
        //         }
 
        //         /* copy the cursor bitmap into the texture */
        //         src = SPA_PTROFF(mb, mb->offset, uint8_t);
        //         dst = cdata;
        //         ostride = SPA_MIN(cstride, mb->stride);
 
        //         for (i = 0; i < mb->size.height; i++) {
        //                 memcpy(dst, src, ostride);
        //                 dst += cstride;
        //                 src += mb->stride;
        //         }
        //         SDL_UnlockTexture(data->cursor);
 
        //         render_cursor = true;
        // }
 
        /* copy video image in texture */
        if (data->is_yuv) {
                sstride = data->stride;
                SDL_UpdateYUVTexture(data->texture,
                                NULL,
                                sdata,
                                sstride,
                                SPA_PTROFF(sdata, sstride * data->size.height, void),
                                sstride / 2,
                                SPA_PTROFF(sdata, 5 * (sstride * data->size.height) / 4, void),
                                sstride / 2);
        }
        else {
                if (SDL_LockTexture(data->texture, NULL, &ddata, &dstride) < 0) {
                        fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
                        goto done;
                }
 
                sstride = buf->datas[0].chunk->stride;
                if (sstride == 0)
                        sstride = buf->datas[0].chunk->size / data->size.height;
                ostride = SPA_MIN(sstride, dstride);
 
                src = sdata;
                dst = ddata;
 
                if (data->format.media_subtype == SPA_MEDIA_SUBTYPE_dsp) {
                        for (i = 0; i < data->size.height; i++) {
                                struct pixel *p = (struct pixel *) src;
                                for (j = 0; j < data->size.width; j++) {
                                        dst[j * 4 + 0] = SPA_CLAMP(p[j].r * 255.0f, 0, 255);
                                        dst[j * 4 + 1] = SPA_CLAMP(p[j].g * 255.0f, 0, 255);
                                        dst[j * 4 + 2] = SPA_CLAMP(p[j].b * 255.0f, 0, 255);
                                        dst[j * 4 + 3] = SPA_CLAMP(p[j].a * 255.0f, 0, 255);
                                }
                                src += sstride;
                                dst += dstride;
                        }
                } else {
                        for (i = 0; i < data->size.height; i++) {
                                memcpy(dst, src, ostride);
                                src += sstride;
                                dst += dstride;
                        }
                }
                SDL_UnlockTexture(data->texture);
        }

        data->ready = true;
 
      done:
        pw_stream_queue_buffer(stream, b);
        pthread_mutex_unlock(&data->lock);
}
 
static void on_stream_state_changed(void *_data, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error)
{
        struct data *data = _data;
        pthread_mutex_lock(&data->lock);
        log_video_info(&data->format);
        fprintf(stderr, "stream state: \"%s\"\n", pw_stream_state_as_string(state));
        switch (state) {
        case PW_STREAM_STATE_UNCONNECTED:
                pw_main_loop_quit(data->loop);
                break;
        case PW_STREAM_STATE_PAUSED:
                /* because we started inactive, activate ourselves now */
                pw_stream_set_active(data->stream, true);
                break;
        case PW_STREAM_STATE_ERROR:
                fprintf(stderr, "stream error: \"%s\"\n", error);
                break;
        default:
                break;
        }
        pthread_mutex_unlock(&data->lock);
}
 
static void
on_stream_io_changed(void *_data, uint32_t id, void *area, uint32_t size)
{
        struct data *data = _data;
        pthread_mutex_lock(&data->lock);
 
        switch (id) {
        case SPA_IO_Position:
                data->position = area;
                break;
        }
        pthread_mutex_unlock(&data->lock);
}
 
/* Be notified when the stream param changes. We're only looking at the
 * format changes.
 *
 * We are now supposed to call pw_stream_finish_format() with success or
 * failure, depending on if we can support the format. Because we gave
 * a list of supported formats, this should be ok.
 *
 * As part of pw_stream_finish_format() we can provide parameters that
 * will control the buffer memory allocation. This includes the metadata
 * that we would like on our buffer, the size, alignment, etc.
 */
static void
on_stream_param_changed(void *_data, uint32_t id, const struct spa_pod *param)
{
        static bool already_set = false;
        if (already_set) return;

        struct data *data = _data;
        pthread_mutex_lock(&data->lock);
        struct pw_stream *stream = data->stream;
        uint8_t params_buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
        const struct spa_pod *params[5];
        Uint32 sdl_format;
        void *d;
        int32_t mult, size;
 
        if (param != NULL && id == SPA_PARAM_Tag) {
                spa_debug_pod(0, NULL, param);
                goto done;
        }
        /* NULL means to clear the format */
        if (param == NULL || id != SPA_PARAM_Format)
                goto done;
 
        fprintf(stderr, "got format:\n");
        spa_debug_format(2, NULL, param);
 
        if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0)
                goto done;
 
        if (data->format.media_type != SPA_MEDIA_TYPE_video)
                goto done;
 
        switch (data->format.media_subtype) {
        case SPA_MEDIA_SUBTYPE_raw:
                already_set = true;
                
                /* call a helper function to parse the format for us. */
                spa_format_video_raw_parse(param, &data->format.info.raw);
                sdl_format = SDL_PIXELFORMAT_BGRA32;
                data->size = SPA_RECTANGLE(data->format.info.raw.size.width,
                                data->format.info.raw.size.height);
                mult = 1;

                printf("Format: %d, Size: %dx%d\n", data->format.info.raw.format, data->size.width, data->size.height);
                break;
        case SPA_MEDIA_SUBTYPE_dsp:
                spa_format_video_dsp_parse(param, &data->format.info.dsp);
                if (data->format.info.dsp.format != SPA_VIDEO_FORMAT_DSP_F32)
                        goto done;
                sdl_format = SDL_PIXELFORMAT_RGBA32;
                data->size = SPA_RECTANGLE(data->position->video.size.width,
                                data->position->video.size.height);
                mult = 4;
                break;
        default:
                sdl_format = SDL_PIXELFORMAT_UNKNOWN;
                break;
        }
 
        if (sdl_format == SDL_PIXELFORMAT_UNKNOWN) {
                pw_stream_set_error(stream, -EINVAL, "unknown pixel format");
                goto done;
        }
        if (data->size.width == 0 || data->size.height == 0) {
                pw_stream_set_error(stream, -EINVAL, "invalid size");
                goto done;
        }
 
        data->texture = SDL_CreateTexture(data->renderer,
                                          sdl_format,
                                          SDL_TEXTUREACCESS_STREAMING,
                                          data->size.width,
                                          data->size.height);
        SDL_LockTexture(data->texture, NULL, &d, &data->stride);
        SDL_UnlockTexture(data->texture);
 
        switch(sdl_format) {
        case SDL_PIXELFORMAT_YV12:
        case SDL_PIXELFORMAT_IYUV:
                size = (data->stride * data->size.height) * 3 / 2;
                data->is_yuv = true;
                break;
        default:
                size = data->stride * data->size.height;
                break;
        }
 
        data->rect.x = 0;
        data->rect.y = 0;
        data->rect.w = data->size.width;
        data->rect.h = data->size.height;
 
        /* a SPA_TYPE_OBJECT_ParamBuffers object defines the acceptable size,
         * number, stride etc of the buffers */
        params[0] = spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
                SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
                SPA_PARAM_BUFFERS_size,    SPA_POD_Int(size * mult),
                SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(data->stride * mult),
                SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemPtr)));
 
        /* a header metadata with timing information */
        params[1] = spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
        /* video cropping information */
        params[2] = spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
                SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
#define CURSOR_META_SIZE(w,h)   (sizeof(struct spa_meta_cursor) + \
                                 sizeof(struct spa_meta_bitmap) + w * h * 4)
        /* cursor information */
        params[3] = spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
                SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
                                CURSOR_META_SIZE(64,64),
                                CURSOR_META_SIZE(1,1),
                                CURSOR_META_SIZE(256,256)));
 
        /* we are done */
        pw_stream_update_params(stream, params, 4);

        done:
                pthread_mutex_unlock(&data->lock);
}
 
/* these are the stream events we listen for */
static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .state_changed = on_stream_state_changed,
        .io_changed = on_stream_io_changed,
        .param_changed = on_stream_param_changed,
        .process = on_process,
};
 
static void do_quit(void *userdata, int signal_number)
{
        struct data *data = userdata;
        pw_main_loop_quit(data->loop);
}
 
struct data data = { 0, };
static Uint64 next_render_time;

Uint64 render_time_left(void)
{
    Uint64 now;

    now = SDL_GetTicksNS();
    if(next_render_time <= now)
        return 0;
    else
        return next_render_time - now;
}

void* render_thread(void *arg) {
        next_render_time = SDL_GetTicksNS() + FRAME_TICKS_NS;
        while (true) {
                SDL_DelayNS(render_time_left());
                if (data.ready) {
                        pthread_mutex_lock(&data.lock);
                        SDL_RenderClear(data.renderer);
                        SDL_RenderTexture(data.renderer, data.texture, &data.rect, NULL);
                        SDL_RenderPresent(data.renderer);
                        pthread_mutex_unlock(&data.lock);
                }
                next_render_time += FRAME_TICKS_NS;
        }
}

int pw_setup(int node_id) {
        const struct spa_pod *params[3];
        uint8_t buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        struct pw_properties *props;
        int res, n_params;
 
        pw_init(NULL, NULL);
 
        /* create a main loop */
        data.lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
        data.loop = pw_main_loop_new(NULL);
 
        pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
        pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);
 
        /* create a simple stream, the simple stream manages to core and remote
         * objects for you if you don't need to deal with them
         *
         * If you plan to autoconnect your stream, you need to provide at least
         * media, category and role properties
         *
         * Pass your events and a user_data pointer as the last arguments. This
         * will inform you about the stream state. The most important event
         * you need to listen to is the process event where you need to consume
         * the data provided to you.
         */
        props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                        PW_KEY_MEDIA_CATEGORY, "Capture",
                        PW_KEY_MEDIA_ROLE, "Screen",
                        NULL),
 
        data.stream = pw_stream_new_simple(
                        pw_main_loop_get_loop(data.loop),
                        "video-play",
                        props,
                        &stream_events,
                        &data);
 
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "vulkan");
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
                fprintf(stderr, "can't initialize SDL: %s\n", SDL_GetError());
                return -1;
        }


        int numdisplays;
        SDL_DisplayID *display_ids = SDL_GetDisplays(&numdisplays);
        printf("Number of displays: %i\n", numdisplays);
        int found_xr_display_index = -1;
        for(int i = 0; i < numdisplays; ++i) {
                if (strcmp(SDL_GetDisplayName(display_ids[i]), "Air 87\"") == 0) {
                        found_xr_display_index = i;
                        break;
                }
        }
 
        if (SDL_CreateWindowAndRenderer
            (WIDTH, HEIGHT, SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN, &data.window, &data.renderer)) {
                fprintf(stderr, "can't create window: %s\n", SDL_GetError());
                return -1;
        }
 
        /* now connect the stream, we need a direction (input/output),
         * an optional target node to connect to, some flags and parameters
         */
        if ((res = pw_stream_connect(data.stream,
                          PW_DIRECTION_INPUT,
                          node_id,
                          PW_STREAM_FLAG_INACTIVE |
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS,  /* try to automatically connect this stream */
                          NULL, 0))            /* extra parameters, see above */ < 0) {
                fprintf(stderr, "can't connect: %s\n", spa_strerror(res));
                return -1;
        }

        pthread_t render_thread_id;
        pthread_create(&render_thread_id, NULL, render_thread, NULL);
 
        /* do things until we quit the mainloop */
        pw_main_loop_run(data.loop);
        pthread_join(render_thread_id, NULL);
}

void pw_teardown() {
    pw_stream_destroy(data.stream);
    pw_main_loop_destroy(data.loop);

    SDL_DestroyTexture(data.texture);
    if (data.cursor)
            SDL_DestroyTexture(data.cursor);
    SDL_DestroyRenderer(data.renderer);
    SDL_DestroyWindow(data.window);
    pw_deinit();
}