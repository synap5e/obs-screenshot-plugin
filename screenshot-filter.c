#include <windows.h>
#include <wininet.h>
#include <obs-module.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>


OBS_DECLARE_MODULE()
//OBS_MODULE_USE_DEFAULT_LOCALE("screenshot-filter", "en-US")

#define do_log(level, format, ...) \
	blog(level, "[screenshot-filter] " format, ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

static bool write_image(
	const char  *destination,
	uint8_t     *image_data_ptr,
	uint32_t    image_data_linesize,
	uint32_t    width,
	uint32_t    height);
static bool write_data(
	char* destination,
	uint8_t *data,
	size_t len,
	char* content_type,
	uint32_t width,
	uint32_t height);
static bool put_data(
	char *url,
	uint8_t *buf,
	size_t len,
	char *content_type,
	int width,
	int height);

#define SETTING_DESTINATION_IS_FILE "destination_is_file"
#define SETTING_DESTINATION_PATH "destinaton_path"
#define SETTING_DESTINATION_URL "destination_url"
#define SETTING_INTERVAL "interval"
#define SETTING_RAW "raw"

struct screenshot_filter_data {
	obs_source_t                   *context;
	HANDLE                         image_writer_thread;

	char                           *destination;
	float                          interval;
	bool                           raw;

	float                          since_last;
	bool                           capture;

	uint32_t                       width;
	uint32_t                       height;
	gs_texrender_t                 *texrender;
	gs_texture_t                   *staging_texture;

	uint8_t                        *data;
	uint32_t                       linesize;
	bool                           ready;

	HANDLE                         mutex;
	bool                           exit;
	bool                           exited;
};

static DWORD CALLBACK write_images_thread(struct screenshot_filter_data *filter)
{
	while (!filter->exit) {
		WaitForSingleObject(filter->mutex, INFINITE);
		// copy all props & data inside the mutex, then do the processing/write/put outside of the mutex
		uint8_t *data = NULL;
		char* destination = filter->destination;
		uint32_t width = filter->width;
		uint32_t height = filter->height;
		uint32_t linesize = filter->linesize;
		bool raw = filter->raw;
		if (filter->ready) {
			data = bzalloc(linesize * height);
			memcpy(data, filter->data, linesize * height);
			filter->ready = false;
		}
		ReleaseMutex(filter->mutex);

		if (data) {
			if (raw)
				write_data(destination, data, linesize * height, "image/rgba32", linesize / 4, height);
			else
				write_image(destination, data, linesize, linesize / 4, height);
			bfree(data);
		}
		Sleep(200);
	}
	filter->exited = true;

	return 0;
}


static const char *screenshot_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Screenshot Filter";
}

static bool is_file_modified(obs_properties_t *props, obs_property_t *unused, obs_data_t *settings)
{
	UNUSED_PARAMETER(unused);

	bool enabled = obs_data_get_bool(settings, SETTING_DESTINATION_IS_FILE);
	obs_property_t *path = obs_properties_get(props, SETTING_DESTINATION_PATH);
	obs_property_t *url = obs_properties_get(props, SETTING_DESTINATION_URL);
	obs_property_set_visible(path, enabled);
	obs_property_set_visible(url, !enabled);

	return true;
}

static obs_properties_t *screenshot_filter_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_bool(props, SETTING_DESTINATION_IS_FILE, "Ouput to file");
	obs_property_set_modified_callback(p, is_file_modified);
	obs_properties_add_path(props, SETTING_DESTINATION_PATH, "Destination", OBS_PATH_FILE_SAVE, "*.*", NULL);
	obs_properties_add_text(props, SETTING_DESTINATION_URL, "Destination (url)", OBS_TEXT_DEFAULT);

	obs_properties_add_float_slider(props, SETTING_INTERVAL, "Interval (seconds)", 0.25, 60, 0.25);

	obs_properties_add_bool(props, SETTING_RAW, "Raw image");

	return props;
}

