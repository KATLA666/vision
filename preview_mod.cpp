#include "preview.h"

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <gbm.h>

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <iostream>
#include <unistd.h>

#define ERRSTR strerror(errno)

//Setup Variables
EGLUtil egl;
DRMUtil drm;
GBMUtil gbm;
bool first_time_ = true;
std::string display_mode = "DRM";
char const *name;
int x;
int y; 
int width;
int height;

static GLint compile_shader(GLenum target, const char *source)
{
	GLuint s = glCreateShader(target);
	glShaderSource(s, 1, (const GLchar **)&source, NULL);
	glCompileShader(s);

	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

	if (!ok)
	{
		GLchar *info;
		GLint size;

		glGetShaderiv(s, GL_INFO_LOG_LENGTH, &size);
		info = (GLchar *)malloc(size);

		glGetShaderInfoLog(s, size, NULL, info);
		throw std::runtime_error("failed to compile shader: " + std::string(info) + "\nsource:\n" +
								 std::string(source));
	}

	return s;
}

static GLint link_program(GLint vs, GLint fs)
{
	GLint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);

	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		/* Some drivers return a size of 1 for an empty log.  This is the size
		 * of a log that contains only a terminating NUL character.
		 */
		GLint size;
		GLchar *info = NULL;
		glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &size);
		if (size > 1)
		{
			info = (GLchar *)malloc(size);
			glGetProgramInfoLog(prog, size, NULL, info);
		}

		throw std::runtime_error("failed to link: " + std::string(info ? info : "<empty log>"));
	}

	return prog;
}

void gl_setup()
{
    // Verhältnisberechnung für das Viewport-Setup
    float w_factor = 1536 / (float)2160;
    float h_factor = 864 / (float)1200;
    float max_dimension = std::max(w_factor, h_factor);
    w_factor /= max_dimension;
    h_factor /= max_dimension;

    // Vertex Shader Source Code
    const char *vs_src = R"(
        attribute vec4 pos;
        varying vec2 texcoord;

        void main() {
            gl_Position = pos;
            texcoord.x = pos.x / 2.0 + 0.5;
            texcoord.y = 0.5 - pos.y / 2.0;
        }
    )";

    // Fragment Shader Source Code
    const char *fs_src = R"(
        #extension GL_OES_EGL_image_external : enable
        precision mediump float;
        uniform samplerExternalOES s;
        varying vec2 texcoord;

        void main() {
            gl_FragColor = texture2D(s, texcoord);
        }
    )";

    // Kompilierung und Verlinkung der Shader
    GLint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLint prog = link_program(vs, fs);
    
    // Benutze das erstellte Programm
    glUseProgram(prog);

    // Vertex-Daten für das Quadrat
    static const float verts[] = {
        -w_factor, -h_factor,   // unteres linkes Eck
         w_factor, -h_factor,   // unteres rechtes Eck
         w_factor,  h_factor,   // oberes rechtes Eck
        -w_factor,  h_factor    // oberes linkes Eck
    };

    // Vertex-Array und Vertex-Buffer-Objekt
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    // Setze den Vertex-Attribut-Zeiger und aktiviere ihn
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    // Erstelle und konfiguriere die Texturen
    GLuint textures[2];
    glGenTextures(2, textures);

    // Textur 1 für External OES
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, textures[0]);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Textur 2 für External OES (optional)
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, textures[1]);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Speicher freigeben: Shader und Programme löschen
    glDeleteShader(vs);
    glDeleteShader(fs);
    glDeleteProgram(prog);

    // Unbind Buffer
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static drmModeConnector *getConnector(drmModeRes *resources)
{
    for (int i = 0; i < resources->count_connectors; i++)
    {
        drmModeConnector *connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
        if (connector->connection == DRM_MODE_CONNECTED)
        {
            return connector;
        }
        drmModeFreeConnector(connector);
    }

    return NULL;
}

static drmModeEncoder *findEncoder(drmModeConnector *connector)
{
if (connector->encoder_id)
    {
        return drmModeGetEncoder(drm.fd, connector->encoder_id);
    }
    return NULL;
}

