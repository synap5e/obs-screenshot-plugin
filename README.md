# screenshot-filter

This [OBS Studio](https://obsproject.com) filter saves images of the attatched source. Images can be RGBA32 .png files or raw bytes. Images can be saved to a local file, local directory, PUT to a web server, or written to a named shared memory. The plugin can be triggered on a timer or on a hotkey.

**Note for users updating from version 1.2.2 and lower**
Version 1.3 changes the default behaviour from using a timer to using hotkeys for non-shmem screenshot filters. If you are using file/HTTP destinations on a timer, you will need to "Enable timer" on the filter.
Named Shared Memory destinations can only use timer mode, and will continue to use the timer with no changes required.

![demo.png](https://raw.githubusercontent.com/synap5e/obs-screenshot-plugin/readme-images/demo.png) 
![hotkeys.png](https://raw.githubusercontent.com/synap5e/obs-screenshot-plugin/readme-images/hotkeys.png) 

## Destinations

### Output to folder
Files will be written on a hotkey/timer to the selected folder with a name in the format `2020-04-27_23-29-34.png`/`2020-04-27_23-29-34.raw`.

### Output to file
The named file will be written to on a hotkey/timer. Note that this will overwrite the file each time.

### Output to URL
The image will be PUT to the specified URL (https is not supported) on hotkey/timer. The headers `Image-Width` and `Image-Height` will be included and may be useful for raw image mode.

### Output to Named Shared Memory Output

To facilitate efficient high frequency access to image data, the 'Ouput to Named Shared Memory' option may be used.
This method uses CreateFileMapping with INVALID_HANDLE_VALUE to create a shared memory region that may be read by other processes.

This output method forces raw image and timer mode.

## Raw output

In this mode, rather than writing/posting a .png file, the screenshot filter writes the image data uncompressed.
The .raw format consists of 16 bytes of header information followed by the raw RGBA data to the named shared memory.
The header comprises of 4 uint32_t's in the format width, height, linesize, index.
The header is then followed by `height * linesize` bytes of image data. 
Note that the linesize and width may differ (e.g. `linesize%32=0`, width not constrained), so to get an image of size width\*height you may need to do strided copy. 

## Timer

In this mode, you can select for the image to be written automatically on a timer (between 250ms and 60s) in addition to on a hotkey.

# Development

## Building/Running Locally
This plugin was developed "in-tree" i.e. checking the project out into the plugins directory of a correctly-building OBS. Out-of-tree builiding should also be possible.

## Github Actions + Versioning
The plugin will build & publish releases automatically. Big thanks to @wkpark for this work.

For incrementing version numbers, remember to update `buildspec.json` to a new version, then tag HEAD with the same version.