static void screenshot_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, SETTING_DESTINATION_IS_FILE, true);
	obs_data_set_default_double(settings, SETTING_INTERVAL, 2.0f);
}

static void screenshot_filter_update(void *data, obs_data_t *settings)
{
	struct screenshot_filter_data *filter = data;

	bool dest_is_file = obs_data_get_bool(settings, SETTING_DESTINATION_IS_FILE);
	char *path = obs_data_get_string(settings, SETTING_DESTINATION_PATH);
	char *url = obs_data_get_string(settings, SETTING_DESTINATION_URL);

	WaitForSingleObject(filter->mutex, INFINITE);

	filter->destination = dest_is_file ? path : url;

	filter->interval = obs_data_get_double(settings, SETTING_INTERVAL);
	filter->raw = obs_data_get_bool(settings, SETTING_RAW);

	ReleaseMutex(filter->mutex);
}

static void *screenshot_filter_create(obs_data_t *settings, obs_source_t *context)
{
	struct screenshot_filter_data *filter = bzalloc(sizeof(struct screenshot_filter_data));
	filter->context = context;

	obs_enter_graphics();
	filter->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	filter->image_writer_thread = CreateThread(NULL, 0, write_images_thread, (LPVOID)filter, 0, NULL);
	if (!filter->image_writer_thread) {
		//error("image_writer_thread: Failed to create thread: %d", GetLastError());
		return NULL;
	}
	info("Created image writer thread");

	filter->interval = 1.0f;

	filter->mutex = CreateMutexA(NULL, FALSE, NULL);

	obs_source_update(context, settings);
	return filter;
}

static void screenshot_filter_destroy(void *data)
{
	struct screenshot_filter_data *filter = data;

	filter->exit = true;
	for (int _ = 0; _ < 500 && !filter->exited; ++_) {
		Sleep(10);
	}
	if (filter->exited) {
		info("Image writer thread stopped");
	} else {
		// This is not good... Hopefully the thread has crashed, otherwise it's going to be working with filter after it has been free'd
		warn("Image writer thread failed to stop");
	}

	obs_enter_graphics();
	gs_texrender_destroy(filter->texrender);
	if (filter->staging_texture) {
		gs_stagesurface_destroy(filter->staging_texture);
	}
	obs_leave_graphics();
	if (filter->data) {
		bfree(filter->data);
	}

	bfree(filter);
}

static void screenshot_filter_tick(void *data, float t)
{
	struct screenshot_filter_data *filter = data;

	obs_source_t *target = obs_filter_get_target(filter->context);

	if (!target) {
		filter->width = 0;
		filter->height = 0;

		if (filter->staging_texture) {
			obs_enter_graphics();
			gs_stagesurface_destroy(filter->staging_texture);
			obs_leave_graphics();
			filter->staging_texture = NULL;
		}
		if (filter->data) {
			bfree(filter->data);
			filter->data = NULL;
		}
		filter->ready = false;

		return;
	}

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);

	if (width != filter->width || height != filter->height) {
		filter->width = width;
		filter->height = height;

		obs_enter_graphics();
		if (filter->staging_texture) {
			gs_stagesurface_destroy(filter->staging_texture);
		}
		filter->staging_texture = gs_stagesurface_create(filter->width, filter->height, GS_RGBA);
		obs_leave_graphics();
		info("Created Staging texture %d by %d: %x", width, height, filter->staging_texture);

		if (filter->data) {
			bfree(filter->data);
		}
		filter->data = bzalloc((width + 32) * height * 4);
		filter->ready = false;
		filter->capture = false;
		filter->since_last = 0.0f;
	}

	filter->since_last += t;
	if (filter->since_last > filter->interval) {
		filter->capture = true;
		filter->since_last = 0.0f;
	}
}