static int matchConfigToVisual(EGLDisplay display, EGLint visualId, EGLConfig *configs, int count)
{
    EGLint id;
    EGLint blue_size, red_size, green_size, alpha_size;
    for (int i = 0; i < count; ++i)
    {
        if (!eglGetConfigAttrib(display, configs[i], EGL_NATIVE_VISUAL_ID, &id))
            continue;
            
        eglGetConfigAttrib(egl.display, configs[i], EGL_RED_SIZE, &red_size);
        eglGetConfigAttrib(egl.display, configs[i], EGL_GREEN_SIZE, &green_size);
        eglGetConfigAttrib(egl.display, configs[i], EGL_BLUE_SIZE, &blue_size);
        eglGetConfigAttrib(egl.display, configs[i], EGL_ALPHA_SIZE, &alpha_size);	
        
        char gbm_format_str[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        memcpy(gbm_format_str, &id, sizeof(EGLint));
        printf("  %d-th GBM format: %s;  sizes(RGBA) = %d,%d,%d,%d,\n",
               i, gbm_format_str, red_size, green_size, blue_size, alpha_size); 
               
        if (id == visualId)
            return i; 
    }
    return -1;
}

void findPlane()
{
	drmModePlaneResPtr planes;
	drmModePlanePtr plane;
	unsigned int i;
	unsigned int j;
	planes = drmModeGetPlaneResources(drm.fd);
	if (!planes)
		throw std::runtime_error("drmModeGetPlaneResources failed: " + std::string(ERRSTR));

	try
	{
		for (i = 0; i < planes->count_planes; ++i)
		{
			plane = drmModeGetPlane(drm.fd, planes->planes[i]);
			if (!planes)
				throw std::runtime_error("drmModeGetPlane failed: " + std::string(ERRSTR));

			if (!(plane->possible_crtcs & (1 << drm.crtcIdx)))
			{
				drmModeFreePlane(plane);
				continue;
			}

			for (j = 0; j < plane->count_formats; ++j)
			{
				if (plane->formats[j] == GBM_FORMAT_XRGB8888)
				{
					break;
				}
			}

			if (j == plane->count_formats)
			{
				drmModeFreePlane(plane);
				continue;
			}
		
			drm.planeId = plane->plane_id;
			drmModeFreePlane(plane);
			break;
		}
	}
	catch (std::exception const &e)
	{
		drmModeFreePlaneResources(planes);
		throw;
	}
	drmModeFreePlaneResources(planes);
}

void setupDRM()
{
	egl.display = eglGetDisplay((EGLNativeDisplayType)gbm.device);
	if (!egl.display)
		throw std::runtime_error("eglGetDisplay() failed");

	if (!eglInitialize(egl.display, &egl.major, &egl.minor))
		throw std::runtime_error("failed to initialize\n");

	//printf("Using display %p with EGL version %d.%d\n", egl_display_, major, minor);
	//printf("EGL Version \"%s\"\n", eglQueryString(egl_display_, EGL_VERSION));
	//printf("EGL Vendor \"%s\"\n", eglQueryString(egl_display_, EGL_VENDOR));
	//printf("EGL Extensions \"%s\"\n", eglQueryString(egl_display_, EGL_EXTENSIONS));

	if (!eglBindAPI(EGL_OPENGL_ES_API))
		throw std::runtime_error("failed to bind api EGL_OPENGL_ES_API\n");
	
	if(!eglGetConfigs(egl.display, NULL, 0, &egl.num_configs) || egl.num_configs < 1)
		throw std::runtime_error("cannot get any configs\n");
		
	egl.configs = (EGLConfig*)malloc(egl.num_configs * sizeof(EGLConfig));
		
	if (!eglChooseConfig(egl.display, conf_attribs, egl.configs, egl.num_configs, &egl.num_configs))
		throw std::runtime_error("couldn't get an EGL visual config");
		
	int configIndex = matchConfigToVisual(egl.display, GBM_FORMAT_XRGB8888, egl.configs, egl.num_configs);
	if (configIndex < 0){
		eglTerminate(egl.display);
		gbm_surface_destroy(gbm.surface);
		gbm_device_destroy(gbm.device);
		throw std::runtime_error("Failed to find matching EGL config!\n");
	}
	
	egl.context = eglCreateContext(egl.display, egl.configs[configIndex], EGL_NO_CONTEXT, ctx_attribs);
	if (egl.context == EGL_NO_CONTEXT)
	{
		eglTerminate(egl.display);
		gbmClean();
		throw std::runtime_error("failed to create context\n");
	}

	egl.surface = eglCreateWindowSurface(egl.display, egl.configs[configIndex], gbm.surface, NULL);
	if (egl.surface == EGL_NO_SURFACE)
	{
		eglDestroyContext(egl.display, egl.context);
		eglTerminate(egl.display);
		gbmClean();
		throw std::runtime_error("failed to create egl surface\n");
	}
	
	free(egl.configs);
}

int makeWindow(char const *name, int x, int y, int width, int height)
{
	//Open DRM device first since its very easy to tell if we can use it or if X11 is the master.
	//We have to try card0 and card1 to see which is valid since it can vary depending on bootup
	drm.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    
	if ((drm.resources = drmModeGetResources(drm.fd)) == NULL) // if we have the right device we can get it's resources
	{
		drm.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC); // if not, try the other one: (1)
		drm.resources = drmModeGetResources(drm.fd);
	}

	/* if (!drmIsMaster(drm.fd)){ //X11 is master, so render to X11
		X11.display = XOpenDisplay(NULL);
		if (!X11.display)
			printf("Couldn't open X display");
		egl.display = eglGetDisplay((EGLNativeDisplayType)X11.display);
		if (!egl.display)
			printf("eglGetDisplay() failed");
		display_mode = "X11";} */
	else{ //DRM is master, continue DRM setup
		drm.connector = getConnector(drm.resources);
		if (!drm.connector) // we could be fancy and listen for hotplug events and wait for connector..
		{
			drmModeFreeResources(drm.resources);
			throw std::runtime_error("no connected connector!\n");
		}
	
		drm.conId = drm.connector->connector_id;
		for (int i = 0; i < drm.connector->count_modes; i++) {
			drm.mode = drm.connector->modes[i];
			printf("resolution: %ix%i %i\n", drm.mode.hdisplay, drm.mode.vdisplay, drm.mode.vrefresh);
			if (drm.mode.hdisplay == 1920 && drm.mode.vdisplay == 1080 && drm.mode.vrefresh == 60) //set display to 1080p 60Hz
				break;
		}
		//drm_mode_ = drm_connector_->modes[0]; // array of resolutions and refresh rates supported by this display
		printf("resolution: %ix%i\n", drm.mode.hdisplay, drm.mode.vdisplay);
		
		drm.encoder = findEncoder(drm.connector);
		if (drm.encoder == NULL)
		{
			drmModeFreeConnector(drm.connector);
			drmModeFreeResources(drm.resources);
			throw std::runtime_error("Unable to get encoder\n");
		}
		
		drm.crtc = drmModeGetCrtc(drm.fd, drm.encoder->crtc_id);
		drm.crtcId = drm.crtc->crtc_id;
		if (drm.resources->count_crtcs <= 0)
			throw std::runtime_error("drm: no crts");
			
		drm.crtcIdx = -1;
		for (int i = 0; i < drm.resources->count_crtcs; ++i)
		{
			if (drm.crtcId == drm.resources->crtcs[i])
			{
				drm.crtcIdx = i;
				break;
			}
		}
		if (drm.crtcIdx == -1)
		{
			drmModeFreeResources(drm.resources);
			throw std::runtime_error("drm: CRTC " + std::to_string(drm.crtcId) + " not found");
		}

		findPlane();
		
		drmModeFreeEncoder(drm.encoder);
		drmModeFreeConnector(drm.connector);
		drmModeFreeResources(drm.resources);
		
		gbm.device = gbm_create_device(drm.fd);
		gbm.surface = gbm_surface_create(gbm.device, drm.mode.hdisplay, drm.mode.vdisplay, GBM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);	
		if (!gbm.surface)
			throw std::runtime_error("failed to create gbm surface\n");
		
		egl.display = eglGetDisplay((EGLNativeDisplayType)gbm.device);
		if (!egl.display)
			throw std::runtime_error("eglGetDisplay() failed");
	}
	
	if (!eglInitialize(egl.display, &egl.major, &egl.minor))
		printf("eglInitialize() failed");
	
	/* if (display_mode == "X11"){
		setupX11(name, x, y, width, height);} */
	else
		setupDRM();
		
	// We have to do eglMakeCurrent in the thread where it will run, but we must do it
	// here temporarily so as to get the maximum texture size.
	eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl.context);
	int max_texture_size = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
	//max_image_width_ = max_image_height_ = max_texture_size;
	// This "undoes" the previous eglMakeCurrent.
	eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	
	/* if (display_mode == "X11")
		glViewport(0, 0, drm.mode.hdisplay, drm.mode.vdisplay); */
	
	//printf("here\n");
		
	return 0;
}

