#include <windows.h>
#include <wininet.h>
#include <obs-module.h>
#include <obs-internal.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("screenshot-filter", "en-US")

#define do_log(level, format, ...) \
	blog(level, "[screenshot-filter] " format, ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

static void capture_key_callback(void *data, obs_hotkey_id id,
				 obs_hotkey_t *key, bool pressed);

static bool write_image(const char *destination, uint8_t *image_data_ptr,
			uint32_t image_data_linesize, uint32_t width,
			uint32_t height, int destination_type);
static bool write_data(char *destination, uint8_t *data, size_t len,
		       char *content_type, uint32_t width, uint32_t height,
		       int destination_type);
static bool put_data(char *url, uint8_t *buf, size_t len, char *content_type,
		     int width, int height);

#define SETTING_DESTINATION_TYPE "destination_type"

#define SETTING_DESTINATION_FOLDER "destination_folder"
#define SETTING_DESTINATION_PATH "destinaton_path"
#define SETTING_DESTINATION_URL "destination_url"
#define SETTING_DESTINATION_SHMEM "destination_shmem"

#define SETTING_DESTINATION_PATH_ID 0
#define SETTING_DESTINATION_URL_ID 1
#define SETTING_DESTINATION_SHMEM_ID 2
#define SETTING_DESTINATION_FOLDER_ID 3

#define SETTING_TIMER "timer"
#define SETTING_INTERVAL "interval"
#define SETTING_RAW "raw"

struct screenshot_filter_data {
	obs_source_t *context;

	HANDLE image_writer_thread;

	int destination_type;
	char *destination;
	bool timer;
	float interval;
	bool raw;
	obs_hotkey_id capture_hotkey_id;

	float since_last;
	bool capture;

	uint32_t width;
	uint32_t height;
	gs_texrender_t *texrender;
	gs_texture_t *staging_texture;

	uint8_t *data;
	uint32_t linesize;
	bool ready;

	uint32_t index;
	char shmem_name[256];
	uint32_t shmem_size;
	HANDLE shmem;

	HANDLE mutex;
	bool exit;
	bool exited;
};

static DWORD CALLBACK write_images_thread(struct screenshot_filter_data *filter)
{
	while (!filter->exit) {
		WaitForSingleObject(filter->mutex, INFINITE);
		// copy all props & data inside the mutex, then do the processing/write/put outside of the mutex
		uint8_t *data = NULL;
		char *destination = filter->destination;
		int destination_type = filter->destination_type;
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

		if (data && width > 10 && height > 10) {
			if (destination_type == SETTING_DESTINATION_SHMEM_ID) {
				if (filter->shmem) {
					uint32_t *buf =
						(uint32_t *)MapViewOfFile(
							filter->shmem,
							FILE_MAP_ALL_ACCESS, 0,
							0, filter->shmem_size);

					if (buf) {
						buf[0] = width;
						buf[1] = height;
						buf[2] = linesize;
						buf[3] = filter->index;
						memcpy(&buf[4], data,
						       linesize * height);
					}

					UnmapViewOfFile(buf);
				}
			} else if (raw)
				write_data(destination, data, linesize * height,
					   "image/rgba32", width, height,
					   destination_type);
			else
				write_image(destination, data, linesize, width,
					    height, destination_type);
			filter->index += 1;
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

static bool is_dest_modified(obs_properties_t *props, obs_property_t *unused,
			     obs_data_t *settings)
{
	UNUSED_PARAMETER(unused);

	int type = obs_data_get_int(settings, SETTING_DESTINATION_TYPE);
	obs_property_set_visible(obs_properties_get(props,
						    SETTING_DESTINATION_FOLDER),
				 type == SETTING_DESTINATION_FOLDER_ID);
	obs_property_set_visible(obs_properties_get(props,
						    SETTING_DESTINATION_PATH),
				 type == SETTING_DESTINATION_PATH_ID);
	obs_property_set_visible(obs_properties_get(props,
						    SETTING_DESTINATION_URL),
				 type == SETTING_DESTINATION_URL_ID);
	obs_property_set_visible(obs_properties_get(props,
						    SETTING_DESTINATION_SHMEM),
				 type == SETTING_DESTINATION_SHMEM_ID);

	obs_property_set_visible(obs_properties_get(props, SETTING_RAW),
				 type != SETTING_DESTINATION_SHMEM_ID);

	obs_property_set_visible(obs_properties_get(props, SETTING_TIMER),
				 type != SETTING_DESTINATION_SHMEM_ID);

	bool is_timer_enable = obs_data_get_bool(settings, SETTING_TIMER);
	obs_property_set_visible(obs_properties_get(props, SETTING_INTERVAL),
				 is_timer_enable ||
					 type == SETTING_DESTINATION_SHMEM_ID);

	return true;
}

static bool is_timer_enable_modified(obs_properties_t *props,
				     obs_property_t *unused,
				     obs_data_t *settings)
{
	UNUSED_PARAMETER(unused);

	int type = obs_data_get_int(settings, SETTING_DESTINATION_TYPE);
	bool is_timer_enable = obs_data_get_bool(settings, SETTING_TIMER);
	obs_property_set_visible(obs_properties_get(props, SETTING_INTERVAL),
				 is_timer_enable ||
					 type == SETTING_DESTINATION_SHMEM_ID);

	return true;
}

static obs_properties_t *screenshot_filter_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_list(
		props, SETTING_DESTINATION_TYPE, "Destination Type",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Output to folder",
				  SETTING_DESTINATION_FOLDER_ID);
	obs_property_list_add_int(p, "Output to file",
				  SETTING_DESTINATION_PATH_ID);
	obs_property_list_add_int(p, "Output to URL",
				  SETTING_DESTINATION_URL_ID);
	obs_property_list_add_int(p, "Output to Named Shared Memory",
				  SETTING_DESTINATION_SHMEM_ID);

	obs_property_set_modified_callback(p, is_dest_modified);
	obs_properties_add_path(props, SETTING_DESTINATION_FOLDER,
				"Destination (folder)", OBS_PATH_DIRECTORY,
				"*.*", NULL);
	obs_properties_add_path(props, SETTING_DESTINATION_PATH, "Destination",
				OBS_PATH_FILE_SAVE, "*.*", NULL);
	obs_properties_add_text(props, SETTING_DESTINATION_URL,
				"Destination (url)", OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, SETTING_DESTINATION_SHMEM,
				"Shared Memory Name", OBS_TEXT_DEFAULT);

	obs_property_t *p_enable_timer =
		obs_properties_add_bool(props, SETTING_TIMER, "Enable timer");
	obs_property_set_modified_callback(p_enable_timer,
					   is_timer_enable_modified);

	obs_properties_add_float(props, SETTING_INTERVAL,
					"Interval (seconds)", 0.25, 86400, 0.25);

	obs_properties_add_bool(props, SETTING_RAW, "Raw image");

	return props;
}

static void screenshot_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, SETTING_DESTINATION_TYPE,
				    SETTING_DESTINATION_FOLDER_ID);
	obs_data_set_default_bool(settings, SETTING_TIMER, false);
	obs_data_set_default_double(settings, SETTING_INTERVAL, 2.0f);
	obs_data_set_default_bool(settings, SETTING_RAW, false);
}

static void screenshot_filter_update(void *data, obs_data_t *settings)
{
	struct screenshot_filter_data *filter = data;

	int type = obs_data_get_int(settings, SETTING_DESTINATION_TYPE);
	char *path = obs_data_get_string(settings, SETTING_DESTINATION_PATH);
	char *url = obs_data_get_string(settings, SETTING_DESTINATION_URL);
	char *shmem_name =
		obs_data_get_string(settings, SETTING_DESTINATION_SHMEM);
	char *folder_path =
		obs_data_get_string(settings, SETTING_DESTINATION_FOLDER);
	bool is_timer_enabled = obs_data_get_bool(settings, SETTING_TIMER);

	WaitForSingleObject(filter->mutex, INFINITE);

	filter->destination_type = type;
	if (type == SETTING_DESTINATION_PATH_ID) {
		filter->destination = path;
	} else if (type == SETTING_DESTINATION_URL_ID) {
		filter->destination = url;
	} else if (type == SETTING_DESTINATION_SHMEM_ID) {
		filter->destination = shmem_name;
	} else if (type == SETTING_DESTINATION_FOLDER_ID) {
		filter->destination = folder_path;
	}
	info("Set destination=%s, %d", filter->destination,
	     filter->destination_type);

	filter->timer = is_timer_enabled ||
			type == SETTING_DESTINATION_SHMEM_ID;
	filter->interval = obs_data_get_double(settings, SETTING_INTERVAL);
	filter->raw = obs_data_get_bool(settings, SETTING_RAW);

	ReleaseMutex(filter->mutex);
}

static void *screenshot_filter_create(obs_data_t *settings,
				      obs_source_t *context)
{
	struct screenshot_filter_data *filter =
		bzalloc(sizeof(struct screenshot_filter_data));
	info("Created filter: %p", filter);

	filter->context = context;

	obs_enter_graphics();
	filter->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	filter->image_writer_thread = CreateThread(NULL, 0, write_images_thread,
						   (LPVOID)filter, 0, NULL);
	if (!filter->image_writer_thread) {
		//error("image_writer_thread: Failed to create thread: %d", GetLastError());
		return NULL;
	}
	info("Created image writer thread %d", filter);

	filter->interval = 2.0f;
	filter->shmem_name[0] = '\0';

	filter->mutex = CreateMutexA(NULL, FALSE, NULL);

	obs_source_update(context, settings);

	return filter;
}

static void screenshot_filter_save(void *data, obs_data_t *settings)
{
	struct screenshot_filter_data *filter = data;

	obs_data_array_t *hotkeys = obs_hotkey_save(filter->capture_hotkey_id);
	obs_data_set_array(settings, "capture_hotkey", hotkeys);
	obs_data_array_release(hotkeys);
}

static void make_hotkey(struct screenshot_filter_data *filter)
{
	char *filter_name = obs_source_get_name(filter->context);

	obs_source_t *parent = obs_filter_get_parent(filter->context);
	char *parent_name = obs_source_get_name(parent);

	char hotkey_name[256];
	char hotkey_description[512];
	snprintf(hotkey_name, 255, "Screenshot Filter.%s.%s", parent_name,
		 filter_name);
	snprintf(hotkey_description, 512, "%s: Take screenshot of \"%s\"",
		 filter_name, parent_name);

	filter->capture_hotkey_id = obs_hotkey_register_frontend(
		hotkey_name, hotkey_description, capture_key_callback,
		filter);

	info("Registered hotkey on %s: %s %s, key=%d", filter_name,
		hotkey_name, hotkey_description,
		filter->capture_hotkey_id);
	
}

static void screenshot_filter_load(void *data, obs_data_t *settings)
{
	struct screenshot_filter_data *filter = data;

	info("Registering hotkey on filter load for filter %p", filter);
	make_hotkey(filter);

	obs_data_array_t *hotkeys =
		obs_data_get_array(settings, "capture_hotkey");
	if (filter->capture_hotkey_id && obs_data_array_count(hotkeys)) {
		info("Restoring hotkey settings for %d",
		     filter->capture_hotkey_id);
		obs_hotkey_load(filter->capture_hotkey_id, hotkeys);
	}
	obs_data_array_release(hotkeys);
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

	WaitForSingleObject(filter->mutex, INFINITE);
	obs_enter_graphics();
	gs_texrender_destroy(filter->texrender);
	if (filter->staging_texture) {
		gs_stagesurface_destroy(filter->staging_texture);
	}
	obs_leave_graphics();
	if (filter->data) {
		bfree(filter->data);
	}

	if (filter->shmem) {
		CloseHandle(filter->shmem);
	}
	ReleaseMutex(filter->mutex);
	CloseHandle(filter->mutex);

	bfree(filter);
}

static void screenshot_filter_remove(void *data, obs_source_t *context)
{
	struct screenshot_filter_data *filter = data;
	if (filter->capture_hotkey_id) {
		obs_hotkey_unregister(filter->capture_hotkey_id);
		filter->capture_hotkey_id = 0;
	}
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

	WaitForSingleObject(filter->mutex, INFINITE);
	bool update = false;
	if (width != filter->width || height != filter->height) {
		update = true;

		filter->width = width;
		filter->height = height;

		obs_enter_graphics();
		if (filter->staging_texture) {
			gs_stagesurface_destroy(filter->staging_texture);
		}
		filter->staging_texture = gs_stagesurface_create(
			filter->width, filter->height, GS_RGBA);
		obs_leave_graphics();
		info("Created Staging texture %d by %d: %x", width, height,
		     filter->staging_texture);

		if (filter->data) {
			bfree(filter->data);
		}
		filter->data = bzalloc((width + 32) * height * 4);
		filter->ready = false;
		filter->capture = false;
		filter->since_last = 0.0f;
	}

	if (filter->destination_type == SETTING_DESTINATION_SHMEM_ID &&
	    filter->destination) {
		if (update || strncmp(filter->destination, filter->shmem_name,
				      sizeof(filter->shmem_name))) {
			info("update shmem");
			if (filter->shmem) {
				info("Closing shmem \"%s\": %x",
				     filter->shmem_name, filter->shmem);
				CloseHandle(filter->shmem);
			}
			filter->shmem_size = 12 + (width + 32) * height * 4;
			wchar_t name[256];
			mbstowcs(name, filter->destination, sizeof(name));
			filter->shmem = CreateFileMapping(INVALID_HANDLE_VALUE,
							  NULL, PAGE_READWRITE,
							  0, filter->shmem_size,
							  name);
			strncpy(filter->shmem_name, filter->destination,
				sizeof(filter->shmem_name));
			info("Created shmem \"%s\": %x", filter->shmem_name,
			     filter->shmem);
		}
	}

	if (filter->timer) {
		filter->since_last += t;
		if (filter->since_last > filter->interval - 0.05) {
			filter->capture = true;
			filter->since_last = 0.0f;
		}
	}

	ReleaseMutex(filter->mutex);
}

static void screenshot_filter_render(void *data, gs_effect_t *effect)
{
	struct screenshot_filter_data *filter = data;
	UNUSED_PARAMETER(effect);

	if (!filter->capture_hotkey_id) {
		info("Registering hotkey on filter render for filter %p", filter);
		make_hotkey(filter);
	}

	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);

	if (!parent || !filter->width || !filter->height || !filter->capture) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	gs_texrender_reset(filter->texrender);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(filter->texrender, filter->width,
			       filter->height)) {
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)filter->width, 0.0f,
			 (float)filter->height, -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async) {
			obs_source_default_render(target);
		} else {
			obs_source_video_render(target);
		}

		gs_texrender_end(filter->texrender);
	}

	gs_blend_state_pop();

	gs_effect_t *effect2 = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(filter->texrender);

	if (tex) {
		gs_stage_texture(filter->staging_texture, tex);

		uint8_t *data;
		uint32_t linesize;
		WaitForSingleObject(filter->mutex, INFINITE);
		if (gs_stagesurface_map(filter->staging_texture, &data,
					&linesize)) {
			memcpy(filter->data, data, linesize * filter->height);
			filter->linesize = linesize;
			filter->ready = true;

			gs_stagesurface_unmap(filter->staging_texture);
		}
		filter->capture = false;
		ReleaseMutex(filter->mutex);

		gs_eparam_t *image =
			gs_effect_get_param_by_name(effect2, "image");
		gs_effect_set_texture(image, tex);

		while (gs_effect_loop(effect2, "Draw"))
			gs_draw_sprite(tex, 0, filter->width, filter->height);
	}
}