static void screenshot_filter_render(void *data, gs_effect_t *effect)
{
	struct screenshot_filter_data *filter = data;
	UNUSED_PARAMETER(effect);

	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);

	if (!parent || !filter->width || !filter->height) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	gs_texrender_reset(filter->texrender);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(filter->texrender, filter->width, filter->height)) {
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)filter->width, 0.0f, (float)filter->height, -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async) {
			obs_source_default_render(target);
		}
		else
		{
			obs_source_video_render(target);
		}

		gs_texrender_end(filter->texrender);
	}

	gs_blend_state_pop();

	gs_effect_t *effect2 = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(filter->texrender);

	if (tex) {
		if (filter->capture) {
			gs_stage_texture(filter->staging_texture, tex);

			uint8_t *data;
			uint32_t linesize;
			WaitForSingleObject(filter->mutex, INFINITE);
			if (gs_stagesurface_map(filter->staging_texture, &data, &linesize)) {
				memcpy(filter->data, data, linesize * filter->height);
				filter->linesize = linesize;
				filter->ready = true;

				gs_stagesurface_unmap(filter->staging_texture);
			}
			filter->capture = false;
			ReleaseMutex(filter->mutex);
		}

		gs_eparam_t *image = gs_effect_get_param_by_name(effect2, "image");
		gs_effect_set_texture(image, tex);

		while (gs_effect_loop(effect2, "Draw"))
			gs_draw_sprite(tex, 0, filter->width, filter->height);
		}
}

// code adapted from https://github.com/obsproject/obs-studio/pull/1269 and https://stackoverflow.com/a/12563019
static bool write_image(
	const char  *destination,
	uint8_t     *image_data_ptr,
	uint32_t    image_data_linesize,
	uint32_t    width,
	uint32_t    height)
{
	bool success = false;
	
	int ret;
	AVFrame *frame;
	AVPacket pkt;

	if (image_data_ptr == NULL)
		goto err_no_image_data;

	AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
	if (codec == NULL)
		goto err_png_codec_not_found;

	AVCodecContext* codec_context = avcodec_alloc_context3(codec);
	if (codec_context == NULL)
		goto err_png_encoder_context_alloc;

	codec_context->bit_rate = 400000;
	codec_context->width = width;
	codec_context->height = height;
	codec_context->time_base = (AVRational) { 1, 25 };
	codec_context->pix_fmt = AV_PIX_FMT_RGBA;

	if (avcodec_open2(codec_context, codec, NULL) != 0)
		goto err_png_encoder_open;

	frame = av_frame_alloc();
	if (frame == NULL)
		goto err_av_frame_alloc;

	frame->format = codec_context->pix_fmt;
	frame->width = codec_context->width;
	frame->height = codec_context->height;

	ret = av_image_alloc(frame->data, frame->linesize, codec_context->width, codec_context->height, codec_context->pix_fmt, 32);
	if (ret < 0)
		goto err_av_image_alloc;

	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	memcpy(frame->data[0], image_data_ptr, image_data_linesize * height);
	frame->pts = 1;

	int got_output = 0;
	ret = avcodec_encode_video2(codec_context, &pkt, frame, &got_output);
	if (ret == 0 && got_output)
	{
		success = write_data(destination, pkt.data, pkt.size, "image/png", width, height);
		av_free_packet(&pkt);
	}

	av_freep(frame->data);

err_av_image_alloc:
	// Failed allocating image data buffer
	av_frame_free(&frame);
	frame = NULL;

err_av_frame_alloc:
	// Failed allocating frame
	avcodec_close(codec_context);

err_png_encoder_open:
	// Failed opening PNG encoder
	avcodec_free_context(&codec_context);
	codec_context = NULL;

err_png_encoder_context_alloc:
	// failed allocating PNG encoder context
	// no need to free AVCodec* codec
err_png_codec_not_found:
	// PNG encoder not found
err_no_image_data:
	// image_data_ptr == nullptr

	return success;
}