void makeBuffer(int fd, libcamera::StreamConfiguration const &info, libcamera::FrameBuffer *buffer, int camera_num)
{
	if (first_time_)
	{
		// This stuff has to be delayed until we know we're in the thread doing the display.
		if (!eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context))
			throw std::runtime_error("eglMakeCurrent failed");
		gl_setup();
		first_time_ = false;
	}

	EGLint attribs[] = {
		EGL_WIDTH, static_cast<EGLint>(info.size.width),
		EGL_HEIGHT, static_cast<EGLint>(info.size.height),
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_YUV420,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(info.stride),
		EGL_DMA_BUF_PLANE1_FD_EXT, fd,
		EGL_DMA_BUF_PLANE1_OFFSET_EXT, static_cast<EGLint>(info.stride * info.size.height),
		EGL_DMA_BUF_PLANE1_PITCH_EXT, static_cast<EGLint>(info.stride / 2),
		EGL_DMA_BUF_PLANE2_FD_EXT, fd,
		EGL_DMA_BUF_PLANE2_OFFSET_EXT, static_cast<EGLint>(info.stride * info.size.height + (info.stride / 2) * (info.size.height / 2)),
		EGL_DMA_BUF_PLANE2_PITCH_EXT, static_cast<EGLint>(info.stride / 2),
		EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC601_EXT, //maybe 701?
		EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT, //maybe full?
		EGL_NONE
	};

	EGLImage image = eglCreateImageKHR(egl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
	if (!image)
		throw std::runtime_error("failed to import fd " + std::to_string(fd));

	if (camera_num == 1)
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, egl.FramebufferName);
	else if (camera_num == 0)
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, egl.FramebufferName2);
	
	//glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	//glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

	eglDestroyImageKHR(egl.display, image);
}

