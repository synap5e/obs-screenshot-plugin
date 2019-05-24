# screenshot-filter

This [OBS Studio](https://obsproject.com) filter saves images of the attatched source at a defined interval. Images can be RGBA32 .png files or raw bytes. Images can be saved to a local file or PUT to an http server.

Note that the file output mode writes to the same file each time.
A directory mode is [planned](https://github.com/synap5e/obs-screenshot-plugin/issues/2)

![demo.png](https://raw.githubusercontent.com/synap5e/obs-screenshot-plugin/readme-images/demo.png) 

### Named Shared Memory Output

To facilitate efficient high frequency access to image data, the 'Ouput to Named Shared Memory' option may be used.
This method uses CreateFileMapping with INVALID_HANDLE_VALUE to create a shared memory region that may be read by other processes.
In this mode, the screenshot filter writes 16 bytes of header information followed by the raw RGBA data to the named shared memory.
The header comprises of 4 uint32_t's in the format width, height, linesize, index.
The header is then followed by `height * linesize` bytes of image data.
