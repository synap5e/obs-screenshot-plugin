#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <wininet.h>
#include <obs-module.h>
#include <obs-hotkey.h>
#include <util/darray.h>
#include <util/platform.h>

// PNG encoding is done with stb_image_write instead of FFmpeg: linking against
// OBS's FFmpeg DLLs ties the plugin to one OBS major version (avcodec-60/61/62).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STBIW_MALLOC(sz) bmalloc(sz)
#define STBIW_REALLOC(p, newsz) brealloc(p, newsz)
#define STBIW_FREE(p) bfree(p)
#include "stb_image_write.h"

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
static bool write_data(const char *destination, uint8_t *data, size_t len,
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
	gs_stagesurf_t *staging_texture;

	uint8_t *data;
	uint32_t linesize;
	bool ready;

	uint32_t index;
	char shmem_name[256];
	uint32_t shmem_size;
	HANDLE shmem;

	HANDLE mutex;
	volatile bool exit;
	bool exited;
};

static DWORD CALLBACK write_images_thread(struct screenshot_filter_data *filter)
{
	while (!filter->exit) {
		WaitForSingleObject(filter->mutex, INFINITE);
		// copy all props & data inside the mutex, then do the processing/write/put outside of the mutex
		uint8_t *data = NULL;
		char *destination = NULL;
		int destination_type = filter->destination_type;
		uint32_t width = filter->width;
		uint32_t height = filter->height;
		uint32_t linesize = filter->linesize;
		bool raw = filter->raw;
		bool ready = filter->ready && width > 10 && height > 10;
		filter->ready = false;

		if (ready && destination_type == SETTING_DESTINATION_SHMEM_ID) {
			// write the mapping while holding the mutex: tick may
			// recreate it with a different size at any time
			size_t frame_size = (size_t)linesize * height;
			if (filter->shmem &&
			    frame_size + 16 <= filter->shmem_size) {
				uint32_t *buf = (uint32_t *)MapViewOfFile(
					filter->shmem, FILE_MAP_ALL_ACCESS, 0,
					0, filter->shmem_size);
				if (buf) {
					buf[0] = width;
					buf[1] = height;
					buf[2] = linesize;
					buf[3] = filter->index;
					memcpy(&buf[4], filter->data,
					       frame_size);
					UnmapViewOfFile(buf);
				}
			}
			filter->index += 1;
		} else if (ready && filter->destination &&
			   *filter->destination) {
			data = bzalloc(linesize * height);
			memcpy(data, filter->data, linesize * height);
			// update() may replace the string while we write
			destination = bstrdup(filter->destination);
		}
		ReleaseMutex(filter->mutex);

		if (data) {
			if (raw)
				write_data(destination, data, linesize * height,
					   "image/rgba32", width, height,
					   destination_type);
			else
				write_image(destination, data, linesize, width,
					    height, destination_type);
			filter->index += 1;
			bfree(data);
			bfree(destination);
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

	int type = (int)obs_data_get_int(settings, SETTING_DESTINATION_TYPE);
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

	int type = (int)obs_data_get_int(settings, SETTING_DESTINATION_TYPE);
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

	obs_properties_add_float(props, SETTING_INTERVAL, "Interval (seconds)",
				 0.25, 86400, 0.25);

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

	int type = (int)obs_data_get_int(settings, SETTING_DESTINATION_TYPE);
	const char *path =
		obs_data_get_string(settings, SETTING_DESTINATION_PATH);
	const char *url =
		obs_data_get_string(settings, SETTING_DESTINATION_URL);
	const char *shmem_name =
		obs_data_get_string(settings, SETTING_DESTINATION_SHMEM);
	const char *folder_path =
		obs_data_get_string(settings, SETTING_DESTINATION_FOLDER);
	bool is_timer_enabled = obs_data_get_bool(settings, SETTING_TIMER);

	WaitForSingleObject(filter->mutex, INFINITE);

	filter->destination_type = type;
	// keep our own copy: strings from settings can be freed when the
	// user edits the properties, while the writer thread still uses them
	const char *destination = NULL;
	if (type == SETTING_DESTINATION_PATH_ID) {
		destination = path;
	} else if (type == SETTING_DESTINATION_URL_ID) {
		destination = url;
	} else if (type == SETTING_DESTINATION_SHMEM_ID) {
		destination = shmem_name;
	} else if (type == SETTING_DESTINATION_FOLDER_ID) {
		destination = folder_path;
	}
	bfree(filter->destination);
	filter->destination = destination ? bstrdup(destination) : NULL;
	info("Set destination=%s, %d", filter->destination,
	     filter->destination_type);

	filter->timer = is_timer_enabled ||
			type == SETTING_DESTINATION_SHMEM_ID;
	filter->interval =
		(float)obs_data_get_double(settings, SETTING_INTERVAL);
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

	filter->interval = 2.0f;
	filter->shmem_name[0] = '\0';

	// the writer thread waits on the mutex right away, so create it first
	filter->mutex = CreateMutexA(NULL, FALSE, NULL);
	if (!filter->mutex) {
		warn("Failed to create mutex: %lu", GetLastError());
		obs_enter_graphics();
		gs_texrender_destroy(filter->texrender);
		obs_leave_graphics();
		bfree(filter);
		return NULL;
	}

	filter->image_writer_thread = CreateThread(
		NULL, 0, (LPTHREAD_START_ROUTINE)write_images_thread,
		(LPVOID)filter, 0, NULL);
	if (!filter->image_writer_thread) {
		warn("Failed to create image writer thread: %lu",
		     GetLastError());
		obs_enter_graphics();
		gs_texrender_destroy(filter->texrender);
		obs_leave_graphics();
		CloseHandle(filter->mutex);
		bfree(filter);
		return NULL;
	}
	info("Created image writer thread for filter %p", filter);

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
	const char *filter_name = obs_source_get_name(filter->context);

	obs_source_t *parent = obs_filter_get_parent(filter->context);
	const char *parent_name = obs_source_get_name(parent);

	char hotkey_name[256];
	char hotkey_description[512];
	snprintf(hotkey_name, 255, "Screenshot Filter.%s.%s", parent_name,
		 filter_name);
	snprintf(hotkey_description, 512, "%s: Take screenshot of \"%s\"",
		 filter_name, parent_name);

	filter->capture_hotkey_id = obs_hotkey_register_frontend(
		hotkey_name, hotkey_description, capture_key_callback, filter);

	info("Registered hotkey on %s: %s %s, key=%llu", filter_name, hotkey_name,
	     hotkey_description,
	     (unsigned long long)filter->capture_hotkey_id);
}

static void screenshot_filter_load(void *data, obs_data_t *settings)
{
	struct screenshot_filter_data *filter = data;

	// render may already have registered it
	if (!filter->capture_hotkey_id) {
		info("Registering hotkey on filter load for filter %p",
		     filter);
		make_hotkey(filter);
	}

	obs_data_array_t *hotkeys =
		obs_data_get_array(settings, "capture_hotkey");
	if (filter->capture_hotkey_id && obs_data_array_count(hotkeys)) {
		info("Restoring hotkey settings for %llu",
		     (unsigned long long)filter->capture_hotkey_id);
		obs_hotkey_load(filter->capture_hotkey_id, hotkeys);
	}
	obs_data_array_release(hotkeys);
}

static void screenshot_filter_destroy(void *data)
{
	struct screenshot_filter_data *filter = data;

	// the hotkey callback must not run on a freed filter
	if (filter->capture_hotkey_id) {
		obs_hotkey_unregister(filter->capture_hotkey_id);
		filter->capture_hotkey_id = 0;
	}

	// Wait for the writer thread to finish for real: freeing the filter
	// while it is still encoding or uploading crashes OBS. Uploads are
	// bounded by the WinINet timeouts set in put_data.
	filter->exit = true;
	WaitForSingleObject(filter->image_writer_thread, INFINITE);
	CloseHandle(filter->image_writer_thread);
	info("Image writer thread stopped");

	// same lock order as tick and render: graphics first, then mutex
	obs_enter_graphics();
	WaitForSingleObject(filter->mutex, INFINITE);
	gs_texrender_destroy(filter->texrender);
	if (filter->staging_texture) {
		gs_stagesurface_destroy(filter->staging_texture);
	}
	if (filter->data) {
		bfree(filter->data);
	}
	bfree(filter->destination);

	if (filter->shmem) {
		CloseHandle(filter->shmem);
	}
	ReleaseMutex(filter->mutex);
	obs_leave_graphics();
	CloseHandle(filter->mutex);

	bfree(filter);
}

static void screenshot_filter_remove(void *data, obs_source_t *context)
{
	struct screenshot_filter_data *filter = data;
	// libobs calls filter_remove even if create() failed and returned NULL
	if (!filter)
		return;
	if (filter->capture_hotkey_id) {
		obs_hotkey_unregister(filter->capture_hotkey_id);
		filter->capture_hotkey_id = 0;
	}
}

static void screenshot_filter_tick(void *data, float t)
{
	struct screenshot_filter_data *filter = data;

	obs_source_t *target = obs_filter_get_target(filter->context);

	uint32_t width = target ? obs_source_get_base_width(target) : 0;
	uint32_t height = target ? obs_source_get_base_height(target) : 0;

	// width/height are only written here, on the graphics thread, so they
	// can be compared before locking. Lock order everywhere: graphics
	// context first, then the filter mutex (render runs inside graphics).
	bool update = width != filter->width || height != filter->height;
	if (update)
		obs_enter_graphics();
	WaitForSingleObject(filter->mutex, INFINITE);
	if (update) {
		filter->width = width;
		filter->height = height;

		if (filter->staging_texture) {
			gs_stagesurface_destroy(filter->staging_texture);
			filter->staging_texture = NULL;
		}
		if (width && height)
			filter->staging_texture = gs_stagesurface_create(
				width, height, GS_RGBA);

		if (filter->data) {
			bfree(filter->data);
			filter->data = NULL;
		}
		// A source can report 0x0 (e.g. a media source restarting);
		// OBS 30+ aborts on bmalloc(0), so only allocate for a real size.
		// only allocate when the GPU accepted the size (<= 16384)
		if (filter->staging_texture) {
			filter->data = bzalloc((size_t)width * height * 4);
			info("Created Staging texture %u by %u: %p", width,
			     height, filter->staging_texture);
		} else {
			info("Unsupported source size %ux%u, capture paused",
			     width, height);
		}
		filter->ready = false;
		filter->capture = false;
		filter->since_last = 0.0f;
	}

	if (!width || !height || !filter->staging_texture) {
		ReleaseMutex(filter->mutex);
		if (update)
			obs_leave_graphics();
		return;
	}

	if (filter->destination_type == SETTING_DESTINATION_SHMEM_ID &&
	    filter->destination) {
		// shmem_name keeps at most sizeof - 1 bytes, compare as many
		if (update || strncmp(filter->destination, filter->shmem_name,
				      sizeof(filter->shmem_name) - 1)) {
			info("update shmem");
			if (filter->shmem) {
				info("Closing shmem \"%s\": %p",
				     filter->shmem_name, filter->shmem);
				CloseHandle(filter->shmem);
			}
			filter->shmem_size = 16 + width * height * 4;
			wchar_t name[256];
			filter->shmem = NULL;
			if (MultiByteToWideChar(CP_UTF8, 0, filter->destination,
						-1, name,
						(int)(sizeof(name) /
						      sizeof(name[0])))) {
				filter->shmem = CreateFileMappingW(
					INVALID_HANDLE_VALUE, NULL,
					PAGE_READWRITE, 0, filter->shmem_size,
					name);
			} else {
				warn("Shared memory name is too long: \"%s\"",
				     filter->destination);
			}
			strncpy(filter->shmem_name, filter->destination,
				sizeof(filter->shmem_name) - 1);
			filter->shmem_name[sizeof(filter->shmem_name) - 1] =
				'\0';
			info("Created shmem \"%s\": %p", filter->shmem_name,
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
	if (update)
		obs_leave_graphics();
}

static void screenshot_filter_render(void *data, gs_effect_t *effect)
{
	struct screenshot_filter_data *filter = data;
	UNUSED_PARAMETER(effect);

	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);

	// no parent: the filter is being removed, don't register a hotkey
	// that nothing would unregister
	if (!filter->capture_hotkey_id && parent) {
		info("Registering hotkey on filter render for filter %p",
		     filter);
		make_hotkey(filter);
	}

	if (!parent || !filter->width || !filter->height || !filter->capture ||
	    !filter->staging_texture || !filter->data) {
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
			// The staging row pitch is driver-aligned and can be
			// larger than width * 4, so copy row by row into a
			// tightly packed buffer.
			uint32_t row_size = filter->width * 4;
			for (uint32_t y = 0; y < filter->height; y++)
				memcpy(filter->data + y * row_size,
				       data + y * linesize, row_size);
			filter->linesize = row_size;
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

static void png_write_callback(void *context, void *data, int size)
{
	struct darray *buf = context;
	darray_push_back_array(sizeof(uint8_t), buf, data, size);
}

static bool write_image(const char *destination, uint8_t *image_data_ptr,
			uint32_t image_data_linesize, uint32_t width,
			uint32_t height, int destination_type)
{
	if (image_data_ptr == NULL)
		return false;

	struct darray png;
	darray_init(&png);

	bool success = false;
	if (stbi_write_png_to_func(png_write_callback, &png, (int)width,
				   (int)height, 4, image_data_ptr,
				   (int)image_data_linesize) &&
	    png.num) {
		success = write_data(destination, png.array, png.num,
				     "image/png", width, height,
				     destination_type);
	} else {
		warn("Failed to encode PNG (%ux%u)", width, height);
	}

	darray_free(&png);
	return success;
}

static bool write_data(const char *destination, uint8_t *data, size_t len,
		       char *content_type, uint32_t width, uint32_t height,
		       int destination_type)
{
	bool success = false;

	if (destination_type == SETTING_DESTINATION_PATH_ID) {
		FILE *of = os_fopen(destination, "wb");

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
			success = put_data((char *)destination, data, len, content_type,
					   width, height);
		}
	}
	if (destination_type == SETTING_DESTINATION_FOLDER_ID) {
		FILE *of = os_fopen(destination, "rb");

		if (of != NULL) {
			fclose(of);
		} else {
			time_t nowunixtime = time(NULL);
			struct tm *nowtime = localtime(&nowunixtime);
			if (!nowtime)
				return false;
			char _file_destination[260];
			char file_destination[260];
			bool png = !strcmp(content_type, "image/png");

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
				// name.png / name_N.png, raw: name / name_N.raw
				if (repeat_count > 0)
					dest_length = snprintf(
						file_destination, 259,
						"%s_%d%s", _file_destination,
						repeat_count,
						png ? ".png" : ".raw");
				else
					dest_length = snprintf(
						file_destination, 259, "%s%s",
						_file_destination,
						png ? ".png" : "");
				repeat_count++;

				if (dest_length <= 0 || dest_length >= 259) {
					break;
				}

				of = os_fopen(file_destination, "rb");
				if (of != NULL) {
					fclose(of);
					continue;
				}

				of = os_fopen(file_destination, "wb");
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
		"OBS Screenshot Plugin/1.6.0",
		INTERNET_OPEN_TYPE_PRECONFIG_WITH_NO_AUTOPROXY, NULL, NULL, 0);
	if (!hIntrn)
		goto err_internet_open;

	// bound how long an unreachable server can block the writer thread
	// (destroy waits for it)
	DWORD timeout_ms = 5000;
	InternetSetOptionA(hIntrn, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout_ms,
			   sizeof(timeout_ms));
	InternetSetOptionA(hIntrn, INTERNET_OPTION_SEND_TIMEOUT, &timeout_ms,
			   sizeof(timeout_ms));
	InternetSetOptionA(hIntrn, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout_ms,
			   sizeof(timeout_ms));
	DWORD retries = 1;
	InternetSetOptionA(hIntrn, INTERNET_OPTION_CONNECT_RETRIES, &retries,
			   sizeof(retries));

	HINTERNET hConn = InternetConnectA(hIntrn, host, port, NULL, NULL,
					   INTERNET_SERVICE_HTTP, 0, 0);
	if (!hConn)
		goto err_internet_connect;

	DWORD dwOpenRequestFlags = INTERNET_FLAG_KEEP_CONNECTION |
				   INTERNET_FLAG_NO_COOKIES |
				   INTERNET_FLAG_NO_CACHE_WRITE |
				   INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD;

	HINTERNET hReq = HttpOpenRequestA(hConn, "PUT", location, "HTTP/1.1",
					  NULL, NULL, dwOpenRequestFlags, 0);
	if (!hReq)
		goto err_http_open_request;

	char header[1024];
	snprintf(header, sizeof(header),
		 "Content-Type: %s\r\nImage-Width: %d\r\nImage-Height: %d\r\n",
		 content_type, width, height);

	if (HttpSendRequestA(hReq, header, (DWORD)strlen(header), buf,
			     (DWORD)len)) {
		success = true;
		info("Uploaded file to %s:%d%s", host, port, location);
	} else {
		warn("Failed to upload file to http://%s:%d%s - %lu", host, port,
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
	const char *filter_name = obs_source_get_name(filter->context);
	info("Got capture_key pressed for %s, id: %llu, key: %s, pressed: %d",
	     filter_name, (unsigned long long)id, obs_hotkey_get_name(key), pressed);

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