void gbmSwapBuffers()
{
	// Lock the front buffer
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm.surface);
	if (!bo)
		throw std::runtime_error("Failed to lock front buffer");

	uint32_t handle = gbm_bo_get_handle(bo).u32;
	uint32_t pitch = gbm_bo_get_stride(bo);
	uint32_t fb;

	// Prepare the framebuffer creation parameters
	uint32_t offsets[4] = { 0 };  // No offsets needed for XRGB format
	uint32_t pitches[4] = { pitch };  // Only one pitch needed for XRGB format
	uint32_t handles[4] = { handle };  // Only one handle needed for XRGB format

	// Add framebuffer
	int ret = drmModeAddFB2(drm.fd, drm.mode.hdisplay, drm.mode.vdisplay, GBM_FORMAT_XRGB8888,
							handles, pitches, offsets, &fb, 0);
	if (ret)
	{
		gbm_surface_release_buffer(gbm.surface, bo);
		throw std::runtime_error("drmModeAddFB2 failed: " + std::string(ERRSTR));
	}

	// Set the plane
	ret = drmModeSetPlane(drm.fd, drm.planeId, drm.crtcId, fb, 0, 0, 0, drm.mode.hdisplay, drm.mode.vdisplay,
						  0, 0, drm.mode.hdisplay << 16, drm.mode.vdisplay << 16);
	if (ret)
	{
		drmModeRmFB(drm.fd, fb);
		gbm_surface_release_buffer(gbm.surface, bo);
		throw std::runtime_error("drmModeSetPlane failed: " + std::string(ERRSTR));
	}

	// Release previous buffer and remove framebuffer
	if (gbm.previousBo)
	{
		drmModeRmFB(drm.fd, gbm.previousFb);
		gbm_surface_release_buffer(gbm.surface, gbm.previousBo);
	}

	// Save current buffer as previous buffer
	gbm.previousBo = bo;
	gbm.previousFb = fb;
}

void displayFrame(int width, int height)
{
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	
	width = width/2;
	
	//Draw camera 1
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, egl.FramebufferName);
	glViewport(0,0,width,height);
	//glBindFramebuffer(GL_FRAMEBUFFER, 0);    // do i need this?
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	
	//Draw camera 2
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, egl.FramebufferName2);
	glViewport(width,0,width,height);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);    // do i need this?
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	
	eglSwapBuffers(egl.display, egl.surface);
	//if (display_mode == "DRM")
	gbmSwapBuffers();
		
	//std::cout << "displayed frame\n";
}

void gbmClean()
{
    // set the previous crtc
    drmModeSetCrtc(drm.fd, drm.crtc->crtc_id, drm.crtc->buffer_id, drm.crtc->x, drm.crtc->y, &drm.conId, 1, &drm.crtc->mode);
    drmModeFreeCrtc(drm.crtc);

    if (gbm.previousBo)
    {
        drmModeRmFB(drm.fd, gbm.previousFb);
        gbm_surface_release_buffer(gbm.surface, gbm.previousBo);
    }

    gbm_surface_destroy(gbm.surface);
    gbm_device_destroy(gbm.device);
}

void cleanup()
{
	eglDestroyContext(egl.display, egl.context);
	eglDestroySurface(egl.display, egl.surface);
	eglTerminate(egl.display);
	
	if (display_mode == "DRM")
	{
		gbmClean();
		close(drm.fd);
	}
}