// code adapted from https://github.com/obsproject/obs-studio/pull/1269 and https://stackoverflow.com/a/12563019
static bool write_image(const char *destination, uint8_t *image_data_ptr,
			uint32_t image_data_linesize, uint32_t width,
			uint32_t height, int destination_type)
{
	bool success = false;

	int ret;
	AVFrame *frame;
	AVPacket pkt;

	if (image_data_ptr == NULL)
		goto err_no_image_data;

	AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
	if (codec == NULL)
		goto err_png_codec_not_found;

	AVCodecContext *codec_context = avcodec_alloc_context3(codec);
	if (codec_context == NULL)
		goto err_png_encoder_context_alloc;

	codec_context->bit_rate = 400000;
	codec_context->width = width;
	codec_context->height = height;
	codec_context->time_base = (AVRational){1, 25};
	codec_context->pix_fmt = AV_PIX_FMT_RGBA;

	if (avcodec_open2(codec_context, codec, NULL) != 0)
		goto err_png_encoder_open;

	frame = av_frame_alloc();
	if (frame == NULL)
		goto err_av_frame_alloc;

	frame->format = codec_context->pix_fmt;
	frame->width = width;
	frame->height = height;

	ret = av_image_alloc(frame->data, frame->linesize, codec_context->width,
			     codec_context->height, codec_context->pix_fmt, 4);
	if (ret < 0)
		goto err_av_image_alloc;

	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	for (int y = 0; y < height; ++y)
		memcpy(frame->data[0] + y * width * 4,
		       image_data_ptr + y * image_data_linesize, width * 4);
	frame->pts = 1;

	int got_output = 0;
	ret = avcodec_encode_video2(codec_context, &pkt, frame, &got_output);
	if (ret == 0 && got_output) {
		success = write_data(destination, pkt.data, pkt.size,
				     "image/png", width, height,
				     destination_type);
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
	// image_data_ptr == NULL

	return success;
}

static bool write_data(char *destination, uint8_t *data, size_t len,
		       char *content_type, uint32_t width, uint32_t height,
		       int destination_type)
{
	bool success = false;

	if (destination_type == SETTING_DESTINATION_PATH_ID) {
		FILE *of = fopen(destination, "wb");

		if (of != NULL) {
			//info("write %s (%d bytes)", destination, len);
			fwrite(data, 1, len, of);
			fclose(of);
			success = true;
		}
	}
	if (destination_type == SETTING_DESTINATION_URL_ID) {
		if (strstr(destination, "http://") != NULL ||
		    strstr(destination, "https://") != NULL) {
			//info("PUT %s (%d bytes)", destination, len);
			success = put_data(destination, data, len, content_type,
					   width, height);
		}
	}
	if (destination_type == SETTING_DESTINATION_FOLDER_ID) {
		FILE *of = fopen(destination, "rb");

		if (of != NULL) {
			fclose(of);
		} else {
			time_t nowunixtime = time(NULL);
			struct tm *nowtime = localtime(&nowunixtime);
			char _file_destination[260];
			char file_destination[260];

			int dest_length = snprintf(
				_file_destination, 259,
				"%s/%d-%02d-%02d_%02d-%02d-%02d", destination,
				nowtime->tm_year + 1900, nowtime->tm_mon + 1,
				nowtime->tm_mday, nowtime->tm_hour,
				nowtime->tm_min, nowtime->tm_sec);

			int repeat_count = 0;
			while (true) {
				if (repeat_count > 5) {
					break;
				}
				dest_length = snprintf(file_destination, 259,
						       "%s", _file_destination);
				if (repeat_count > 0) {
					dest_length = snprintf(
						file_destination, 259,
						"%s_%d.raw", _file_destination,
						repeat_count);
				}
				repeat_count++;

				if (!strcmp(content_type, "image/png")) {
					dest_length = snprintf(
						file_destination, 259, "%s.png",
						file_destination);
				}

				if (dest_length <= 0) {
					break;
				}

				of = fopen(file_destination, "rb");
				if (of != NULL) {
					fclose(of);
					continue;
				}

				of = fopen(file_destination, "wb");
				if (of == NULL) {
					continue;
				} else {
					fwrite(data, 1, len, of);
					fclose(of);
					success = true;
					break;
				}
			}
		}
	}

	return success;
}
static bool put_data(char *url, uint8_t *buf, size_t len, char *content_type,
		     int width, int height)
{
	bool success = false;

	char *host_start = strstr(url, "://");
	if (host_start == NULL)
		return false;
	host_start += 3;

	char *host_end;
	char *port_start = strchr(host_start, ':');
	char *location_start = strchr(host_start, '/');
	if (!location_start)
		location_start = "";

	int port;

	char *https = NULL;
	https = strstr(url, "https://");
	if (https == url) {
		port = 443;

		// unsupported
		warn("https unsupported");
		return false;
	} else {
		port = 80;
	}
	host_end = location_start;

	if (port_start != NULL) {
		// have port specifier
		host_end = port_start;

		char port_str[16] = {0};
		strncat(port_str, port_start + 1,
			min(sizeof(port_str) - 1,
			    (location_start - host_end) - 1));
		if (strlen(port_str) == 0)
			return false;
		port = atoi(port_str);

	}

	char host[128] = {0};
	strncat(host, host_start, min(sizeof(host) - 1, host_end - host_start));
	char *location = location_start;

	HINTERNET hIntrn = InternetOpenA(
		"OBS Screenshot Plugin/1.2.1",
		INTERNET_OPEN_TYPE_PRECONFIG_WITH_NO_AUTOPROXY, NULL, NULL, 0);
	if (!hIntrn)
		goto err_internet_open;

	HINTERNET hConn = InternetConnectA(hIntrn, host, port, NULL, NULL,
					   INTERNET_SERVICE_HTTP, 0, NULL);
	if (!hConn)
		goto err_internet_connect;

	DWORD dwOpenRequestFlags = INTERNET_FLAG_KEEP_CONNECTION |
				   INTERNET_FLAG_NO_COOKIES |
				   INTERNET_FLAG_NO_CACHE_WRITE |
				   INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD;

	HINTERNET hReq = HttpOpenRequestA(hConn, "PUT", location, "HTTP/1.1",
					  NULL, NULL, dwOpenRequestFlags, NULL);
	if (!hReq)
		goto err_http_open_request;

	char header[1024] = {0};
	snprintf(header, sizeof(header), "%sContent-Type: %s\r\n", header,
		 content_type);
	snprintf(header, sizeof(header), "%sImage-Width: %d\r\n", header,
		 width);
	snprintf(header, sizeof(header), "%sImage-Height: %d\r\n", header,
		 height);

	if (HttpSendRequestA(hReq, header, strnlen(header, sizeof(header)), buf,
			     len)) {
		success = true;
		info("Uploaded file to %s:%d%s", host, port, location);
	} else {
		warn("Failed to upload file to http://%s:%d%s - %d", host, port,
		     location, GetLastError());
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

static void capture_key_callback(void *data, obs_hotkey_id id,
				 obs_hotkey_t *key, bool pressed)
{
	struct screenshot_filter_data *filter = data;
	char *filter_name = obs_source_get_name(filter->context);
	info("Got capture_key pressed for %s, id: %d, key: %s, pressed: %d",
	     filter_name, id, key->name, pressed);

	if (id != filter->capture_hotkey_id || !pressed)
		return;

	info("Triggering capture");
	WaitForSingleObject(filter->mutex, INFINITE);
	filter->capture = true;
	ReleaseMutex(filter->mutex);
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

	.save = screenshot_filter_save,
	.load = screenshot_filter_load,

	.video_tick = screenshot_filter_tick,
	.video_render = screenshot_filter_render,

	.filter_remove = screenshot_filter_remove};

bool obs_module_load(void)
{
	obs_register_source(&screenshot_filter);
	return true;
}