static bool write_data(
	char* destination,
	uint8_t *data,
	size_t len,
	char* content_type,
	uint32_t width,
	uint32_t height)
{
	bool success = false;
	if (strstr(destination, "http://") == NULL && strstr(destination, "https://") == NULL) {

		FILE *of = fopen(destination, "wb");

		if (of != NULL) {
			//info("write %s (%d bytes)", destination, len);
			fwrite(data, 1, len, of);
			fclose(of);
			success = true;
		}
	}
	else {
		//info("PUT %s (%d bytes)", destination, len);
		success = put_data(destination, data, len, content_type, width, height);
	}
	return success;
}
static bool put_data(char *url, uint8_t *buf, size_t len, char *content_type, int width, int height)
{
	bool success = false;

	char *host_start = strchr(url, '/');
	if (host_start == NULL)
		return false;
	host_start += 2;

	char *host_end;
	char *port_start = strchr(host_start, ':');
	char *location_start = strchr(host_start, '/');
	if (!location_start)
		return false;

	int port;
	if (port_start != NULL) {
		// have port specifier
		host_end = port_start;

		char port_str[16] = { 0 };
		strncat(port_str, port_start + 1, min(sizeof(port_str) - 1, (location_start - host_end) - 1));
		if (strlen(port_str) == 0)
			return false;
		port = atoi(port_str);

	}
	else {
		char *https = strstr(url, "https");
		if (https == NULL || https > host_start) {
			port = 443;

			// unsupported
			warn("https unsupported");
			return false;
		}
		else {
			port = 80;
		}
		host_end = location_start;
	}

	char host[128] = { 0 };
	strncat(host, host_start, min(sizeof(host) - 1, host_end - host_start));
	char *location = location_start;

	HINTERNET hIntrn = InternetOpenA(
		"OBS Screenshot Plugin/1.0.0",
		INTERNET_OPEN_TYPE_PRECONFIG_WITH_NO_AUTOPROXY,
		NULL,
		NULL,
		0
	);
	if (!hIntrn)
		goto err_internet_open;

	HINTERNET hConn = InternetConnectA(
		hIntrn,
		host,
		port,
		NULL,
		NULL,
		INTERNET_SERVICE_HTTP,
		0,
		NULL
	);
	if (!hConn)
		goto err_internet_connect;

	DWORD dwOpenRequestFlags =
		INTERNET_FLAG_KEEP_CONNECTION |
		INTERNET_FLAG_NO_COOKIES |
		INTERNET_FLAG_NO_CACHE_WRITE |
		INTERNET_FLAG_NO_UI |
		INTERNET_FLAG_RELOAD;

	HINTERNET hReq = HttpOpenRequestA(
		hConn,
		"PUT",
		location,
		"HTTP/1.1",
		NULL,
		NULL,
		dwOpenRequestFlags,
		NULL
	);
	if (!hReq)
		goto err_http_open_request;

	char header[1024] = { 0 };
	snprintf(header, sizeof(header), "%sContent-Type: %s\r\n", header, content_type);
	snprintf(header, sizeof(header), "%sImage-Width: %d\r\n", header, width);
	snprintf(header, sizeof(header), "%sImage-Height: %d\r\n", header, height);

	if (HttpSendRequestA(
		hReq,
		header,
		strnlen(header, sizeof(header)),
		buf,
		len))
	{
		success = true;
		info("Uploaded file to %s:%d%s", host, port, location);
	}
	else {
		warn("Failed to upload file to http://%s:%d%s - %d", host, port, location, GetLastError());
	}
	
	InternetCloseHandle(hReq);

err_http_open_request:
	InternetCloseHandle(hConn);

err_internet_connect:
	InternetCloseHandle(hIntrn);

err_internet_open:
	// nothing to close

	return success;
}


struct obs_source_info screenshot_filter = {
	.id = "screenshot_filter",

	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,

	.get_name = screenshot_filter_get_name,

	.get_properties = screenshot_filter_properties,
	.get_defaults = screenshot_filter_defaults,
	.update = screenshot_filter_update,

	.create = screenshot_filter_create,
	.destroy = screenshot_filter_destroy,

	.video_tick = screenshot_filter_tick,
	.video_render = screenshot_filter_render
};


bool obs_module_load(void)
{
	obs_register_source(&screenshot_filter);
	return true;
}

